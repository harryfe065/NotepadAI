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

#ifndef GIT_COMMIT_FETCHER_H
#define GIT_COMMIT_FETCHER_H

#include "GitCommitDetail.h"

#include <QByteArray>
#include <QCache>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

#include <functional>
#include <memory>

class GitProcessRunner;
class IGitProcessRunner;
class GitWatcher;

// Fetch for the commit detail tab: ONE
//   `git -c color.ui=never show --no-color --no-ext-diff -M
//    --diff-merges=first-parent -p --format=<header>%x1e [-w] <sha>`
// whose output is split into the header and the patch (git < 2.31 rejects
// --diff-merges: the command is retried once with `-m --first-parent`).
// The repo's issue URL base (for `#N` links) is read once per repo with
// `git config --get remote.origin.url`. requestLinguistGenerated runs
// `git check-attr -z --stdin linguist-generated` for the commit view.
//
// One in-flight fetch at a time per fetcher. Generation token guards stale
// callbacks. LRU cache holds up to 16 fully-built CommitDetail; in-memory
// only (no disk persistence per J2).
class GitCommitFetcher : public QObject
{
    Q_OBJECT
public:
    explicit GitCommitFetcher(QObject *parent = nullptr);
    ~GitCommitFetcher() override;

    using Callback = std::function<void(std::shared_ptr<const GitCommitDetail>)>;
    using GeneratedCallback = std::function<void(QVector<bool>)>;

    // Configure the repository toplevel. Reconfiguring with a different value
    // cancels any in-flight fetch and invalidates the cache (cache is keyed
    // per-fetcher, so this means "fresh cache for new repo").
    void setRepoRoot(const QString &repoToplevel);
    QString repoRoot() const { return m_repoRoot; }

    void setRunnerScope(const QString &scope);

    // Invalidate the cache. Called when GitWatcher reports HEAD or refs
    // change (force-push/rebase can leave SHAs valid but pointing to GC'd
    // objects in a stale cache).
    void invalidateAll();

    // Fetch a single commit detail. On cache hit, callback fires in the next
    // event-loop tick with the cached object. On miss, spawns one git show
    // (plus, once per repo, the remote URL lookup) and invokes the callback
    // with either a complete detail or null on failure.
    //
    // truncated=true if the output hit the 5MB cap.
    void requestDetail(const QByteArray &sha, bool ignoreWhitespace, Callback cb);

    // Force re-fetch ignoring the cap (called when user clicks "Show all" on
    // a truncated detail).
    void requestDetailUncapped(const QByteArray &sha, bool ignoreWhitespace, Callback cb);

    // linguist-generated per path (working-tree attributes), in `paths`
    // order. Empty on failure. Cancelled by the next request.
    void requestLinguistGenerated(const QVector<QByteArray> &paths, GeneratedCallback cb);

    // "<web>/issues/" for the repo's origin on a recognised forge, else
    // empty. Known after the first requestDetail completes.
    QByteArray issueUrlBase() const { return m_issueUrlBase; }

    // Cancel the in-flight fetch (no callback fires).
    void cancel();

    // Hook GitWatcher → invalidateAll on HEAD/refs change.
    void connectWatcher(GitWatcher *watcher);

    // Pure helpers.
    static QByteArray issueUrlBaseFor(const QByteArray &remoteUrl);
    // Splits `git show --format=<header>%x1e -p` output; false when the
    // header is malformed.
    static bool splitShowOutput(const QByteArray &out, GitCommitDetail &detail);
    static QVector<bool> parseCheckAttr(const QByteArray &out, const QVector<QByteArray> &paths);

signals:
    // Diagnostic — most callers use the per-request Callback. Emitted on
    // completion (success or failure) of any request.
    void requestCompleted(const QByteArray &sha, bool ok);

private:
    // Per-request inflight state (one at a time).
    struct InFlight {
        QByteArray sha;
        Callback   cb;
        std::shared_ptr<GitCommitDetail> partial;
        bool       uncapped = false;
        bool       ignoreWhitespace = false;
        quint64    generation = 0;
    };
    void start(const QByteArray &sha, bool ignoreWhitespace, bool uncapped, Callback cb);
    void launchShow(InFlight &state, bool legacyMerges);
    void launchRemoteUrl(InFlight &state);
    void completeOk(InFlight &state);
    void completeFail(InFlight &state);
    static QByteArray cacheKey(const QByteArray &sha, bool ignoreWhitespace);

    // Format string parsed by parseHeader:
    //   %H%x1f%P%x1f%an%x1f%ae%x1f%at%x1f%cn%x1f%ce%x1f%ct%x1f%s%x1f%B%x1e
    static void parseHeader(const QByteArray &bytes, GitCommitDetail &out);
    // Body trailer parsing — split body into main message + trailing
    // "Key: value" cluster.
    static void parseTrailers(GitCommitDetail &out);

    QString          m_repoRoot;
    QString          m_runnerScope;
    IGitProcessRunner *m_runner = nullptr;
    QByteArray       m_issueUrlBase;
    bool             m_issueUrlKnown = false;

    // LRU cache. QCache uses a "cost" budget; here every entry costs 1 so
    // the budget is the row count.
    QCache<QByteArray, std::shared_ptr<const GitCommitDetail>> m_cache;
    static constexpr int kCacheCapacity = 16;
    static constexpr qint64 kDefaultCapBytes = 5 * 1024 * 1024;

    quint64  m_generation = 0;
    std::unique_ptr<InFlight> m_inflight;
};

#endif // GIT_COMMIT_FETCHER_H
