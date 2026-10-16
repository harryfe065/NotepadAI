#ifndef TILEMAPCANVAS_H
#define TILEMAPCANVAS_H

#include "TileMapDocument.h"

#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QTimer>
#include <QWidget>

#include <vector>

// Paints a TileMap::Document with zoom and pan on the CPU.
//
// Every tile layer is rasterised into 256 px chunks (258 with a 1 px border,
// so bilinear scaling never shows seams) at the power-of-two level that matches
// the zoom, and chunks live in an LRU cache keyed by (layer, level, cx, cy).
// Panning at a fixed zoom therefore only blits cached chunks and rasterises the
// strip that scrolled into view; a paint rasterises for at most ~8 ms and
// schedules itself again when chunks are still missing, nearest-to-centre first.
//
//   level 0       tiles drawn 1:1 (magnified by the view when zoomed in)
//   level 1, 2    tiles drawn from a 2x / 4x box-downscaled copy of the tileset
//   level >= 3    one rect of the tile's average colour per tile (a tile is
//                 under 2 px there, so the exact pixels do not matter)
//
// Animated tiles are excluded from chunks and drawn live on top of their layer
// (from level <= 2); objects, image layers and the grid are drawn per paint.
class TileMapCanvas : public QWidget
{
    Q_OBJECT

public:
    explicit TileMapCanvas(QWidget *parent = nullptr);
    ~TileMapCanvas() override;

    // keepView: keep zoom, pan and layer toggles (file reloads).
    void setDocument(TileMap::DocumentPtr doc, bool keepView);
    TileMap::DocumentPtr document() const { return m_doc; }

    void setGroupVisible(const QString &group, bool visible);
    bool groupVisible(const QString &group) const;
    void setGridVisible(bool on);
    void setObjectsVisible(bool on);
    void setAnimating(bool on);
    void setColors(const QColor &background, const QColor &grid, const QColor &object);

    double zoom() const { return m_zoom; }
    bool isFit() const { return m_fit; }

public slots:
    void zoomIn();
    void zoomOut();
    void zoomToFit();
    void zoomToActual();

signals:
    void zoomChanged(double zoom, bool fit);
    void hoverTextChanged(const QString &text);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    static constexpr int kChunkPx = 256;
    static constexpr int kChunkPad = 1;
    static constexpr int kChunkImage = kChunkPx + 2 * kChunkPad;
    static constexpr qint64 kCacheBytes = qint64(192) << 20;
    static constexpr int kMaxLevel = 14;

    struct Chunk {
        QImage image;       // null = nothing in this chunk
        quint64 used = 0;
    };
    struct TilesetCache {
        QImage level[2];                 // 2x and 4x box-downscaled sheets
        std::vector<uint32_t> average;   // premultiplied ARGB per tile
        std::vector<uint8_t> averageDone;
    };

    static quint64 chunkKey(int layer, int level, int cx, int cy);
    int levelForZoom() const;
    QPointF mapToView(const QPointF &p) const;
    QPointF viewToMap(const QPointF &p) const;
    QRectF visibleMapRect() const;
    void applyZoom(double zoom, const QPointF &anchor, bool fit);
    void clampCenter();
    void emitView();
    void resetCaches();
    double minZoom() const;

    bool ensureChunk(int layerIndex, int level, int cx, int cy);
    void renderChunk(Chunk &chunk, const TileMap::Layer &layer, int level, int cx, int cy);
    void drawChunkInstance(QPainter &p, const TileMap::Instance &in, int level);
    const QImage &levelImage(size_t tileset, int level);
    uint32_t averageColor(size_t tileset, int tile);
    void evictChunks();

    void drawLiveTiles(QPainter &p, const TileMap::Layer &layer, const QRectF &visible, qint64 timeMs);
    void drawTile(QPainter &p, const TileMap::Instance &in, int tileOverride, bool smooth);
    void drawObjects(QPainter &p, const TileMap::Layer &layer, const QRectF &visible);
    void drawGrid(QPainter &p);
    void updateHover(const QPointF &pos);
    void updateAnimationTimer();

    TileMap::DocumentPtr m_doc;
    std::vector<bool> m_layerOn;
    QHash<QString, bool> m_groupOverride;
    bool m_showGrid = true;
    bool m_showObjects = true;
    bool m_animating = false;
    QColor m_background, m_gridColor, m_objectColor;

    double m_zoom = 1.0;
    QPointF m_center;
    bool m_fit = true;
    bool m_viewInitialised = false;

    QHash<quint64, Chunk> m_chunks;
    qint64 m_chunkBytes = 0;
    quint64 m_tick = 0;
    std::vector<TilesetCache> m_tilesetCache;
    std::vector<uint32_t> m_scratch;

    QTimer m_animTimer;
    QElapsedTimer m_animClock;

    bool m_panning = false;
    QPointF m_lastPan;
    QPointF m_hoverPos;
    bool m_hoverValid = false;
};

#endif // TILEMAPCANVAS_H
