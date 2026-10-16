#include "AssetPreviewWidget.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

AssetPreviewWidget::AssetPreviewWidget(const QString &typeId, const QString &displayName, QWidget *parent)
    : PreviewContentWidget(parent)
    , m_typeId(typeId)
    , m_displayName(displayName)
{
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(0, 0, 0, 0);
    m_rootLayout->setSpacing(0);

    m_toolbar = new QFrame(this);
    m_toolbar->setObjectName(QStringLiteral("assetPreviewToolbar"));
    m_toolbarLayout = new QHBoxLayout(m_toolbar);
    m_toolbarLayout->setContentsMargins(8, 4, 8, 4);
    m_toolbarLayout->setSpacing(4);
    m_rootLayout->addWidget(m_toolbar);

    m_banner = new QFrame(this);
    m_banner->setObjectName(QStringLiteral("assetPreviewBanner"));
    auto *bannerLayout = new QHBoxLayout(m_banner);
    bannerLayout->setContentsMargins(8, 6, 8, 6);
    bannerLayout->setSpacing(6);
    m_bannerText = new QLabel(m_banner);
    m_bannerText->setWordWrap(true);
    m_bannerText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bannerLayout->addWidget(m_bannerText, 1);
    m_bannerAction = new QPushButton(m_banner);
    m_bannerAction->hide();
    bannerLayout->addWidget(m_bannerAction);
    connect(m_bannerAction, &QPushButton::clicked, this, [this]() {
        // Copy first: the callback may replace the banner (and its callback).
        const std::function<void()> cb = m_bannerCallback;
        if (cb) cb();
    });
    m_banner->hide();
    m_rootLayout->addWidget(m_banner);

    m_info = new QLabel(this);
    m_info->setObjectName(QStringLiteral("assetPreviewInfo"));
    m_info->setContentsMargins(8, 4, 8, 4);
    m_info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_info->setTextFormat(Qt::PlainText);
    // The info line is the only place that changes on mouse move; a fixed size
    // policy keeps those updates from relaying out the whole widget.
    m_info->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_rootLayout->addWidget(m_info);

    // Subclass controls go before this stretch; "Open as text" stays right.
    m_toolbarLayout->addStretch(1);
    m_openAsText = new QToolButton(m_toolbar);
    m_openAsText->setText(tr("Open as text"));
    m_openAsText->setToolTip(tr("Open the file's bytes in a text editor tab"));
    m_openAsText->setAutoRaise(true);
    m_toolbarLayout->addWidget(m_openAsText);
    connect(m_openAsText, &QToolButton::clicked, this, [this]() {
        if (!m_path.isEmpty())
            emit openAsTextRequested(m_path);
    });

    applyTheme(palette(), false);
}

void AssetPreviewWidget::applyTheme(const QPalette &pal, bool isDark)
{
    m_isDark = isDark;
    const QColor muted = pal.color(QPalette::PlaceholderText);
    QPalette infoPal = m_info->palette();
    infoPal.setColor(QPalette::WindowText, muted);
    m_info->setPalette(infoPal);

    if (m_banner->isVisible())
        showBanner(m_bannerKind, m_bannerText->text(), m_bannerAction->text(), m_bannerCallback);

    themeChanged(isDark);
}

void AssetPreviewWidget::loadFromFile(const QString &path)
{
    m_path = path;
    m_loadPending = true;
    if (isVisible())
        runPendingLoad();
}

void AssetPreviewWidget::reloadFromDisk()
{
    if (m_path.isEmpty())
        return;
    m_loadPending = true;
    if (isVisible())
        runPendingLoad();
}

void AssetPreviewWidget::showEvent(QShowEvent *event)
{
    PreviewContentWidget::showEvent(event);
    if (m_loadPending) {
        // Queued: the first show of a tab happens inside ADS's tab switch; let
        // it finish painting the tab strip before the subclass starts work.
        QTimer::singleShot(0, this, [this]() {
            if (isVisible())
                runPendingLoad();
        });
    }
}

void AssetPreviewWidget::runPendingLoad()
{
    if (!m_loadPending)
        return;
    m_loadPending = false;
    const bool reload = m_loadedOnce;
    m_loadedOnce = true;
    startLoad(reload);
}

void AssetPreviewWidget::setContentWidget(QWidget *content)
{
    if (m_content) {
        m_rootLayout->removeWidget(m_content);
        m_content->deleteLater();
    }
    m_content = content;
    // Content sits between the banner and the info line.
    m_rootLayout->insertWidget(2, content, 1);
}

void AssetPreviewWidget::setInfoText(const QString &text)
{
    if (m_info->text() != text)
        m_info->setText(text);
}

void AssetPreviewWidget::showBanner(BannerKind kind, const QString &text,
                                    const QString &actionText, std::function<void()> action)
{
    m_bannerKind = kind;
    m_bannerText->setText(text);
    m_bannerCallback = std::move(action);
    m_bannerAction->setText(actionText);
    m_bannerAction->setVisible(!actionText.isEmpty() && m_bannerCallback);

    // ui-dna: neutral by default; the soft warning/error tints only for
    // warnings and errors. Those tints are light, so their text stays dark in
    // both themes.
    QString style;
    switch (kind) {
    case BannerKind::Neutral: {
        const QColor bg = palette().color(QPalette::AlternateBase);
        const QColor border = palette().color(QPalette::Mid);
        style = QStringLiteral("QFrame#assetPreviewBanner{background:%1;border-bottom:1px solid %2;}")
                    .arg(bg.name(), border.name());
        break;
    }
    case BannerKind::Warning:
        style = QStringLiteral("QFrame#assetPreviewBanner{background:#fff3cd;border-bottom:1px solid #ffeeba;}"
                               "QFrame#assetPreviewBanner QLabel{color:#333;}");
        break;
    case BannerKind::Error:
        style = QStringLiteral("QFrame#assetPreviewBanner{background:#f8d7da;border-bottom:1px solid #f5c6cb;}"
                               "QFrame#assetPreviewBanner QLabel{color:#333;}");
        break;
    }
    m_banner->setStyleSheet(style);
    m_banner->show();
}

void AssetPreviewWidget::hideBanner()
{
    m_banner->hide();
    m_bannerCallback = {};
}

QToolButton *AssetPreviewWidget::addToolButton(const QString &text, const QString &toolTip, bool checkable)
{
    auto *button = new QToolButton(m_toolbar);
    button->setText(text);
    button->setToolTip(toolTip);
    button->setAutoRaise(true);
    button->setCheckable(checkable);
    addToolWidget(button);
    return button;
}

void AssetPreviewWidget::addToolWidget(QWidget *widget)
{
    widget->setParent(m_toolbar);
    // Insert before the stretch that keeps "Open as text" on the right.
    m_toolbarLayout->insertWidget(m_toolbarLayout->count() - 2, widget);
}

void AssetPreviewWidget::addToolGap()
{
    m_toolbarLayout->insertSpacing(m_toolbarLayout->count() - 2, 8);
}
