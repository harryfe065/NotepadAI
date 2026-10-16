#include "ImageCanvas.h"

#include <QFutureWatcher>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QSvgRenderer>
#include <QWheelEvent>
#include <QtConcurrent>
#include <QtMath>

#include <array>
#include <cmath>

namespace {

// Surfaces up to this size get their pyramid built synchronously (~1-2 ms).
constexpr qint64 kSyncPyramidPixels = 1 << 20;
// Without a pyramid, zooming out on a bigger surface would convert the whole
// image on the UI thread; wait for the worker instead.
constexpr qint64 kNeedPyramidPixels = 4 << 20;

constexpr std::array<double, 25> kZoomSteps = {
    1.0 / 64, 1.0 / 32, 1.0 / 16, 1.0 / 8, 1.0 / 4, 1.0 / 3, 1.0 / 2, 2.0 / 3, 1.0, 1.5, 2, 3, 4,
    6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256};

bool sameParams(const ImageOps::ViewParams &a, const ImageOps::ViewParams &b)
{
    return a.channel == b.channel && a.exposure == b.exposure;
}

} // namespace

ImageCanvas::ImageCanvas(QWidget *parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(64, 64);

    m_frameTimer.setSingleShot(true);
    connect(&m_frameTimer, &QTimer::timeout, this, [this]() {
        if (!m_doc || !m_doc->isAnimation) return;
        m_layer = (m_layer + 1) % std::max(1, m_doc->layerCount);
        emit frameChanged(m_layer);
        update();
        scheduleFrame();
    });

    m_vectorTimer.setSingleShot(true);
    m_vectorTimer.setInterval(80);
    connect(&m_vectorTimer, &QTimer::timeout, this, &ImageCanvas::startVectorRender);

    setColors(palette().color(QPalette::Base), QColor(255, 255, 255), QColor(204, 204, 204));
}

ImageCanvas::~ImageCanvas() = default;

void ImageCanvas::setDocument(ImageDocumentPtr doc, bool keepView, ImagePyramidPtr initialPyramid)
{
    const bool sameShape = m_doc && doc && m_doc->surfaces.size() == doc->surfaces.size()
                           && m_doc->width == doc->width && m_doc->height == doc->height
                           && m_doc->mipCount == doc->mipCount && m_doc->faceCount == doc->faceCount
                           && m_doc->layerCount == doc->layerCount;
    m_doc = std::move(doc);
    ++m_docGeneration;
    m_pyramids.clear();
    m_pyramidJobs.clear();
    m_crop = {};
    m_vectorTile = {};

    if (!keepView || !sameShape) {
        m_layer = m_face = m_mip = 0;
        m_viewInitialised = false;
    }
    if (m_doc && initialPyramid && currentSurfaceIndex() == 0)
        m_pyramids.insert(0, std::move(initialPyramid));

    updateMathGeometry();
    if (!m_viewInitialised && m_doc && !size().isEmpty()) {
        const double z = ImageViewMath::initialZoom(m_math.viewport, m_math.image);
        m_fit = z == ImageViewMath::fitZoom(m_math.viewport, m_math.image);
        m_math.zoom = z;
        m_math.center = QPointF(m_math.image.width() / 2.0, m_math.image.height() / 2.0);
        m_viewInitialised = true;
    } else if (m_fit) {
        m_math.zoom = ImageViewMath::fitZoom(m_math.viewport, m_math.image);
    }
    m_math.clampCenter();

    if (!m_doc || !m_doc->isAnimation) {
        m_frameTimer.stop();
        if (m_playing) {
            m_playing = false;
            emit playingChanged(false);
        }
    } else if (!keepView) {
        m_playing = true;
        emit playingChanged(true);
    }
    scheduleFrame();
    viewChanged();
}

int ImageCanvas::currentSurfaceIndex() const
{
    if (!m_doc) return -1;
    return m_doc->surfaceIndex(m_layer, m_face, m_mip);
}

const QImage *ImageCanvas::currentSurface() const
{
    const int idx = currentSurfaceIndex();
    if (idx < 0 || idx >= static_cast<int>(m_doc->surfaces.size())) return nullptr;
    return &m_doc->surfaces[static_cast<size_t>(idx)];
}

QSizeF ImageCanvas::logicalSize() const
{
    const QImage *s = currentSurface();
    if (!s) return {};
    // A vector document is measured in its intrinsic units; its raster is a
    // higher-resolution level of that, so 100% means the SVG's own size.
    if (m_doc->isVector())
        return QSizeF(m_doc->width, m_doc->height);
    return QSizeF(s->width(), s->height());
}

