#include "ImagePreviewWidget.h"

#include "ImageCanvas.h"

#include <QComboBox>
#include <QFileInfo>
#include <QLabel>
#include <QSlider>
#include <QToolButton>
#include <QtConcurrent>

#include <cmath>

namespace {

constexpr int kExposureStepsPerEv = 10;  // slider units per EV stop
constexpr int kExposureRangeEv = 12;

QString zoomText(double zoom)
{
    const double pct = zoom * 100.0;
    if (pct >= 10.0 || std::abs(pct - std::round(pct)) < 0.05)
        return QStringLiteral("%1%").arg(qRound(pct));
    return QStringLiteral("%1%").arg(pct, 0, 'f', 1);
}

// "3 / 12", plus the frame's tags (Aseprite) when it has any.
QString frameText(const ImageDocument &doc, int frame)
{
    QString text = QStringLiteral("%1 / %2").arg(frame + 1).arg(doc.layerCount);
    if (frame < doc.layerNames.size() && !doc.layerNames[frame].isEmpty())
        text += QStringLiteral("  ") + doc.layerNames[frame];
    return text;
}

} // namespace

ImagePreviewWidget::ImagePreviewWidget(const QString &typeId, const QString &displayName, QWidget *parent)
    : AssetPreviewWidget(typeId, displayName, parent)
    , m_cancel(std::make_shared<std::atomic_bool>(false))
{
    m_canvas = new ImageCanvas(this);
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

    m_channel = new QComboBox;
    m_channel->setToolTip(tr("Channels to show"));
    m_channel->addItem(QStringLiteral("RGBA"), int(ImageOps::Channel::Rgba));
    m_channel->addItem(QStringLiteral("RGB"), int(ImageOps::Channel::Rgb));
    m_channel->addItem(QStringLiteral("R"), int(ImageOps::Channel::R));
    m_channel->addItem(QStringLiteral("G"), int(ImageOps::Channel::G));
    m_channel->addItem(QStringLiteral("B"), int(ImageOps::Channel::B));
    m_channel->addItem(QStringLiteral("A"), int(ImageOps::Channel::A));
    addToolWidget(m_channel);
    m_checker = addToolButton(tr("Checker"), tr("Checkerboard under transparent pixels"), true);
    m_checker->setChecked(true);
    m_grid = addToolButton(tr("Grid"), tr("Pixel grid from 800%"), true);
    m_grid->setChecked(true);
    m_tile = addToolButton(tr("Tile"), tr("Repeat 3 × 3 to check seams"), true);
    addToolGap();

    m_mip = new QComboBox;
    m_mip->setToolTip(tr("Mip level"));
    addToolWidget(m_mip);
    m_face = new QComboBox;
    m_face->setToolTip(tr("Cubemap face"));
    addToolWidget(m_face);
    m_layer = new QComboBox;
    m_layer->setToolTip(tr("Array layer"));
    addToolWidget(m_layer);

    m_prevFrame = addToolButton(QStringLiteral("‹"), tr("Previous frame (,)"));
    m_play = addToolButton(tr("Pause"), tr("Play or pause (Space)"));
    m_nextFrame = addToolButton(QStringLiteral("›"), tr("Next frame (.)"));
    m_frameLabel = new QLabel;
    addToolWidget(m_frameLabel);

    m_exposureLabel = new QLabel;
    m_exposureLabel->setMinimumWidth(m_exposureLabel->fontMetrics().horizontalAdvance(QStringLiteral("EV +10.0")));
    addToolWidget(m_exposureLabel);
    m_exposure = new QSlider(Qt::Horizontal);
    m_exposure->setRange(-kExposureRangeEv * kExposureStepsPerEv, kExposureRangeEv * kExposureStepsPerEv);
    m_exposure->setValue(0);
    m_exposure->setFixedWidth(140);
    m_exposure->setToolTip(tr("Exposure in EV stops (double-click the label to reset)"));
    addToolWidget(m_exposure);

    for (QWidget *w : {static_cast<QWidget *>(m_mip), static_cast<QWidget *>(m_face), static_cast<QWidget *>(m_layer),
                       static_cast<QWidget *>(m_prevFrame), static_cast<QWidget *>(m_play),
                       static_cast<QWidget *>(m_nextFrame), static_cast<QWidget *>(m_frameLabel),
                       static_cast<QWidget *>(m_exposureLabel), static_cast<QWidget *>(m_exposure)})
        w->hide();

    connect(m_zoomOut, &QToolButton::clicked, m_canvas, &ImageCanvas::zoomOut);
    connect(m_zoomIn, &QToolButton::clicked, m_canvas, &ImageCanvas::zoomIn);
    connect(m_fit, &QToolButton::clicked, m_canvas, &ImageCanvas::zoomToFit);
    connect(m_actual, &QToolButton::clicked, m_canvas, &ImageCanvas::zoomToActual);
    connect(m_canvas, &ImageCanvas::zoomChanged, this, &ImagePreviewWidget::updateZoomLabel);

    connect(m_channel, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        ImageOps::ViewParams params = m_canvas->viewParams();
        params.channel = static_cast<ImageOps::Channel>(m_channel->currentData().toInt());
        m_canvas->setViewParams(params);
    });
    connect(m_checker, &QToolButton::toggled, m_canvas, &ImageCanvas::setCheckerboard);
    connect(m_grid, &QToolButton::toggled, m_canvas, &ImageCanvas::setPixelGrid);
    connect(m_tile, &QToolButton::toggled, m_canvas, &ImageCanvas::setTiled);

    connect(m_mip, QOverload<int>::of(&QComboBox::activated), this, [this](int mip) {
        m_canvas->setSurface(m_canvas->layer(), m_canvas->face(), mip);
        updateInfo();
    });
    connect(m_face, QOverload<int>::of(&QComboBox::activated), this, [this](int face) {
        m_canvas->setSurface(m_canvas->layer(), face, m_canvas->mip());
    });
    connect(m_layer, QOverload<int>::of(&QComboBox::activated), this, [this](int layer) {
        m_canvas->setSurface(layer, m_canvas->face(), m_canvas->mip());
    });

    connect(m_play, &QToolButton::clicked, this, [this]() { m_canvas->setPlaying(!m_canvas->isPlaying()); });
    connect(m_prevFrame, &QToolButton::clicked, this, [this]() { m_canvas->stepFrame(-1); });
    connect(m_nextFrame, &QToolButton::clicked, this, [this]() { m_canvas->stepFrame(1); });
    connect(m_canvas, &ImageCanvas::playingChanged, this, [this](bool playing) {
        m_play->setText(playing ? tr("Pause") : tr("Play"));
    });
    connect(m_canvas, &ImageCanvas::frameChanged, this, [this](int layer) {
        const ImageDocumentPtr doc = m_canvas->document();
        if (!doc) return;
        if (doc->isAnimation)
            m_frameLabel->setText(frameText(*doc, layer));
        else if (m_layer->count() > layer && m_layer->currentIndex() != layer)
            m_layer->setCurrentIndex(layer);
    });

    connect(m_exposure, &QSlider::valueChanged, this, [this](int v) {
        const float ev = float(v) / kExposureStepsPerEv;
        m_exposureLabel->setText(QStringLiteral("EV %1%2").arg(ev >= 0 ? QStringLiteral("+") : QString())
                                     .arg(double(ev), 0, 'f', 1));
        ImageOps::ViewParams params = m_canvas->viewParams();
        params.exposure = ev;
        m_canvas->setViewParams(params);
    });
    m_exposureLabel->setText(QStringLiteral("EV +0.0"));
    m_exposureLabel->installEventFilter(this);

    connect(m_canvas, &ImageCanvas::hoverPixelChanged, this, [this](const QString &text) {
        m_hoverText = text;
        updateInfo();
    });

    connect(&m_watcher, &QFutureWatcher<ImageDecoder::Result>::finished, this, &ImagePreviewWidget::onDecoded);
}

