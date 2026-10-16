#include "TileMapCanvas.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

using namespace TileMap;

namespace {

constexpr double kMaxZoom = 64.0;
constexpr qint64 kPaintBudgetNs = 8'000'000;     // chunk rasterising per paint
constexpr int kMaxLiveAnimated = 20000;          // animated tiles drawn per layer and paint
constexpr int kMinGridPx = 6;                    // grid lines closer than this are hidden

QTransform flipTransform(const QRectF &dest, uint8_t flags)
{
    // Local coordinates: the tile centred on the origin. The diagonal flip
    // (transpose) comes first, then horizontal and vertical.
    const QPointF c = dest.center();
    QTransform f;
    f.translate(c.x(), c.y());
    f.scale((flags & FlipH) ? -1.0 : 1.0, (flags & FlipV) ? -1.0 : 1.0);
    if (flags & FlipD)
        f = QTransform(0, 1, 1, 0, 0, 0) * f;
    return f;
}

void drawImageFlipped(QPainter &p, const QRectF &dest, const QImage &img, const QRectF &src, uint8_t flags)
{
    p.save();
    p.setTransform(flipTransform(dest, flags), true);
    p.drawImage(QRectF(-dest.width() / 2.0, -dest.height() / 2.0, dest.width(), dest.height()), img, src);
    p.restore();
}

} // namespace

TileMapCanvas::TileMapCanvas(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    m_background = palette().color(QPalette::Base);
    m_gridColor = QColor(128, 128, 128, 90);
    m_objectColor = palette().color(QPalette::Highlight);

    m_animTimer.setInterval(33);
    connect(&m_animTimer, &QTimer::timeout, this, qOverload<>(&QWidget::update));
}

TileMapCanvas::~TileMapCanvas() = default;

// ---------------------------------------------------------------- document

void TileMapCanvas::resetCaches()
{
    m_chunks.clear();
    m_chunkBytes = 0;
    m_tilesetCache.clear();
    if (m_doc)
        m_tilesetCache.resize(m_doc->tilesets.size());
}

void TileMapCanvas::setDocument(DocumentPtr doc, bool keepView)
{
    const bool sameBounds = keepView && m_doc && doc && m_doc->bounds == doc->bounds;
    m_doc = std::move(doc);
    resetCaches();

    m_layerOn.assign(m_doc ? m_doc->layers.size() : 0, true);
    if (m_doc) {
        for (size_t i = 0; i < m_doc->layers.size(); ++i)
            m_layerOn[i] = m_doc->layers[i].visible;
        // Toggles the user made survive a reload: remembered by group name.
        for (size_t i = 0; i < m_doc->layers.size(); ++i) {
            const auto it = m_groupOverride.constFind(m_doc->layers[i].group);
            if (it != m_groupOverride.constEnd())
                m_layerOn[i] = it.value();
        }
    }
    if (!sameBounds || !m_viewInitialised) {
        m_fit = true;
        m_viewInitialised = false;
        if (width() > 0 && m_doc)
            zoomToFit();
    }
    updateAnimationTimer();
    update();
}

void TileMapCanvas::setGroupVisible(const QString &group, bool visible)
{
    m_groupOverride.insert(group, visible);
    if (!m_doc)
        return;
    for (size_t i = 0; i < m_doc->layers.size(); ++i) {
        if (m_doc->layers[i].group == group)
            m_layerOn[i] = visible;
    }
    update();
}

bool TileMapCanvas::groupVisible(const QString &group) const
{
    const auto it = m_groupOverride.constFind(group);
    if (it != m_groupOverride.constEnd())
        return it.value();
    if (m_doc) {
        for (const Layer &l : m_doc->layers) {
            if (l.group == group)
                return l.visible;
        }
    }
    return true;
}

void TileMapCanvas::setGridVisible(bool on)
{
    m_showGrid = on;
    update();
}

