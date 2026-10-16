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

#include "FileEncodingDetector.h"
#include "GitController.h"

#include <QDir>
#include <QFile>

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

void GitDiffFetcher::request(const GitStatusEntry &entry, bool ignoreWhitespace)
{
    if (entry.relPath.isEmpty()) return;

    const QString repo = m_controller->currentRepo();
    const GitDiffCache::Key key = GitDiffCache::keyFor(repo, entry.relPath, entry.stagedSide);
    if (auto cached = m_cache.get(key); cached && cached.doc) {
        emit documentReady(entry.relPath, entry.stagedSide, cached.doc);
        return;
    }

    if (entry.section == GitStatusEntry::Untracked) {
        // Read and decode the file in DiffView's pool job.
        m_inflight.remove(entry.relPath);
        const QString rel = entry.relPath;
        emit patchReady(rel, entry.stagedSide, QByteArray(),
                        [repo, rel] { return synthDiffForUntracked(repo, rel); });
        return;
    }

    // Requests run in order and all reply: a repeat for the same path (e.g.
    // after Hide whitespace) supersedes the earlier one by token.
    const quint64 token = ++m_nextToken;
    m_inflight.insert(entry.relPath, Pending{entry.stagedSide, token});
    m_controller->requestDiff(entry.relPath, entry.stagedSide, entry.origRelPath, ignoreWhitespace, token);
}

void GitDiffFetcher::remember(const QString &relPath, bool stagedSide, std::shared_ptr<const DiffDoc::Document> doc)
{
    if (!doc || !doc->input || !doc->input->parsed) return;
    const auto key = GitDiffCache::keyFor(m_controller->currentRepo(), relPath, stagedSide);
    const qsizetype bytes = doc->text.size();
    m_cache.put(key, GitDiffCache::Entry{doc->input->parsed, std::move(doc)}, bytes);
}

void GitDiffFetcher::onDiffReady(const QString &relPath, bool stagedSide, const QByteArray &diff, quint64 token)
{
    const auto it = m_inflight.find(relPath);
    if (it == m_inflight.end()) return;  // not ours / stale
    if (it->stagedSide != stagedSide || it->token != token) return;
    m_inflight.erase(it);
    emit patchReady(relPath, stagedSide, diff, {});
}

void GitDiffFetcher::onDiffFailed(const QString &relPath, bool stagedSide, const QString &message, quint64 token)
{
    const auto it = m_inflight.find(relPath);
    if (it == m_inflight.end() || it->stagedSide != stagedSide || it->token != token) return;   // stale
    m_inflight.erase(it);
    emit failed(relPath, stagedSide, message);
}

void GitDiffFetcher::onStatusUpdated()
{
    m_cache.clear();
}
