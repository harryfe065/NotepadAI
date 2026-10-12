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

#include <QtTest>

#include "TerminalRowLayout.h"

#include <vterm.h>

#include <cstring>
#include <vector>

namespace {

VTermScreenCell cell(uint32_t ch)
{
    VTermScreenCell c;
    std::memset(&c, 0, sizeof(c));
    c.chars[0] = ch;
    c.width = 1;
    c.fg.type = VTERM_COLOR_DEFAULT_FG;
    c.bg.type = VTERM_COLOR_DEFAULT_BG;
    return c;
}

std::vector<VTermScreenCell> text(const char *s)
{
    std::vector<VTermScreenCell> row;
    for (const char *p = s; *p; ++p) {
        row.push_back(cell(*p == ' ' ? 0 : static_cast<uint32_t>(*p)));
    }
    return row;
}

void setIndexedFg(VTermScreenCell &c, uint8_t idx)
{
    c.fg.type = VTERM_COLOR_INDEXED;
    c.fg.indexed.idx = idx;
}

void setIndexedBg(VTermScreenCell &c, uint8_t idx)
{
    c.bg.type = VTERM_COLOR_INDEXED;
    c.bg.indexed.idx = idx;
}

TerminalPalette palette()
{
    return TerminalPalette::fromScheme(TerminalColorScheme::darkScheme());
}

} // namespace

class TestTerminalRowLayout : public QObject
{
    Q_OBJECT

private slots:
    void colors_defaultsComeFromScheme();
    void colors_indexedCoversAll256();
    void colors_reverseSwapsAndIsNotDefault();
    void colors_dimBlendsTowardBackground();
    void colors_concealIsInvisible();

    void layout_plainTextIsOneRun();
    void layout_blanksDoNotBreakRuns();
    void layout_colourChangeSplitsRuns();
    void layout_boldSplitsRuns();
    void layout_defaultBackgroundEmitsNoRects();
    void layout_adjacentBackgroundsMerge();
    void layout_selectionOverridesBackground();
    void layout_wideCharSkipsSpacer();
    void layout_boxDrawingIsSpecial();
    void layout_combiningIsCluster();
    void layout_underlinesMerge();
};

void TestTerminalRowLayout::colors_defaultsComeFromScheme()
{
    const TerminalPalette p = palette();
    const TerminalCellColors c = resolveTerminalCellColors(cell('a'), p);
    QCOMPARE(c.fg, p.defaultFg);
    QCOMPARE(c.bg, p.defaultBg);
    QVERIFY(c.defaultBg);
    QVERIFY(!c.invisible);
}

void TestTerminalRowLayout::colors_indexedCoversAll256()
{
    const TerminalPalette p = palette();
    VTermScreenCell c = cell('a');
    setIndexedFg(c, 1);
    QCOMPARE(resolveTerminalCellColors(c, p).fg, p.indexed[1]);
    // The cube and grey ramp used to fall back to the default colour.
    setIndexedFg(c, 196);
    QCOMPARE(resolveTerminalCellColors(c, p).fg, qRgb(0xFF, 0x00, 0x00));
    setIndexedFg(c, 21);
    QCOMPARE(resolveTerminalCellColors(c, p).fg, qRgb(0x00, 0x00, 0xFF));
    setIndexedFg(c, 244);
    QCOMPARE(resolveTerminalCellColors(c, p).fg, qRgb(128, 128, 128));
    setIndexedBg(c, 16);
    QCOMPARE(resolveTerminalCellColors(c, p).bg, qRgb(0, 0, 0));
    QVERIFY(!resolveTerminalCellColors(c, p).defaultBg);
}

void TestTerminalRowLayout::colors_reverseSwapsAndIsNotDefault()
{
    const TerminalPalette p = palette();
    VTermScreenCell c = cell('a');
    c.attrs.reverse = 1;
    const TerminalCellColors r = resolveTerminalCellColors(c, p);
    QCOMPARE(r.fg, p.defaultBg);
    QCOMPARE(r.bg, p.defaultFg);
    QVERIFY(!r.defaultBg);
}