void TileMapCanvas::setObjectsVisible(bool on)
{
    m_showObjects = on;
    update();
}

void TileMapCanvas::setAnimating(bool on)
{
    m_animating = on;
    updateAnimationTimer();
    update();
}

void TileMapCanvas::setColors(const QColor &background, const QColor &grid, const QColor &object)
{
    m_background = background;
    m_gridColor = grid;
    m_objectColor = object;
    update();
}

void TileMapCanvas::updateAnimationTimer()
{
    const bool run = m_animating && m_doc && m_doc->hasAnimations && isVisible();
    if (run && !m_animTimer.isActive()) {
        m_animClock.start();
        m_animTimer.start();
    } else if (!run) {
        m_animTimer.stop();
    }
}

void TileMapCanvas::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    updateAnimationTimer();
}

void TileMapCanvas::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    updateAnimationTimer();
}

// -------------------------------------------------------------------- view

QPointF TileMapCanvas::mapToView(const QPointF &p) const
{
    return {(p.x() - m_center.x()) * m_zoom + width() / 2.0, (p.y() - m_center.y()) * m_zoom + height() / 2.0};
}

QPointF TileMapCanvas::viewToMap(const QPointF &p) const
{
    return {(p.x() - width() / 2.0) / m_zoom + m_center.x(), (p.y() - height() / 2.0) / m_zoom + m_center.y()};
}

QRectF TileMapCanvas::visibleMapRect() const
{
    return QRectF(viewToMap({0, 0}), viewToMap({double(width()), double(height())}));
}

double TileMapCanvas::minZoom() const
{
    double lower = 1.0 / 64.0;
    if (m_doc && !m_doc->bounds.isEmpty() && width() > 0 && height() > 0) {
        const double fit = std::min(width() / m_doc->bounds.width(), height() / m_doc->bounds.height());
        lower = std::min(lower, fit * 0.5);
    }
    return std::max(lower, std::ldexp(1.0, -kMaxLevel));
}

void TileMapCanvas::clampCenter()
{
    if (!m_doc)
        return;
    const QRectF b = m_doc->bounds;
    auto axis = [this](double c, double lo, double len, double viewLen) {
        const double shown = viewLen / m_zoom;
        if (len <= shown)
            return lo + len / 2.0;
        return std::clamp(c, lo + shown / 2.0, lo + len - shown / 2.0);
    };
    m_center.setX(axis(m_center.x(), b.left(), b.width(), width()));
    m_center.setY(axis(m_center.y(), b.top(), b.height(), height()));
}

void TileMapCanvas::emitView()
{
    update();
    emit zoomChanged(m_zoom, m_fit);
    if (m_hoverValid)
        updateHover(m_hoverPos);
}

void TileMapCanvas::applyZoom(double zoom, const QPointF &anchor, bool fit)
{
    if (!m_doc)
        return;
    zoom = std::clamp(zoom, minZoom(), kMaxZoom);
    const QPointF fixed = viewToMap(anchor);
    m_zoom = zoom;
    m_center = QPointF(fixed.x() - (anchor.x() - width() / 2.0) / zoom, fixed.y() - (anchor.y() - height() / 2.0) / zoom);
    m_fit = fit;
    clampCenter();
    emitView();
}

void TileMapCanvas::zoomToFit()
{
    if (!m_doc || width() <= 0 || height() <= 0)
        return;
    const QRectF b = m_doc->bounds;
    constexpr double kMargin = 16.0;
    const double fit = std::min((width() - 2 * kMargin) / b.width(), (height() - 2 * kMargin) / b.height());
    m_zoom = std::clamp(fit, minZoom(), kMaxZoom);
    // Small maps open on a whole zoom (crisp pixels), big ones fit.
    if (fit >= 1.0 && !m_viewInitialised)
        m_zoom = std::clamp(std::floor(fit), 1.0, 4.0);
    m_center = b.center();
    m_fit = true;
    m_viewInitialised = true;
    clampCenter();
    emitView();
}

