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

#include "GitStatusParser.h"

#include <QByteArrayView>

#include <array>
#include <cstring>

namespace {

// Split the first `firstN` space-separated fields off `line`; out[firstN] gets
// the rest verbatim (the path may contain spaces). Views into `line`, no
// allocation. False when the record has fewer fields than expected.
bool splitFields(QByteArrayView line, int firstN, QByteArrayView *out)
{
    const char *d = line.data();
    const qsizetype n = line.size();
    qsizetype start = 0;
    for (int field = 0; field < firstN; ++field) {
        const void *sp = std::memchr(d + start, ' ', size_t(n - start));
        if (sp == nullptr) return false;
        const qsizetype i = static_cast<const char *>(sp) - d;
        out[field] = line.sliced(start, i - start);
        start = i + 1;
    }
    out[firstN] = line.sliced(start);
    return true;
}

// The XY field is two status letters out of a small alphabet. Hand out shared
// copies of one QString per pair instead of allocating one per record.
QString internXy(QByteArrayView xyB)
{
    static constexpr char kAlphabet[] = ".MTADRCU?! ";
    static constexpr int kN = sizeof(kAlphabet) - 1;
    static const std::array<QString, kN * kN> table = []() {
        std::array<QString, kN * kN> t;
        for (int a = 0; a < kN; ++a)
            for (int b = 0; b < kN; ++b) {
                const char pair[2] = { kAlphabet[a], kAlphabet[b] };
                t[a * kN + b] = QString::fromLatin1(pair, 2);
            }
        return t;
    }();
    if (xyB.size() == 2) {
        const char *a = static_cast<const char *>(std::memchr(kAlphabet, xyB.at(0), kN));
        const char *b = static_cast<const char *>(std::memchr(kAlphabet, xyB.at(1), kN));
        if (a != nullptr && b != nullptr)
            return table[(a - kAlphabet) * kN + (b - kAlphabet)];
    }
    return QString::fromLatin1(xyB);
}

GitStatusEntry makeEntry(GitStatusEntry::Section sec,
                         GitStatusEntry::Change ch,
                         QByteArrayView rawPath,
                         QByteArrayView rawOrig,
                         bool stagedSide,
                         const QString &xy)
{
    GitStatusEntry e;
    e.section = sec;
    e.change = ch;
    e.relPath = QString::fromUtf8(rawPath);
    if (!rawOrig.isEmpty()) e.origRelPath = QString::fromUtf8(rawOrig);
    e.stagedSide = stagedSide;
    e.xy = xy;
    // Not valid UTF-8 means git emitted the raw bytes (core.quotepath=false)
    // and the decoded QString cannot round-trip them.
    e.hasUnstableEncoding = !rawPath.isValidUtf8() || !rawOrig.isValidUtf8();
    return e;
}

// Decode the porcelain v2 `sub` field. Format: "<S|N><C|.><M|.><U|.>", where
// S=submodule, C=commit changed, M=tracked content modified, U=has untracked.
// For non-submodules ("N..."), all flags stay false.
void applySubFlags(GitStatusEntry &e, QByteArrayView subField)
{
    if (subField.size() < 1 || subField.at(0) != 'S') return;
    e.isSubmodule = true;
    if (subField.size() >= 3 && subField.at(2) == 'M') e.subHasModifiedContent = true;
}

// One changed record ('1' or '2') yields up to two entries: staged (X) and
// worktree (Y).
void appendChanged(GitStatusEntries &result, QByteArrayView xyB, QByteArrayView subB,
                   QByteArrayView rawPath, QByteArrayView rawOrig)
{
    if (xyB.size() < 2) return;
    const char x = xyB.at(0);
    const char y = xyB.at(1);
    const QString xy = internXy(xyB);
    if (x != '.' && x != ' ') {
        GitStatusEntry e = makeEntry(GitStatusEntry::Staged,
                                     GitStatusParser::xyToChange(x, y, true),
                                     rawPath, rawOrig, true, xy);
        applySubFlags(e, subB);
        result.append(std::move(e));
    }
    if (y != '.' && y != ' ') {
        GitStatusEntry e = makeEntry(GitStatusEntry::Tracked,
                                     GitStatusParser::xyToChange(x, y, false),
                                     rawPath, rawOrig, false, xy);
        applySubFlags(e, subB);
        result.append(std::move(e));
    }
}

} // namespace

bool GitStatusParser::isUnmerged(char x, char y)
{
    // From git status docs: any of DD AU UD UA DU AA UU
    if (x == 'U' || y == 'U') return true;
    if (x == 'A' && y == 'A') return true;
    if (x == 'D' && y == 'D') return true;
    return false;
}

