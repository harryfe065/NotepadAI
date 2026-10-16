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

#ifndef DIFF_VIEW_H
#define DIFF_VIEW_H

#include <cstdint>
#include "DiffDocument.h"
#include "DiffFileTreeModel.h"
#include "GitDiffPalette.h"

#include <QFont>
#include <QMargins>
#include <QObject>
#include <QPointer>

#include <functional>
#include <memory>

class DiffStickyHeader;
class GitStatusItemDelegate;
class QLineEdit;
class QSortFilterProxyModel;
class QTimer;
class QToolButton;
class QTreeView;
class QWidget;
class ScintillaNext;

namespace DiffLanguages { class Catalog; }

// Presents a diff document in a diff editor (diff preview and commit view).
// Owns the editor's diff styles, markers, indicators, line-number margin,
// sticky file header, toolbar and (commit view) changed-files tree, which sit
// in the editor's viewport margins so the tab stays a plain editor tab.
//
// Everything heavy (parse, classify, lex, build, tree) runs on the pool; the
// UI thread applies the finished document with one append and one style
// flush. Latest wins: a newer show() drops older results.
class DiffView : public QObject
{
    Q_OBJECT
public:
    enum class Mode : std::uint8_t { Preview, Commit };

    // Where a file should be opened.
    enum class Side : std::uint8_t {
        Working,          // the working-tree file
        Revision,         // the file at the shown commit
        ParentRevision,   // the file at the commit's first parent (deleted files)
    };

    // What to show. The patch is parsed on the pool unless `parsed` is set;
    // `producePatch`, when set, runs on the pool to produce it.
    struct Source {
        QByteArray patch;
        std::function<QByteArray()> producePatch;
        std::shared_ptr<const GitDiffParser::Result> parsed;
        std::shared_ptr<const GitCommitDetail> commit;
        QVector<bool> linguistGenerated;   // per file
        QByteArray issueUrlBase;
    };

    struct Content {
        std::shared_ptr<const DiffDoc::Document> doc;
        std::shared_ptr<const DiffFileTreeModel::Tree> tree;
    };

    DiffView(ScintillaNext *editor, Mode mode);
    ~DiffView() override;

    static DiffView *of(const ScintillaNext *editor);

    ScintillaNext *editor() const;
    Mode mode() const { return m_mode; }

    void setRepoRoot(const QString &repoRoot) { m_repoRoot = repoRoot; }
    void setDark(bool dark);

    // Builds `source` on the pool and applies it. `keepPosition` keeps the
    // scroll position and caret (refresh of the same target).
    void show(Source source, bool keepPosition = false);
    // Applies a finished content now (cache hit), dropping builds in flight.
    void showContent(Content content, bool keepPosition = false);
    // Drops builds in flight; the shown document stays.
    void cancel();

    // The pool part of show(): parse, classify, languages, build, tree.
    static Content build(Source source, Mode mode, const std::shared_ptr<const DiffLanguages::Catalog> &catalog,
                         qint64 nowSecs);

    const DiffDoc::Document *document() const { return m_doc.get(); }
    int fileAtTop() const { return m_fileAtTop; }

    void nextHunk(int direction);
    void nextFile(int direction);
    void revealFile(int file);
    void setFileCollapsed(int file, bool collapsed);
    void setAllCollapsed(bool collapsed);
    void jumpToSource();

signals:
    void contentApplied(const DiffView::Content &content);
    void openFileRequested(const QString &relPath, int line, DiffView::Side side);
    void openCommitRequested(const QByteArray &sha);
    void revealRequested(const QString &relPath);
    void ignoreWhitespaceChanged(bool ignore);
    void fileAtTopChanged(int file);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct LineInfo {
        qint32 oldLn = -1;
        qint32 newLn = -1;
        DiffDoc::RowKind kind = DiffDoc::RowKind::Blank;
        bool filled = false;          // margin text set for this line
        bool decorated = true;        // markers and word indicators set
    };
    struct FileState {
        qint32 bodyRows = 0;
        bool collapsed = false;
        bool loading = false;
        qint32 chevronLink = -1;
        // The rows under the header now: null = the document's inline body.
        std::shared_ptr<const DiffDoc::Rows> current;
        std::shared_ptr<const DiffDoc::Rows> body;          // built fragment, kept
        std::shared_ptr<const DiffDoc::Rows> placeholder;
    };
    struct LinkRef {
        DiffDoc::LinkKind kind = DiffDoc::LinkKind::Url;
        qint32 file = -1;
        QByteArray payload;
    };

    qintptr send(unsigned int msg, quintptr w = 0, qintptr l = 0) const;

