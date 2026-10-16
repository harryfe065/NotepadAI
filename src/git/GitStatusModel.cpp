/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Notepad Next is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Notepad Next.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "GitStatusModel.h"

#include "GitDiffPalette.h"

#include <QBrush>
#include <QCoreApplication>
#include <QSet>

namespace {
constexpr quintptr SECTION_PARENT = quintptr(-1);

// Beyond this many inserted/removed runs a reset is cheaper than replaying
// the edit script (each run shifts the bucket tail once).
constexpr int kMaxStructuralRuns = 64;


// Everything status reports about a row, numstat excluded (that arrives
// later and is carried over).
bool sameStatus(const GitStatusEntry &a, const GitStatusEntry &b)
{
    return a.change == b.change && a.stagedSide == b.stagedSide
        && a.isSubmodule == b.isSubmodule
        && a.subHasModifiedContent == b.subHasModifiedContent
        && a.hasUnstableEncoding == b.hasUnstableEncoding
        && a.isModeOnly == b.isModeOnly
        && a.xy == b.xy && a.origRelPath == b.origRelPath
        && a.oursSha == b.oursSha && a.theirsSha == b.theirsSha;
}

void carryNumstat(GitStatusEntry &to, const GitStatusEntry &from)
{
    to.addedLines = from.addedLines;
    to.deletedLines = from.deletedLines;
    to.isBinary = from.isBinary;
    to.subAddedLines = from.subAddedLines;
    to.subDeletedLines = from.subDeletedLines;
}

struct Run {
    enum Kind : std::uint8_t { Keep, Remove, Insert };
    Kind kind;
    int count;
};

void pushRun(QVector<Run> &runs, Run::Kind kind)
{
    if (!runs.isEmpty() && runs.last().kind == kind) ++runs.last().count;
    else runs.append({ kind, 1 });
}

// Edit script turning `cur` into `next`, matching rows by relPath with a
// merge walk. Valid for any order (every next row is kept or inserted in
// order, every cur row kept or removed); minimal when both lists share an
// order, which git's path-sorted output gives.
QVector<Run> diffRuns(const GitStatusEntries &cur, const GitStatusEntries &next)
{
    QVector<Run> runs;
    qsizetype i = 0;
    qsizetype j = 0;
    while (i < cur.size() && j < next.size()) {
        const int c = QString::compare(cur.at(i).relPath, next.at(j).relPath);
        if (c == 0) { pushRun(runs, Run::Keep); ++i; ++j; }
        else if (c < 0) { pushRun(runs, Run::Remove); ++i; }
        else { pushRun(runs, Run::Insert); ++j; }
    }
    for (; i < cur.size(); ++i) pushRun(runs, Run::Remove);
    for (; j < next.size(); ++j) pushRun(runs, Run::Insert);
    return runs;
}

int structuralRuns(const QVector<Run> &runs)
{
    int n = 0;
    for (const Run &r : runs) n += r.kind != Run::Keep;
    return n;
}
} // namespace

GitStatusModel::GitStatusModel(QObject *parent) : QAbstractItemModel(parent)
{
    rebuildVisible();
}

QString GitStatusModel::sectionTitle(GitStatusEntry::Section s)
{
    switch (s) {
        case GitStatusEntry::Conflicts: return QCoreApplication::translate("GitStatusModel", "Conflicts");
        case GitStatusEntry::Staged:    return QCoreApplication::translate("GitStatusModel", "Staged");
        case GitStatusEntry::Tracked:   return QCoreApplication::translate("GitStatusModel", "Tracked");
        case GitStatusEntry::Untracked: return QCoreApplication::translate("GitStatusModel", "Untracked");
        default: return QString();
    }
}

QString GitStatusModel::changeShort(GitStatusEntry::Change c)
{
    switch (c) {
        case GitStatusEntry::Added:        return QStringLiteral("A");
        case GitStatusEntry::Modified:     return QStringLiteral("M");
        case GitStatusEntry::Deleted:      return QStringLiteral("D");
        case GitStatusEntry::Renamed:      return QStringLiteral("R");
        case GitStatusEntry::Copied:       return QStringLiteral("C");
        case GitStatusEntry::TypeChanged:  return QStringLiteral("T");
        case GitStatusEntry::Untracked_:   return QStringLiteral("?");
        case GitStatusEntry::Unmerged:     return QStringLiteral("U");
    }
    return QStringLiteral("?");
}