void TileMapCanvas::zoomToActual()
{
    applyZoom(1.0, QPointF(width() / 2.0, height() / 2.0), false);
}

void TileMapCanvas::zoomIn()
{
    applyZoom(m_zoom * 1.5, QPointF(width() / 2.0, height() / 2.0), false);
}

void TileMapCanvas::zoomOut()
{
    applyZoom(m_zoom / 1.5, QPointF(width() / 2.0, height() / 2.0), false);
}

void TileMapCanvas::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (!m_doc)
        return;
    if (m_fit)
        zoomToFit();
    else {
        clampCenter();
        update();
    }
}

int TileMapCanvas::levelForZoom() const
{
    if (m_zoom >= 1.0)
        return 0;
    return std::clamp(int(std::floor(std::log2(1.0 / m_zoom) + 1e-9)), 0, kMaxLevel);
}

// ------------------------------------------------------------------ chunks

quint64 TileMapCanvas::chunkKey(int layer, int level, int cx, int cy)
{
    constexpr qint64 kBias = qint64(1) << 20;
    const quint64 ux = quint64(qint64(cx) + kBias) & 0x1FFFFF;
    const quint64 uy = quint64(qint64(cy) + kBias) & 0x1FFFFF;
    return (quint64(layer) << 48) | (quint64(level) << 42) | (ux << 21) | uy;
}

const QImage &TileMapCanvas::levelImage(size_t tileset, int level)
{
    QImage &img = m_tilesetCache[tileset].level[level - 1];
    if (img.isNull()) {
        const QImage &src = m_doc->tilesets[tileset].image;
        img = src.scaled(std::max(1, src.width() >> level), std::max(1, src.height() >> level), Qt::IgnoreAspectRatio,
                         Qt::SmoothTransformation);
    }
    return img;
}

uint32_t TileMapCanvas::averageColor(size_t tileset, int tile)
{
    const Tileset &ts = m_doc->tilesets[tileset];
    TilesetCache &cache = m_tilesetCache[tileset];
    const size_t count = size_t(std::max(ts.tileCount, int(ts.tiles.size())));
    if (tile < 0 || size_t(tile) >= count)
        return 0xA0808080u;
    if (cache.average.size() != count) {
        cache.average.assign(count, 0);
        cache.averageDone.assign(count, 0);
    }
    if (cache.averageDone[size_t(tile)])
        return cache.average[size_t(tile)];

    const QImage *img = nullptr;
    QRect r;
    if (ts.isCollection()) {
        img = ts.collectionImage(tile);
        if (img)
            r = img->rect();
    } else {
        img = &ts.image;
        r = ts.sheetRect(tile);
    }
    uint32_t out = 0;
    if (img && !r.isEmpty()) {
        quint64 sa = 0, sr = 0, sg = 0, sb = 0;
        for (int y = r.top(); y <= r.bottom(); ++y) {
            const auto *line = reinterpret_cast<const QRgb *>(img->constScanLine(y));
            for (int x = r.left(); x <= r.right(); ++x) {
                const QRgb px = line[x];
                sa += qAlpha(px);
                sr += qRed(px);
                sg += qGreen(px);
                sb += qBlue(px);
            }
        }
        const quint64 n = quint64(r.width()) * quint64(r.height());
        out = qUnpremultiply(qRgba(int(sr / n), int(sg / n), int(sb / n), int(sa / n)));
    }
    cache.average[size_t(tile)] = out;
    cache.averageDone[size_t(tile)] = 1;
    return out;
}

