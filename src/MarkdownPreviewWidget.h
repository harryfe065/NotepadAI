#ifndef MARKDOWNPREVIEWWIDGET_H
#define MARKDOWNPREVIEWWIDGET_H

#include "PreviewContentWidget.h"

#include <QString>

class MarkdownView;
class NotepadNextApplication;

// The file Markdown preview. It renders with the same engine as the chat
// transcript (MarkdownView: math, allowlisted HTML, <details>, code surfaces,
// copy button, safe links) in the same typeface, and caps the text column so
// long lines stay readable on wide windows.
class MarkdownPreviewWidget : public PreviewContentWidget
{
    Q_OBJECT

public:
    // Text column cap, in device-independent pixels (about 100 characters of body
    // text); wider windows get centred margins instead of ever-longer lines.
    static constexpr int kMaxContentWidth = 860;

    // Previews above this many characters are cut (a notice is appended) so a huge
    // file cannot stall the UI thread: rendering is synchronous, like chat's.
    static constexpr int kMaxSourceChars = 512 * 1024;

    explicit MarkdownPreviewWidget(NotepadNextApplication *app, QWidget *parent = nullptr);

    QString typeId() const override { return QStringLiteral("markdown"); }
    QString displayName() const override { return m_title; }
    void setContent(const QString &text, const QString &basePath) override;
    void refresh(const QString &text) override;
    void applyTheme(const QPalette &palette, bool isDark) override;

    // Approximate scroll sync with the source editor: the same proportional
    // mapping the editor's scroll position implies (line / total lines).
    void scrollToLine(int line);

    // Exposed for tests and for the owner's chrome.
    MarkdownView *view() const { return m_view; }

private:
    void render(const QString &text);
    void applyFont();

    NotepadNextApplication *m_app;
    MarkdownView *m_view;
    QString m_title;
    QString m_basePath;
    int m_sourceLines = 1;
};

#endif // MARKDOWNPREVIEWWIDGET_H
