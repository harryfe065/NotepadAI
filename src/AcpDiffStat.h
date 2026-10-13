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

#ifndef ACP_DIFF_STAT_H
#define ACP_DIFF_STAT_H

#include <QString>

#include "AcpProtocol.h"

class QJsonArray;
class QJsonObject;

// Lines added / removed by a tool call's `diff` content blocks, for the
// "Worked for Xs · N tool calls · +6 -10" fold row. Reads the same block
// shapes AcpToolCallCard renders: a compact diff (unified or omp numbered)
// wins, else oldText/newText (oldText null or empty = new file).
//
// Cost: one linear scan of the compact diff, or one xdiff (histogram) run
// over oldText/newText; above kMaxExactDiffChars per side the count falls back
// to common prefix/suffix trimming, linear in the text size. Pure, no state.
namespace AcpDiffStat {

inline constexpr qsizetype kMaxExactDiffChars = 512 * 1024;

// Added / removed marks, shared by the tool-call card's diff rows and the fold
// row. Content accents (like the diff itself), not chrome.
inline constexpr char kAddedColor[] = "#4caf50";
inline constexpr char kRemovedColor[] = "#d9534f";

struct Lines
{
    int added = 0;
    int removed = 0;

    bool isEmpty() const noexcept { return added == 0 && removed == 0; }
    Lines &operator+=(const Lines &other) noexcept
    {
        added += other.added;
        removed += other.removed;
        return *this;
    }
};

Lines forBlock(const QJsonObject &block);
Lines forContent(const QJsonArray &content);
// Only a completed call changed anything: a pending call's diff is a proposal,
// a failed or cancelled one may not have landed.
Lines forToolCall(const AcpProtocol::AcpToolCall &tc);

// "+6 -10"; empty when nothing changed.
QString label(const Lines &lines);

} // namespace AcpDiffStat

#endif // ACP_DIFF_STAT_H
