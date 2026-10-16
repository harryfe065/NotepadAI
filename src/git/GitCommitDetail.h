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

#ifndef GIT_COMMIT_DETAIL_H
#define GIT_COMMIT_DETAIL_H

#include <QByteArray>
#include <QString>
#include <QVector>

#include <cstdint>

// Parsed output of one `git show --diff-merges=first-parent -p
// --format=<header>%x1e <sha>` (GitCommitFetcher).
//
// The header fields (author, message, etc.) come from the format part, before
// the RS byte; `diffBytes` is the unified diff after it (a merge's diff is
// against its first parent).
//
// `truncated` is true when the output exceeded the stdout cap (default 5 MB);
// the commit card says so.
struct GitCommitDetail {
    QByteArray sha;                 // full 40 hex
    QByteArray parents;             // space-separated parent SHAs
    QByteArray authorName;
    QByteArray authorEmail;
    qint64     authorTime = 0;      // author epoch seconds
    QByteArray committerName;
    QByteArray committerEmail;
    qint64     commitTime = 0;      // committer epoch seconds
    QByteArray subject;             // first line of body
    QByteArray body;                // full message (subject + remainder)

    // Trailers parsed from the body cluster at the end of the message
    // (Linux-kernel convention: trailing lines matching ^[A-Z][A-Za-z-]+: .+).
    struct Trailer {
        QByteArray key;             // e.g. "Co-Authored-By"
        QByteArray value;           // e.g. "Alice <alice@example.com>"
    };
    QVector<Trailer> trailers;

    // Raw unified diff bytes (against the first parent for merge commits).
    QByteArray diffBytes;
    bool       truncated = false;

    bool isMerge() const {
        if (parents.isEmpty()) return false;
        for (char c : parents) if (c == ' ') return true;
        return false;
    }

    bool isEmpty() const { return sha.isEmpty(); }
};

#endif // GIT_COMMIT_DETAIL_H
