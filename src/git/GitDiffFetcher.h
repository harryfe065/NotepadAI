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

#ifndef GIT_DIFF_FETCHER_H
#define GIT_DIFF_FETCHER_H

#include "GitDiffCache.h"
#include "GitStatusEntry.h"

#include <QHash>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

class GitController;

// Async wrapper around GitController::requestDiff that:
//   1) checks the LRU cache of built diff documents before spawning git
//   2) for untracked files, hands back a producer that synthesises the diff
//      from disk content (run on the pool by DiffView, never the UI thread)
//   3) clears the cache when status changes
//
// Parsing, lexing and building happen in DiffView's pool job; the host
// stores the finished document back with remember().
class GitDiffFetcher : public QObject
{
    Q_OBJECT
public:
    explicit GitDiffFetcher(GitController *controller, QObject *parent = nullptr);

    // Request a diff for the given entry. Emits documentReady (cache hit) or
    // patchReady once for the new request.
    void request(const GitStatusEntry &entry, bool ignoreWhitespace);

    // Caches the document built for (relPath, stagedSide).
    void remember(const QString &relPath, bool stagedSide, std::shared_ptr<const DiffDoc::Document> doc);

    GitDiffCache *cache() { return &m_cache; }

signals:
    void documentReady(const QString &relPath, bool stagedSide, const std::shared_ptr<const DiffDoc::Document> &doc);
    // `patch` is the diff, or empty with `produce` set (runs off the UI thread).
    void patchReady(const QString &relPath, bool stagedSide, const QByteArray &patch,
                    const std::function<QByteArray()> &produce);
    void failed(const QString &relPath, bool stagedSide, const QString &message);

private slots:
    void onDiffReady(const QString &relPath, bool stagedSide, const QByteArray &diff, quint64 token);
    void onDiffFailed(const QString &relPath, bool stagedSide, const QString &message, quint64 token);
    void onStatusUpdated();

private:
    GitController *m_controller;
    GitDiffCache m_cache;

    struct Pending {
        bool stagedSide = false;
        quint64 token = 0;                // the latest request; older replies are stale
    };
    QHash<QString, Pending> m_inflight;   // keyed by relPath
    quint64 m_nextToken = 0;

    // Generates a synthetic "all added" diff for an untracked file. Thread-safe.
    static QByteArray synthDiffForUntracked(const QString &repoRoot, const QString &relPath);
};

#endif // GIT_DIFF_FETCHER_H
