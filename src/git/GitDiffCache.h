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

#ifndef GIT_DIFF_CACHE_H
#define GIT_DIFF_CACHE_H

#include "DiffDocument.h"
#include "GitDiffParser.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

#include <cstdint>
#include <memory>

// LRU cache for parsed diffs, keyed by (repo, relPath, stagedSide). Stores the
// parsed Result + the built diff document (DiffView) to skip re-parsing,
// re-lexing and re-building on tab re-activation. Cleared en bloc on every status refresh
// (GitController::statusRefreshed), since a file can change while its status
// stays the same — smart invalidation would require extra git spawns to verify blob
// shas, and the win is negligible compared to the cost.
class GitDiffCache
{
public:
    using Key = quint64;   // xxHash64 of "repo\0relPath\0staged?"

    struct Entry {
        std::shared_ptr<const GitDiffParser::Result> parsed;
        std::shared_ptr<const DiffDoc::Document> doc;   // may be null
        explicit operator bool() const { return static_cast<bool>(parsed); }
    };

    explicit GitDiffCache(qsizetype capacityBytes = 50 * 1024 * 1024);

    static Key keyFor(const QString &repo, const QString &relPath, bool stagedSide);

    Entry get(Key k);
    void put(Key k, Entry value, qsizetype rawDiffBytes);

    // Drop everything. Called on statusRefreshed.
    void clear();

    qsizetype sizeBytes() const { return m_sizeBytes; }
    qsizetype capacityBytes() const { return m_capacity; }

private:
    struct Slot {
        Entry value;
        qsizetype bytes;
    };

    qsizetype m_capacity;
    qsizetype m_sizeBytes = 0;
    QHash<Key, Slot> m_map;
    QList<Key> m_order; // front = LRU, back = MRU

    void touch(Key k);
    void evictWhileOver();
};

#endif // GIT_DIFF_CACHE_H
