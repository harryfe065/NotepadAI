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

#ifndef MARKDOWN_VIEW_H
#define MARKDOWN_VIEW_H

#include <QFont>
#include <QSet>
#include <QString>
#include <QTextBrowser>
#include <QVector>

class QPainter;
class QTextDocument;
class QToolButton;
class QUrl;

// The one markdown renderer shared by the chat transcript (AcpMessageWidget) and
// the file Markdown preview. It owns the whole pipeline:
//
//   source -> MarkdownPreprocessor (math, allowlisted HTML, <details>, fences)
//          -> QTextDocument::setMarkdown (NoHTML) -> toHtml -> setHtml
//          -> math images, code skinning, alignment, spacing, copy regions
//
// plus everything that has to live with the rendered document: the painted code
// surfaces, the hover "copy code" button, <details> toggling, and safe link
// routing (http/https/mailto only). Owners decide chrome (scrollbars, frame,
// height fitting) and may cap the content column with setMaxContentWidth().
class MarkdownView : public QTextBrowser
{
    Q_OBJECT

public:
    explicit MarkdownView(QWidget *parent = nullptr);

    // The typeface for prose. Code uses the chat font when it is fixed-pitch, else
    // a programming font (see codeFontFamily in the .cpp). `rerender` = false lets
    // an owner that re-renders itself right after avoid a double render.
    void setChatFont(const QFont &font, bool rerender = true);
    QFont chatFont() const { return m_chatFont; }
    bool hasChatFont() const { return m_chatFontSet; }

    // Replace the markdown source and render it now (owners debounce).
    void setMarkdownSource(const QString &text);
    const QString &markdownSource() const { return m_source; }

    // Re-render the current source (palette / font / DPI changed).
    void rerender();

    // Cap the text column to `px` device-independent pixels, centred in the view
    // (0 = no cap). Implemented with viewport margins, so scrollbars stay at the
    // window edge while the text never gets wider than a comfortable measure.
    void setMaxContentWidth(int px);
    int maxContentWidth() const { return m_maxContentWidth; }

    // Document mode (the file preview): separate paragraphs, headings and lists
    // with modest gaps. Off by default — chat bubbles keep the flat layout, where
    // every block's top/bottom margin is zero.
    void setDocumentSpacing(bool on);
    bool documentSpacing() const { return m_documentSpacing; }

    // Zero the implicit paragraph leading of every block (code surfaces and
    // display math keep their own margins). Also used for plain-text bubbles.
    static void normalizeBlockMargins(QTextDocument *doc);

signals:
    // Emitted after every render, including the immediate one a <details> click
    // triggers, so owners that fit their height to the document can re-fit.
    void rendered();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct CodeRegion { int start; int end; QString text; };

    void onAnchorClicked(const QUrl &url);
    void applyViewportMargins();

    // code affordances
    void ensureCopyButton();
    void rebuildCopyIcon();
    void scanCodeRegions();
    void paintCodeSurfaces(QPainter &p);
    void updateCopyButtonForPos(const QPoint &viewportPos);
    QString codeStyleSheet() const;
    void styleCodeInDocument();

    QString m_source;
    QFont m_chatFont;
    bool m_chatFontSet = false;
    int m_maxContentWidth = 0;
    bool m_documentSpacing = false;
    int m_appliedSide = -1;          // viewport side margin currently in effect
    bool m_inMarginUpdate = false;   // re-entrancy guard (relayout can toggle the scrollbar)

    QToolButton *m_copyCodeBtn = nullptr;
    QVector<CodeRegion> m_codeRegions;
    int m_hoverCodeIndex = -1;

    // <details> ordinals whose open state is flipped from the markup default;
    // kept across re-renders so streamed chunks do not collapse an opened section.
    QSet<int> m_detailsFlipped;
};

#endif // MARKDOWN_VIEW_H
