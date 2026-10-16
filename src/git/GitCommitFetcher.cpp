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

#include "GitCommitFetcher.h"

#include "GitProcessRunner.h"
#include "GitRunnerFactory.h"
#include "GitWatcher.h"

#include <QByteArray>
#include <QHash>
#include <QStringList>
#include <QTimer>

#include <algorithm>

namespace {

constexpr int kCommitTimeoutMs = 60000;

// Format string for `git show --format=...`.
// Fields in order:
//   %H = full SHA, %P = parents, %an = author name, %ae = author email,
//   %at = author epoch, %cn = committer name, %ce = committer email,
//   %ct = committer epoch, %s = subject, %B = full body
//   each separated by US (0x1f), terminated by RS (0x1e). The patch follows.
constexpr const char *kShowHeaderFormat =
    "--format=%H%x1f%P%x1f%an%x1f%ae%x1f%at%x1f%cn%x1f%ce%x1f%ct%x1f%s%x1f%B%x1e";

qint64 parseI64(const QByteArray &b)
{
    qint64 v = 0;
    bool neg = false;
    qsizetype i = 0;
    if (i < b.size() && b[i] == '-') { neg = true; ++i; }
    for (; i < b.size(); ++i) {
        const char c = b[i];
        if (c < '0' || c > '9') return 0;
        v = v * 10 + (c - '0');
    }
    return neg ? -v : v;
}

} // namespace

GitCommitFetcher::GitCommitFetcher(QObject *parent)
    : QObject(parent), m_runner(GitRunnerFactory::createForRepo(QString(), this))
{
    m_cache.setMaxCost(kCacheCapacity);
    m_runner->setMaxOutputBytes(kDefaultCapBytes);
}

GitCommitFetcher::~GitCommitFetcher() = default;

void GitCommitFetcher::setRunnerScope(const QString &scope)
{
    if (scope == m_runnerScope) return;
    m_runnerScope = scope;
    if (!m_repoRoot.isEmpty()) {
        if (m_runner) m_runner->asQObject()->deleteLater();
        const QString s = m_runnerScope.isEmpty() ? m_repoRoot : m_runnerScope;
        m_runner = GitRunnerFactory::createForRepo(s, this);
        m_runner->setMaxOutputBytes(kDefaultCapBytes);
    }
}

void GitCommitFetcher::setRepoRoot(const QString &repoToplevel)
{
    if (repoToplevel == m_repoRoot) return;
    cancel();
    invalidateAll();
    m_repoRoot = repoToplevel;
    m_issueUrlBase.clear();
    m_issueUrlKnown = false;
    if (m_runner) m_runner->asQObject()->deleteLater();
    const QString scope = m_runnerScope.isEmpty() ? m_repoRoot : m_runnerScope;
    m_runner = GitRunnerFactory::createForRepo(scope, this);
    m_runner->setMaxOutputBytes(kDefaultCapBytes);
}

void GitCommitFetcher::invalidateAll()
{
    m_cache.clear();
}

void GitCommitFetcher::connectWatcher(GitWatcher *watcher)
{
    if (!watcher) return;
    connect(watcher, &GitWatcher::headChanged, this, &GitCommitFetcher::invalidateAll);
    connect(watcher, &GitWatcher::refsChanged, this, &GitCommitFetcher::invalidateAll);
}

QByteArray GitCommitFetcher::cacheKey(const QByteArray &sha, bool ignoreWhitespace)
{
    return ignoreWhitespace ? sha + QByteArrayLiteral(":w") : sha;
}

void GitCommitFetcher::requestDetail(const QByteArray &sha, bool ignoreWhitespace, Callback cb)
{
    if (sha.isEmpty() || m_repoRoot.isEmpty()) {
        if (cb) cb({});
        return;
    }
    // Cache hit — fire callback asynchronously to keep call-site semantics
    // consistent (always "later" not "sometimes immediate").
    if (auto *hit = m_cache.object(cacheKey(sha, ignoreWhitespace))) {
        std::shared_ptr<const GitCommitDetail> shared = *hit;
        cancel();
        const quint64 gen = m_generation;
        QTimer::singleShot(0, this, [this, gen, cb = std::move(cb), shared]() {
            if (gen == m_generation && cb) cb(shared);
        });
        emit requestCompleted(sha, true);
        return;
    }
    start(sha, ignoreWhitespace, false, std::move(cb));
}