QSizeF ImageCanvas::contentSize() const
{
    const QSizeF one = logicalSize();
    return m_tiled ? one * 3.0 : one;
}

QRectF ImageCanvas::tileRect(int i, int j) const
{
    const QSizeF one = logicalSize();
    return {i * one.width(), j * one.height(), one.width(), one.height()};
}

void ImageCanvas::updateMathGeometry()
{
    m_math.viewport = QSizeF(size());
    m_math.image = contentSize();
}

void ImageCanvas::setSurface(int layer, int face, int mip)
{
    if (!m_doc) return;
    layer = std::clamp(layer, 0, m_doc->layerCount - 1);
    face = std::clamp(face, 0, m_doc->faceCount - 1);
    mip = std::clamp(mip, 0, m_doc->mipCount - 1);
    if (layer == m_layer && face == m_face && mip == m_mip) return;
    const QSizeF before = contentSize();
    m_layer = layer;
    m_face = face;
    m_mip = mip;
    updateMathGeometry();
    if (contentSize() != before) {
        // A different mip has a different size: keep the same relative spot.
        if (!before.isEmpty())
            m_math.center = QPointF(m_math.center.x() / before.width() * m_math.image.width(),
                                    m_math.center.y() / before.height() * m_math.image.height());
        if (m_fit)
            m_math.zoom = ImageViewMath::fitZoom(m_math.viewport, m_math.image);
        m_math.clampCenter();
    }
    emit frameChanged(m_layer);
    viewChanged();
}

void ImageCanvas::setViewParams(const ImageOps::ViewParams &params)
{
    if (sameParams(params, m_params)) return;
    m_params = params;
    update();
    if (m_hoverValid) updateHover(m_hoverPos);
}

void ImageCanvas::setTiled(bool tiled)
{
    if (m_tiled == tiled) return;
    const QSizeF before = contentSize();
    m_tiled = tiled;
    m_crop = {};
    updateMathGeometry();
    if (!before.isEmpty()) {
        // Keep the centre texel: the original image becomes the middle tile.
        const QSizeF one = logicalSize();
        const QPointF shift(one.width(), one.height());
        m_math.center += tiled ? shift : -shift;
    }
    if (m_fit)
        m_math.zoom = ImageViewMath::fitZoom(m_math.viewport, m_math.image);
    m_math.clampCenter();
    viewChanged();
}

void ImageCanvas::setCheckerboard(bool on)
{
    if (m_checker == on) return;
    m_checker = on;
    update();
}

void ImageCanvas::setPixelGrid(bool on)
{
    if (m_grid == on) return;
    m_grid = on;
    update();
}

void ImageCanvas::setColors(const QColor &background, const QColor &checkA, const QColor &checkB)
{
    m_background = background;
    m_checkA = checkA;
    m_checkB = checkB;
    m_checkTile = QPixmap();
    update();
}

void ImageCanvas::setPlaying(bool playing)
{
    if (!m_doc || !m_doc->isAnimation) playing = false;
    if (m_playing == playing) return;
    m_playing = playing;
    if (playing)
        scheduleFrame();
    else
        m_frameTimer.stop();
    emit playingChanged(playing);
}

void ImageCanvas::stepFrame(int delta)
{
    if (!m_doc || m_doc->layerCount <= 1) return;
    setPlaying(false);
    const int n = m_doc->layerCount;
    setSurface(((m_layer + delta) % n + n) % n, m_face, m_mip);
}

void ImageCanvas::scheduleFrame()
{
    if (!m_playing || !m_doc || !m_doc->isAnimation || !isVisible()) return;
    int delay = m_layer < m_doc->frameDelaysMs.size() ? m_doc->frameDelaysMs[m_layer] : 100;
    // Browsers treat 0-10 ms GIF delays as 100 ms; do the same so such files
    // play at their intended speed instead of spinning.
    if (delay <= 10) delay = 100;
    m_frameTimer.start(delay);
}

ImagePyramidPtr ImageCanvas::pyramidFor(int surfaceIndex)
{
    if (!m_doc || m_doc->isAnimation) return {};
    if (auto it = m_pyramids.constFind(surfaceIndex); it != m_pyramids.constEnd())
        return it.value();
    const QImage &s = m_doc->surfaces[static_cast<size_t>(surfaceIndex)];
    if (qint64(s.width()) * s.height() <= kSyncPyramidPixels) {
        ImagePyramidPtr pyr = ImageOps::buildPyramid(s);
        m_pyramids.insert(surfaceIndex, pyr);
        return pyr;
    }
    requestPyramid(surfaceIndex);
    return {};
}

