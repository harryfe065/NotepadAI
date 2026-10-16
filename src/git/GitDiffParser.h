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

#ifndef GIT_DIFF_PARSER_H
#define GIT_DIFF_PARSER_H

#include <QByteArray>
#include <QString>
#include <QVector>

#include <cstdint>

// Unified-diff parser producing flat POD parallel arrays — cache friendly for
// the downstream painter, which walks the arrays in lock-step and pushes whole
// blocks to Scintilla via SCI_SETSTYLINGEX. Each row corresponds to ONE display
// line in the final Scintilla buffer (header rows have no source line numbers).
class GitDiffParser
{
public:
    enum class LineKind : std::uint8_t {
        FileHeader,   // "diff --git ...", "--- a/...", "+++ b/...", binary banner
        HunkHeader,   // "@@ -O,L +N,L @@"
        Context,      // " " prefix
        Added,        // "+" prefix
        Deleted,      // "-" prefix
        NoNewline,    // "\ No newline at end of file"
        Empty         // separator (e.g. between files in a combined patch)
    };

    // One file of a (possibly multi-file) diff. Rows [firstRow, endRow) of
    // the parallel arrays belong to it; its extended header lines (index,
    // mode, rename, ---/+++) stay in the arrays as FileHeader rows and are
    // summarised here so a renderer can replace them with one header.
    struct FileSpan {
        qint32     firstRow = 0;
        qint32     endRow = 0;
        QByteArray oldPath;          // empty for an added file
        QByteArray newPath;          // empty for a deleted file
        char       status = 'M';     // 'A','M','D','R','C','T'
        quint8     similarity = 0;   // rename / copy percentage
        quint32    oldMode = 0;      // 0 = not stated
        quint32    newMode = 0;
        bool       binary = false;
        bool       submodule = false;
        qint32     added = 0;
        qint32     deleted = 0;
        qint32     bodyRows = 0;     // context + added + deleted lines
        qint64     bodyBytes = 0;    // their bytes, without prefixes
        qint32     maxLineBytes = 0; // longest body line (without prefix)

        // Path to show / open: the new path, or the old one for a deletion.
        const QByteArray &path() const { return newPath.isEmpty() ? oldPath : newPath; }
    };

    struct HunkSpan {
        qint32 headerRow = 0;        // row of "@@ ... @@"
        qint32 fileIndex = 0;        // index into Result::files
    };

    struct Result {
        // Parallel arrays (length == row count).
        QVector<QByteArray> texts;   // line text WITHOUT prefix (we add it back in display)
        QVector<LineKind>   kinds;
        QVector<qint32>     oldLn;   // -1 = no source line
        QVector<qint32>     newLn;

        // Files in order, covering every row; hunks in order.
        QVector<FileSpan>   files;
        QVector<HunkSpan>   hunks;

        // Statistics
        qint32 addedCount = 0;
        qint32 deletedCount = 0;
        bool   isBinary = false;
    };

    static Result parse(const QByteArray &diff);
};

#endif // GIT_DIFF_PARSER_H
