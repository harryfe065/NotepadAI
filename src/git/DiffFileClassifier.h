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

#ifndef DIFF_FILE_CLASSIFIER_H
#define DIFF_FILE_CLASSIFIER_H

#include "GitDiffParser.h"

#include <cstdint>

// Decides how one file of a diff is presented: which placeholder replaces its
// body and whether it starts collapsed. Pure; runs on the pool.
namespace DiffFileClassifier {

// First matching rule wins, in this order.
enum class Reason : std::uint8_t {
    None,
    Binary,       // "Binary file not shown."
    Submodule,    // header only
    RenamedOnly,  // "File renamed without changes."
    ModeOnly,     // "File mode changed."
    Lockfile,     // "Lockfile not shown. Load diff"
    Generated,    // linguist-generated or a generated / minified name
    Deleted,      // "This file was deleted. Load diff"
    Large,        // > kMaxRows rows, > kMaxBytes bytes, or a line > kMaxLineBytes
};

inline constexpr qint32 kMaxRows = 1500;
inline constexpr qint64 kMaxBytes = 256 * 1024;
inline constexpr qint32 kMaxLineBytes = 5000;

// `linguistGenerated` is the file's linguist-generated attribute.
Reason classify(const GitDiffParser::FileSpan &file, bool linguistGenerated);

// Whether a file with this reason starts collapsed in a view of `fileCount`
// files. A view of one file never collapses it.
bool startsCollapsed(Reason reason, qsizetype fileCount);

// Whether the body can be shown on request ("Load diff").
bool canLoad(Reason reason);

} // namespace DiffFileClassifier

#endif // DIFF_FILE_CLASSIFIER_H
