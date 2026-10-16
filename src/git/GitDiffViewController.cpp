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

#include "GitDiffViewController.h"

#include "ApplicationSettings.h"
#include "DiffView.h"
#include "DiffViewApp.h"
#include "EditorManager.h"
#include "GitController.h"
#include "GitDiffFetcher.h"
#include "LineNumbers.h"
#include "ScintillaNext.h"

GitDiffViewController::GitDiffViewController(GitController *controller,
                                             EditorManager *editorManager,
                                             QObject *parent)
    : QObject(parent), m_controller(controller), m_editorManager(editorManager)
{
    m_fetcher = new GitDiffFetcher(controller, this);
    connect(m_fetcher, &GitDiffFetcher::patchReady, this, &GitDiffViewController::onPatchReady);
    connect(m_fetcher, &GitDiffFetcher::documentReady, this, &GitDiffViewController::onDocumentReady);
    connect(m_fetcher, &GitDiffFetcher::failed, this, &GitDiffViewController::onFetchFailed);
}

ScintillaNext *GitDiffViewController::previewEditor() const
{
    return m_previewEditor.data();
}

void GitDiffViewController::setDarkPalette(bool dark)
{
    if (m_isDark == dark) return;
    m_isDark = dark;
    // Restyles the shown document in place; nothing is refetched.
    if (m_view) m_view->setDark(dark);
}

bool GitDiffViewController::isCurrent(const QString &relPath, bool stagedSide) const
{
    return relPath == m_current.relPath && stagedSide == m_current.stagedSide;
}

void GitDiffViewController::showDiffFor(const GitStatusEntry &entry)
{
    if (entry.hasUnstableEncoding) {
        emit diffFailed(entry.relPath,
                        tr("Diff unavailable: filename has unsupported encoding."));
        return;
    }
    m_keepPosition = m_previewEditor && isCurrent(entry.relPath, entry.stagedSide);
    m_current = entry;
    ensurePreviewEditor();
    updatePreviewTitle(entry.relPath, entry.stagedSide);
    m_view->setRepoRoot(m_controller->currentRepo());
    // Latest wins: a build still running for the previous file must not land
    // (it would also be cached under this file's key).
    m_view->cancel();
    ApplicationSettings *settings = DiffViewApp::settings();
    m_fetcher->request(entry, settings && settings->diffIgnoreWhitespace());
}

ScintillaNext *GitDiffViewController::ensurePreviewEditor()
{
    if (m_previewEditor) return m_previewEditor;
    ScintillaNext *e = m_editorManager->createEditor(tr("[Diff Preview]"));
    m_editorManager->registerAsDiffView(e);

    // The default LineNumbers decorator generates buffer-line numbers
    // (1, 2, 3, ...) which are meaningless for a unified diff. DiffView
    // fills margin 0 itself with the diff's own old/new file line numbers,
    // so the decorator must stay disabled on this editor.
    if (auto *ln = e->findChild<LineNumbers *>(QString(), Qt::FindDirectChildrenOnly)) {
        ln->setEnabled(false);
    }

    auto *view = new DiffView(e, DiffView::Mode::Preview);
    view->setDark(m_isDark);
    connect(view, &DiffView::contentApplied, this, [this](const DiffView::Content &content) {
        m_fetcher->remember(m_current.relPath, m_current.stagedSide, content.doc);
        if (m_previewEditor) emit diffRendered(m_previewEditor);
    });
    connect(view, &DiffView::openFileRequested, this,
            [this](const QString &relPath, int line, DiffView::Side) { emit openFileRequested(relPath, line); });
    connect(view, &DiffView::revealRequested, this, &GitDiffViewController::revealRequested);
    connect(view, &DiffView::ignoreWhitespaceChanged, this, [this](bool) {
        m_fetcher->cache()->clear();
        if (m_previewEditor && !m_current.relPath.isEmpty()) showDiffFor(m_current);
    });
    m_view = view;
    m_previewEditor = e;
    emit newPreviewEditorCreated(e);
    return e;
}

void GitDiffViewController::updatePreviewTitle(const QString &relPath, bool stagedSide)
{
    if (!m_previewEditor) return;
    const QString name = stagedSide
        ? tr("[Diff: %1 (staged)]").arg(relPath)
        : tr("[Diff: %1]").arg(relPath);
    m_previewEditor->setName(name);
}

void GitDiffViewController::onPatchReady(const QString &relPath, bool stagedSide, const QByteArray &patch,
                                         const std::function<QByteArray()> &produce)
{
    if (!isCurrent(relPath, stagedSide) || !m_view) return; // stale
    DiffView::Source source;
    source.patch = patch;
    source.producePatch = produce;
    m_view->show(std::move(source), m_keepPosition);
}

void GitDiffViewController::onDocumentReady(const QString &relPath, bool stagedSide,
                                            const std::shared_ptr<const DiffDoc::Document> &doc)
{
    if (!isCurrent(relPath, stagedSide) || !m_view) return;
    m_view->showContent(DiffView::Content{doc, nullptr}, m_keepPosition);
}

void GitDiffViewController::onFetchFailed(const QString &relPath, bool stagedSide, const QString &message)
{
    if (!isCurrent(relPath, stagedSide)) return;
    emit diffFailed(relPath, message);
}
