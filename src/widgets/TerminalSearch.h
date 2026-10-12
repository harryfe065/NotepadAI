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

#ifndef TERMINALSEARCH_H
#define TERMINALSEARCH_H

#include <vterm.h>

#include <QRegularExpression>
#include <QString>

#include <cstdint>
#include <vector>

struct TerminalSearchOptions
{
    QString query;
    bool caseSensitive = false;
    bool regex = false;
    bool wholeWord = false;
};

// A match on one terminal row. `line` is an absolute line id that survives
// scrollback eviction (lines ever dropped + index in history/live rows).
struct TerminalSearchMatch
{
    qint64 line = 0;
    int col = 0;
    int width = 0;
};

// Compiled query; an empty or invalid pattern yields an invalid expression.
QRegularExpression terminalSearchRegex(const TerminalSearchOptions &options);

// Text of a row with the column of every UTF-16 unit. Blank cells read as
// spaces; the right half of a wide character contributes nothing.
void terminalRowText(const VTermScreenCell *cells, int cols, QString &text, std::vector<int> &colOf);

// Appends every match of `re` in one row. Scratch buffers are reused.
// `skipBlankRows` skips all-blank rows without running the engine; pass it
// only when the query cannot match spaces (a literal without one).
void searchTerminalRow(const QRegularExpression &re, qint64 line, const VTermScreenCell *cells, int cols,
                       std::vector<TerminalSearchMatch> &out, QString &text, std::vector<int> &colOf,
                       bool skipBlankRows = false);

#endif