void TileMapCanvas::drawChunkInstance(QPainter &p, const Instance &in, int level)
{
    const QRectF dest(in.x, in.y, in.w, in.h);
    if (in.tileset < 0) {
        p.fillRect(dest, QColor::fromRgba(QRgb(uint32_t(in.tile))));
        return;
    }
    const size_t tsi = size_t(in.tileset);
    if (level >= 3) {
        p.fillRect(dest, QColor::fromRgba(averageColor(tsi, in.tile)));
        return;
    }
    const Tileset &ts = m_doc->tilesets[tsi];
    const QImage *img = nullptr;
    QRectF src;
    if (ts.isCollection()) {
        img = ts.collectionImage(in.tile);
        if (!img)
            return;
        src = QRectF(img->rect());
    } else {
        const QRect r = ts.sheetRect(in.tile);
        if (r.isEmpty())
            return;
        if (level == 0) {
            img = &ts.image;
            src = QRectF(r);
        } else {
            img = &levelImage(tsi, level);
            const double s = 1.0 / double(1 << level);
            src = QRectF(r.x() * s, r.y() * s, r.width() * s, r.height() * s);
        }
    }
    if (in.flags & (FlipH | FlipV | FlipD))
        drawImageFlipped(p, dest, *img, src, in.flags);
    else
        p.drawImage(dest, *img, src);
}

void TileMapCanvas::renderChunk(Chunk &chunk, const Layer &layer, int level, int cx, int cy)
{
    const double unit = double(1 << level);
    const double ext = kChunkPx * unit;
    const double ox = cx * ext, oy = cy * ext;
    const double pad = kChunkPad * unit;
    const QRectF area(ox - pad, oy - pad, ext + 2 * pad, ext + 2 * pad);

    m_scratch.clear();
    layer.grid.query(area, m_scratch);
    chunk.image = QImage();
    if (m_scratch.empty())
        return;

    QImage img(kChunkImage, kChunkImage, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::SmoothPixmapTransform, level > 0);
    p.scale(1.0 / unit, 1.0 / unit);
    p.translate(-area.left(), -area.top());

    bool any = false;
    for (const uint32_t idx : m_scratch) {
        const Instance &in = layer.instances[idx];
        if (in.flags & Animated)
            continue;
        if (in.x >= area.right() || in.y >= area.bottom() || in.x + in.w <= area.left() || in.y + in.h <= area.top())
            continue;
        drawChunkInstance(p, in, level);
        any = true;
    }
    p.end();
    if (any)
        chunk.image = std::move(img);
}

bool TileMapCanvas::ensureChunk(int layerIndex, int level, int cx, int cy)
{
    const quint64 key = chunkKey(layerIndex, level, cx, cy);
    auto it = m_chunks.find(key);
    if (it != m_chunks.end()) {
        it->used = m_tick;
        return false;
    }
    Chunk chunk;
    chunk.used = m_tick;
    renderChunk(chunk, m_doc->layers[size_t(layerIndex)], level, cx, cy);
    m_chunkBytes += chunk.image.sizeInBytes();
    m_chunks.insert(key, std::move(chunk));
    return true;
}

void TileMapCanvas::evictChunks()
{
    // Chunks touched by the current paint stay; the rest go oldest first.
    std::vector<std::pair<quint64, quint64>> candidates;   // (used, key)
    candidates.reserve(size_t(m_chunks.size()));
    for (auto it = m_chunks.constBegin(); it != m_chunks.constEnd(); ++it) {
        if (it->used < m_tick)
            candidates.emplace_back(it->used, it.key());
    }
    std::sort(candidates.begin(), candidates.end());
    const qint64 target = kCacheBytes / 4 * 3;
    for (const auto &c : candidates) {
        if (m_chunkBytes <= target)
            break;
        const auto it = m_chunks.constFind(c.second);
        if (it == m_chunks.constEnd())
            continue;
        m_chunkBytes -= it->image.sizeInBytes();
        m_chunks.erase(it);
    }
}

// ---------------------------------------------------------------- painting