void GitCommitFetcher::requestDetailUncapped(const QByteArray &sha, bool ignoreWhitespace, Callback cb)
{
    if (sha.isEmpty() || m_repoRoot.isEmpty()) {
        if (cb) cb({});
        return;
    }
    // Evict any cached truncated copy.
    m_cache.remove(cacheKey(sha, ignoreWhitespace));
    start(sha, ignoreWhitespace, true, std::move(cb));
}

void GitCommitFetcher::start(const QByteArray &sha, bool ignoreWhitespace, bool uncapped, Callback cb)
{
    cancel();
    m_inflight = std::make_unique<InFlight>();
    m_inflight->sha = sha;
    m_inflight->cb = std::move(cb);
    m_inflight->partial = std::make_shared<GitCommitDetail>();
    m_inflight->generation = m_generation;
    m_inflight->uncapped = uncapped;
    m_inflight->ignoreWhitespace = ignoreWhitespace;
    launchShow(*m_inflight, false);
}

void GitCommitFetcher::cancel()
{
    if (m_runner && m_runner->isRunning()) m_runner->cancelAsync();
    ++m_generation;
    if (m_inflight) m_inflight.reset();
    m_runner->setMaxOutputBytes(kDefaultCapBytes);
}

void GitCommitFetcher::launchShow(InFlight &state, bool legacyMerges)
{
    const quint64 myGen = state.generation;
    m_runner->setMaxOutputBytes(state.uncapped ? 0 : kDefaultCapBytes);
    QStringList argv = {
        QStringLiteral("-c"), QStringLiteral("color.ui=never"),
        QStringLiteral("-c"), QStringLiteral("core.quotepath=false"),
        QStringLiteral("show"), QStringLiteral("--no-color"), QStringLiteral("--no-ext-diff"),
        // Renames are always detected, whatever diff.renames says.
        QStringLiteral("-M"), QStringLiteral("-p"),
        QStringLiteral("--src-prefix=a/"), QStringLiteral("--dst-prefix=b/"),
        QString::fromLatin1(kShowHeaderFormat)
    };
    // A merge shows its diff against the first parent.
    if (legacyMerges)
        argv << QStringLiteral("-m") << QStringLiteral("--first-parent");
    else
        argv << QStringLiteral("--diff-merges=first-parent");
    if (state.ignoreWhitespace) argv << QStringLiteral("-w");
    argv << QString::fromLatin1(state.sha);

    m_runner->run(m_repoRoot, argv, QByteArray(), kCommitTimeoutMs, false,
                  [this, myGen, legacyMerges](int exit, const QByteArray &out, const QByteArray &err) {
        if (myGen != m_generation) return;
        if (!m_inflight) return;
        const bool truncated = (exit == GitProcessRunner::kExitTruncated);
        if (exit != 0 && !truncated) {
            // git < 2.31 has no --diff-merges.
            if (!legacyMerges && err.contains("diff-merges")) {
                launchShow(*m_inflight, true);
                return;
            }
            qWarning("git show failed: %s", qPrintable(QString::fromUtf8(err).trimmed()));
            completeFail(*m_inflight);
            return;
        }
        if (!splitShowOutput(out, *m_inflight->partial)) {
            completeFail(*m_inflight);
            return;
        }
        m_inflight->partial->truncated = truncated;
        if (!m_issueUrlKnown)
            launchRemoteUrl(*m_inflight);
        else
            completeOk(*m_inflight);
    });
}

void GitCommitFetcher::launchRemoteUrl(InFlight &state)
{
    const quint64 myGen = state.generation;
    m_runner->setMaxOutputBytes(kDefaultCapBytes);
    const QStringList argv = { QStringLiteral("config"), QStringLiteral("--get"),
                               QStringLiteral("remote.origin.url") };
    m_runner->run(m_repoRoot, argv, QByteArray(), kCommitTimeoutMs, false,
                  [this, myGen](int exit, const QByteArray &out, const QByteArray &) {
        if (myGen != m_generation) return;
        if (!m_inflight) return;
        // exit 1 = no origin: #N stays plain text.
        m_issueUrlBase = exit == 0 ? issueUrlBaseFor(out.trimmed()) : QByteArray();
        m_issueUrlKnown = true;
        completeOk(*m_inflight);
    });
}

