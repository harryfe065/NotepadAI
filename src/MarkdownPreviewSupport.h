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

#ifndef MARKDOWN_PREVIEW_SUPPORT_H
#define MARKDOWN_PREVIEW_SUPPORT_H

#include <QString>

// The two pure pieces of MarkdownPreviewWidget, kept free of the application so
// they can be unit-tested.
namespace MarkdownPreviewSupport {

// Cap a document before it reaches the synchronous renderer. Over the cap the text
// is cut at the last line boundary (unless that would discard more than half the
// budget, then at the cap itself) and a truncation notice is appended.
inline QString clampSource(const QString &text, int maxChars)
{
    if (text.size() <= maxChars)
        return text;
    int cut = int(text.lastIndexOf(QLatin1Char('\n'), maxChars));
    if (cut < maxChars / 2)
        cut = maxChars;
    return text.left(cut)
           + QStringLiteral("\n\n---\n\n*[Preview truncated — file too large]*\n");
}

// Where in the rendered document a source line lands, as a 0..1 fraction of the
// scroll range: (line - 1) / (totalLines - 1), clamped. 0 for degenerate input.
inline double scrollFraction(int line, int totalLines)
{
    if (totalLines <= 1)
        return 0.0;
    const double f = double(line - 1) / double(totalLines - 1);
    return f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
}

} // namespace MarkdownPreviewSupport

#endif // MARKDOWN_PREVIEW_SUPPORT_H
