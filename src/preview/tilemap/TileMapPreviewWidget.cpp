#include "TileMapPreviewWidget.h"

#include "ImageDecoder.h"
#include "TileMapCanvas.h"

#include <QAction>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QSet>
#include <QToolButton>
#include <QtConcurrent>

#include <cmath>

namespace {

constexpr int kMaxWarningsShown = 3;

QString zoomText(double zoom)
{
    const double pct = zoom * 100.0;
    if (pct >= 10.0 || std::abs(pct - std::round(pct)) < 0.05)
        return QStringLiteral("%1%").arg(qRound(pct));
    return QStringLiteral("%1%").arg(pct, 0, 'f', 1);
}

} // namespace

TileMapPreviewWidget::TileMapPreviewWidget(const QString &typeId, const QString &displayName, QWidget *parent)
    : AssetPreviewWidget(typeId, displayName, parent)
    , m_cancel(std::make_shared<std::atomic_bool>(false))
{
    m_canvas = new TileMapCanvas(this);
    setContentWidget(m_canvas);

    m_zoomOut = addToolButton(QStringLiteral("−"), tr("Zoom out (-)"));
    m_zoomLabel = new QLabel(QStringLiteral("100%"));
    m_zoomLabel->setMinimumWidth(m_zoomLabel->fontMetrics().horizontalAdvance(QStringLiteral("10000%")));
    m_zoomLabel->setAlignment(Qt::AlignCenter);
    addToolWidget(m_zoomLabel);
    m_zoomIn = addToolButton(QStringLiteral("+"), tr("Zoom in (+)"));
    m_fit = addToolButton(tr("Fit"), tr("Fit to the tab (0)"), true);
    m_actual = addToolButton(QStringLiteral("1:1"), tr("Actual size (1)"));
    addToolGap();

    m_layers = addToolButton(tr("Layers"), tr("Show or hide layers"));
    m_layerMenu = new QMenu(m_layers);
    m_layers->setMenu(m_layerMenu);
    m_layers->setPopupMode(QToolButton::InstantPopup);
    m_grid = addToolButton(tr("Grid"), tr("Tile grid"), true);
    m_grid->setChecked(true);
    m_objects = addToolButton(tr("Objects"), tr("Object and entity shapes"), true);
    m_objects->setChecked(true);
    m_animate = addToolButton(tr("Animate"), tr("Play animated tiles"), true);
    m_animate->setChecked(true);
    m_objects->hide();
    m_animate->hide();

    connect(m_zoomOut, &QToolButton::clicked, m_canvas, &TileMapCanvas::zoomOut);
    connect(m_zoomIn, &QToolButton::clicked, m_canvas, &TileMapCanvas::zoomIn);
    connect(m_fit, &QToolButton::clicked, m_canvas, &TileMapCanvas::zoomToFit);
    connect(m_actual, &QToolButton::clicked, m_canvas, &TileMapCanvas::zoomToActual);
    connect(m_canvas, &TileMapCanvas::zoomChanged, this, &TileMapPreviewWidget::updateZoomLabel);
    connect(m_grid, &QToolButton::toggled, m_canvas, &TileMapCanvas::setGridVisible);
    connect(m_objects, &QToolButton::toggled, m_canvas, &TileMapCanvas::setObjectsVisible);
    connect(m_animate, &QToolButton::toggled, m_canvas, &TileMapCanvas::setAnimating);
    connect(m_canvas, &TileMapCanvas::hoverTextChanged, this, [this](const QString &text) {
        m_hoverText = text;
        updateInfo();
    });
    connect(&m_watcher, &QFutureWatcher<TileMapLoader::Result>::finished, this, &TileMapPreviewWidget::onLoaded);

    m_canvas->setAnimating(true);
}

TileMapPreviewWidget::~TileMapPreviewWidget()
{
    m_cancel->store(true);
}

void TileMapPreviewWidget::cancelLoad()
{
    m_cancel->store(true);
    m_cancel = std::make_shared<std::atomic_bool>(false);
}

