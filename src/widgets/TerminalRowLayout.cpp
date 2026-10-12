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

#include "TerminalRowLayout.h"

#include "TerminalBoxDrawing.h"

#include <algorithm>

namespace {

constexpr uint32_t kWideSpacer = 0xFFFFFFFFu;

QRgb paletteColor(const VTermColor &c, const TerminalPalette &palette, QRgb fallback)
{
    if (VTERM_COLOR_IS_INDEXED(&c)) {
        return palette.indexed[c.indexed.idx];
    }
    if (VTERM_COLOR_IS_RGB(&c)) {
        return qRgb(c.rgb.red, c.rgb.green, c.rgb.blue);
    }
    return fallback;
}

// Faint text: 70% of the way from the background to the foreground (Zed's
// alpha 0.7), pre-blended so runs stay opaque and batchable.
QRgb faint(QRgb fg, QRgb bg)
{
    auto mix = [](int f, int b) { return (b * 3 + f * 7) / 10; };
    return qRgb(mix(qRed(fg), qRed(bg)), mix(qGreen(fg), qGreen(bg)), mix(qBlue(fg), qBlue(bg)));
}

} // namespace

TerminalCellColors resolveTerminalCellColors(const VTermScreenCell &cell,
                                             const TerminalPalette &palette)
{
    TerminalCellColors out;
    const bool defFg = VTERM_COLOR_IS_DEFAULT_FG(&cell.fg);
    const bool defBg = VTERM_COLOR_IS_DEFAULT_BG(&cell.bg);
    out.fg = defFg ? palette.defaultFg : paletteColor(cell.fg, palette, palette.defaultFg);
    out.bg = defBg ? palette.defaultBg : paletteColor(cell.bg, palette, palette.defaultBg);
    out.defaultBg = defBg;

    if (cell.attrs.reverse) {
        std::swap(out.fg, out.bg);
        out.defaultBg = false;
    }
    if (cell.attrs.dim) {
        out.fg = faint(out.fg, out.bg);
    }
    if (cell.attrs.conceal) {
        out.invisible = true;
    }
    return out;
}

void TerminalRowLayout::clear()
{
    backgrounds.clear();
    glyphs.clear();
    runs.clear();
    clusters.clear();
    specials.clear();
    decorations.clear();
}

void layoutTerminalRow(const VTermScreenCell *cells, int cols,
                       const TerminalPalette &palette,
                       int selFrom, int selTo,
                       TerminalRowLayout &out)
{
    out.clear();
    const bool hasSelection = selFrom < selTo;

    int col = 0;
    while (col < cols) {
        const VTermScreenCell &cell = cells[col];
        const uint32_t ch = cell.chars[0];
        if (ch == kWideSpacer) {
            // Right half of a wide character; its left half already covered it.
            ++col;
            continue;
        }
        const int width = std::min(cell.width >= 2 ? 2 : 1, cols - col);
        const TerminalCellColors colors = resolveTerminalCellColors(cell, palette);

        const bool selected = hasSelection && col < selTo && col + width > selFrom;
        if (selected || !colors.defaultBg) {
            const QRgb bg = selected ? palette.selection : colors.bg;
            TerminalRowLayout::Background *last = out.backgrounds.empty() ? nullptr : &out.backgrounds.back();
            if (last && last->col + last->span == col && last->color == bg) {
                last->span += width;
            } else {
                out.backgrounds.push_back({col, width, bg});
            }
        }

        if (!colors.invisible) {
            auto decorate = [&](uint8_t kind) {
                if (!out.decorations.empty()) {
                    TerminalRowLayout::Decoration &last = out.decorations.back();
                    if (last.kind == kind && last.color == colors.fg && last.col + last.span == col) {
                        last.span += width;
                        return;
                    }
                }
                out.decorations.push_back({col, width, colors.fg, kind});
            };
            switch (cell.attrs.underline) {
            case VTERM_UNDERLINE_SINGLE: decorate(TerminalRowLayout::Underline); break;
            case VTERM_UNDERLINE_DOUBLE: decorate(TerminalRowLayout::DoubleUnderline); break;
            case VTERM_UNDERLINE_CURLY:  decorate(TerminalRowLayout::CurlyUnderline); break;
            default: break;
            }
            if (cell.attrs.strike) {
                decorate(TerminalRowLayout::Strike);
            }

            if (ch != 0 && ch != U' ') {
                const auto font = static_cast<uint8_t>((cell.attrs.bold ? TerminalFontBold : 0)
                                                     | (cell.attrs.italic ? TerminalFontItalic : 0));
                if (cell.chars[1] != 0) {
                    TerminalRowLayout::Cluster cluster{col, width, colors.fg, font, {}};
                    for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; ++i) {
                        cluster.chars[i] = cell.chars[i];
                    }
                    out.clusters.push_back(cluster);
                } else if (TerminalBoxDrawing::isProcedural(ch)) {
                    out.specials.push_back({col, width, ch, colors.fg});
                } else {
                    if (out.runs.empty() || out.runs.back().fg != colors.fg || out.runs.back().font != font) {
                        out.runs.push_back({static_cast<int>(out.glyphs.size()), 0, colors.fg, font});
                    }
                    out.glyphs.push_back({col, ch});
                    ++out.runs.back().glyphCount;
                }
            }
        }
        col += width;
    }
}