void TileMapCanvas::drawTile(QPainter &p, const Instance &in, int tile, bool smooth)
{
    const QRectF dest(in.x, in.y, in.w, in.h);
    if (in.tileset < 0) {
        p.fillRect(dest, QColor::fromRgba(QRgb(uint32_t(in.tile))));
        return;
    }
    const Tileset &ts = m_doc->tilesets[size_t(in.tileset)];
    const QImage *img = nullptr;
    QRectF src;
    if (ts.isCollection()) {
        img = ts.collectionImage(tile);
        if (!img)
            return;
        src = QRectF(img->rect());
    } else {
        const QRect r = ts.sheetRect(tile);
        if (r.isEmpty())
            return;
        img = &ts.image;
        src = QRectF(r);
    }
    p.setRenderHint(QPainter::SmoothPixmapTransform, smooth);
    if (in.flags & (FlipH | FlipV | FlipD))
        drawImageFlipped(p, dest, *img, src, in.flags);
    else
        p.drawImage(dest, *img, src);
}

void TileMapCanvas::drawLiveTiles(QPainter &p, const Layer &layer, const QRectF &visible, qint64 timeMs)
{
    if (layer.animated.empty())
        return;
    const bool smooth = m_zoom < 1.0;
    int drawn = 0;
    for (const uint32_t idx : layer.animated) {
        const Instance &in = layer.instances[idx];
        if (in.x >= visible.right() || in.y >= visible.bottom() || in.x + in.w <= visible.left()
            || in.y + in.h <= visible.top())
            continue;
        if (++drawn > kMaxLiveAnimated)
            break;
        int tile = in.tile;
        if (in.tileset >= 0) {
            const Tileset &ts = m_doc->tilesets[size_t(in.tileset)];
            const auto it = ts.animations.constFind(in.tile);
            if (it != ts.animations.constEnd() && !it->empty()) {
                const int total = ts.animationTotalMs(in.tile);
                qint64 t = total > 0 ? timeMs % total : 0;
                for (const AnimFrame &f : *it) {
                    tile = f.tile;
                    t -= std::max(1, f.durationMs);
                    if (t < 0)
                        break;
                }
            }
        }
        drawTile(p, in, tile, smooth);
    }
}

void TileMapCanvas::drawObjects(QPainter &p, const Layer &layer, const QRectF &visible)
{
    const double margin = 12.0 / m_zoom;
    const bool labels = m_zoom >= 0.4;
    QFont labelFont = font();
    labelFont.setPixelSize(11);

    for (const Object &o : layer.objects) {
        if (!o.visible)
            continue;
        QRectF bounds;
        switch (o.shape) {
        case Object::Shape::Polygon:
        case Object::Shape::Polyline:
            bounds = o.points.translated(o.pos).boundingRect();
            break;
        case Object::Shape::Point:
            bounds = QRectF(o.pos, QSizeF(0, 0));
            break;
        case Object::Shape::Tile:
            bounds = QRectF(o.tile.x, o.tile.y, o.tile.w, o.tile.h);
            break;
        default:
            bounds = QRectF(o.pos, o.size);
            break;
        }
        const double reach = std::hypot(bounds.width(), bounds.height()) * (o.rotation != 0 ? 1.0 : 0.0);
        if (!visible.intersects(bounds.normalized().adjusted(-margin - reach, -margin - reach, margin + reach, margin + reach)))
            continue;

        QColor color = o.color.isValid() ? o.color : (layer.color.isValid() ? layer.color : m_objectColor);
        QPen pen(color, 1.5);
        pen.setCosmetic(true);
        QColor fill = color;
        fill.setAlpha(40);

        p.setPen(pen);
        p.setBrush(fill);
        p.setRenderHint(QPainter::Antialiasing, true);
        switch (o.shape) {
        case Object::Shape::Rect:
        case Object::Shape::Ellipse:
            p.save();
            p.translate(o.pos);
            p.rotate(o.rotation);
            if (o.shape == Object::Shape::Rect)
                p.drawRect(QRectF(QPointF(0, 0), o.size));
            else
                p.drawEllipse(QRectF(QPointF(0, 0), o.size));
            p.restore();
            break;
        case Object::Shape::Polygon:
        case Object::Shape::Polyline:
            p.save();
            p.translate(o.pos);
            p.rotate(o.rotation);
            if (o.shape == Object::Shape::Polygon)
                p.drawPolygon(o.points);
            else
                p.drawPolyline(o.points);
            p.restore();
            break;
        case Object::Shape::Tile:
            p.setRenderHint(QPainter::Antialiasing, false);
            drawTile(p, o.tile, o.tile.tile, m_zoom < 1.0);
            break;
        case Object::Shape::Point:
        case Object::Shape::Text:
            break;
        }

        // View-space extras: point marker, text body, name label.
        p.save();
        p.resetTransform();
        const QPointF v = mapToView(o.shape == Object::Shape::Polygon || o.shape == Object::Shape::Polyline
                                        ? bounds.topLeft() : o.pos);
        if (o.shape == Object::Shape::Point) {
            p.drawEllipse(v, 4.0, 4.0);
        } else if (o.shape == Object::Shape::Text) {
            p.setPen(color);
            p.setBrush(Qt::NoBrush);
            p.setFont(labelFont);
            p.drawText(QRectF(v, QSizeF(std::max(40.0, o.size.width() * m_zoom), std::max(16.0, o.size.height() * m_zoom))),
                       Qt::TextWordWrap, o.text);
        }
        if (labels && !o.name.isEmpty()) {
            p.setRenderHint(QPainter::Antialiasing, false);
            p.setPen(color);
            p.setFont(labelFont);
            p.drawText(v + QPointF(3, -3), o.name);
        }
        p.restore();
    }
}