void ImageCanvas::requestPyramid(int surfaceIndex)
{
    if (m_pyramidJobs.contains(surfaceIndex)) return;
    m_pyramidJobs.insert(surfaceIndex, true);
    const QImage src = m_doc->surfaces[static_cast<size_t>(surfaceIndex)];
    const quint64 gen = m_docGeneration;
    auto *watcher = new QFutureWatcher<ImagePyramidPtr>(this);
    connect(watcher, &QFutureWatcher<ImagePyramidPtr>::finished, this, [this, watcher, surfaceIndex, gen]() {
        watcher->deleteLater();
        if (gen != m_docGeneration) return;
        m_pyramidJobs.remove(surfaceIndex);
        // A handful of surfaces (mip/face browsing) is plenty; drop the rest.
        if (m_pyramids.size() >= 6) {
            const int keep = currentSurfaceIndex();
            for (auto it = m_pyramids.begin(); it != m_pyramids.end();) {
                if (it.key() != keep) it = m_pyramids.erase(it);
                else ++it;
            }
        }
        m_pyramids.insert(surfaceIndex, watcher->result());
        update();
    });
    watcher->setFuture(QtConcurrent::run([src]() { return ImageOps::buildPyramid(src); }));
}

void ImageCanvas::applyZoom(double zoom, QPointF anchor, bool fit)
{
    if (!m_doc) return;
    m_fit = fit;
    m_math.zoomAt(anchor, zoom);
    viewChanged();
}

void ImageCanvas::zoomIn()
{
    const double z = m_math.zoom * 1.001;
    for (double step : kZoomSteps) {
        if (step > z) {
            applyZoom(step, QPointF(width() / 2.0, height() / 2.0), false);
            return;
        }
    }
}

void ImageCanvas::zoomOut()
{
    const double z = m_math.zoom / 1.001;
    for (auto it = kZoomSteps.rbegin(); it != kZoomSteps.rend(); ++it) {
        if (*it < z) {
            applyZoom(*it, QPointF(width() / 2.0, height() / 2.0), false);
            return;
        }
    }
}

void ImageCanvas::zoomToFit()
{
    if (!m_doc) return;
    m_fit = true;
    m_math.zoom = ImageViewMath::fitZoom(m_math.viewport, m_math.image);
    m_math.center = QPointF(m_math.image.width() / 2.0, m_math.image.height() / 2.0);
    m_math.clampCenter();
    viewChanged();
}

void ImageCanvas::zoomToActual()
{
    applyZoom(1.0, QPointF(width() / 2.0, height() / 2.0), false);
}

void ImageCanvas::viewChanged()
{
    update();
    emit zoomChanged(m_math.zoom, m_fit);
    scheduleVectorRender();
    if (m_hoverValid) updateHover(m_hoverPos);
}

void ImageCanvas::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateMathGeometry();
    if (!m_doc) return;
    if (!m_viewInitialised) {
        const double z = ImageViewMath::initialZoom(m_math.viewport, m_math.image);
        m_fit = z == ImageViewMath::fitZoom(m_math.viewport, m_math.image);
        m_math.zoom = z;
        m_math.center = QPointF(m_math.image.width() / 2.0, m_math.image.height() / 2.0);
        m_viewInitialised = true;
    } else if (m_fit) {
        m_math.zoom = ImageViewMath::fitZoom(m_math.viewport, m_math.image);
    }
    m_math.clampCenter();
    viewChanged();
}

QImage ImageCanvas::wholeLevel(int surfaceIndex, int levelIndex, const QImage &level)
{
    if (m_crop.surface == surfaceIndex && m_crop.level == levelIndex && sameParams(m_crop.params, m_params)
        && m_crop.rect == level.rect() && !m_crop.image.isNull())
        return m_crop.image;
    m_crop.surface = surfaceIndex;
    m_crop.level = levelIndex;
    m_crop.params = m_params;
    m_crop.rect = level.rect();
    m_crop.image = ImageOps::renderCrop(level, level.rect(), m_params);
    return m_crop.image;
}

