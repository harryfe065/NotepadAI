#include "ModelPreviewWidget.h"

#include "ImageDecoder.h"
#include "ModelViewWindow.h"

#include <QComboBox>
#include <QFileInfo>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QSignalBlocker>
#include <QSlider>
#include <QToolButton>
#include <QtConcurrent>

#include <cmath>

namespace {

constexpr int kTimelineSteps = 1000;
constexpr int kMaxWarningsShown = 3;

QString seconds(float s)
{
    return QString::number(double(s), 'f', 2);
}

} // namespace

ModelPreviewWidget::ModelPreviewWidget(const QString &typeId, const QString &displayName, QWidget *parent)
    : AssetPreviewWidget(typeId, displayName, parent)
    , m_cancel(std::make_shared<std::atomic_bool>(false))
{
    m_view = new ModelViewWindow;
    m_container = QWidget::createWindowContainer(m_view, this); // takes ownership of the window
    m_container->setFocusPolicy(Qt::StrongFocus);
    m_container->setMinimumSize(64, 64);
    m_container->setToolTip(tr("Drag to orbit, Shift/middle/right-drag to pan, wheel to zoom, double-click to frame"));
    setContentWidget(m_container);

    m_shading = new QComboBox;
    m_shading->setToolTip(tr("Shading"));
    m_shading->addItem(tr("Shaded"), int(ModelRenderer::Shading::Shaded));
    m_shading->addItem(tr("Albedo"), int(ModelRenderer::Shading::Albedo));
    m_shading->addItem(tr("Normals"), int(ModelRenderer::Shading::Normals));
    m_shading->addItem(tr("UV checker"), int(ModelRenderer::Shading::UvChecker));
    addToolWidget(m_shading);
    m_wire = addToolButton(tr("Wire"), tr("Wireframe overlay (W)"), true);
    m_grid = addToolButton(tr("Grid"), tr("Ground grid (G)"), true);
    m_grid->setChecked(true);
    m_skeleton = addToolButton(tr("Skeleton"), tr("Show the joints and bones"), true);
    addToolGap();

    m_frame = addToolButton(tr("Frame"), tr("Frame the model (F or double-click)"));
    m_viewMenu = addToolButton(tr("View"), tr("View presets"));
    auto *menu = new QMenu(m_viewMenu);
    const std::pair<QString, ModelCamera::View> presets[] = {
        {tr("Front\t1"), ModelCamera::View::Front},       {tr("Back\tCtrl+1"), ModelCamera::View::Back},
        {tr("Right\t3"), ModelCamera::View::Right},       {tr("Left\tCtrl+3"), ModelCamera::View::Left},
        {tr("Top\t7"), ModelCamera::View::Top},           {tr("Bottom\tCtrl+7"), ModelCamera::View::Bottom},
        {tr("Three-quarter\t0"), ModelCamera::View::Iso}};
    for (const auto &[text, view] : presets) {
        const ModelCamera::View v = view;
        connect(menu->addAction(text), &QAction::triggered, this, [this, v]() { m_view->setView(v); });
    }
    m_viewMenu->setMenu(menu);
    m_viewMenu->setPopupMode(QToolButton::InstantPopup);
    m_ortho = addToolButton(tr("Ortho"), tr("Orthographic projection (5)"), true);
    m_zUp = addToolButton(tr("Z-up"), tr("The file is Z-up: rotate it upright"), true);
    addToolGap();

    m_animation = new QComboBox;
    m_animation->setToolTip(tr("Animation"));
    m_animation->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_animation->setMaximumWidth(240);
    addToolWidget(m_animation);
    m_play = addToolButton(tr("Play"), tr("Play or pause (Space)"));
    m_timeline = new QSlider(Qt::Horizontal);
    m_timeline->setRange(0, kTimelineSteps);
    m_timeline->setFixedWidth(160);
    m_timeline->setToolTip(tr("Animation time"));
    addToolWidget(m_timeline);
    m_timeLabel = new QLabel;
    m_timeLabel->setMinimumWidth(m_timeLabel->fontMetrics().horizontalAdvance(QStringLiteral("000.00 / 000.00 s")));
    addToolWidget(m_timeLabel);
    for (QWidget *w : {static_cast<QWidget *>(m_skeleton), static_cast<QWidget *>(m_animation),
                       static_cast<QWidget *>(m_play), static_cast<QWidget *>(m_timeline),
                       static_cast<QWidget *>(m_timeLabel)})
        w->hide();

    connect(m_shading, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        m_view->setShading(static_cast<ModelRenderer::Shading>(m_shading->currentData().toInt()));
    });
    connect(m_wire, &QToolButton::toggled, m_view, &ModelViewWindow::setWireframe);
    connect(m_grid, &QToolButton::toggled, m_view, &ModelViewWindow::setGrid);
    connect(m_skeleton, &QToolButton::toggled, m_view, &ModelViewWindow::setSkeleton);
    connect(m_frame, &QToolButton::clicked, m_view, &ModelViewWindow::frameAll);
    connect(m_ortho, &QToolButton::toggled, m_view, &ModelViewWindow::setOrthographic);
    connect(m_zUp, &QToolButton::toggled, m_view, &ModelViewWindow::setZUp);

    connect(m_animation, QOverload<int>::of(&QComboBox::activated), this, [this](int row) {
        const int index = m_animation->itemData(row).toInt();
        m_view->setAnimation(index);
        m_view->setPlaying(index >= 0);
        syncControls();
    });
    connect(m_play, &QToolButton::clicked, this, [this]() { m_view->setPlaying(!m_view->isPlaying()); });
    connect(m_timeline, &QSlider::sliderMoved, this, [this](int value) {
        m_view->setPlaying(false);
        m_view->setTime(float(value) / kTimelineSteps * m_view->duration());
    });
    connect(m_view, &ModelViewWindow::timeChanged, this, [this](float s) {
        const float length = m_view->duration();
        if (!m_timeline->isSliderDown()) {
            const QSignalBlocker block(m_timeline);
            m_timeline->setValue(length > 0.0f ? qRound(s / length * kTimelineSteps) : 0);
        }
        updateTimeLabel(s);
    });
    connect(m_view, &ModelViewWindow::playingChanged, this, [this](bool playing) {
        m_play->setText(playing ? tr("Pause") : tr("Play"));
    });
    connect(m_view, &ModelViewWindow::uploadProgressChanged, this, &ModelPreviewWidget::updateInfo);
    connect(m_view, &ModelViewWindow::settingsChangedByKey, this, &ModelPreviewWidget::syncControls);
    connect(m_view, &ModelViewWindow::glFailed, this, [this](const QString &message) {
        // Only a missing context is permanent; an upload error belongs to one document.
        if (m_view->glUnavailable())
            m_glError = message;
        showBanner(BannerKind::Error, message);
    });
    connect(m_view, &ModelViewWindow::resourcesLost, this, [this]() { startLoad(true); });

    connect(&m_watcher, &QFutureWatcher<ModelLoader::Result>::finished, this, &ModelPreviewWidget::onLoaded);
    themeChanged(isDarkTheme());
}

