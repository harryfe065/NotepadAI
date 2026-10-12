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

#ifndef TERMINALCOLORSCHEME_H
#define TERMINALCOLORSCHEME_H

#include <QColor>

struct TerminalColorScheme
{
    QColor ansi[16];
    QColor defaultBg;
    QColor defaultFg;
    QColor cursor;
    QColor selection;

    static TerminalColorScheme lightScheme();
    static TerminalColorScheme darkScheme();
};

// xterm's fixed palette for indices 16..255: a 6x6x6 colour cube followed by a
// 24-step grey ramp. Indices below 16 come from the scheme.
QRgb xtermIndexedColor(int index);

// Flattened, render-ready form of a scheme: every palette lookup on the paint
// path is one array read instead of a QColor conversion.
struct TerminalPalette
{
    QRgb indexed[256] = {};
    QRgb defaultFg = 0;
    QRgb defaultBg = 0;
    QRgb cursor = 0;
    QRgb selection = 0;

    static TerminalPalette fromScheme(const TerminalColorScheme &scheme);
};

#endif