void ImageCanvas::paintSurface(QPainter &p, const QImage *level, int levelIndex, double sx, double sy)
{
    const int sidx = currentSurfaceIndex();
    const QRectF visible = m_math.visibleImageRect();
    if (visible.isEmpty()) return;

    // Visible rect in level texels, rounded outward.
    const QRect need = QRectF(visible.x() * sx, visible.y() * sy, visible.width() * sx, visible.height() * sy)
                           .toAlignedRect()
                           .intersected(level->rect());
    if (need.isEmpty()) return;

    const bool reuse = m_crop.surface == sidx && m_crop.level == levelIndex && sameParams(m_crop.params, m_params)
                       && !m_crop.image.isNull() && m_crop.rect.contains(need);
    if (!reuse) {
        // 20% margin each side so small pans and scrolls reuse the crop.
        const int mx = std::max(16, need.width() / 5), my = std::max(16, need.height() / 5);
        const QRect rect = need.adjusted(-mx, -my, mx, my).intersected(level->rect());
        m_crop.surface = sidx;
        m_crop.level = levelIndex;
        m_crop.params = m_params;
        m_crop.rect = rect;
        m_crop.image = ImageOps::renderCrop(*level, rect, m_params);
    }
    if (m_crop.image.isNull()) return;

    const QRect &r = m_crop.rect;
    const QPointF topLeft = m_math.imageToView(QPointF(r.x() / sx, r.y() / sy));
    const QRectF target(topLeft, QSizeF(r.width() / sx * m_math.zoom, r.height() / sy * m_math.zoom));
    p.drawImage(target, m_crop.image, QRectF(m_crop.image.rect()));
}

void ImageCanvas::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), m_background);
    const QImage *surf = currentSurface();
    if (!surf || surf->isNull()) return;

    const int sidx = currentSurfaceIndex();
    const double dpr = devicePixelRatioF();
    const double deviceScale = m_math.zoom * dpr;
    const QSizeF logical = logicalSize();
    // Texels of the surface per logical unit (1, or the SVG raster scale).
    const double base = surf->width() / logical.width();

    const QImage *level = surf;
    int levelIndex = 0;
    ImagePyramidPtr pyr = pyramidFor(sidx);
    if (pyr) {
        levelIndex = ImageViewMath::levelFor(deviceScale / base, static_cast<int>(pyr->levels.size()));
        level = &pyr->levels[static_cast<size_t>(levelIndex)];
    } else if (!m_doc->isAnimation && qint64(surf->width()) * surf->height() > kNeedPyramidPixels
               && deviceScale / base < 0.5) {
        p.setPen(palette().color(QPalette::PlaceholderText));
        p.drawText(rect(), Qt::AlignCenter, tr("Preparing preview…"));
        return;
    }
    // Level texels per logical unit.
    const double sx = level->width() / logical.width();
    const double sy = level->height() / logical.height();

    // Texel magnification on screen: below 2x bilinear looks right, above it
    // nearest keeps pixel art and texel boundaries crisp.
    const double magnify = deviceScale / sx;
    p.setRenderHint(QPainter::SmoothPixmapTransform, magnify < 2.0);

    const QRectF imageView = m_math.imageRectInView();
    if (m_checker && m_doc->hasAlpha && m_params.channel == ImageOps::Channel::Rgba) {
        if (m_checkTile.isNull() || !qFuzzyCompare(m_checkTile.devicePixelRatio(), dpr)) {
            const int cell = qRound(8 * dpr);
            QPixmap tile(cell * 2, cell * 2);
            tile.fill(m_checkA);
            QPainter tp(&tile);
            tp.fillRect(0, 0, cell, cell, m_checkB);
            tp.fillRect(cell, cell, cell, cell, m_checkB);
            tp.end();
            tile.setDevicePixelRatio(dpr);
            m_checkTile = tile;
        }
        p.save();
        p.setBrushOrigin(imageView.topLeft());
        p.fillRect(imageView.intersected(QRectF(rect())), QBrush(m_checkTile));
        p.restore();
    }

    if (m_tiled) {
        const QImage whole = wholeLevel(sidx, levelIndex, *level);
        for (int j = 0; j < 3; ++j) {
            for (int i = 0; i < 3; ++i) {
                const QRectF t = tileRect(i, j);
                const QRectF target(m_math.imageToView(t.topLeft()), t.size() * m_math.zoom);
                if (target.intersects(QRectF(rect())))
                    p.drawImage(target, whole, QRectF(whole.rect()));
            }
        }
        return;
    }

    paintSurface(p, level, levelIndex, sx, sy);

    if (m_doc->isVector() && !m_vectorTile.image.isNull() && qFuzzyCompare(m_vectorTile.deviceScale, deviceScale)
        && m_params.channel == ImageOps::Channel::Rgba) {
        const QRectF target(m_math.imageToView(m_vectorTile.imageRect.topLeft()),
                            m_vectorTile.imageRect.size() * m_math.zoom);
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        p.drawImage(target, m_vectorTile.image, QRectF(m_vectorTile.image.rect()));
    }

    if (m_grid && m_math.zoom >= 8.0) {
        const QRectF vis = m_math.visibleImageRect();
        QVector<QLineF> lines;
        const int x0 = static_cast<int>(std::floor(vis.left())), x1 = static_cast<int>(std::ceil(vis.right()));
        const int y0 = static_cast<int>(std::floor(vis.top())), y1 = static_cast<int>(std::ceil(vis.bottom()));
        lines.reserve((x1 - x0 + 1) + (y1 - y0 + 1));
        const double top = std::max(0.0, imageView.top()), bottom = std::min(double(height()), imageView.bottom());
        const double left = std::max(0.0, imageView.left()), right = std::min(double(width()), imageView.right());
        for (int x = x0; x <= x1; ++x) {
            const double vx = m_math.imageToView(QPointF(x, 0)).x();
            lines.append(QLineF(vx, top, vx, bottom));
        }
        for (int y = y0; y <= y1; ++y) {
            const double vy = m_math.imageToView(QPointF(0, y)).y();
            lines.append(QLineF(left, vy, right, vy));
        }
        // Media overlay: the underlay is unknown, so a neutral translucent grey.
        p.setPen(QPen(QColor(128, 128, 128, 110), 0));
        p.drawLines(lines);
    }
}