ModelPreviewWidget::~ModelPreviewWidget()
{
    // The worker holds its own copies; it only needs to stop early.
    m_cancel->store(true);
}

void ModelPreviewWidget::cancelLoad()
{
    m_cancel->store(true);
    m_cancel = std::make_shared<std::atomic_bool>(false);
}

void ModelPreviewWidget::startLoad(bool reload)
{
    // setFuture() detaches the watcher from a previous load, so a stale
    // result never arrives; the flag only stops that worker early.
    cancelLoad();
    m_pendingReload = reload;
    const QString path = filePath();
    const QFileInfo info(path);
    if (!info.exists()) {
        showBanner(BannerKind::Error, tr("The file no longer exists."));
        return;
    }
    if (info.size() > ModelLoader::kMaxFileBytes) {
        showBanner(BannerKind::Error, tr("Models over %1 are not previewed.")
                                          .arg(ImageDecoder::formatBytes(ModelLoader::kMaxFileBytes)));
        return;
    }
    if (info.size() > ModelLoader::kProgressBytes) {
        showBanner(BannerKind::Neutral, tr("Loading %1 model…").arg(ImageDecoder::formatBytes(info.size())),
                   tr("Cancel"), [this]() {
                       cancelLoad();
                       showBanner(BannerKind::Neutral, tr("Loading cancelled."), tr("Retry"),
                                  [this]() { startLoad(m_pendingReload); });
                   });
    }

    const std::shared_ptr<std::atomic_bool> cancel = m_cancel;
    m_watcher.setFuture(QtConcurrent::run([path, cancel]() { return ModelLoader::load(path, *cancel); }));
}

void ModelPreviewWidget::onLoaded()
{
    const ModelLoader::Result result = m_watcher.result();
    if (result.cancelled)
        return;
    if (!result.doc) {
        if (m_view->document() && m_pendingReload)
            showBanner(BannerKind::Warning, tr("Could not reload the model: %1").arg(result.error));
        else
            showBanner(BannerKind::Error,
                       result.tooLarge ? result.error : tr("Could not open the model: %1").arg(result.error));
        return;
    }
    if (result.doc->primitives.empty()) {
        m_view->clearDocument();
        showBanner(BannerKind::Error, tr("The file has no geometry to show."));
        syncControls();
        updateInfo();
        return;
    }

    m_view->setDocument(result.doc, m_pendingReload);
    if (m_view->glUnavailable()) {
        // Replaces the loading banner; the info line still describes the file.
        showBanner(BannerKind::Error, m_glError);
    } else if (!result.doc->warnings.isEmpty()) {
        QStringList shown = result.doc->warnings.mid(0, kMaxWarningsShown);
        if (result.doc->warnings.size() > kMaxWarningsShown)
            shown << tr("and %n more", nullptr, int(result.doc->warnings.size() - kMaxWarningsShown));
        showBanner(BannerKind::Warning, shown.join(QStringLiteral("\n")));
    } else {
        hideBanner();
    }
    syncControls();
    updateInfo();
}

