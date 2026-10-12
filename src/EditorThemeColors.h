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

#ifndef EDITORTHEMECOLORS_H
#define EDITORTHEMECOLORS_H

#include <QRgb>

// The editor's base colours per theme. EditorManager paints Scintilla with
// them and the terminal's "follow app" scheme takes the same ones, so the two
// cannot drift apart.
struct EditorThemeColors
{
    QRgb foreground;
    QRgb background;
    QRgb selection;

    static constexpr EditorThemeColors forTheme(bool dark)
    {
        // Dark: VS Code Dark+ (selection #264F78); light: plain black on white
        // with the pale blue selection used elsewhere.
        return dark ? EditorThemeColors{0xFFD4D4D4u, 0xFF1E1E1Eu, 0xFF264F78u}
                    : EditorThemeColors{0xFF000000u, 0xFFFFFFFFu, 0xFFADD6FFu};
    }
};

// Scintilla colours are 0xBBGGRR.
constexpr int toScintillaColour(QRgb c)
{
    return (qBlue(c) << 16) | (qGreen(c) << 8) | qRed(c);
}

#endif