void ImageCanvas::scheduleVectorRender()
{
    if (!m_doc || !m_doc->isVector() || m_tiled) return;
    const double deviceScale = m_math.zoom * devicePixelRatioF();
    // The base raster is sharp up to its own scale; re-rasterise only past it.
    if (deviceScale <= m_doc->rasterScale * 1.001) {
        m_vectorTile = {};
        return;
    }
    m_vectorTimer.start();
}

void ImageCanvas::startVectorRender()
{
    if (!m_doc || !m_doc->isVector() || !isVisible()) return;
    const double deviceScale = m_math.zoom * devicePixelRatioF();
    if (deviceScale <= m_doc->rasterScale * 1.001) return;
    const QRectF visible = m_math.visibleImageRect();
    if (visible.isEmpty()) return;
    const QSize out(std::max(1, qCeil(visible.width() * deviceScale)), std::max(1, qCeil(visible.height() * deviceScale)));
    // Bounded by the viewport in device pixels; a stale request is dropped.
    const quint64 gen = ++m_vectorGeneration;
    const quint64 docGen = m_docGeneration;
    const QByteArray data = m_doc->vectorData;
    const QSizeF surfaceSize = logicalSize();
    const double dpr = devicePixelRatioF();

    auto *watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, this,
            [this, watcher, gen, docGen, visible, deviceScale]() {
        watcher->deleteLater();
        if (gen != m_vectorGeneration || docGen != m_docGeneration) return;
        QImage img = watcher->result();
        if (img.isNull()) return;
        m_vectorTile.image = std::move(img);
        m_vectorTile.imageRect = visible;
        m_vectorTile.deviceScale = deviceScale;
        update();
    });
    watcher->setFuture(QtConcurrent::run([data, out, visible, deviceScale, surfaceSize, dpr]() {
        Q_UNUSED(dpr);
        QSvgRenderer renderer(data);
        if (!renderer.isValid()) return QImage();
        QImage img(out, QImage::Format_ARGB32_Premultiplied);
        if (img.isNull()) return img;
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(deviceScale, deviceScale);
        p.translate(-visible.x(), -visible.y());
        renderer.render(&p, QRectF(QPointF(0, 0), surfaceSize));
        p.end();
        return img;
    }));
}

