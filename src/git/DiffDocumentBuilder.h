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

#ifndef DIFF_DOCUMENT_BUILDER_H
#define DIFF_DOCUMENT_BUILDER_H

#include "DiffDocument.h"

// Turns a parsed diff (plus the commit, for the card) into the bytes, styles
// and row tables of a diff editor buffer. Pure: no Scintilla, no Lua, safe on
// any thread. Lexing goes through GitDiffSyntaxMapper with a per-thread state.
namespace DiffDocumentBuilder {

// Reasons for every file. `linguistGenerated` is per file, or empty.
QVector<DiffFileClassifier::Reason> classifyFiles(const GitDiffParser::Result &parsed,
                                                  const QVector<bool> &linguistGenerated);

std::shared_ptr<const DiffDoc::Document> build(std::shared_ptr<const DiffDoc::Input> input);

// The rows an expanded file shows under its header (Load diff / re-expand).
// Byte-identical to the same file built inline.
DiffDoc::Rows buildBody(const DiffDoc::Input &input, qint32 fileIndex);

// The one row a collapsed file shows under its header.
DiffDoc::Rows buildPlaceholder(const DiffDoc::Input &input, qint32 fileIndex);

// GitHub's five diffstat squares.
struct Squares {
    int green = 0;
    int red = 0;
    int grey = 5;
};
Squares squares(qint64 added, qint64 deleted);

} // namespace DiffDocumentBuilder

#endif // DIFF_DOCUMENT_BUILDER_H