void TestTerminalRowLayout::colors_dimBlendsTowardBackground()
{
    TerminalPalette p = palette();
    p.defaultFg = qRgb(200, 200, 200);
    p.defaultBg = qRgb(0, 0, 0);
    VTermScreenCell c = cell('a');
    c.attrs.dim = 1;
    const QRgb fg = resolveTerminalCellColors(c, p).fg;
    QCOMPARE(qRed(fg), 140);
    QCOMPARE(qGreen(fg), 140);
    QCOMPARE(qBlue(fg), 140);
}

void TestTerminalRowLayout::colors_concealIsInvisible()
{
    VTermScreenCell c = cell('a');
    c.attrs.conceal = 1;
    QVERIFY(resolveTerminalCellColors(c, palette()).invisible);

    TerminalRowLayout layout;
    layoutTerminalRow(&c, 1, palette(), 0, 0, layout);
    QVERIFY(layout.glyphs.empty());
}

void TestTerminalRowLayout::layout_plainTextIsOneRun()
{
    const auto row = text("hello");
    TerminalRowLayout layout;
    layoutTerminalRow(row.data(), static_cast<int>(row.size()), palette(), 0, 0, layout);
    QCOMPARE(layout.runs.size(), std::size_t(1));
    QCOMPARE(layout.runs[0].glyphCount, 5);
    QCOMPARE(layout.glyphs[4].col, 4);
    QCOMPARE(static_cast<uint32_t>(layout.glyphs[4].cp), static_cast<uint32_t>('o'));
}

void TestTerminalRowLayout::layout_blanksDoNotBreakRuns()
{
    const auto row = text("ls -la    src");
    TerminalRowLayout layout;
    layoutTerminalRow(row.data(), static_cast<int>(row.size()), palette(), 0, 0, layout);
    QCOMPARE(layout.runs.size(), std::size_t(1));
    QCOMPARE(layout.runs[0].glyphCount, 8);
    QCOMPARE(layout.glyphs.back().col, 12);
}

void TestTerminalRowLayout::layout_colourChangeSplitsRuns()
{
    auto row = text("abcd");
    setIndexedFg(row[2], 2);
    setIndexedFg(row[3], 2);
    TerminalRowLayout layout;
    layoutTerminalRow(row.data(), 4, palette(), 0, 0, layout);
    QCOMPARE(layout.runs.size(), std::size_t(2));
    QCOMPARE(layout.runs[0].glyphCount, 2);
    QCOMPARE(layout.runs[1].glyphCount, 2);
    QCOMPARE(layout.runs[1].fg, palette().indexed[2]);
}

void TestTerminalRowLayout::layout_boldSplitsRuns()
{
    auto row = text("ab");
    row[1].attrs.bold = 1;
    row[1].attrs.italic = 1;
    TerminalRowLayout layout;
    layoutTerminalRow(row.data(), 2, palette(), 0, 0, layout);
    QCOMPARE(layout.runs.size(), std::size_t(2));
    QCOMPARE(static_cast<int>(layout.runs[1].font), TerminalFontBold | TerminalFontItalic);
}

void TestTerminalRowLayout::layout_defaultBackgroundEmitsNoRects()
{
    const auto row = text("plain text");
    TerminalRowLayout layout;
    layoutTerminalRow(row.data(), static_cast<int>(row.size()), palette(), 0, 0, layout);
    QVERIFY(layout.backgrounds.empty());
}

