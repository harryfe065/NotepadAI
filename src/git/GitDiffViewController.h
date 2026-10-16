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

#ifndef GIT_DIFF_VIEW_CONTROLLER_H
#define GIT_DIFF_VIEW_CONTROLLER_H

#include "DiffDocument.h"
#include "GitStatusEntry.h"

#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>
#include <memory>

class DiffView;
class EditorManager;
class GitController;
class GitDiffFetcher;
class ScintillaNext;

// Glue between the git tab single-click and the editor tab area. Owns ONE
// preview tab at a time — clicking a different file replaces its content
// rather than spawning a new tab. Double-click on the same file (handled in
// the host MainWindow) opens the real file in a separate, persistent tab.
// A DiffView presents the tab: built on the pool, applied on the UI thread.
class GitDiffViewController : public QObject
{
    Q_OBJECT
public:
    GitDiffViewController(GitController *controller,
                          EditorManager *editorManager,
                          QObject *parent = nullptr);

    // Returns the preview editor (creating it if necessary). The host
    // (MainWindow) is responsible for inserting newly-created editors into
    // the docked area via newPreviewEditorCreated.
    ScintillaNext *previewEditor() const;

    void setDarkPalette(bool dark);

public slots:
    void showDiffFor(const GitStatusEntry &entry);

signals:
    // Fired when a new preview editor needs to be docked. MainWindow connects
    // this to its addEditor / switchToEditor path.
    void newPreviewEditorCreated(ScintillaNext *editor);
    // Fired when the preview content has been rendered (used to switch to it).
    void diffRendered(ScintillaNext *editor);
    // Fired when fetching fails — host shows a banner / status message.
    void diffFailed(const QString &relPath, const QString &message);
    // Jump to source / path click: the working-tree file at `line` (1-based).
    void openFileRequested(const QString &relPath, int line);
    void revealRequested(const QString &relPath);

private slots:
    void onPatchReady(const QString &relPath, bool stagedSide, const QByteArray &patch,
                      const std::function<QByteArray()> &produce);
    void onDocumentReady(const QString &relPath, bool stagedSide,
                         const std::shared_ptr<const DiffDoc::Document> &doc);
    void onFetchFailed(const QString &relPath, bool stagedSide, const QString &message);

private:
    GitController *m_controller;
    EditorManager *m_editorManager;
    GitDiffFetcher *m_fetcher;
    QPointer<ScintillaNext> m_previewEditor;
    QPointer<DiffView> m_view;
    bool m_isDark = false;

    GitStatusEntry m_current;
    bool m_keepPosition = false;      // re-showing the same file keeps the scroll

    ScintillaNext *ensurePreviewEditor();
    void updatePreviewTitle(const QString &relPath, bool stagedSide);
    bool isCurrent(const QString &relPath, bool stagedSide) const;
};

#endif // GIT_DIFF_VIEW_CONTROLLER_H
