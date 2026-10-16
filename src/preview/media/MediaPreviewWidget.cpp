#include "MediaPreviewWidget.h"

#include "MediaPluginLoader.h"

#include <QHideEvent>
#include <QShowEvent>

// The plugin's window onto the asset chrome. Nested, so it reaches the
// protected AssetPreviewWidget API through the owning widget.
class MediaPreviewWidget::Host final : public MediaPlugin::Host
{
public:
    explicit Host(MediaPreviewWidget *owner) : m_owner(owner) {}

    void addToolWidget(QWidget *widget) override { m_owner->addToolWidget(widget); }
    QToolButton *addToolButton(const QString &text, const QString &toolTip, bool checkable) override
    {
        return m_owner->addToolButton(text, toolTip, checkable);
    }
    void addToolGap() override { m_owner->addToolGap(); }
    void setInfoText(const QString &text) override { m_owner->setInfoText(text); }
    void setInfoToolTip(const QString &text) override { m_owner->setInfoToolTip(text); }
    void showBanner(Banner kind, const QString &text, const QString &actionText,
                    std::function<void()> action) override
    {
        const BannerKind k = kind == Banner::Error ? BannerKind::Error
                             : kind == Banner::Warning ? BannerKind::Warning : BannerKind::Neutral;
        m_owner->showBanner(k, text, actionText, std::move(action));
    }
    void hideBanner() override { m_owner->hideBanner(); }
    void openAsText() override { m_owner->requestOpenAsText(); }

private:
    MediaPreviewWidget *m_owner;
};

MediaPreviewWidget::MediaPreviewWidget(const QString &typeId, const QString &displayName, MediaPlugin::Kind kind,
                                       QWidget *parent)
    : AssetPreviewWidget(typeId, displayName, parent)
    , m_kind(kind)
    , m_host(std::make_unique<Host>(this))
{
    // Nothing loads here: a restored session may hold media tabs that are
    // never looked at. The plugin is prefetched on the first show (startLoad).
    connect(&m_pluginWatcher, &QFutureWatcher<QString>::finished, this, &MediaPreviewWidget::onPluginReady);
}

MediaPreviewWidget::~MediaPreviewWidget()
{
    // The view may still emit (a player stopping) while it is torn down; delete
    // it while the host it talks to is alive.
    delete m_view;
    m_view = nullptr;
}

void MediaPreviewWidget::startLoad(bool reload)
{
    m_pendingReload = reload;
    if (m_view) {
        m_view->load(filePath(), reload);
        return;
    }
    if (m_waitingForPlugin)
        return;
    m_waitingForPlugin = true;
    const QFuture<QString> future = MediaPluginLoader::prefetch();
    if (future.isFinished()) {
        onPluginReady();
        return;
    }
    showBanner(BannerKind::Neutral, tr("Loading media support…"));
    m_pluginWatcher.setFuture(future);
}

void MediaPreviewWidget::onPluginReady()
{
    if (!m_waitingForPlugin)
        return;
    m_waitingForPlugin = false;
    QString error;
    MediaPlugin::Factory *factory = MediaPluginLoader::factory(&error);
    if (factory)
        m_view = factory->create(m_kind, m_host.get(), this);
    if (!m_view) {
        if (error.isEmpty())
            error = tr("This build has no %1 viewer.").arg(displayName());
        showBanner(BannerKind::Error, tr("Media support could not be loaded: %1").arg(error), tr("Open as text"),
                   [this]() { requestOpenAsText(); });
        return;
    }
    AssetPreviewWidget::hideBanner();
    setContentWidget(m_view->widget());
    m_view->themeChanged(palette(), isDarkTheme());
    m_view->setTabVisible(isVisible());
    m_view->load(filePath(), m_pendingReload);
}

void MediaPreviewWidget::themeChanged(bool isDark)
{
    if (m_view)
        m_view->themeChanged(palette(), isDark);
}

void MediaPreviewWidget::showEvent(QShowEvent *event)
{
    AssetPreviewWidget::showEvent(event);
    if (m_view)
        m_view->setTabVisible(true);
}

void MediaPreviewWidget::hideEvent(QHideEvent *event)
{
    AssetPreviewWidget::hideEvent(event);
    if (m_view)
        m_view->setTabVisible(false);
}
