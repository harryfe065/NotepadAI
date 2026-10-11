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

#ifndef CHAT_FONT_H
#define CHAT_FONT_H

#include <QFont>

#include "ApplicationSettings.h"

// The one definition of "the chat typeface", shared by everything that renders
// markdown with MarkdownView (the chat transcript and the file Markdown preview).
//
// Two modes (see the ChatFont group in ApplicationSettings). The hinting policy
// is set on the QFont directly in BOTH branches: these are plain Qt widgets, so
// Scintilla's platform-layer flag doesn't reach them — without it, thin fonts
// (e.g. Lilex) look blurry here while the Scintilla editor looks sharp.
inline QFont chatFontFromSettings(ApplicationSettings *settings)
{
    if (!settings) return QFont();

    if (settings->chatFontUseDefault()) {
        // Default mode: follow the editor's Default Font (historical behavior).
        QFont f(settings->fontName(), settings->fontSize());
        f.setHintingPreference(!settings->fontHinting()
            ? QFont::PreferNoHinting
            : QFont::PreferFullHinting);
        return f;
    }

    // Custom mode: a chat-specific family/size/sharpen, independent of the editor.
    QFont f(settings->chatFontFamily(), settings->chatFontSizePt());
    f.setHintingPreference(!settings->chatFontSharpen()
        ? QFont::PreferNoHinting
        : QFont::PreferFullHinting);
    return f;
}

#endif // CHAT_FONT_H