void GitStatusModel::rebuildVisible()
{
    m_visibleSections.clear();
    for (int s = 0; s < GitStatusEntry::SectionCount; ++s) {
        if (!m_buckets[s].isEmpty()) m_visibleSections.append(static_cast<GitStatusEntry::Section>(s));
    }
}

int GitStatusModel::visibleRowOf(GitStatusEntry::Section s) const
{
    for (int v = 0; v < m_visibleSections.size(); ++v)
        if (m_visibleSections.at(v) == s) return v;
    return -1;
}

QModelIndex GitStatusModel::sectionIndex(GitStatusEntry::Section s) const
{
    const int v = visibleRowOf(s);
    return v < 0 ? QModelIndex() : createIndex(v, 0, SECTION_PARENT);
}

void GitStatusModel::resetTo(GitStatusEntries (&next)[GitStatusEntry::SectionCount])
{
    beginResetModel();
    for (int s = 0; s < GitStatusEntry::SectionCount; ++s) {
        // Keep the last known numstat for rows that survive, like the
        // incremental path does.
        QHash<QString, const GitStatusEntry *> old;
        old.reserve(m_buckets[s].size());
        for (const GitStatusEntry &e : std::as_const(m_buckets[s])) old.insert(e.relPath, &e);
        for (GitStatusEntry &e : next[s]) {
            const auto it = old.constFind(e.relPath);
            if (it != old.constEnd()) carryNumstat(e, **it);
        }
    }
    for (int s = 0; s < GitStatusEntry::SectionCount; ++s) m_buckets[s] = std::move(next[s]);
    rebuildVisible();
    endResetModel();
}

void GitStatusModel::setEntries(GitStatusEntries entries)
{
    ++m_changeSerial;
    GitStatusEntries next[GitStatusEntry::SectionCount];
    for (GitStatusEntry &e : entries) next[e.section].append(std::move(e));
    entries.clear();

    // Plan first: a fragmented script (mass rename, repo switch) resets.
    QVector<Run> runs[GitStatusEntry::SectionCount];
    int structural = 0;
    for (int s = 0; s < GitStatusEntry::SectionCount; ++s) {
        if (m_buckets[s].isEmpty() || next[s].isEmpty()) continue;
        runs[s] = diffRuns(m_buckets[s], next[s]);
        structural += structuralRuns(runs[s]);
    }
    if (structural > kMaxStructuralRuns) {
        resetTo(next);
        return;
    }

    for (int si = 0; si < GitStatusEntry::SectionCount; ++si) {
        const auto s = static_cast<GitStatusEntry::Section>(si);
        GitStatusEntries &bucket = m_buckets[s];
        GitStatusEntries &incoming = next[s];
        if (bucket.isEmpty() && incoming.isEmpty()) continue;

        if (incoming.isEmpty()) {
            const int v = visibleRowOf(s);
            beginRemoveRows(QModelIndex(), v, v);
            bucket.clear();
            rebuildVisible();
            endRemoveRows();
            continue;
        }
        if (bucket.isEmpty()) {
            // Sections before s are already final, so the new row's position
            // is the number of visible sections below s.
            int v = 0;
            while (v < m_visibleSections.size() && m_visibleSections.at(v) < s) ++v;
            beginInsertRows(QModelIndex(), v, v);
            bucket = std::move(incoming);
            rebuildVisible();
            endInsertRows();
            continue;
        }

        const QModelIndex parentIdx = sectionIndex(s);
        const qsizetype oldCount = bucket.size();
        qsizetype row = 0;
        qsizetype j = 0;
        qsizetype firstChanged = -1;
        qsizetype lastChanged = -1;
        const auto flushChanged = [&]() {
            if (firstChanged < 0) return;
            emit dataChanged(createIndex(int(firstChanged), 0, quintptr(s)),
                             createIndex(int(lastChanged), 0, quintptr(s)));
            firstChanged = -1;
        };
        for (const Run &r : std::as_const(runs[s])) {
            switch (r.kind) {
            case Run::Keep:
                for (int k = 0; k < r.count; ++k, ++row, ++j) {
                    GitStatusEntry &cur = bucket[row];
                    if (sameStatus(cur, incoming.at(j))) continue;
                    GitStatusEntry fresh = std::move(incoming[j]);
                    carryNumstat(fresh, cur);
                    cur = std::move(fresh);
                    if (firstChanged >= 0 && lastChanged + 1 != row) flushChanged();
                    if (firstChanged < 0) firstChanged = row;
                    lastChanged = row;
                }
                break;
            case Run::Remove:
                flushChanged();
                beginRemoveRows(parentIdx, int(row), int(row + r.count - 1));
                bucket.remove(row, r.count);
                endRemoveRows();
                break;
            case Run::Insert:
                flushChanged();
                beginInsertRows(parentIdx, int(row), int(row + r.count - 1));
                bucket.insert(row, r.count, GitStatusEntry());
                for (int k = 0; k < r.count; ++k) bucket[row + k] = std::move(incoming[j + k]);
                endInsertRows();
                row += r.count;
                j += r.count;
                break;
            }
        }
        flushChanged();
        // Section title carries the row count.
        if (bucket.size() != oldCount)
            emit dataChanged(parentIdx, parentIdx, { Qt::DisplayRole });
    }
}