void TileMapCanvas::drawGrid(QPainter &p)
{
    if (!m_doc || m_doc->gridW <= 0 || m_doc->gridH <= 0)
        return;
    const double sx = m_doc->gridW * m_zoom, sy = m_doc->gridH * m_zoom;
    if (sx < kMinGridPx || sy < kMinGridPx)
        return;
    const QRectF vis = visibleMapRect();
    QVector<QLineF> lines;
    const int x0 = int(std::floor(vis.left() / m_doc->gridW)), x1 = int(std::ceil(vis.right() / m_doc->gridW));
    const int y0 = int(std::floor(vis.top() / m_doc->gridH)), y1 = int(std::ceil(vis.bottom() / m_doc->gridH));
    lines.reserve((x1 - x0 + 1) + (y1 - y0 + 1));
    for (int x = x0; x <= x1; ++x) {
        const double vx = std::round(mapToView(QPointF(x * m_doc->gridW, 0)).x()) + 0.5;
        lines.append(QLineF(vx, 0, vx, height()));
    }
    for (int y = y0; y <= y1; ++y) {
        const double vy = std::round(mapToView(QPointF(0, y * m_doc->gridH)).y()) + 0.5;
        lines.append(QLineF(0, vy, width(), vy));
    }
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(QPen(m_gridColor, 0));
    p.drawLines(lines);
}

