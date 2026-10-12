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

#ifndef TERMINALLINKDETECTOR_H
#define TERMINALLINKDETECTOR_H

#include <QString>

#include <cstdint>

// A clickable span in one logical terminal line.
struct TerminalLink
{
    enum Kind : uint8_t { None, Url, Path };
    Kind kind = None;
    int start = 0;    // UTF-16 index into the scanned text
    int length = 0;
    QString target;   // the URL, or the path as written (not yet resolved)
    int line = 0;     // 1-based; 0 when the text gave no position
    int column = 0;
};

// The URL or file path covering `index` in `text`, as the terminal shows them
// on Ctrl+hover. Understands URLs (http, https, file, mailto, ssh, git, ...),
// `path:line:col`, `path(line,col)` (MSVC), `path:line` and Python's
// `File "path", line N`. Paths must look like paths (a separator or a file
// extension) so plain words never light up; whether the file exists is the
// caller's job. Trailing punctuation and unbalanced brackets are trimmed.
TerminalLink findTerminalLinkAt(const QString &text, int index);

#endif
