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

#include "TerminalContrast.h"
#include "TerminalRowLayout.h"

#include <cmath>
#include <cstring>

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

} // namespace

class TestTerminalContrast : public QObject
{
    Q_OBJECT

private slots:
    void apca_referenceValues();
    void ensure_passingColourIsUnchanged();
    void ensure_darkBlueOnBlackIsLifted();
    void ensure_yellowOnWhiteIsDarkened();
    void ensure_disabledIsIdentity();
    void cache_matchesDirectCall();
    void layout_ansiTextIsAdjusted();
    void layout_truecolorIsLeftAlone();
    void layout_selectedTruecolorIsAdjusted();
    void layout_boxDrawingKeepsColour();
};

void TestTerminalContrast::apca_referenceValues()
{
    // Published APCA-W3 anchors: black on white ~106, white on black ~-108.
    QVERIFY(std::abs(TerminalContrast::apca(qRgb(0, 0, 0), qRgb(255, 255, 255)) - 106.04) < 0.5);
    QVERIFY(std::abs(TerminalContrast::apca(qRgb(255, 255, 255), qRgb(0, 0, 0)) + 107.88) < 0.5);
    QCOMPARE(TerminalContrast::apca(qRgb(128, 128, 128), qRgb(128, 128, 128)), 0.0);
}

void TestTerminalContrast::ensure_passingColourIsUnchanged()
{
    const QRgb fg = qRgb(220, 220, 220);
    QCOMPARE(TerminalContrast::ensureMinimum(fg, qRgb(30, 30, 30), 45), fg);
}

void TestTerminalContrast::ensure_darkBlueOnBlackIsLifted()
{
    const QRgb bg = qRgb(0, 0, 0);
    const QRgb fg = qRgb(0, 0, 0xAA);
    QVERIFY(std::abs(TerminalContrast::apca(fg, bg)) < 45);
    const QRgb out = TerminalContrast::ensureMinimum(fg, bg, 45);
    QVERIFY(std::abs(TerminalContrast::apca(out, bg)) >= 45);
    // Still recognisably blue: the blue channel dominates.
    QVERIFY(qBlue(out) >= qRed(out));
}

void TestTerminalContrast::ensure_yellowOnWhiteIsDarkened()
{
    const QRgb bg = qRgb(255, 255, 255);
    const QRgb out = TerminalContrast::ensureMinimum(qRgb(0xE5, 0xE5, 0x10), bg, 45);
    QVERIFY(std::abs(TerminalContrast::apca(out, bg)) >= 45);
    QVERIFY(qGray(out) < 0xE5);
}

void TestTerminalContrast::ensure_disabledIsIdentity()
{
    const QRgb fg = qRgb(10, 10, 12);
    QCOMPARE(TerminalContrast::ensureMinimum(fg, qRgb(0, 0, 0), 0), fg);
    TerminalContrastCache cache;
    QVERIFY(!cache.enabled());
    QCOMPARE(cache.adjust(fg, qRgb(0, 0, 0)), fg);
}

void TestTerminalContrast::cache_matchesDirectCall()
{
    TerminalContrastCache cache;
    cache.setMinimum(45);
    const QRgb fg = qRgb(0x24, 0x72, 0xC8);
    const QRgb bg = qRgb(0x1E, 0x1E, 0x1E);
    const QRgb direct = TerminalContrast::ensureMinimum(fg, bg, 45);
    QCOMPARE(cache.adjust(fg, bg), direct);
    QCOMPARE(cache.adjust(fg, bg), direct);
}

void TestTerminalContrast::layout_ansiTextIsAdjusted()
{
    TerminalPalette p = TerminalPalette::fromScheme(TerminalColorScheme::darkScheme());
    p.defaultBg = qRgb(0, 0, 0);
    p.indexed[4] = qRgb(0, 0, 0x90);
    VTermScreenCell c = cell('a');
    c.fg.type = VTERM_COLOR_INDEXED;
    c.fg.indexed.idx = 4;
    TerminalContrastCache cache;
    cache.setMinimum(45);
    TerminalRowLayout layout;
    layoutTerminalRow(&c, 1, p, 0, 0, layout, &cache);
    QVERIFY(layout.runs[0].fg != p.indexed[4]);
    QVERIFY(std::abs(TerminalContrast::apca(layout.runs[0].fg, p.defaultBg)) >= 45);
}

void TestTerminalContrast::layout_truecolorIsLeftAlone()
{
    TerminalPalette p = TerminalPalette::fromScheme(TerminalColorScheme::darkScheme());
    p.defaultBg = qRgb(0, 0, 0);
    VTermScreenCell c = cell('a');
    c.fg.type = VTERM_COLOR_RGB;
    c.fg.rgb.blue = 0x90;
    TerminalContrastCache cache;
    cache.setMinimum(45);
    TerminalRowLayout layout;
    layoutTerminalRow(&c, 1, p, 0, 0, layout, &cache);
    QCOMPARE(layout.runs[0].fg, qRgb(0, 0, 0x90));
}

void TestTerminalContrast::layout_selectedTruecolorIsAdjusted()
{
    TerminalPalette p = TerminalPalette::fromScheme(TerminalColorScheme::darkScheme());
    p.selection = qRgb(0x26, 0x4F, 0x78);
    VTermScreenCell c = cell('a');
    c.fg.type = VTERM_COLOR_RGB;
    c.fg.rgb.red = 0x26;
    c.fg.rgb.green = 0x4F;
    c.fg.rgb.blue = 0x80;
    TerminalContrastCache cache;
    cache.setMinimum(45);
    TerminalRowLayout layout;
    layoutTerminalRow(&c, 1, p, 0, 1, layout, &cache);
    QVERIFY(std::abs(TerminalContrast::apca(layout.runs[0].fg, p.selection)) >= 45);
}

void TestTerminalContrast::layout_boxDrawingKeepsColour()
{
    TerminalPalette p = TerminalPalette::fromScheme(TerminalColorScheme::darkScheme());
    p.defaultBg = qRgb(0, 0, 0);
    p.indexed[8] = qRgb(0x30, 0x30, 0x30);
    VTermScreenCell c = cell(0x2500);
    c.fg.type = VTERM_COLOR_INDEXED;
    c.fg.indexed.idx = 8;
    TerminalContrastCache cache;
    cache.setMinimum(45);
    TerminalRowLayout layout;
    layoutTerminalRow(&c, 1, p, 0, 0, layout, &cache);
    QCOMPARE(layout.specials[0].fg, qRgb(0x30, 0x30, 0x30));
}

QTEST_APPLESS_MAIN(TestTerminalContrast)

#include "test_terminal_contrast.moc"
