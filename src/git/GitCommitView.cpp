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

#include "GitCommitView.h"

#include "ApplicationSettings.h"
#include "CrashContext.h"
#include "DiffViewApp.h"
#include "EditorManager.h"
#include "GitCommitFetcher.h"
#include "GitDiffPalette.h"
#include "GitFileAtShaFetcher.h"
#include "GitWatcher.h"
#include "LineNumbers.h"
#include "ScintillaNext.h"

#include "Scintilla.h"

#include <QFutureWatcher>
#include <QtConcurrentRun>

namespace {

// Built commit documents kept for revisits (cost = approximate bytes).
constexpr qsizetype kContentCacheBytes = 64 * 1024 * 1024;

bool ignoreWhitespace()
{
    ApplicationSettings *settings = DiffViewApp::settings();
    return settings && settings->diffIgnoreWhitespace();
}

qsizetype contentCost(const DiffView::Content &content)
{
    if (!content.doc) return 1;
    // Text + style bytes + the parsed rows kept for Load diff.
    return 3 * content.doc->text.size() + 16 * qsizetype(content.doc->rowCount()) + 4096;
}

QString tabTitleForCommit(const GitCommitDetail &detail)
{
    QString shortSha = QString::fromLatin1(detail.sha.left(7));
    QString subj = QString::fromUtf8(detail.subject);
    if (subj.size() > 60) subj = subj.left(60) + QStringLiteral("…");
    if (shortSha.isEmpty()) return subj;
    if (subj.isEmpty()) return shortSha;
    return shortSha + QStringLiteral(" — ") + subj;
}

QString placeholderTitle(const QByteArray &sha)
{
    return QStringLiteral("[Commit ") + QString::fromLatin1(sha.left(7)) + QStringLiteral("]");
}

QString tabTitleForFileAtSha(const QByteArray &sha, const QString &relPath)
{
    return QString::fromLatin1(sha.left(7)) + QLatin1Char(':') + relPath;
}

QByteArray makeFileAtShaKey(const QByteArray &sha, const QString &relPath)
{
    QByteArray k = sha;
    k += ':';
    k += relPath.toUtf8();
    return k;
}

int bgr(const QColor &c)
{
    return c.red() | (c.green() << 8) | (c.blue() << 16);
}

void goToLine(ScintillaNext *editor, int line)
{
    if (line <= 1) return;
    editor->ensureVisible(line - 1);
    editor->gotoLine(line - 1);
    editor->verticalCentreCaret();
}

} // namespace

GitCommitView::GitCommitView(const QString &repoRoot,
                              EditorManager *editorManager,
                              QObject *parent)
    : QObject(parent),
      m_repoRoot(repoRoot),
      m_editorManager(editorManager),
      m_fetcher(new GitCommitFetcher(this)),
      m_fileFetcher(new GitFileAtShaFetcher(this))
{
    m_contentCache.setMaxCost(kContentCacheBytes);
    m_fetcher->setRepoRoot(repoRoot);
    m_fileFetcher->setRepoRoot(repoRoot);
}

GitCommitView::~GitCommitView() = default;

QByteArray GitCommitView::contentKey(const QByteArray &sha, bool ignoreWhitespace)
{
    return ignoreWhitespace ? sha + QByteArrayLiteral(":w") : sha;
}

void GitCommitView::setRepoRoot(const QString &repoRoot)
{
    if (repoRoot == m_repoRoot) return;
    m_repoRoot = repoRoot;
    m_fetcher->setRepoRoot(repoRoot);
    m_fileFetcher->setRepoRoot(repoRoot);
    m_contentCache.clear();
    if (m_view) m_view->setRepoRoot(repoRoot);
}

void GitCommitView::setRunnerScope(const QString &scope)
{
    m_fetcher->setRunnerScope(scope);
    m_fileFetcher->setRunnerScope(scope);
}

void GitCommitView::setDarkPalette(bool dark)
{
    if (m_dark == dark) return;
    m_dark = dark;
    // Restyles the shown document in place; nothing is refetched.
    if (m_view) m_view->setDark(dark);
    styleFileAtShaEditor();
}

void GitCommitView::styleFileAtShaEditor()
{
    ScintillaNext *e = m_fileAtShaEditor.data();
    if (!e) return;
    // Registered as a diff view, so the editor theme skips it: paint the
    // plain text in the diff canvas colours.
    const DiffViewColors colors = DiffViewColors::make(m_dark);
    e->send(SCI_STYLESETFORE, STYLE_DEFAULT, bgr(colors.text));
    e->send(SCI_STYLESETBACK, STYLE_DEFAULT, bgr(colors.canvas));
    e->send(SCI_STYLECLEARALL);
    e->send(SCI_STYLESETFORE, STYLE_LINENUMBER, bgr(colors.muted));
    e->send(SCI_SETCARETFORE, bgr(colors.text));
}

