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

#include "TerminalColorScheme.h"

#include <iterator>

namespace {

struct PresetDef {
    const char *id;
    const char *name;
    QRgb ansi[16];
    QRgb bg;
    QRgb fg;
    QRgb cursor;
    QRgb selection;
};

// Palettes as published by each theme's terminal port.
constexpr PresetDef kPresets[] = {
    {"campbell", "Campbell",
     {0x0C0C0C, 0xC50F1F, 0x13A10E, 0xC19C00, 0x0037DA, 0x881798, 0x3A96DD, 0xCCCCCC,
      0x767676, 0xE74856, 0x16C60C, 0xF9F1A5, 0x3B78FF, 0xB4009E, 0x61D6D6, 0xF2F2F2},
     0x0C0C0C, 0xCCCCCC, 0xFFFFFF, 0x3A4A64},
    {"one-dark", "One Dark",
     {0x282C34, 0xE06C75, 0x98C379, 0xE5C07B, 0x61AFEF, 0xC678DD, 0x56B6C2, 0xDCDFE4,
      0x5A6374, 0xE06C75, 0x98C379, 0xE5C07B, 0x61AFEF, 0xC678DD, 0x56B6C2, 0xDCDFE4},
     0x282C34, 0xDCDFE4, 0x528BFF, 0x3E4451},
    {"one-light", "One Light",
     {0x383A42, 0xE45649, 0x50A14F, 0xC18401, 0x0184BC, 0xA626A4, 0x0997B3, 0xFAFAFA,
      0x4F525D, 0xDF6C75, 0x98C379, 0xE4C07A, 0x61AFEF, 0xC577DD, 0x56B5C1, 0xFFFFFF},
     0xFAFAFA, 0x383A42, 0x526FFF, 0xD7DCF0},
    {"solarized-dark", "Solarized Dark",
     {0x073642, 0xDC322F, 0x859900, 0xB58900, 0x268BD2, 0xD33682, 0x2AA198, 0xEEE8D5,
      0x002B36, 0xCB4B16, 0x586E75, 0x657B83, 0x839496, 0x6C71C4, 0x93A1A1, 0xFDF6E3},
     0x002B36, 0x839496, 0x93A1A1, 0x274642},
    {"solarized-light", "Solarized Light",
     {0x073642, 0xDC322F, 0x859900, 0xB58900, 0x268BD2, 0xD33682, 0x2AA198, 0xEEE8D5,
      0x002B36, 0xCB4B16, 0x586E75, 0x657B83, 0x839496, 0x6C71C4, 0x93A1A1, 0xFDF6E3},
     0xFDF6E3, 0x657B83, 0x586E75, 0xE4DDC8},
    {"dracula", "Dracula",
     {0x21222C, 0xFF5555, 0x50FA7B, 0xF1FA8C, 0xBD93F9, 0xFF79C6, 0x8BE9FD, 0xF8F8F2,
      0x6272A4, 0xFF6E6E, 0x69FF94, 0xFFFFA5, 0xD6ACFF, 0xFF92DF, 0xA4FFFF, 0xFFFFFF},
     0x282A36, 0xF8F8F2, 0xF8F8F2, 0x44475A},
    {"tokyo-night", "Tokyo Night",
     {0x15161E, 0xF7768E, 0x9ECE6A, 0xE0AF68, 0x7AA2F7, 0xBB9AF7, 0x7DCFFF, 0xA9B1D6,
      0x414868, 0xF7768E, 0x9ECE6A, 0xE0AF68, 0x7AA2F7, 0xBB9AF7, 0x7DCFFF, 0xC0CAF5},
     0x1A1B26, 0xC0CAF5, 0xC0CAF5, 0x33467C},
    {"gruvbox-dark", "Gruvbox Dark",
     {0x282828, 0xCC241D, 0x98971A, 0xD79921, 0x458588, 0xB16286, 0x689D6A, 0xA89984,
      0x928374, 0xFB4934, 0xB8BB26, 0xFABD2F, 0x83A598, 0xD3869B, 0x8EC07C, 0xEBDBB2},
     0x282828, 0xEBDBB2, 0xEBDBB2, 0x504945},
};

// "Follow app" light: VS Code Light+ terminal ANSI on the editor's own
// background, foreground and selection colours.
constexpr PresetDef kAppLight = {
    "light", "Light",
    {0x000000, 0xCD3131, 0x00BC00, 0x949800, 0x0451A5, 0xBC05BC, 0x0598BC, 0x555555,
     0x666666, 0xCD3131, 0x14CE14, 0xB5BA00, 0x0451A5, 0xBC05BC, 0x0598BC, 0xA5A5A5},
    0xFFFFFF, 0x1E1E1E, 0x1E1E1E, 0xADD6FF};

// "Follow app" dark: VS Code Dark+ terminal ANSI on the editor's background
// (#1E1E1E), foreground (#D4D4D4) and selection (#264F78).
constexpr PresetDef kAppDark = {
    "dark", "Dark",
    {0x000000, 0xCD3131, 0x0DBC79, 0xE5E510, 0x2472C8, 0xBC3FBC, 0x11A8CD, 0xE5E5E5,
     0x666666, 0xF14C4C, 0x23D18B, 0xF5F543, 0x3B8EEA, 0xD670D6, 0x29B8DB, 0xE5E5E5},
    0x1E1E1E, 0xD4D4D4, 0xD4D4D4, 0x264F78};

TerminalColorScheme fromDef(const PresetDef &d)
{
    TerminalColorScheme s;
    for (int i = 0; i < 16; ++i) {
        s.ansi[i] = QColor::fromRgb(d.ansi[i]);
    }
    s.defaultBg = QColor::fromRgb(d.bg);
    s.defaultFg = QColor::fromRgb(d.fg);
    s.cursor = QColor::fromRgb(d.cursor);
    s.selection = QColor::fromRgb(d.selection);
    return s;
}

} // namespace