void ModelPreviewWidget::syncControls()
{
    const QSignalBlocker blockWire(m_wire);
    const QSignalBlocker blockGrid(m_grid);
    const QSignalBlocker blockOrtho(m_ortho);
    const QSignalBlocker blockZUp(m_zUp);
    m_wire->setChecked(m_view->settings().wireframe);
    m_grid->setChecked(m_view->settings().grid);
    m_ortho->setChecked(m_view->isOrthographic());
    m_zUp->setChecked(m_view->zUp());

    const ModelDocumentPtr doc = m_view->document();
    const bool skeleton = m_view->hasSkeleton();
    m_skeleton->setVisible(skeleton);
    if (!skeleton && m_skeleton->isChecked())
        m_skeleton->setChecked(false);

    const bool animated = doc && !doc->animations.empty();
    {
        const QSignalBlocker block(m_animation);
        m_animation->clear();
        if (animated) {
            m_animation->addItem(tr("Rest pose"), -1);
            for (size_t i = 0; i < doc->animations.size(); ++i) {
                const QString &name = doc->animations[i].name;
                m_animation->addItem(name.isEmpty() ? tr("Animation %1").arg(i + 1) : name, int(i));
            }
            m_animation->setCurrentIndex(m_view->animation() + 1);
        }
    }
    m_animation->setVisible(animated);
    const bool clip = animated && m_view->animation() >= 0;
    m_play->setVisible(clip);
    m_timeline->setVisible(clip);
    m_timeLabel->setVisible(clip);
    m_play->setText(m_view->isPlaying() ? tr("Pause") : tr("Play"));
    updateTimeLabel(m_view->time());
}

void ModelPreviewWidget::updateTimeLabel(float s)
{
    if (!m_timeLabel->isHidden())
        m_timeLabel->setText(tr("%1 / %2 s").arg(seconds(s), seconds(m_view->duration())));
}

void ModelPreviewWidget::updateInfo()
{
    const ModelDocumentPtr doc = m_view->document();
    if (!doc) {
        setInfoText(QString());
        return;
    }
    const QLocale locale;
    const ModelStats &s = doc->stats;
    QStringList parts;
    parts << doc->format;
    if (!doc->detail.isEmpty())
        parts << doc->detail;
    parts << tr("%n meshes", nullptr, s.meshes);
    if (s.triangles > 0)
        parts << tr("%1 triangles").arg(locale.toString(qulonglong(s.triangles)));
    if (s.lines > 0)
        parts << tr("%1 lines").arg(locale.toString(qulonglong(s.lines)));
    if (s.points > 0)
        parts << tr("%1 points").arg(locale.toString(qulonglong(s.points)));
    parts << tr("%1 vertices").arg(locale.toString(qulonglong(s.vertices)));
    if (!doc->materials.empty())
        parts << tr("%n materials", nullptr, int(doc->materials.size()));
    if (!doc->textures.empty())
        parts << tr("%n textures", nullptr, int(doc->textures.size()));
    if (s.joints > 0)
        parts << tr("%n joints", nullptr, s.joints);
    if (!doc->animations.empty())
        parts << tr("%n animations", nullptr, int(doc->animations.size()));
    parts << ImageDecoder::formatBytes(doc->fileSize);
    QString text = parts.join(QStringLiteral("  ·  "));
    const int percent = m_view->uploadPercent();
    if (percent < 100)
        text += QStringLiteral("        ") + tr("Uploading to the GPU %1%").arg(percent);
    setInfoText(text);
}

void ModelPreviewWidget::themeChanged(bool isDark)
{
    if (!m_view)
        return; // the base constructor applies the first theme before ours exists
    const QPalette pal = palette();
    QColor text = pal.color(QPalette::Text);
    ModelRenderer::Colors colors;
    colors.background = pal.color(QPalette::Base);
    colors.gridMinor = text;
    colors.gridMinor.setAlpha(30);
    colors.gridMajor = text;
    colors.gridMajor.setAlpha(75);
    colors.wire = isDark ? QColor(255, 255, 255, 70) : QColor(0, 0, 0, 90);
    colors.skeleton = pal.color(QPalette::Highlight);
    m_view->setColors(colors);
}
