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

#ifndef TERMINALBOXDRAWING_H
#define TERMINALBOXDRAWING_H

#include <QColor>
#include <QRectF>

class QPainter;

// Box drawing (U+2500-257F), block elements (U+2580-259F) and the Powerline
// separators (U+E0B0-E0B7) are drawn as geometry fitted to the cell instead of
// font glyphs, so TUI frames and prompts tile without seams at any line height.
namespace TerminalBoxDrawing {

inline bool isProcedural(char32_t cp)
{
    return (cp >= 0x2500 && cp <= 0x259F) || (cp >= 0xE0B0 && cp <= 0xE0B7);
}

// Weights of the four arms of a box-drawing character: 0 none, 1 light,
// 2 heavy, 3 double. Returns false for characters that are not plain arms
// (dashes, arcs, diagonals, blocks, Powerline).
struct Arms { int left = 0; int right = 0; int up = 0; int down = 0; };
bool armsFor(char32_t cp, Arms *out);

// Paints `cp` into `cell` (logical pixels). `lightStroke` is the light line
// thickness in logical pixels; `dpr` snaps every edge to device pixels.
void paint(QPainter &p, const QRectF &cell, char32_t cp, QColor color,
           qreal lightStroke, qreal dpr);

} // namespace TerminalBoxDrawing

#endif