TerminalColorScheme TerminalColorScheme::lightScheme()
{
    return fromDef(kAppLight);
}

TerminalColorScheme TerminalColorScheme::darkScheme()
{
    return fromDef(kAppDark);
}

QStringList TerminalColorScheme::presetIds()
{
    QStringList ids;
    ids.reserve(static_cast<qsizetype>(std::size(kPresets)));
    for (const PresetDef &d : kPresets) {
        ids.append(QString::fromLatin1(d.id));
    }
    return ids;
}

QString TerminalColorScheme::presetName(const QString &id)
{
    for (const PresetDef &d : kPresets) {
        if (id == QLatin1String(d.id)) {
            return QString::fromLatin1(d.name);
        }
    }
    return QString();
}

TerminalColorScheme TerminalColorScheme::forSetting(const QString &id, bool appIsDark)
{
    for (const PresetDef &d : kPresets) {
        if (id == QLatin1String(d.id)) {
            return fromDef(d);
        }
    }
    return appIsDark ? darkScheme() : lightScheme();
}

QRgb xtermIndexedColor(int index)
{
    if (index < 16 || index > 255) {
        return qRgb(0, 0, 0);
    }
    if (index < 232) {
        static constexpr int kLevels[6] = {0x00, 0x5F, 0x87, 0xAF, 0xD7, 0xFF};
        const int i = index - 16;
        return qRgb(kLevels[i / 36], kLevels[(i / 6) % 6], kLevels[i % 6]);
    }
    const int grey = 8 + (index - 232) * 10;
    return qRgb(grey, grey, grey);
}

TerminalPalette TerminalPalette::fromScheme(const TerminalColorScheme &scheme)
{
    TerminalPalette p;
    for (int i = 0; i < 16; ++i) {
        p.indexed[i] = scheme.ansi[i].rgb();
    }
    for (int i = 16; i < 256; ++i) {
        p.indexed[i] = xtermIndexedColor(i);
    }
    p.defaultFg = scheme.defaultFg.rgb();
    p.defaultBg = scheme.defaultBg.rgb();
    p.cursor = scheme.cursor.rgb();
    p.selection = scheme.selection.rgb();
    return p;
}