void TestTerminalRowLayout::layout_adjacentBackgroundsMerge()
{
    auto row = text("abcdef");
    for (int i = 1; i <= 3; ++i) setIndexedBg(row[i], 4);
    setIndexedBg(row[4], 5);
    TerminalRowLayout layout;
    layoutTerminalRow(row.data(), 6, palette(), 0, 0, layout);
    QCOMPARE(layout.backgrounds.size(), std::size_t(2));
    QCOMPARE(layout.backgrounds[0].col, 1);
    QCOMPARE(layout.backgrounds[0].span, 3);
    QCOMPARE(layout.backgrounds[1].col, 4);
    QCOMPARE(layout.backgrounds[1].span, 1);
}

void TestTerminalRowLayout::layout_selectionOverridesBackground()
{
    auto row = text("abcdef");
    setIndexedBg(row[0], 4);
    TerminalRowLayout layout;
    const TerminalPalette p = palette();
    layoutTerminalRow(row.data(), 6, p, 0, 3, layout);
    QCOMPARE(layout.backgrounds.size(), std::size_t(1));
    QCOMPARE(layout.backgrounds[0].col, 0);
    QCOMPARE(layout.backgrounds[0].span, 3);
    QCOMPARE(layout.backgrounds[0].color, p.selection);
    // Text keeps its colour on the selection.
    QCOMPARE(layout.runs.size(), std::size_t(1));
}

void TestTerminalRowLayout::layout_wideCharSkipsSpacer()
{
    std::vector<VTermScreenCell> row = {cell(0x4E2D), cell(0xFFFFFFFFu), cell('x')};
    row[0].width = 2;
    setIndexedBg(row[0], 3);
    TerminalRowLayout layout;
    layoutTerminalRow(row.data(), 3, palette(), 0, 0, layout);
    QCOMPARE(layout.glyphs.size(), std::size_t(2));
    QCOMPARE(layout.glyphs[0].col, 0);
    QCOMPARE(layout.glyphs[1].col, 2);
    QCOMPARE(layout.backgrounds.size(), std::size_t(1));
    QCOMPARE(layout.backgrounds[0].span, 2);
}

void TestTerminalRowLayout::layout_boxDrawingIsSpecial()
{
    std::vector<VTermScreenCell> row = {cell(0x2500), cell(0x2502), cell(0xE0B0), cell('a')};
    TerminalRowLayout layout;
    layoutTerminalRow(row.data(), 4, palette(), 0, 0, layout);
    QCOMPARE(layout.specials.size(), std::size_t(3));
    QCOMPARE(static_cast<uint32_t>(layout.specials[2].cp), 0xE0B0u);
    QCOMPARE(layout.glyphs.size(), std::size_t(1));
}

void TestTerminalRowLayout::layout_combiningIsCluster()
{
    std::vector<VTermScreenCell> row = {cell('e')};
    row[0].chars[1] = 0x0301;
    TerminalRowLayout layout;
    layoutTerminalRow(row.data(), 1, palette(), 0, 0, layout);
    QCOMPARE(layout.clusters.size(), std::size_t(1));
    QCOMPARE(static_cast<uint32_t>(layout.clusters[0].chars[1]), 0x0301u);
    QVERIFY(layout.glyphs.empty());
}

void TestTerminalRowLayout::layout_underlinesMerge()
{
    auto row = text("ab cd");
    for (auto &c : row) c.attrs.underline = VTERM_UNDERLINE_SINGLE;
    row[4].attrs.strike = 1;
    TerminalRowLayout layout;
    layoutTerminalRow(row.data(), 5, palette(), 0, 0, layout);
    int underlines = 0;
    int strikes = 0;
    for (const auto &d : layout.decorations) {
        if (d.kind == TerminalRowLayout::Underline) {
            ++underlines;
            QCOMPARE(d.col, 0);
            QCOMPARE(d.span, 5);
        } else if (d.kind == TerminalRowLayout::Strike) {
            ++strikes;
        }
    }
    QCOMPARE(underlines, 1);
    QCOMPARE(strikes, 1);
}

QTEST_APPLESS_MAIN(TestTerminalRowLayout)

#include "test_terminal_row_layout.moc"