int GitStatusModel::columnCount(const QModelIndex &) const { return 1; }

int GitStatusModel::rowCount(const QModelIndex &parent) const
{
    if (!parent.isValid()) return m_visibleSections.size();
    if (parent.internalId() != SECTION_PARENT) return 0;
    const int row = parent.row();
    if (row < 0 || row >= m_visibleSections.size()) return 0;
    return m_buckets[m_visibleSections.at(row)].size();
}

QModelIndex GitStatusModel::index(int row, int col, const QModelIndex &parent) const
{
    if (col != 0) return {};
    if (!parent.isValid()) {
        if (row < 0 || row >= m_visibleSections.size()) return {};
        return createIndex(row, col, SECTION_PARENT);
    }
    if (parent.internalId() != SECTION_PARENT) return {};
    if (parent.row() < 0 || parent.row() >= m_visibleSections.size()) return {};
    const GitStatusEntry::Section s = m_visibleSections.at(parent.row());
    if (row < 0 || row >= m_buckets[s].size()) return {};
    return createIndex(row, col, quintptr(s));
}

QModelIndex GitStatusModel::parent(const QModelIndex &child) const
{
    if (!child.isValid()) return {};
    if (child.internalId() == SECTION_PARENT) return {};
    if (child.internalId() >= GitStatusEntry::SectionCount) return {};
    return sectionIndex(static_cast<GitStatusEntry::Section>(child.internalId()));
}

Qt::ItemFlags GitStatusModel::flags(const QModelIndex &index) const
{
    if (!index.isValid()) return Qt::NoItemFlags;
    Qt::ItemFlags f = Qt::ItemIsEnabled;
    if (index.internalId() != SECTION_PARENT) f |= Qt::ItemIsSelectable;
    return f;
}

bool GitStatusModel::isSection(const QModelIndex &i) const
{
    return i.isValid() && i.internalId() == SECTION_PARENT;
}

GitStatusEntry::Section GitStatusModel::sectionOf(const QModelIndex &i) const
{
    if (!i.isValid()) return GitStatusEntry::Tracked;
    if (isSection(i)) {
        const int row = i.row();
        if (row < 0 || row >= m_visibleSections.size()) return GitStatusEntry::Tracked;
        return m_visibleSections.at(row);
    }
    if (i.internalId() >= GitStatusEntry::SectionCount) return GitStatusEntry::Tracked;
    return static_cast<GitStatusEntry::Section>(i.internalId());
}

int GitStatusModel::entriesInSection(GitStatusEntry::Section s) const
{
    return m_buckets[s].size();
}

QStringList GitStatusModel::relPathsInSection(GitStatusEntry::Section s) const
{
    const auto &bucket = m_buckets[s];
    QStringList out;
    out.reserve(bucket.size());
    for (const GitStatusEntry &e : bucket)
        out.append(e.relPath);
    return out;
}

int GitStatusModel::totalEntries() const
{
    int t = 0;
    for (int s = 0; s < GitStatusEntry::SectionCount; ++s) t += m_buckets[s].size();
    return t;
}

