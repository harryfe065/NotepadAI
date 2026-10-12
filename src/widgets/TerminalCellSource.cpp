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

#include "TerminalCellSource.h"
#include "ScrollbackBuffer.h"

#include <algorithm>

QString extractSelectionText(const TerminalCellSource &src,
                             QPoint start, QPoint end)
{
    const int rows = src.rowCount();
    const int cols = src.cols();
    if (rows <= 0 || cols <= 0) {
        return QString();
    }

    // Clamp into the valid grid. The widget should never feed us out-of-range
    // values, but stub sources used by tests (and future callers) may.
    auto clampPoint = [rows, cols](QPoint p) {
        p.setX(std::clamp(p.x(), 0, cols - 1));
        p.setY(std::clamp(p.y(), 0, rows - 1));
        return p;
    };
    start = clampPoint(start);
    end   = clampPoint(end);

    // Normalise so `lo` is the upper-left in row-major order.
    QPoint lo = start;
    QPoint hi = end;
    if (hi.y() < lo.y() || (hi.y() == lo.y() && hi.x() < lo.x())) {
        std::swap(lo, hi);
    }

    QString out;
    for (int row = lo.y(); row <= hi.y(); ++row) {
        const int startCol = (row == lo.y()) ? lo.x() : 0;
        const int endCol   = (row == hi.y()) ? hi.x() : (cols - 1);
        const bool wraps = row != hi.y() && src.rowWraps(row);
        const qsizetype lineStart = out.size();
        for (int col = startCol; col <= endCol; ++col) {
            const VTermScreenCell cell = src.cellAt(row, col);
            if (cell.chars[0] == kTerminalWideSpacer) {
                continue;
            }
            if (cell.chars[0] == 0) {
                out.append(QLatin1Char(' '));
                continue;
            }
            for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; ++i) {
                const char32_t cp = cell.chars[i];
                out.append(QString::fromUcs4(&cp, 1));
            }
        }
        if (!wraps) {
            // The screen pads every row with blanks; they are not text.
            qsizetype trimmed = out.size();
            while (trimmed > lineStart && out.at(trimmed - 1) == QLatin1Char(' ')) {
                --trimmed;
            }
            out.truncate(trimmed);
            if (row != hi.y()) {
                out.append(QLatin1Char('\n'));
            }
        }
    }
    return out;
}

QString cellTextBetween(const TerminalCellSource &src, int startRow, int startCol, int endRow, int endCol)
{
    const int cols = src.cols();
    QString out;
    for (int row = startRow; row <= endRow; ++row) {
        if (row < 0 || row >= src.rowCount()) return QString();
        const int from = row == startRow ? startCol : 0;
        const int to = row == endRow ? endCol : cols;
        for (int col = std::max(0, from); col < to && col < cols; ++col) {
            const VTermScreenCell cell = src.cellAt(row, col);
            if (cell.chars[0] == kTerminalWideSpacer) continue;
            if (cell.chars[0] == 0) {
                out.append(QLatin1Char(' '));
                continue;
            }
            for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; ++i) {
                const char32_t cp = cell.chars[i];
                out.append(QString::fromUcs4(&cp, 1));
            }
        }
    }
    return out;
}

bool logicalLineAt(const TerminalCellSource &src, int row, int maxRows, TerminalLogicalLine *out)
{
    const int total = src.rowCount();
    if (row < 0 || row >= total) return false;
    int first = row;
    while (first > 0 && row - first < maxRows && src.rowWraps(first - 1)) --first;
    int last = row;
    while (last + 1 < total && last - row < maxRows && src.rowWraps(last)) ++last;

    out->text.clear();
    out->rowOf.clear();
    out->colOf.clear();
    const int cols = src.cols();
    for (int r = first; r <= last; ++r) {
        for (int col = 0; col < cols; ++col) {
            const VTermScreenCell cell = src.cellAt(r, col);
            const uint32_t ch = cell.chars[0];
            if (ch == kTerminalWideSpacer) continue;
            if (ch == 0) {
                out->text.append(QLatin1Char(' '));
                out->rowOf.push_back(r);
                out->colOf.push_back(col);
                continue;
            }
            for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; ++i) {
                const char32_t cp = cell.chars[i];
                for (const QChar c : QString::fromUcs4(&cp, 1)) {
                    out->text.append(c);
                    out->rowOf.push_back(r);
                    out->colOf.push_back(col);
                }
            }
        }
    }
    return true;
}
