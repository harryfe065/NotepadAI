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

#ifndef TERMINALROWLAYOUT_H
#define TERMINALROWLAYOUT_H

#include "TerminalColorScheme.h"

#include <vterm.h>

#include <QRgb>

#include <cstdint>
#include <vector>

// Resolved colours of one cell after palette lookup, reverse video, faint and
// conceal. `defaultBg` cells need no background fill (the row is pre-filled).
struct TerminalCellColors
{
    QRgb fg = 0;
    QRgb bg = 0;
    bool defaultBg = true;
    bool invisible = false;
};

TerminalCellColors resolveTerminalCellColors(const VTermScreenCell &cell,
                                             const TerminalPalette &palette);

// Font variant bits shared with TerminalGlyphCache.
enum TerminalFontStyle : uint8_t {
    TerminalFontRegular = 0,
    TerminalFontBold = 1,
    TerminalFontItalic = 2,
};

// One row of cells turned into paint primitives, batched the way Zed does it:
// adjacent cells with the same non-default background become one rect, and
// glyphs sharing colour and font variant become one run (blank cells do not
// break a run, since glyph positions are explicit). Pure data: no fonts, so it
// is unit-testable and reusable across frames without allocating.
struct TerminalRowLayout
{
    struct Background { int col; int span; QRgb color; };
    struct Glyph { int col; char32_t cp; };
    struct Run { int firstGlyph; int glyphCount; QRgb fg; uint8_t font; };
    // Multi-codepoint cells (combining marks, ZWJ sequences); drawn shaped.
    struct Cluster { int col; int width; QRgb fg; uint8_t font; char32_t chars[VTERM_MAX_CHARS_PER_CELL]; };
    // Box drawing / blocks / Powerline, drawn as geometry.
    struct Special { int col; int width; char32_t cp; QRgb fg; };
    enum DecorationKind : uint8_t { Underline = 1, DoubleUnderline, CurlyUnderline, Strike };
    struct Decoration { int col; int span; QRgb color; uint8_t kind; };

    std::vector<Background> backgrounds;
    std::vector<Glyph> glyphs;
    std::vector<Run> runs;
    std::vector<Cluster> clusters;
    std::vector<Special> specials;
    std::vector<Decoration> decorations;

    void clear();
};

// Lays out `cols` cells. Cells in [selFrom, selTo) get the selection
// background; pass selFrom >= selTo for no selection.
void layoutTerminalRow(const VTermScreenCell *cells, int cols,
                       const TerminalPalette &palette,
                       int selFrom, int selTo,
                       TerminalRowLayout &out);

#endif