void GitCommitFetcher::requestLinguistGenerated(const QVector<QByteArray> &paths, GeneratedCallback cb)
{
    if (paths.isEmpty() || m_repoRoot.isEmpty()) {
        if (cb) cb({});
        return;
    }
    cancel();
    const quint64 myGen = m_generation;
    QByteArray input;
    for (const QByteArray &p : paths) {
        input += p;
        input += '\0';
    }
    const QStringList argv = { QStringLiteral("check-attr"), QStringLiteral("-z"), QStringLiteral("--stdin"),
                               QStringLiteral("linguist-generated") };
    m_runner->run(m_repoRoot, argv, input, kCommitTimeoutMs, false,
                  [this, myGen, paths, cb = std::move(cb)](int exit, const QByteArray &out, const QByteArray &) {
        if (myGen != m_generation) return;
        if (cb) cb(exit == 0 ? parseCheckAttr(out, paths) : QVector<bool>());
    });
}

QVector<bool> GitCommitFetcher::parseCheckAttr(const QByteArray &out, const QVector<QByteArray> &paths)
{
    // -z output: <path> NUL <attribute> NUL <value> NUL, per path.
    QHash<QByteArray, bool> generated;
    const QList<QByteArray> fields = out.split('\0');
    for (qsizetype i = 0; i + 2 < fields.size(); i += 3) {
        const QByteArray &value = fields[i + 2];
        if (value == "set" || value == "true") generated.insert(fields[i], true);
    }
    QVector<bool> result(paths.size(), false);
    for (qsizetype i = 0; i < paths.size(); ++i) result[i] = generated.value(paths[i], false);
    return result;
}

QByteArray GitCommitFetcher::issueUrlBaseFor(const QByteArray &remoteUrl)
{
    QByteArray url = remoteUrl.trimmed();
    if (url.isEmpty()) return {};
    QByteArray host;
    QByteArray path;
    const qsizetype scheme = url.indexOf("://");
    if (scheme >= 0) {
        QByteArray rest = url.mid(scheme + 3);
        const qsizetype slash = rest.indexOf('/');
        if (slash < 0) return {};
        host = rest.left(slash);
        path = rest.mid(slash + 1);
        const qsizetype at = host.lastIndexOf('@');
        if (at >= 0) host = host.mid(at + 1);            // credentials / ssh user
        const qsizetype colon = host.indexOf(':');
        if (colon >= 0) host = host.left(colon);          // port
    } else {
        // scp-like: [user@]host:owner/repo
        const qsizetype colon = url.indexOf(':');
        if (colon < 0) return {};
        host = url.left(colon);
        path = url.mid(colon + 1);
        const qsizetype at = host.lastIndexOf('@');
        if (at >= 0) host = host.mid(at + 1);
    }
    while (path.endsWith('/')) path.chop(1);
    if (path.endsWith(".git")) path.chop(4);
    while (path.startsWith('/')) path = path.mid(1);
    host = host.toLower();
    if (host.isEmpty() || path.isEmpty() || !path.contains('/')) return {};

    const QByteArray web = "https://" + host + '/' + path;
    if (host == "github.com" || host == "codeberg.org" || host == "bitbucket.org" || host.contains("gitea"))
        return web + "/issues/";
    if (host.contains("gitlab")) return web + "/-/issues/";
    return {};
}

bool GitCommitFetcher::splitShowOutput(const QByteArray &out, GitCommitDetail &detail)
{
    parseHeader(out, detail);
    if (detail.sha.isEmpty()) return false;
    parseTrailers(detail);
    const qsizetype rs = out.indexOf('\x1e');
    qsizetype patch = rs < 0 ? out.size() : rs + 1;
    while (patch < out.size() && (out[patch] == '\n' || out[patch] == '\r')) ++patch;
    detail.diffBytes = out.mid(patch);
    return true;
}

void GitCommitFetcher::completeOk(InFlight &state)
{
    auto shared = std::shared_ptr<const GitCommitDetail>(std::move(state.partial));
    // Cache. QCache takes ownership of the heap pointer; wrap the shared_ptr
    // in another heap allocation so QCache can delete it.
    if (!state.uncapped) {
        m_cache.insert(cacheKey(state.sha, state.ignoreWhitespace),
                       new std::shared_ptr<const GitCommitDetail>(shared));
    }
    auto cb = std::move(state.cb);
    const QByteArray sha = state.sha;
    m_inflight.reset();
    m_runner->setMaxOutputBytes(kDefaultCapBytes);
    if (cb) cb(shared);
    emit requestCompleted(sha, true);
}

void GitCommitFetcher::completeFail(InFlight &state)
{
    auto cb = std::move(state.cb);
    const QByteArray sha = state.sha;
    m_inflight.reset();
    m_runner->setMaxOutputBytes(kDefaultCapBytes);
    if (cb) cb({});
    emit requestCompleted(sha, false);
}

