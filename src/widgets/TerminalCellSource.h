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

#ifndef TERMINALCELLSOURCE_H
#define TERMINALCELLSOURCE_H

#include <vterm.h>

#include <QPoint>
#include <QString>

#include <vector>

// Unified row view: history rows precede live rows; lets extractSelectionText be unit-testable.
class TerminalCellSource
{
public:
    virtual ~TerminalCellSource() = default;

    virtual int rowCount() const = 0;
    virtual int cols() const = 0;
    virtual VTermScreenCell cellAt(int row, int col) const = 0;
    // True when `row` soft-wraps into the next row (both are one logical line).
    virtual bool rowWraps(int row) const
    {
        Q_UNUSED(row);
        return false;
    }
};

// Normalises ordering and clamps OOB. Rows join with '\n' except after a row
// that soft-wraps (it continues the same line); trailing blanks of each line
// are dropped, inner blank cells emit one space, and the right half of a wide
// character emits nothing.
QString extractSelectionText(const TerminalCellSource &src,
                             QPoint start, QPoint end);

// The text of cells from (startRow, startCol) to (endRow, endCol), end
// exclusive: blank cells read as spaces, the right half of a wide character
// as nothing. Null when a row is out of range.
QString cellTextBetween(const TerminalCellSource &src, int startRow, int startCol, int endRow, int endCol);

// The logical line around `row`: soft-wrapped rows joined, at most `maxRows`
// each way, with the row and column of every UTF-16 unit.
struct TerminalLogicalLine {
    QString text;
    std::vector<int> rowOf;
    std::vector<int> colOf;
};
bool logicalLineAt(const TerminalCellSource &src, int row, int maxRows, TerminalLogicalLine *out);

#endif