void TileMapCanvas::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), (m_doc && m_doc->background.isValid()) ? m_doc->background : m_background);
    if (!m_doc)
        return;
    ++m_tick;

    const int level = levelForZoom();
    const double ext = std::ldexp(double(kChunkPx), level);
    const QRectF visible = visibleMapRect();
    const qint64 timeMs = (m_animating && m_animClock.isValid()) ? m_animClock.elapsed() : 0;

    // Chunks the view needs, nearest to the centre first.
    const int cx0 = int(std::floor(visible.left() / ext)), cx1 = int(std::floor(visible.right() / ext));
    const int cy0 = int(std::floor(visible.top() / ext)), cy1 = int(std::floor(visible.bottom() / ext));
    std::vector<std::pair<int, int>> wanted;
    if (qint64(cx1 - cx0 + 1) * (cy1 - cy0 + 1) <= 4096) {
        const QPointF mid = m_center;
        for (int cy = cy0; cy <= cy1; ++cy) {
            for (int cx = cx0; cx <= cx1; ++cx)
                wanted.emplace_back(cx, cy);
        }
        std::sort(wanted.begin(), wanted.end(), [&](const auto &a, const auto &b) {
            const double da = std::hypot((a.first + 0.5) * ext - mid.x(), (a.second + 0.5) * ext - mid.y());
            const double db = std::hypot((b.first + 0.5) * ext - mid.x(), (b.second + 0.5) * ext - mid.y());
            return da < db;
        });
    }

    // Pass 1: rasterise missing chunks within the budget.
    bool pending = false;
    int rendered = 0;
    QElapsedTimer budget;
    budget.start();
    for (const auto &c : wanted) {
        for (size_t li = 0; li < m_doc->layers.size(); ++li) {
            const Layer &layer = m_doc->layers[li];
            if (layer.kind != Layer::Kind::Tiles || !m_layerOn[li] || layer.instances.empty())
                continue;
            if (m_chunks.contains(chunkKey(int(li), level, c.first, c.second))) {
                m_chunks[chunkKey(int(li), level, c.first, c.second)].used = m_tick;
                continue;
            }
            if (rendered > 0 && budget.nsecsElapsed() > kPaintBudgetNs) {
                pending = true;
                continue;
            }
            ensureChunk(int(li), level, c.first, c.second);
            ++rendered;
        }
    }

    // Level frames under everything.
    p.setRenderHint(QPainter::Antialiasing, false);
    const bool labelLevels = m_doc->levels.size() > 1;
    for (const Level &lv : m_doc->levels) {
        const QRectF r(mapToView(lv.rect.topLeft()), lv.rect.size() * m_zoom);
        if (!r.intersects(QRectF(rect())))
            continue;
        if (lv.background.isValid())
            p.fillRect(r, lv.background);
        QColor outline = m_gridColor;
        outline.setAlpha(std::max(outline.alpha(), 120));
        p.setPen(QPen(outline, 0));
        p.setBrush(Qt::NoBrush);
        p.drawRect(r.adjusted(0, 0, -1, -1));
        if (labelLevels && r.width() > 60 && !lv.name.isEmpty()) {
            p.setPen(palette().color(QPalette::PlaceholderText));
            p.drawText(r.topLeft() + QPointF(4, 14), lv.name);
        }
    }

    // Pass 2: layers bottom to top.
    QTransform world;
    world.translate(width() / 2.0, height() / 2.0);
    world.scale(m_zoom, m_zoom);
    world.translate(-m_center.x(), -m_center.y());

    for (size_t li = 0; li < m_doc->layers.size(); ++li) {
        const Layer &layer = m_doc->layers[li];
        if (!m_layerOn[li])
            continue;
        p.setOpacity(layer.opacity);
        switch (layer.kind) {
        case Layer::Kind::Tiles: {
            p.resetTransform();
            // Neighbouring chunks overlap by their 1 px border so bilinear
            // scaling leaves no seams; translucent layers skip the overlap.
            const double grow = layer.opacity >= 0.999 ? kChunkPad * std::ldexp(1.0, level) * m_zoom : 0.0;
            p.setRenderHint(QPainter::SmoothPixmapTransform, m_zoom * std::ldexp(1.0, level) < 1.0);
            for (const auto &c : wanted) {
                const auto it = m_chunks.constFind(chunkKey(int(li), level, c.first, c.second));
                if (it == m_chunks.constEnd() || it->image.isNull())
                    continue;
                const QRectF target(mapToView(QPointF(c.first * ext, c.second * ext)), QSizeF(ext * m_zoom, ext * m_zoom));
                p.drawImage(target.adjusted(-grow, -grow, grow, grow), it->image);
            }
            if (!layer.animated.empty() && level <= 2) {
                p.setTransform(world);
                drawLiveTiles(p, layer, visible, timeMs);
                p.resetTransform();
            }
            break;
        }
        case Layer::Kind::Objects:
            if (m_showObjects) {
                p.setTransform(world);
                drawObjects(p, layer, visible);
                p.resetTransform();
            }
            break;
        case Layer::Kind::Image:
            if (!layer.image.isNull()) {
                p.setRenderHint(QPainter::SmoothPixmapTransform, m_zoom < 1.0);
                const QRectF target(mapToView(layer.imagePos), QSizeF(layer.image.size()) * m_zoom);
                if (target.intersects(QRectF(rect())))
                    p.drawImage(target, layer.image);
            }
            break;
        }
    }
    p.setOpacity(1.0);
    p.resetTransform();

    if (m_showGrid)
        drawGrid(p);

    if (m_chunkBytes > kCacheBytes)
        evictChunks();
    if (pending)
        QTimer::singleShot(0, this, qOverload<>(&QWidget::update));
}