void GitCommitFetcher::parseHeader(const QByteArray &bytes, GitCommitDetail &out)
{
    // Layout (10 US-separated fields, terminated by RS):
    //   sha, parents, an, ae, at, cn, ce, ct, subject, body
    const char *data = bytes.constData();
    const qsizetype n = bytes.size();
    qsizetype seps[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
    int sepN = 0;
    qsizetype rsEnd = n;
    for (qsizetype i = 0; i < n; ++i) {
        const char c = data[i];
        if (c == '\x1f' && sepN < 9) {
            seps[sepN++] = i;
        } else if (c == '\x1e') {
            rsEnd = i;
            break;
        }
    }
    if (sepN != 9) return;   // malformed → leave out.sha empty as fail signal
    out.sha             = QByteArray(data,             seps[0]);
    out.parents         = QByteArray(data + seps[0]+1, seps[1] - seps[0] - 1);
    out.authorName      = QByteArray(data + seps[1]+1, seps[2] - seps[1] - 1);
    out.authorEmail     = QByteArray(data + seps[2]+1, seps[3] - seps[2] - 1);
    out.authorTime      = parseI64(QByteArray(data + seps[3]+1, seps[4] - seps[3] - 1));
    out.committerName   = QByteArray(data + seps[4]+1, seps[5] - seps[4] - 1);
    out.committerEmail  = QByteArray(data + seps[5]+1, seps[6] - seps[5] - 1);
    out.commitTime      = parseI64(QByteArray(data + seps[6]+1, seps[7] - seps[6] - 1));
    out.subject         = QByteArray(data + seps[7]+1, seps[8] - seps[7] - 1);
    out.body            = QByteArray(data + seps[8]+1, rsEnd  - seps[8] - 1);
}

void GitCommitFetcher::parseTrailers(GitCommitDetail &out)
{
    // Look for a "trailer block" at the END of the body: one or more lines
    // matching ^[A-Z][A-Za-z-]+: .+ preceded by a blank line (or being the
    // very first line, for body-less commits — rare but possible).
    //
    // We scan back from the end, collecting matching lines into a vector,
    // then verify the block is preceded by a blank line OR is the entire
    // body. Otherwise, no trailers parsed (defensive against false matches
    // mid-body like "URL: https://...").
    const QByteArray &body = out.body;
    if (body.isEmpty()) return;

    QVector<QByteArrayView> lines;
    qsizetype lineStart = body.size();
    for (qsizetype i = body.size() - 1; i >= 0; --i) {
        if (body[i] == '\n') {
            lines.append(QByteArrayView(body.constData() + i + 1, lineStart - i - 1));
            lineStart = i;
        }
    }
    // First line (no leading \n).
    lines.append(QByteArrayView(body.constData(), lineStart));

    auto isTrailerLine = [](QByteArrayView ln) -> bool {
        if (ln.isEmpty()) return false;
        const char c0 = ln[0];
        if (c0 < 'A' || c0 > 'Z') return false;
        qsizetype i = 1;
        while (i < ln.size()) {
            const char c = ln[i];
            const bool letterOrDash =
                (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-';
            if (!letterOrDash) break;
            ++i;
        }
        if (i >= ln.size() || ln[i] != ':') return false;
        if (i + 1 >= ln.size() || ln[i+1] != ' ') return false;
        return ln.size() > i + 2;
    };

    // Walk from end (lines[0]) to find the first non-trailer line.
    int trailerCount = 0;
    while (trailerCount < lines.size() && isTrailerLine(lines[trailerCount])) {
        ++trailerCount;
    }
    if (trailerCount == 0) return;
    // The line just before the cluster (lines[trailerCount]) must be empty,
    // OR the cluster IS the whole body.
    const bool clusterIsAll = (trailerCount == lines.size());
    if (!clusterIsAll) {
        if (!lines[trailerCount].isEmpty()) return;   // false match — abort
    }

    out.trailers.reserve(trailerCount);
    // Restore order (we walked backwards). lines[trailerCount-1] is oldest.
    for (int i = trailerCount - 1; i >= 0; --i) {
        const auto &ln = lines[i];
        // Find ":"
        qsizetype colon = -1;
        for (qsizetype k = 0; k < ln.size(); ++k) {
            if (ln[k] == ':') { colon = k; break; }
        }
        if (colon < 0) continue;
        GitCommitDetail::Trailer t;
        t.key = QByteArray(ln.data(), colon);
        t.value = QByteArray(ln.data() + colon + 2, ln.size() - colon - 2);
        out.trailers.append(t);
    }
}