void GitCommitView::setWatcher(GitWatcher *watcher)
{
    if (!watcher) return;
    m_fetcher->connectWatcher(watcher);
    connect(watcher, &GitWatcher::headChanged, this, [this] { m_contentCache.clear(); });
    connect(watcher, &GitWatcher::refsChanged, this, [this] { m_contentCache.clear(); });
}

ScintillaNext *GitCommitView::ensureDetailEditor(const QByteArray &sha)
{
    if (m_detailEditor) {
        // Placeholder title while the new content is fetched.
        m_detailEditor->setName(placeholderTitle(sha));
        return m_detailEditor;
    }
    ScintillaNext *editor = m_editorManager->createEditor(placeholderTitle(sha));
    m_editorManager->registerAsDiffView(editor);
    // DiffView fills margin 0 with the diff's own line numbers.
    if (auto *ln = editor->findChild<LineNumbers *>(QString(), Qt::FindDirectChildrenOnly))
        ln->setEnabled(false);

    auto *view = new DiffView(editor, DiffView::Mode::Commit);
    view->setDark(m_dark);
    view->setRepoRoot(m_repoRoot);
    connect(view, &DiffView::contentApplied, this, [this](const DiffView::Content &content) {
        if (!m_currentKey.isEmpty() && !m_contentCache.contains(m_currentKey))
            m_contentCache.insert(m_currentKey, new DiffView::Content(content), contentCost(content));
        if (m_detailEditor) emit commitDetailRendered(m_detailEditor);
    });
    connect(view, &DiffView::openCommitRequested, this, [this](const QByteArray &sha) {
        openForSha(sha);
        emit commitShown(sha);
    });
    connect(view, &DiffView::openFileRequested, this, &GitCommitView::onOpenFileRequested);
    connect(view, &DiffView::revealRequested, this, &GitCommitView::revealRequested);
    connect(view, &DiffView::ignoreWhitespaceChanged, this, [this](bool) {
        if (m_detailEditor && !m_currentSha.isEmpty()) load(m_currentSha, true);
    });
    // User closed the tab → drop our state so the next click creates a fresh one.
    connect(editor, &QObject::destroyed, this, [this](QObject *) {
        ++m_loadSerial;
        m_currentSha.clear();
        m_currentKey.clear();
    });
    m_view = view;
    m_detailEditor = editor;
    emit newCommitEditorCreated(editor);
    return editor;
}

void GitCommitView::openForSha(const QByteArray &sha)
{
    if (sha.isEmpty() || !m_editorManager) return;
    CrashContext::setLastAction(QStringLiteral("openCommitDetail:")
                                 + QString::fromLatin1(sha.left(7)));

    // Same SHA already showing in the shared tab → just focus, no refetch.
    if (m_detailEditor && sha == m_currentSha) {
        emit focusExistingCommitEditor(m_detailEditor.data());
        return;
    }
    ensureDetailEditor(sha);
    load(sha, false);
}

void GitCommitView::load(const QByteArray &sha, bool keepPosition)
{
    const quint64 serial = ++m_loadSerial;
    const bool ws = ignoreWhitespace();
    m_currentSha = sha;
    m_currentKey = contentKey(sha, ws);
    m_view->cancel();

    if (const DiffView::Content *hit = m_contentCache.object(m_currentKey)) {
        m_fetcher->cancel();
        const DiffView::Content content = *hit;
        if (content.doc && content.doc->input && content.doc->input->commit)
            m_detailEditor->setName(tabTitleForCommit(*content.doc->input->commit));
        m_view->showContent(content, keepPosition);
        return;
    }

    m_fetcher->requestDetail(sha, ws,
        [this, serial, sha, keepPosition](const std::shared_ptr<const GitCommitDetail> &detail) {
        // Stale guard — the user may have clicked another SHA meanwhile.
        if (serial != m_loadSerial || !m_detailEditor) return;
        if (!detail || detail->isEmpty()) {
            emit fetchFailed(sha, tr("Could not load commit %1")
                                    .arg(QString::fromLatin1(sha.left(7))));
            return;
        }
        showDetail(serial, detail, keepPosition);
    });
}