QVariant GitStatusModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid()) return {};

    if (isSection(index)) {
        const auto sec = sectionOf(index);
        const int count = m_buckets[sec].size();
        if (role == Qt::DisplayRole)
            return QStringLiteral("%1 (%2)").arg(sectionTitle(sec)).arg(count);
        if (role == IsSectionRole) return true;
        if (role == SectionRole)   return int(sec);
        return {};
    }

    const auto sec = sectionOf(index);
    const auto &bucket = m_buckets[sec];
    if (index.row() < 0 || index.row() >= bucket.size()) return {};
    const GitStatusEntry &e = bucket.at(index.row());

    switch (role) {
        case Qt::DisplayRole: {
            QString prefix = changeShort(e.change);
            QString path = e.relPath;
            if (e.change == GitStatusEntry::Renamed && !e.origRelPath.isEmpty())
                path = QStringLiteral("%1 → %2").arg(e.origRelPath, e.relPath);
            return QStringLiteral("%1  %2").arg(prefix, path);
        }
        case Qt::ToolTipRole:    return e.relPath;
        case Qt::ForegroundRole: {
            const auto &pal = GitDiffPalette::current(m_isDark);
            if (sec == GitStatusEntry::Conflicts) return QBrush(pal.fgConflict);
            switch (e.change) {
                case GitStatusEntry::Added:        return QBrush(pal.fgAdded);
                case GitStatusEntry::Untracked_:   return QBrush(pal.fgUntracked);
                case GitStatusEntry::Modified:     return QBrush(pal.fgModified);
                case GitStatusEntry::TypeChanged:  return QBrush(pal.fgModified);
                case GitStatusEntry::Deleted:      return QBrush(pal.fgDeleted);
                case GitStatusEntry::Renamed:      return QBrush(pal.fgRenamed);
                case GitStatusEntry::Copied:       return QBrush(pal.fgRenamed);
                case GitStatusEntry::Unmerged:     return QBrush(pal.fgConflict);
            }
            return {};
        }
        case RelPathRole:                return e.relPath;
        case OrigPathRole:               return e.origRelPath;
        case ChangeRole:                 return int(e.change);
        case StagedSideRole:             return e.stagedSide;
        case SectionRole:                return int(e.section);
        case XyRole:                     return e.xy;
        case IsSectionRole:              return false;
        case AddedLinesRole:             return e.addedLines;
        case DeletedLinesRole:           return e.deletedLines;
        case IsBinaryRole:               return e.isBinary;
        case HasUnstableEncodingRole:    return e.hasUnstableEncoding;
        case OursShaRole:                return e.oursSha;
        case TheirsShaRole:              return e.theirsSha;
        case IsSubmoduleRole:            return e.isSubmodule;
        case SubAddedLinesRole:          return e.subAddedLines;
        case SubDeletedLinesRole:        return e.subDeletedLines;
        default: return {};
    }
}

void GitStatusModel::mergeNumstat(const QHash<QString, GitNumstatParser::Stat> &stats, bool stagedSide)
{
    for (int si = 0; si < GitStatusEntry::SectionCount; ++si) {
        const bool sectionIsStaged = (si == GitStatusEntry::Staged);
        if (sectionIsStaged != stagedSide) continue;
        // Untracked rows never appear in `git diff --numstat`.
        if (si == GitStatusEntry::Untracked) continue;
        auto &bucket = m_buckets[si];
        int first = -1;
        int last = -1;
        static const QList<int> kNumstatRoles{ Qt::DisplayRole, GitStatusModel::AddedLinesRole,
                                               GitStatusModel::DeletedLinesRole, GitStatusModel::IsBinaryRole };
        const auto flush = [&]() {
            if (first < 0) return;
            emit dataChanged(createIndex(first, 0, quintptr(si)),
                             createIndex(last, 0, quintptr(si)), kNumstatRoles);
            first = -1;
        };
        for (int r = 0; r < bucket.size(); ++r) {
            GitStatusEntry &e = bucket[r];
            qint32 added = -1;
            qint32 deleted = -1;
            bool binary = false;
            const auto it = stats.constFind(e.relPath);
            if (it != stats.constEnd()) {
                added = it->added;
                deleted = it->deleted;
                binary = it->isBinary;
            }
            if (e.addedLines == added && e.deletedLines == deleted && e.isBinary == binary) {
                flush();
                continue;
            }
            e.addedLines = added;
            e.deletedLines = deleted;
            e.isBinary = binary;
            if (first < 0) first = r;
            last = r;
        }
        flush();
    }
}

void GitStatusModel::mergeSubmoduleStats(const QString &relPath, qint32 added, qint32 deleted)
{
    if (relPath.isEmpty()) return;
    // Apply to every section: a submodule with both pointer + content change
    // can appear in both Staged and Tracked, and we want both rows to show the
    // same inner stats.
    for (int s = 0; s < GitStatusEntry::SectionCount; ++s) {
        auto &bucket = m_buckets[s];
        for (int r = 0; r < bucket.size(); ++r) {
            if (bucket.at(r).relPath != relPath) continue;
            GitStatusEntry &e = bucket[r];
            if (e.subAddedLines == added && e.subDeletedLines == deleted) continue;
            e.subAddedLines = added;
            e.subDeletedLines = deleted;
            const QModelIndex idx = createIndex(r, 0, quintptr(s));
            emit dataChanged(idx, idx, { Qt::DisplayRole, SubAddedLinesRole, SubDeletedLinesRole });
        }
    }
}

