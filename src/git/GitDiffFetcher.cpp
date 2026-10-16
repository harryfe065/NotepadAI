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

#include "GitDiffFetcher.h"

#include "ApplicationSettings.h"
#include "FileEncodingDetector.h"
#include "GitController.h"
#include "GitDiffSyntaxMapper.h"
#include "NotepadNextApplication.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QTimer>

GitDiffFetcher::GitDiffFetcher(GitController *controller, QObject *parent)
    : QObject(parent), m_controller(controller)
{
    connect(controller, &GitController::diffReady, this, &GitDiffFetcher::onDiffReady);
    connect(controller, &GitController::diffFailed, this, &GitDiffFetcher::onDiffFailed);
    // statusRefreshed, not statusUpdated: a file already listed as modified
    // can change content without changing the status output.
    connect(controller, &GitController::statusRefreshed, this, &GitDiffFetcher::onStatusUpdated);
}

QByteArray GitDiffFetcher::synthDiffForUntracked(const QString &repoRoot, const QString &relPath)
{
    QFile f(QDir(repoRoot).filePath(relPath));
    if (!f.open(QIODevice::ReadOnly)) {
        QByteArray out;
        out.append("diff --git a/" + relPath.toUtf8() + " b/" + relPath.toUtf8() + "\n");
        out.append("Binary files differ\n");
        return out;
    }
    // Read up to 4 MiB to keep memory bounded; very large untracked files
    // are usually binary anyway.
    constexpr qsizetype kMax = 4 * 1024 * 1024;
    const QByteArray raw = f.read(kMax);
    const qint64 fullSize = f.size();
    f.close();

    QString decoded;
    const bool ok = FileEncodingDetector::decode(raw, decoded);
    QByteArray out;
    out.reserve(raw.size() + 256);
    const QByteArray relU8 = relPath.toUtf8();
    out.append("diff --git a/" + relU8 + " b/" + relU8 + "\n");
    out.append("new file mode 100644\n");
    out.append("--- /dev/null\n");
    out.append("+++ b/" + relU8 + "\n");

    if (!ok) {
        out.append("Binary files differ\n");
        return out;
    }

    const QByteArray utf8 = decoded.toUtf8();
    // Count lines (LF-terminated; trailing line without LF counts too).
    int lineCount = 0;
    for (qsizetype i = 0; i < utf8.size(); ++i) if (utf8.at(i) == '\n') ++lineCount;
    if (!utf8.isEmpty() && utf8.back() != '\n') ++lineCount;
    if (lineCount == 0) return out;

    out.append(QStringLiteral("@@ -0,0 +1,%1 @@\n").arg(lineCount).toUtf8());

    qsizetype i = 0;
    while (i < utf8.size()) {
        qsizetype j = i;
        while (j < utf8.size() && utf8.at(j) != '\n') ++j;
        out.append('+');
        out.append(utf8.constData() + i, j - i);
        out.append('\n');
        i = (j < utf8.size()) ? j + 1 : j;
    }
    if (fullSize > raw.size()) {
        out.append("\\ Truncated (file too large to display)\n");
    }
    return out;
}

void GitDiffFetcher::request(const GitStatusEntry &entry)
{
    if (entry.relPath.isEmpty()) return;

    const QString repo = m_controller->currentRepo();
    const GitDiffCache::Key key = GitDiffCache::keyFor(repo, entry.relPath, entry.stagedSide);
    if (auto cached = m_cache.get(key)) {
        emit parsedReady(entry.relPath, entry.stagedSide, cached.parsed, cached.overlay);
        return;
    }

    Pending pending;
    pending.stagedSide = entry.stagedSide;
    pending.isUntracked = (entry.section == GitStatusEntry::Untracked);
    pending.entry = entry;
    m_inflight.insert(entry.relPath, pending);

    if (pending.isUntracked) {
        // Build synthetic diff on the UI thread; for v1 this is fine — the
        // 4 MiB read budget keeps the worst case <30 ms on SSD.
        QPointer<GitDiffFetcher> guard(this);
        QString rel = entry.relPath;
        bool staged = entry.stagedSide;
        QTimer::singleShot(0, this, [guard, repo, rel, staged]() {
            if (!guard) return;
            const QByteArray diff = synthDiffForUntracked(repo, rel);
            guard->onDiffReady(rel, staged, diff);
        });
        return;
    }

    m_controller->requestDiff(entry.relPath, entry.stagedSide);
}

void GitDiffFetcher::onDiffReady(const QString &relPath, bool stagedSide, const QByteArray &diff)
{
    Q_UNUSED(stagedSide);
    const auto it = m_inflight.find(relPath);
    if (it == m_inflight.end()) return;  // not ours / stale
    if (it->stagedSide != stagedSide) return;

    auto parsed = std::make_shared<const GitDiffParser::Result>(GitDiffParser::parse(diff));

    // Compute the syntax overlay sync on the UI thread. Empty/null overlay
    // means the painter falls back to fixed-style rendering (solid line
    // backgrounds, no token colors). Honor the user opt-out setting.
    std::shared_ptr<const GitDiffSyntaxMapper::Overlay> overlay;
    if (!parsed->isBinary && !parsed->kinds.isEmpty()) {
        auto *app = qobject_cast<NotepadNextApplication*>(QCoreApplication::instance());
        bool enabled = true;
        if (app && app->getSettings()) {
            enabled = app->getSettings()->syntaxHighlightDiffEnabled();
        }
        if (enabled && app) {
            // Resolve lexers from the entry paths. For renames the OLD side
            // uses origRelPath if present; otherwise both sides use relPath.
            const GitStatusEntry &entry = it->entry;
            const QString oldPath = entry.origRelPath.isEmpty() ? relPath : entry.origRelPath;
            const QString oldLang = app->detectLanguageFromPath(oldPath);
            const QString newLang = app->detectLanguageFromPath(relPath);
            const QString oldLex  = app->resolveLexerName(oldLang);
            const QString newLex  = app->resolveLexerName(newLang);

            GitDiffSyntaxMapper::Input in;
            in.parsed = parsed.get();
            in.oldLexerName = oldLex;
            in.newLexerName = newLex;
            auto computed = GitDiffSyntaxMapper::map(in, m_mapperState);
            if (computed.ok) {
                overlay = std::make_shared<const GitDiffSyntaxMapper::Overlay>(std::move(computed));
            }
        }
    }

    const auto key = GitDiffCache::keyFor(m_controller->currentRepo(), relPath, stagedSide);
    m_cache.put(key, GitDiffCache::Entry{ parsed, overlay }, diff.size());
    m_inflight.erase(it);
    emit parsedReady(relPath, stagedSide, parsed, overlay);
}

void GitDiffFetcher::onDiffFailed(const QString &relPath, bool stagedSide, const QString &message)
{
    const auto it = m_inflight.find(relPath);
    if (it != m_inflight.end() && it->stagedSide == stagedSide) m_inflight.erase(it);
    emit failed(relPath, stagedSide, message);
}

void GitDiffFetcher::onStatusUpdated()
{
    m_cache.clear();
}