void TileMapPreviewWidget::startLoad(bool reload)
{
    cancelLoad();
    m_pendingReload = reload;
    const QString path = filePath();
    const QFileInfo info(path);
    if (!info.exists()) {
        showBanner(BannerKind::Error, tr("The file no longer exists."));
        return;
    }
    if (info.size() > TileMapLoader::kMaxFileBytes) {
        showBanner(BannerKind::Error, tr("Maps over %1 are not previewed.")
                                          .arg(ImageDecoder::formatBytes(TileMapLoader::kMaxFileBytes)));
        return;
    }
    if (info.size() > TileMapLoader::kProgressBytes) {
        showBanner(BannerKind::Neutral, tr("Loading %1 map…").arg(ImageDecoder::formatBytes(info.size())),
                   tr("Cancel"), [this]() {
                       cancelLoad();
                       showBanner(BannerKind::Neutral, tr("Loading cancelled."), tr("Retry"),
                                  [this]() { startLoad(m_pendingReload); });
                   });
    }

    const std::shared_ptr<std::atomic_bool> cancel = m_cancel;
    m_watcher.setFuture(QtConcurrent::run([path, cancel]() { return TileMapLoader::load(path, *cancel); }));
}

void TileMapPreviewWidget::onLoaded()
{
    const TileMapLoader::Result result = m_watcher.result();
    if (result.cancelled)
        return;
    if (!result.doc) {
        if (m_canvas->document() && m_pendingReload)
            showBanner(BannerKind::Warning, tr("Could not reload the map: %1").arg(result.error));
        else
            showBanner(BannerKind::Error, tr("Could not open the map: %1").arg(result.error));
        return;
    }

    m_canvas->setDocument(result.doc, m_pendingReload);
    if (!result.doc->warnings.isEmpty()) {
        QStringList shown = result.doc->warnings.mid(0, kMaxWarningsShown);
        if (result.doc->warnings.size() > kMaxWarningsShown)
            shown << tr("and %n more", nullptr, int(result.doc->warnings.size() - kMaxWarningsShown));
        showBanner(BannerKind::Warning, shown.join(QStringLiteral("\n")));
    } else {
        hideBanner();
    }

    m_objects->setVisible(result.doc->hasObjects);
    m_animate->setVisible(result.doc->hasAnimations);
    m_grid->setVisible(result.doc->gridW > 0);
    rebuildLayerMenu();
    updateZoomLabel(m_canvas->zoom(), m_canvas->isFit());
    updateInfo();
}

void TileMapPreviewWidget::rebuildLayerMenu()
{
    m_layerMenu->clear();
    const TileMap::DocumentPtr doc = m_canvas->document();
    if (!doc) {
        m_layers->setEnabled(false);
        return;
    }
    QSet<QString> seen;
    // Top layer first, like the editors list them.
    for (size_t i = doc->layers.size(); i-- > 0;) {
        const TileMap::Layer &layer = doc->layers[i];
        if (seen.contains(layer.group))
            continue;
        seen.insert(layer.group);
        QAction *action = m_layerMenu->addAction(layer.name.isEmpty() ? tr("(unnamed)") : layer.name);
        action->setCheckable(true);
        action->setChecked(m_canvas->groupVisible(layer.group));
        const QString group = layer.group;
        connect(action, &QAction::toggled, this, [this, group](bool on) { m_canvas->setGroupVisible(group, on); });
    }
    m_layers->setEnabled(!seen.isEmpty());
    m_layers->setToolTip(tr("%n layer(s)", nullptr, int(seen.size())));
}

void TileMapPreviewWidget::updateZoomLabel(double zoom, bool fit)
{
    m_zoomLabel->setText(zoomText(zoom));
    m_fit->setChecked(fit);
}

void TileMapPreviewWidget::updateInfo()
{
    const TileMap::DocumentPtr doc = m_canvas->document();
    if (!doc) {
        setInfoText(QString());
        return;
    }
    QStringList parts;
    parts << doc->format << doc->detail;
    if (doc->layers.size() > 1 || !doc->tilesetSheet)
        parts << tr("%n layer(s)", nullptr, int(doc->layers.size()));
    if (!doc->tilesets.empty())
        parts << tr("%n tileset(s)", nullptr, int(doc->tilesets.size()));
    if (doc->tileCount > 0)
        parts << tr("%L1 tiles").arg(doc->tileCount);
    parts << ImageDecoder::formatBytes(doc->fileSize);
    QString text = parts.join(QStringLiteral("  ·  "));
    if (!m_hoverText.isEmpty())
        text += QStringLiteral("        ") + m_hoverText;
    setInfoText(text);
}

void TileMapPreviewWidget::themeChanged(bool isDark)
{
    Q_UNUSED(isDark);
    const QColor text = palette().color(QPalette::Text);
    QColor grid = text;
    grid.setAlpha(40);
    m_canvas->setColors(palette().color(QPalette::Base), grid, palette().color(QPalette::Highlight));
}