QStringList GitStatusModel::modifiedSubmodulePaths() const
{
    QStringList out;
    for (int s = 0; s < GitStatusEntry::SectionCount; ++s) {
        for (const GitStatusEntry &e : m_buckets[s]) {
            if (!e.isSubmodule || !e.subHasModifiedContent) continue;
            if (!out.contains(e.relPath)) out.append(e.relPath);
        }
    }
    return out;
}

void GitStatusModel::setDarkPalette(bool isDark)
{
    if (m_isDark == isDark) return;
    m_isDark = isDark;
    if (m_visibleSections.isEmpty()) return;
    const QModelIndex topLeft = index(0, 0);
    const QModelIndex botRight = index(m_visibleSections.size() - 1, 0);
    emit dataChanged(topLeft, botRight, { Qt::ForegroundRole });
    // Children rows too
    for (int v = 0; v < m_visibleSections.size(); ++v) {
        const QModelIndex secIdx = index(v, 0);
        const int rc = rowCount(secIdx);
        if (rc == 0) continue;
        emit dataChanged(index(0, 0, secIdx), index(rc - 1, 0, secIdx), { Qt::ForegroundRole });
    }
}

GitStatusEntries GitStatusModel::allEntries() const
{
    GitStatusEntries out;
    int total = 0;
    for (int s = 0; s < GitStatusEntry::SectionCount; ++s) {
        total += m_buckets[s].size();
    }
    out.reserve(total);
    for (int s = 0; s < GitStatusEntry::SectionCount; ++s) {
        for (const GitStatusEntry &e : m_buckets[s]) {
            out.append(e);
        }
    }
    return out;
}

namespace {
// Ordered, de-duplicated path collector (selections can name a section and
// its rows at once).
struct PathCollector {
    QStringList out;
    QSet<QString> seen;
    void add(const QString &p)
    {
        if (!seen.contains(p)) { seen.insert(p); out.append(p); }
    }
};
} // namespace

QStringList GitStatusModel::relPathsAtIndexes(const QModelIndexList &idxs) const
{
    PathCollector c;
    for (const QModelIndex &i : idxs) {
        if (!i.isValid()) continue;
        const auto sec = sectionOf(i);
        if (isSection(i)) {
            for (const GitStatusEntry &e : m_buckets[sec]) c.add(e.relPath);
        } else if (i.row() < m_buckets[sec].size()) {
            c.add(m_buckets[sec].at(i.row()).relPath);
        }
    }
    return c.out;
}

QStringList GitStatusModel::stagedSelectionPaths(const QModelIndexList &idxs) const
{
    PathCollector c;
    for (const QModelIndex &i : idxs) {
        if (!i.isValid()) continue;
        const auto sec = sectionOf(i);
        if (sec != GitStatusEntry::Staged) continue;
        if (isSection(i)) {
            for (const GitStatusEntry &e : m_buckets[sec]) c.add(e.relPath);
        } else if (i.row() < m_buckets[sec].size()) {
            c.add(m_buckets[sec].at(i.row()).relPath);
        }
    }
    return c.out;
}

QStringList GitStatusModel::unstagedSelectionPaths(const QModelIndexList &idxs) const
{
    PathCollector c;
    for (const QModelIndex &i : idxs) {
        if (!i.isValid()) continue;
        const auto sec = sectionOf(i);
        if (sec != GitStatusEntry::Tracked && sec != GitStatusEntry::Untracked) continue;
        if (isSection(i)) {
            for (const GitStatusEntry &e : m_buckets[sec]) c.add(e.relPath);
        } else if (i.row() < m_buckets[sec].size()) {
            c.add(m_buckets[sec].at(i.row()).relPath);
        }
    }
    return c.out;
}

bool GitStatusModel::selectionHasStaged(const QItemSelection &sel) const
{
    for (const QItemSelectionRange &r : sel) {
        if (!r.isValid() || r.model() != this || !r.parent().isValid()) continue;
        if (sectionOf(r.parent()) == GitStatusEntry::Staged) return true;
    }
    return false;
}

bool GitStatusModel::selectionHasUnstaged(const QItemSelection &sel) const
{
    for (const QItemSelectionRange &r : sel) {
        if (!r.isValid() || r.model() != this || !r.parent().isValid()) continue;
        const auto sec = sectionOf(r.parent());
        if (sec == GitStatusEntry::Tracked || sec == GitStatusEntry::Untracked) return true;
    }
    return false;
}
