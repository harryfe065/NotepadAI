#include "MarkdownPreviewWidget.h"

#include "ApplicationSettings.h"
#include "MarkdownPreviewSupport.h"
#include "NotepadNextApplication.h"
#include "widgets/ChatFont.h"
#include "widgets/MarkdownView.h"

#include <QFrame>
#include <QScrollBar>
#include <QTextDocument>
#include <QVBoxLayout>

MarkdownPreviewWidget::MarkdownPreviewWidget(NotepadNextApplication *app, QWidget *parent)
    : PreviewContentWidget(parent)
    , m_app(app)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_view = new MarkdownView(this);
    m_view->setFrameShape(QFrame::NoFrame);
    m_view->setReadOnly(true);
    // Breathing room inside the capped column (top/bottom of the page, and a side
    // inset so text never touches the column edge).
    m_view->document()->setDocumentMargin(24);
    m_view->setMaxContentWidth(kMaxContentWidth);
    m_view->setDocumentSpacing(true); // a document, not a chat bubble: separate the blocks
    m_view->setPalette(app->palette());
    layout->addWidget(m_view);

    applyFont();

    // The preview follows the chat typeface, so follow the same settings live.
    if (auto *settings = app->getSettings()) {
        auto follow = [this](auto signal, ApplicationSettings *s) {
            connect(s, signal, this, [this]() { applyFont(); });
        };
        follow(&ApplicationSettings::fontNameChanged, settings);
        follow(&ApplicationSettings::fontSizeChanged, settings);
        follow(&ApplicationSettings::fontHintingChanged, settings);
        follow(&ApplicationSettings::chatFontUseDefaultChanged, settings);
        follow(&ApplicationSettings::chatFontFamilyChanged, settings);
        follow(&ApplicationSettings::chatFontSizePtChanged, settings);
        follow(&ApplicationSettings::chatFontSharpenChanged, settings);
    }
}

void MarkdownPreviewWidget::applyFont()
{
    m_view->setChatFont(chatFontFromSettings(m_app->getSettings()));
}

void MarkdownPreviewWidget::setContent(const QString &text, const QString &basePath)
{
    m_basePath = basePath;
    if (!basePath.isEmpty())
        m_view->setSearchPaths({basePath}); // relative image paths resolve against the file
    render(text);
}

void MarkdownPreviewWidget::refresh(const QString &text)
{
    render(text);
}

void MarkdownPreviewWidget::render(const QString &text)
{
    m_sourceLines = int(text.count(QLatin1Char('\n'))) + 1;
    // Re-rendering rebuilds the document, which resets the scroll position: keep
    // the reader where they were while the file is being edited.
    QScrollBar *bar = m_view->verticalScrollBar();
    const int keep = bar->value();
    m_view->setMarkdownSource(MarkdownPreviewSupport::clampSource(text, kMaxSourceChars));
    bar->setValue(qMin(keep, bar->maximum()));
}

void MarkdownPreviewWidget::applyTheme(const QPalette &palette, bool isDark)
{
    Q_UNUSED(isDark);
    // A palette change re-renders the view (code surface, math tint, links).
    m_view->setPalette(palette);
}

void MarkdownPreviewWidget::scrollToLine(int line)
{
    QScrollBar *bar = m_view->verticalScrollBar();
    if (!bar || bar->maximum() <= 0) return;
    bar->setValue(int(MarkdownPreviewSupport::scrollFraction(line, m_sourceLines)
                      * bar->maximum()));
}
