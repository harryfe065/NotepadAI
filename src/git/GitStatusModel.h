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

#ifndef GIT_STATUS_MODEL_H
#define GIT_STATUS_MODEL_H

#include "GitNumstatParser.h"
#include "GitStatusEntry.h"

#include <QAbstractItemModel>
#include <QHash>
#include <QItemSelection>

#include <cstdint>

// Two-level tree:
//   Section row (parent = invalid) holds title + count + child entries
//   File row (parent = section) holds one GitStatusEntry
//
// Sections with zero children are hidden.
class GitStatusModel : public QAbstractItemModel
{
    Q_OBJECT
public:
    enum Roles : std::uint16_t {
        RelPathRole = Qt::UserRole + 1,
        ChangeRole,
        OrigPathRole,
        StagedSideRole,
        SectionRole,
        XyRole,
        IsSectionRole,
        EntryRole,
        AddedLinesRole,
        DeletedLinesRole,
        IsBinaryRole,
        HasUnstableEncodingRole,
        OursShaRole,
        TheirsShaRole,
        IsSubmoduleRole,
        SubAddedLinesRole,
        SubDeletedLinesRole
    };

    explicit GitStatusModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QModelIndex index(int row, int col, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    // Replace the contents with `entries` (parser order). Rows whose path stays
    // in a section are kept in place, so selection, expansion, scroll and the
    // last known numstat survive (numstat is refreshed by mergeNumstat later);
    // only inserted/removed runs and changed rows are signalled. Falls back to
    // a model reset when the edit script is too fragmented.
    void setEntries(GitStatusEntries entries);
    // Bumped by every setEntries() (numstat merges excluded), so observers
    // can tell whether the status rows moved since they last looked.
    std::uint64_t changeSerial() const { return m_changeSerial; }
    // Merge numstat hash (keyed by relPath) into the matching side; rows of
    // that side missing from `stats` go back to unknown (-1). Emits one
    // dataChanged per run of rows whose values changed; a repeat call with
    // the same stats emits nothing.
    void mergeNumstat(const QHash<QString, GitNumstatParser::Stat> &stats, bool stagedSide);

    // Merge aggregated inner-submodule stats. Applies to ALL entries with the
    // matching relPath (a submodule can show in both Staged and Tracked).
    void mergeSubmoduleStats(const QString &relPath, qint32 added, qint32 deleted);

    // Enumerate unique relPaths of submodule entries with modified content
    // (sub field 'S.M.' / 'SCM.' / etc). Pointer-only changes are excluded —
    // their inner diff is empty anyway, so spawning a process would waste cost.
    QStringList modifiedSubmodulePaths() const;

    void setDarkPalette(bool isDark);

    bool isSection(const QModelIndex &i) const;
    GitStatusEntry::Section sectionOf(const QModelIndex &i) const;
    int entriesInSection(GitStatusEntry::Section s) const;
    int totalEntries() const;
    bool hasConflicts() const { return !m_buckets[GitStatusEntry::Conflicts].isEmpty(); }
    bool hasStaged() const   { return !m_buckets[GitStatusEntry::Staged].isEmpty(); }

    // Helpers for the view: pick the rel paths under a list of indexes
    // (sections expand to their children).
    QStringList relPathsAtIndexes(const QModelIndexList &idxs) const;
    QStringList relPathsInSection(GitStatusEntry::Section s) const;
    QStringList stagedSelectionPaths(const QModelIndexList &idxs) const;
    QStringList unstagedSelectionPaths(const QModelIndexList &idxs) const;
    // True when a selection (child rows of sections) reaches the staged /
    // unstaged side. O(ranges), no path list is built.
    bool selectionHasStaged(const QItemSelection &sel) const;
    bool selectionHasUnstaged(const QItemSelection &sel) const;

    // Concatenated view of all sections' entries — section-order:
    // Conflicts, Staged, Tracked, Untracked. Returned by value (cheap copy
    // on typical dirty-set sizes). Used by file-tree-git-decorations to
    // feed PathStatusIndex without duplicating parser state.
    GitStatusEntries allEntries() const;

    static QString sectionTitle(GitStatusEntry::Section s);
    static QString changeShort(GitStatusEntry::Change c);

private:
    // Child indexes carry their Section value as internalId (sections carry
    // SECTION_PARENT), so they stay valid while sections appear or vanish
    // above them.
    GitStatusEntries m_buckets[GitStatusEntry::SectionCount];
    // Map from visible row index → real Section value. Sections with 0 entries are skipped.
    QVector<GitStatusEntry::Section> m_visibleSections;
    std::uint64_t m_changeSerial = 0;
    bool m_isDark = false;
    void rebuildVisible();
    int visibleRowOf(GitStatusEntry::Section s) const;
    QModelIndex sectionIndex(GitStatusEntry::Section s) const;
    void resetTo(GitStatusEntries (&next)[GitStatusEntry::SectionCount]);
};

#endif // GIT_STATUS_MODEL_H