ImagePreviewWidget::~ImagePreviewWidget()
{
    // The worker holds its own copies; it only needs to stop early.
    m_cancel->store(true);
}

void ImagePreviewWidget::cancelDecode()
{
    m_cancel->store(true);
    m_cancel = std::make_shared<std::atomic_bool>(false);
}

void ImagePreviewWidget::startLoad(bool reload)
{
    // setFuture() below detaches the watcher from any previous decode, so a
    // stale result can never arrive; the flag only stops that worker early.
    cancelDecode();
    m_pendingReload = reload;
    const QString path = filePath();

    if (!QFileInfo::exists(path)) {
        showBanner(BannerKind::Error, tr("The file no longer exists."));
        return;
    }

    const qint64 estimate = ImageDecoder::estimateDecodedBytes(path);
    if (estimate > ImageDecoder::kMaxDecodedBytes) {
        showBanner(BannerKind::Error,
                   tr("This image needs %1 of memory to preview; the limit is %2.")
                       .arg(ImageDecoder::formatBytes(estimate), ImageDecoder::formatBytes(ImageDecoder::kMaxDecodedBytes)));
        return;
    }
    if (estimate > ImageDecoder::kProgressBytes) {
        showBanner(BannerKind::Neutral, tr("Decoding %1 image…").arg(ImageDecoder::formatBytes(estimate)),
                   tr("Cancel"), [this]() {
                       cancelDecode();
                       showBanner(BannerKind::Neutral, tr("Decoding cancelled."), tr("Retry"),
                                  [this]() { startLoad(m_pendingReload); });
                   });
    }

    const std::shared_ptr<std::atomic_bool> cancel = m_cancel;
    const bool vector = typeId() == QLatin1String("svg");
    m_watcher.setFuture(QtConcurrent::run([path, cancel, vector]() {
        return ImageDecoder::decode(path, *cancel, vector);
    }));
}

