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

#include "TerminalColorScheme.h"


class TestTerminalColorScheme : public QObject
{
    Q_OBJECT

private slots:
    void lightScheme_hasSixteenValidAnsiColors();
    void darkScheme_hasSixteenValidAnsiColors();

    void lightScheme_bgIsLighterThanFg();
    void darkScheme_bgIsDarkerThanFg();

    void lightAndDarkSchemes_producesDifferentPalettes();

    void lightScheme_cursorAndSelectionAreValid();
    void darkScheme_cursorAndSelectionAreValid();

    void xtermIndexed_cubeAndGreyRamp();
    void palette_flattensSchemeAndFixedColours();
};

void TestTerminalColorScheme::lightScheme_hasSixteenValidAnsiColors()
{
    const TerminalColorScheme s = TerminalColorScheme::lightScheme();
    for (int i = 0; i < 16; ++i) {
        QVERIFY2(s.ansi[i].isValid(), qPrintable(QStringLiteral("ansi[%1] invalid").arg(i)));
    }
    QVERIFY(s.defaultBg.isValid());
    QVERIFY(s.defaultFg.isValid());
}

void TestTerminalColorScheme::darkScheme_hasSixteenValidAnsiColors()
{
    const TerminalColorScheme s = TerminalColorScheme::darkScheme();
    for (int i = 0; i < 16; ++i) {
        QVERIFY2(s.ansi[i].isValid(), qPrintable(QStringLiteral("ansi[%1] invalid").arg(i)));
    }
    QVERIFY(s.defaultBg.isValid());
    QVERIFY(s.defaultFg.isValid());
}

void TestTerminalColorScheme::lightScheme_bgIsLighterThanFg()
{
    const TerminalColorScheme s = TerminalColorScheme::lightScheme();
    QVERIFY(s.defaultBg.lightness() > s.defaultFg.lightness());
}

void TestTerminalColorScheme::darkScheme_bgIsDarkerThanFg()
{
    const TerminalColorScheme s = TerminalColorScheme::darkScheme();
    QVERIFY(s.defaultBg.lightness() < s.defaultFg.lightness());
}

void TestTerminalColorScheme::lightAndDarkSchemes_producesDifferentPalettes()
{
    const TerminalColorScheme light = TerminalColorScheme::lightScheme();
    const TerminalColorScheme dark  = TerminalColorScheme::darkScheme();
    bool anyDifferent = false;
    for (int i = 0; i < 16; ++i) {
        if (light.ansi[i] != dark.ansi[i]) {
            anyDifferent = true;
            break;
        }
    }
    QVERIFY(anyDifferent);
}

void TestTerminalColorScheme::lightScheme_cursorAndSelectionAreValid()
{
    const TerminalColorScheme s = TerminalColorScheme::lightScheme();
    QVERIFY(s.cursor.isValid());
    QVERIFY(s.selection.isValid());
}

void TestTerminalColorScheme::darkScheme_cursorAndSelectionAreValid()
{
    const TerminalColorScheme s = TerminalColorScheme::darkScheme();
    QVERIFY(s.cursor.isValid());
    QVERIFY(s.selection.isValid());
}

void TestTerminalColorScheme::xtermIndexed_cubeAndGreyRamp()
{
    QCOMPARE(xtermIndexedColor(16), qRgb(0, 0, 0));
    QCOMPARE(xtermIndexedColor(17), qRgb(0, 0, 0x5F));
    QCOMPARE(xtermIndexedColor(208), qRgb(0xFF, 0x87, 0x00));
    QCOMPARE(xtermIndexedColor(231), qRgb(0xFF, 0xFF, 0xFF));
    QCOMPARE(xtermIndexedColor(232), qRgb(8, 8, 8));
    QCOMPARE(xtermIndexedColor(255), qRgb(238, 238, 238));
}

void TestTerminalColorScheme::palette_flattensSchemeAndFixedColours()
{
    const TerminalColorScheme s = TerminalColorScheme::darkScheme();
    const TerminalPalette p = TerminalPalette::fromScheme(s);
    for (int i = 0; i < 16; ++i) {
        QCOMPARE(p.indexed[i], s.ansi[i].rgb());
    }
    for (int i = 16; i < 256; ++i) {
        QCOMPARE(p.indexed[i], xtermIndexedColor(i));
    }
    QCOMPARE(p.defaultBg, s.defaultBg.rgb());
    QCOMPARE(p.selection, s.selection.rgb());
}

QTEST_MAIN(TestTerminalColorScheme)

#include "test_terminal_color_scheme.moc"