void ImageCanvas::updateHover(QPointF pos)
{
    m_hoverPos = pos;
    m_hoverValid = true;
    const QImage *surf = currentSurface();
    if (!surf || surf->isNull()) {
        emit hoverPixelChanged(QString());
        return;
    }
    QPointF ip = m_math.viewToImage(pos);
    const QSizeF content = contentSize();
    if (ip.x() < 0 || ip.y() < 0 || ip.x() >= content.width() || ip.y() >= content.height()) {
        emit hoverPixelChanged(QString());
        return;
    }
    const QSizeF one = logicalSize();
    if (m_tiled)
        ip = QPointF(std::fmod(ip.x(), one.width()), std::fmod(ip.y(), one.height()));
    const int x = static_cast<int>(std::floor(ip.x()));
    const int y = static_cast<int>(std::floor(ip.y()));
    // Vector documents: report intrinsic units, sample the raster beneath.
    const double base = surf->width() / one.width();
    const int tx = std::min(surf->width() - 1, static_cast<int>(ip.x() * base));
    const int ty = std::min(surf->height() - 1, static_cast<int>(ip.y() * base));
    emit hoverPixelChanged(QStringLiteral("x %1  y %2   ").arg(x).arg(y) + ImageOps::describePixel(*surf, tx, ty));
}

void ImageCanvas::wheelEvent(QWheelEvent *event)
{
    if (!m_doc) return;
    const bool touchpad = event->pointingDevice()
                          && event->pointingDevice()->type() == QInputDevice::DeviceType::TouchPad;
    const bool zoomGesture = (event->modifiers() & Qt::ControlModifier) || !touchpad;
    if (zoomGesture) {
        const double notches = event->angleDelta().y() / 120.0;
        if (notches == 0.0) return;
        applyZoom(m_math.zoom * std::pow(2.0, notches * 0.25), event->position(), false);
    } else {
        QPointF delta = event->pixelDelta();
        if (delta.isNull()) delta = QPointF(event->angleDelta()) / 4.0;
        m_math.panBy(delta);
        viewChanged();
    }
    event->accept();
}

bool ImageCanvas::event(QEvent *event)
{
    if (event->type() == QEvent::NativeGesture) {
        auto *g = static_cast<QNativeGestureEvent *>(event);
        if (g->gestureType() == Qt::ZoomNativeGesture) {
            applyZoom(m_math.zoom * (1.0 + g->value()), g->position(), false);
            return true;
        }
        if (g->gestureType() == Qt::SmartZoomNativeGesture) {
            if (m_fit) applyZoom(1.0, g->position(), false);
            else zoomToFit();
            return true;
        }
    }
    return QWidget::event(event);
}

void ImageCanvas::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton) {
        m_panning = true;
        m_lastPan = event->position();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void ImageCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (m_panning) {
        const QPointF delta = event->position() - m_lastPan;
        m_lastPan = event->position();
        m_math.panBy(delta);
        viewChanged();
    }
    updateHover(event->position());
}

void ImageCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_panning && (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton)) {
        m_panning = false;
        unsetCursor();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void ImageCanvas::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) return;
    if (m_fit || std::abs(m_math.zoom - 1.0) > 1e-9) applyZoom(1.0, event->position(), false);
    else zoomToFit();
}

void ImageCanvas::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);
    m_hoverValid = false;
    emit hoverPixelChanged(QString());
}

void ImageCanvas::keyPressEvent(QKeyEvent *event)
{
    const int step = 48;
    switch (event->key()) {
    case Qt::Key_Plus:
    case Qt::Key_Equal: zoomIn(); break;
    case Qt::Key_Minus: zoomOut(); break;
    case Qt::Key_0:
    case Qt::Key_F: zoomToFit(); break;
    case Qt::Key_1: zoomToActual(); break;
    case Qt::Key_Left: m_math.panBy({double(step), 0}); viewChanged(); break;
    case Qt::Key_Right: m_math.panBy({double(-step), 0}); viewChanged(); break;
    case Qt::Key_Up: m_math.panBy({0, double(step)}); viewChanged(); break;
    case Qt::Key_Down: m_math.panBy({0, double(-step)}); viewChanged(); break;
    case Qt::Key_Space: setPlaying(!m_playing); break;
    case Qt::Key_Comma: stepFrame(-1); break;
    case Qt::Key_Period: stepFrame(1); break;
    default: QWidget::keyPressEvent(event); return;
    }
    event->accept();
}

void ImageCanvas::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    scheduleFrame();
    scheduleVectorRender();
}

void ImageCanvas::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    m_frameTimer.stop();
    // A hidden tab keeps the pyramids (cheap to show again) but frees the
    // viewport-sized crop and vector tile.
    m_crop = {};
    m_vectorTile = {};
}