GitStatusEntry::Change GitStatusParser::xyToChange(char x, char y, bool stagedSide)
{
    const char c = stagedSide ? x : y;
    switch (c) {
        case 'A': return GitStatusEntry::Added;
        case 'M': return GitStatusEntry::Modified;
        case 'D': return GitStatusEntry::Deleted;
        case 'R': return GitStatusEntry::Renamed;
        case 'C': return GitStatusEntry::Copied;
        case 'T': return GitStatusEntry::TypeChanged;
        case '?': return GitStatusEntry::Untracked_;
        case 'U': return GitStatusEntry::Unmerged;
        default:  return GitStatusEntry::Modified;
    }
}

GitStatusEntries GitStatusParser::parsePorcelainV2(const QByteArray &input,
                                                    Header *header)
{
    GitStatusEntries result;
    // Shortest useful record is "? x\0"; real ones average well above 64 bytes.
    result.reserve(input.size() / 64);

    // Records are nul-separated. Renamed records contain TWO paths separated by nul,
    // so we walk record-by-record based on the leading marker. All fields are
    // views into `input`; only the decoded paths allocate.
    const QByteArrayView all(input);
    const char *base = all.data();
    const qsizetype n = all.size();
    qsizetype i = 0;
    const auto nextSegment = [&]() {
        const void *z = std::memchr(base + i, '\0', size_t(n - i));
        const qsizetype end = z ? static_cast<const char *>(z) - base : n;
        const QByteArrayView seg = all.sliced(i, end - i);
        i = end + 1;
        return seg;
    };

    QByteArrayView fields[11];
    while (i < n) {
        const QByteArrayView rec = nextSegment();
        if (rec.isEmpty()) continue;

        const char marker = rec.at(0);

        if (marker == '1') {
            // "1 XY sub mH mI mW hH hI path" — path is field index 8.
            if (!splitFields(rec, 8, fields)) continue;
            appendChanged(result, fields[1], fields[2], fields[8], {});
        }
        else if (marker == '2') {
            // "2 XY sub mH mI mW hH hI X<score> path\0origPath" — path is
            // field index 9; the orig path is the next nul-terminated segment.
            if (!splitFields(rec, 9, fields)) continue;
            const QByteArrayView rawOrig = i < n ? nextSegment() : QByteArrayView();
            appendChanged(result, fields[1], fields[2], fields[9], rawOrig);
        }
        else if (marker == 'u') {
            // "u XY sub m1 m2 m3 mW h1 h2 h3 path"
            //   field index: 0  1  2  3  4  5  6  7  8  9  10
            // h2 = stage2 (ours), h3 = stage3 (theirs)
            if (!splitFields(rec, 10, fields)) continue;
            GitStatusEntry e = makeEntry(GitStatusEntry::Conflicts,
                                         GitStatusEntry::Unmerged,
                                         fields[10], {}, false, internXy(fields[1]));
            e.oursSha   = QString::fromLatin1(fields[8]);
            e.theirsSha = QString::fromLatin1(fields[9]);
            result.append(std::move(e));
        }
        else if (marker == '?') {
            // "? path"
            static const QString kUntrackedXy = QStringLiteral("??");
            result.append(makeEntry(GitStatusEntry::Untracked,
                                    GitStatusEntry::Untracked_,
                                    rec.size() > 2 ? rec.sliced(2) : QByteArrayView(),
                                    {}, false, kUntrackedXy));
        }
        else if (marker == '#' && header) {
            // '#' = porcelain v2 branch header (only present with --branch).
            // '!' = ignored — dropped like any other unknown marker.
            // Field layout: "# branch.<key> <value...>"
            if (rec.startsWith("# branch.upstream ")) {
                header->hasUpstream = true;
            } else if (rec.startsWith("# branch.ab ")) {
                // "+<ahead> -<behind>" — parse two signed integers.
                const QByteArrayView rest = rec.sliced(qsizetype(strlen("# branch.ab ")));
                const qsizetype space = rest.indexOf(' ');
                if (space > 0 && rest.at(0) == '+' && space + 1 < rest.size()
                    && rest.at(space + 1) == '-') {
                    bool okA = false, okB = false;
                    const int a = rest.sliced(1, space - 1).toInt(&okA);
                    const int b = rest.sliced(space + 2).toInt(&okB);
                    if (okA) header->ahead  = a;
                    if (okB) header->behind = b;
                    header->hasUpstream = true;
                }
            }
        }
    }
    return result;
}