void ImagePreviewWidget::onDecoded()
{
    const ImageDecoder::Result result = m_watcher.result();
    if (result.cancelled)
        return;
    if (!result.doc) {
        if (m_canvas->document() && m_pendingReload)
            showBanner(BannerKind::Warning, tr("Could not reload the image: %1").arg(result.error));
        else
            showBanner(BannerKind::Error, result.tooLarge ? result.error
                                                          : tr("Could not open the image: %1").arg(result.error));
        return;
    }
    hideBanner();
    m_canvas->setDocument(result.doc, m_pendingReload, result.pyramid);
    documentReady(result.doc, m_pendingReload);
}

void ImagePreviewWidget::documentReady(const ImageDocumentPtr &doc, bool reload)
{
    Q_UNUSED(reload);
    syncSurfaceControls();
    m_channel->setEnabled(!doc->isVector());
    m_tile->setEnabled(true);
    const bool hdr = doc->isHdr;
    m_exposureLabel->setVisible(hdr);
    m_exposure->setVisible(hdr);
    updateZoomLabel(m_canvas->zoom(), m_canvas->isFit());
    updateInfo();
}

void ImagePreviewWidget::syncSurfaceControls()
{
    const ImageDocumentPtr doc = m_canvas->document();
    if (!doc) return;

    m_mip->clear();
    for (int i = 0; i < doc->mipCount; ++i) {
        const QImage &s = doc->surface(0, 0, i);
        m_mip->addItem(tr("Mip %1  %2 × %3").arg(i).arg(s.width()).arg(s.height()));
    }
    m_mip->setCurrentIndex(m_canvas->mip());
    m_mip->setVisible(doc->mipCount > 1);

    m_face->clear();
    for (int i = 0; i < doc->faceCount; ++i)
        m_face->addItem(i < doc->faceNames.size() ? doc->faceNames[i] : tr("Face %1").arg(i));
    m_face->setCurrentIndex(m_canvas->face());
    m_face->setVisible(doc->faceCount > 1);

    m_layer->clear();
    if (!doc->isAnimation) {
        for (int i = 0; i < doc->layerCount; ++i)
            m_layer->addItem(i < doc->layerNames.size() ? doc->layerNames[i] : tr("Layer %1").arg(i));
        m_layer->setCurrentIndex(m_canvas->layer());
    }
    m_layer->setVisible(!doc->isAnimation && doc->layerCount > 1);

    const bool anim = doc->isAnimation;
    m_play->setVisible(anim);
    m_prevFrame->setVisible(anim);
    m_nextFrame->setVisible(anim);
    m_frameLabel->setVisible(anim);
    if (anim) {
        m_play->setText(m_canvas->isPlaying() ? tr("Pause") : tr("Play"));
        m_frameLabel->setText(frameText(*doc, m_canvas->layer()));
    }
}

void ImagePreviewWidget::updateZoomLabel(double zoom, bool fit)
{
    m_zoomLabel->setText(zoomText(zoom));
    m_fit->setChecked(fit);
}

void ImagePreviewWidget::updateInfo()
{
    const ImageDocumentPtr doc = m_canvas->document();
    if (!doc) {
        setInfoText(QString());
        return;
    }
    QStringList parts;
    parts << QStringLiteral("%1 × %2").arg(doc->width).arg(doc->height) << doc->format;
    if (!doc->detail.isEmpty()) parts << doc->detail;
    if (doc->mipCount > 1) parts << tr("%n mips", nullptr, doc->mipCount);
    if (doc->faceCount == 6) parts << tr("cubemap");
    if (doc->isAnimation) parts << tr("%n frames", nullptr, doc->layerCount);
    else if (doc->layerCount > 1) parts << tr("%n layers", nullptr, doc->layerCount);
    parts << ImageDecoder::formatBytes(doc->fileSize);
    QString text = parts.join(QStringLiteral("  ·  "));
    if (!m_hoverText.isEmpty())
        text += QStringLiteral("        ") + m_hoverText;
    setInfoText(text);
}

bool ImagePreviewWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_exposureLabel && event->type() == QEvent::MouseButtonDblClick) {
        m_exposure->setValue(0);
        return true;
    }
    return AssetPreviewWidget::eventFilter(watched, event);
}

void ImagePreviewWidget::themeChanged(bool isDark)
{
    const QColor base = palette().color(QPalette::Base);
    // Checkerboard from the live palette: the base colour and a shade of it,
    // so it reads as "transparent" in both themes without hard-coded colours.
    const QColor shade = isDark ? base.lighter(150) : base.darker(118);
    m_canvas->setColors(base, base, shade);
}