// ------------------------------------------------------------------- input

void TileMapCanvas::wheelEvent(QWheelEvent *event)
{
    if (!m_doc)
        return;
    const bool touchpad = !event->pixelDelta().isNull() && !(event->modifiers() & Qt::ControlModifier);
    if (touchpad) {
        m_center -= QPointF(event->pixelDelta()) / m_zoom;
        m_fit = false;
        clampCenter();
        emitView();
    } else {
        const double steps = event->angleDelta().y() / 120.0;
        applyZoom(m_zoom * std::pow(1.25, steps), event->position(), false);
    }
    event->accept();
}

void TileMapCanvas::mousePressEvent(QMouseEvent *event)
{
    setFocus();
    if (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton) {
        m_panning = true;
        m_lastPan = event->position();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void TileMapCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (m_panning && m_doc) {
        const QPointF delta = event->position() - m_lastPan;
        m_lastPan = event->position();
        m_center -= delta / m_zoom;
        m_fit = false;
        clampCenter();
        emitView();
    }
    updateHover(event->position());
}

void TileMapCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_panning && (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton)) {
        m_panning = false;
        unsetCursor();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void TileMapCanvas::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_doc)
        return;
    if (m_fit || std::abs(m_zoom - 1.0) > 1e-9)
        applyZoom(1.0, event->position(), false);
    else
        zoomToFit();
}

void TileMapCanvas::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);
    m_hoverValid = false;
    emit hoverTextChanged(QString());
}

void TileMapCanvas::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Plus:
    case Qt::Key_Equal: zoomIn(); break;
    case Qt::Key_Minus: zoomOut(); break;
    case Qt::Key_0: zoomToFit(); break;
    case Qt::Key_1: zoomToActual(); break;
    default: QWidget::keyPressEvent(event); return;
    }
    event->accept();
}

void TileMapCanvas::updateHover(const QPointF &pos)
{
    m_hoverPos = pos;
    m_hoverValid = true;
    if (!m_doc) {
        emit hoverTextChanged(QString());
        return;
    }
    const QPointF m = viewToMap(pos);
    QString text;
    if (m_doc->gridW > 0 && m_doc->gridH > 0) {
        text = QStringLiteral("%1, %2").arg(int(std::floor(m.x() / m_doc->gridW))).arg(int(std::floor(m.y() / m_doc->gridH)));
    } else {
        text = QStringLiteral("x %1  y %2").arg(int(std::floor(m.x()))).arg(int(std::floor(m.y())));
    }
    const Layer *layer = nullptr;
    if (const Instance *in = hitTest(*m_doc, m_layerOn, m, &layer))
        text += QStringLiteral("  ·  ") + describeInstance(*m_doc, *layer, *in);
    emit hoverTextChanged(text);
}