    void configureEditor();
    void applyStyles();
    void refreshMetrics();
    void apply(Content content, bool keepPosition);

    void rebuildTables();
    void stampMarkers(qint32 firstLine, qint32 count, bool clearFirst);
    void paintRows(const DiffDoc::Rows &rows, qintptr base, bool clearFirst, bool words = true);
    // Markers and word indicators of a new document: the visible window at
    // once, the rest in slices (decorateSlice). Splices flush it first.
    void startDecoration();
    void decorateLines(qint32 from, qint32 to);
    void decorateSlice();
    void flushDecoration();
    void visibleWindow(qint32 &from, qint32 &to) const;
    void setLinkIndicator(int indicator, qintptr pos, qintptr len, int link);
    void markAllStyled();
    void finishEdit();

    void updateMarginWidth();
    void fillMargins();
    int formatMargin(const LineInfo &line, char *out) const;

    void replaceBody(int file, std::shared_ptr<const DiffDoc::Rows> rows, bool collapsed);
    void replaceBodies(const QVector<std::pair<int, std::shared_ptr<const DiffDoc::Rows>>> &bodies, bool collapsed);
    void swapChevron(int file, bool collapsed);
    std::shared_ptr<const DiffDoc::Rows> placeholderOf(int file);
    void startBodyJob(const QVector<int> &files);
    void expandFile(int file);
    void collapseFile(int file);
    void toggleFile(int file);

    int fileOfLine(qint32 line) const;
    qint32 topDocLine() const;
    void scrollLineToTop(qint32 line);
    qint32 navLine() const;
    void moveTo(qint32 line, bool header);

    void onUpdateUi();
    void updateSticky();
    void activateLink(int link);
    int linkAt(qintptr pos) const;
    void showHeaderMenu(int file, const QPoint &globalPos);
    void openFile(int file, int line, Side side);
    Side revisionSide(int file) const;
    QString relPathOf(int file) const;
    QString absolutePathOf(int file) const;

    void buildChrome();
    void layoutChrome();
    void setTreeShown(bool shown);
    bool treeAvailable() const;
    void syncTreeSelection();
    void retintChrome();
    void focusFilter();

    QPointer<ScintillaNext> m_editor;
    Mode m_mode;
    QString m_repoRoot;
    bool m_dark = false;
    DiffViewColors m_colors;
    quint64 m_generation = 0;     // show() requests; latest wins
    quint64 m_docSerial = 0;      // applied documents; guards body jobs and queued clicks

    std::shared_ptr<const DiffDoc::Document> m_doc;
    std::shared_ptr<const DiffFileTreeModel::Tree> m_tree;
    QVector<LineInfo> m_lines;
    QVector<qint32> m_headerLines;
    QVector<qint32> m_hunkLines;
    QVector<FileState> m_files;
    QVector<LinkRef> m_links;
    int m_pressedLink = -1;
    QTimer *m_decorateTimer = nullptr;
    qint32 m_decorateNext = -1;   // next line the pump decorates, -1 = done
    int m_fileAtTop = -1;
    int m_stickyFile = -1;
    int m_oldDigits = 0;
    int m_newDigits = 0;
    QFont m_font;                 // editor font at the current zoom
    int m_lineHeight = 16;
    int m_textBase = 0;           // x of the text column before horizontal scroll
    QMargins m_chromeMargins;

    int m_indAddWord = -1;
    int m_indDelWord = -1;
    int m_indStrike = -1;
    int m_indLink = -1;
    int m_indChevron = -1;

    DiffStickyHeader *m_sticky = nullptr;
    QWidget *m_toolbar = nullptr;
    QToolButton *m_treeButton = nullptr;
    QToolButton *m_expandAll = nullptr;
    QToolButton *m_collapseAll = nullptr;
    QToolButton *m_whitespace = nullptr;
    QToolButton *m_wrap = nullptr;
    QToolButton *m_prevHunk = nullptr;
    QToolButton *m_nextHunk = nullptr;
    QWidget *m_treeHost = nullptr;
    QWidget *m_treeHandle = nullptr;
    QLineEdit *m_filter = nullptr;
    QTreeView *m_treeView = nullptr;
    DiffFileTreeModel *m_treeModel = nullptr;
    QSortFilterProxyModel *m_treeProxy = nullptr;
    GitStatusItemDelegate *m_treeDelegate = nullptr;
    int m_treeWidth = 256;
    bool m_treeVisible = true;
    int m_dragStartWidth = 0;
};

Q_DECLARE_METATYPE(DiffView::Side)

#endif // DIFF_VIEW_H