void GitCommitView::showDetail(quint64 serial, const std::shared_ptr<const GitCommitDetail> &detail,
                               bool keepPosition)
{
    m_detailEditor->setName(tabTitleForCommit(*detail));

    // Parse on the pool: check-attr needs the file list before the build.
    using Parsed = std::shared_ptr<const GitDiffParser::Result>;
    auto *watcher = new QFutureWatcher<Parsed>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, serial, detail, keepPosition] {
        watcher->deleteLater();
        if (serial != m_loadSerial || !m_view) return;
        Parsed parsed = watcher->result();
        DiffView::Source source;
        source.parsed = parsed;
        source.commit = detail;
        if (parsed->files.size() < 2) {
            source.issueUrlBase = m_fetcher->issueUrlBase();
            m_view->show(std::move(source), keepPosition);
            return;
        }
        // Generated files start collapsed only in views of 2+ files.
        QVector<QByteArray> paths;
        paths.reserve(parsed->files.size());
        for (const auto &f : parsed->files) paths.append(f.path());
        m_fetcher->requestLinguistGenerated(paths,
            [this, serial, keepPosition, source = std::move(source)](QVector<bool> generated) mutable {
            if (serial != m_loadSerial || !m_view) return;
            source.linguistGenerated = std::move(generated);
            source.issueUrlBase = m_fetcher->issueUrlBase();
            m_view->show(std::move(source), keepPosition);
        });
    });
    watcher->setFuture(QtConcurrent::run([detail]() -> Parsed {
        return std::make_shared<const GitDiffParser::Result>(GitDiffParser::parse(detail->diffBytes));
    }));
}

void GitCommitView::onOpenFileRequested(const QString &relPath, int line, DiffView::Side side)
{
    // The commit on screen: a newer one may already be loading.
    const DiffDoc::Document *doc = m_view ? m_view->document() : nullptr;
    const GitCommitDetail *shown = doc && doc->input ? doc->input->commit.get() : nullptr;
    switch (side) {
        case DiffView::Side::Working:
            emit openWorkingFileRequested(relPath, line);
            break;
        case DiffView::Side::Revision:
            if (shown) openFileAtSha(shown->sha, relPath, line);
            break;
        case DiffView::Side::ParentRevision: {
            if (!shown) return;
            const QByteArray parents = shown->parents;
            const qsizetype space = parents.indexOf(' ');
            const QByteArray first = space < 0 ? parents : parents.left(space);
            if (!first.isEmpty()) openFileAtSha(first, relPath, line);
            break;
        }
    }
}

void GitCommitView::openFileAtSha(const QByteArray &sha, const QString &relPath, int line)
{
    if (sha.isEmpty() || relPath.isEmpty() || !m_editorManager) return;
    const QByteArray key = makeFileAtShaKey(sha, relPath);
    m_fileAtShaLine = line;

    // Same (sha, relPath) already showing → just focus.
    if (m_fileAtShaEditor && key == m_currentFileAtShaKey) {
        goToLine(m_fileAtShaEditor.data(), line);
        emit focusExistingFileAtShaEditor(m_fileAtShaEditor.data());
        return;
    }

    const bool needNewEditor = !m_fileAtShaEditor;
    ScintillaNext *editor = m_fileAtShaEditor.data();

    if (needNewEditor) {
        editor = m_editorManager->createEditor(tabTitleForFileAtSha(sha, relPath));
        m_editorManager->registerAsDiffView(editor);
        m_fileAtShaEditor = editor;
        styleFileAtShaEditor();
        connect(editor, &QObject::destroyed, this, [this](QObject *) {
            m_currentFileAtShaKey.clear();
        });
        emit newFileAtShaEditorCreated(editor);
    } else {
        editor->setName(tabTitleForFileAtSha(sha, relPath));
    }

    m_currentFileAtShaKey = key;

    m_fileFetcher->request(sha, relPath,
        [this, key](const QByteArray &bytes, bool truncated, const QString &err) {
        if (key != m_currentFileAtShaKey) return;     // stale
        ScintillaNext *e = m_fileAtShaEditor.data();
        if (!e) return;
        e->send(SCI_SETREADONLY, 0, 0);
        e->send(SCI_CLEARALL, 0, 0);
        if (!err.isEmpty()) {
            const QByteArray msg = tr("Could not load file: %1").arg(err).toUtf8();
            e->send(SCI_APPENDTEXT, msg.size(),
                    reinterpret_cast<sptr_t>(msg.constData()));
        } else {
            e->send(SCI_APPENDTEXT, bytes.size(),
                    reinterpret_cast<sptr_t>(bytes.constData()));
            if (truncated) {
                const QByteArray foot =
                    QByteArrayLiteral("\n... file truncated (too large) ...\n");
                e->send(SCI_APPENDTEXT, foot.size(),
                        reinterpret_cast<sptr_t>(foot.constData()));
            }
        }
        e->send(SCI_SETREADONLY, 1, 0);
        goToLine(e, m_fileAtShaLine);
    });
}
