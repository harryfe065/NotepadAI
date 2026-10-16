#ifndef IMAGECANVAS_H
#define IMAGECANVAS_H

#include "ImageDocument.h"
#include "ImageViewMath.h"

#include <QHash>
#include <QImage>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <functional>

// Paints one surface of an ImageDocument with zoom and pan, on the CPU.
//
// Per paint the work is bounded by the viewport, never by the image: the
// visible rect is taken from the pyramid level matching the display scale,
// converted once (channel view, exposure, premultiply) into a crop that is
// cached with a margin, and scaled by QPainter. Pyramids are built on the
// global thread pool. SVG documents are re-rasterised for the visible region at
// device resolution when the zoom passes the base raster.
class ImageCanvas : public QWidget
{
    Q_OBJECT

public:
    explicit ImageCanvas(QWidget *parent = nullptr);
    ~ImageCanvas() override;

    // keepView: keep zoom, pan and surface selection when the size matches
    // (file reloads). `initialPyramid` may carry the decoder's pyramid of
    // surface 0 so the first paint needs no extra pass.
    void setDocument(ImageDocumentPtr doc, bool keepView, ImagePyramidPtr initialPyramid = {});
    ImageDocumentPtr document() const { return m_doc; }

    void setSurface(int layer, int face, int mip);
    int layer() const { return m_layer; }
    int face() const { return m_face; }
    int mip() const { return m_mip; }

    void setViewParams(const ImageOps::ViewParams &params);
    ImageOps::ViewParams viewParams() const { return m_params; }
    void setTiled(bool tiled);
    void setCheckerboard(bool on);
    void setPixelGrid(bool on);
    void setColors(const QColor &background, const QColor &checkA, const QColor &checkB);

    // Animation (documents with isAnimation).
    void setPlaying(bool playing);
    bool isPlaying() const { return m_playing; }
    void stepFrame(int delta);

    double zoom() const { return m_math.zoom; }
    bool isFit() const { return m_fit; }

    // Vector overlay painted over the image (atlas frames, glyph boxes). It gets
    // the canvas painter in view coordinates and the current view math
    // (imageToView / visibleImageRect) so it culls to what is on screen. Empty
    // clears it. updateOverlay() repaints after the overlay's own state changed.
    using OverlayPainter = std::function<void(QPainter &, const ImageViewMath &)>;
    void setOverlay(OverlayPainter painter);
    void updateOverlay() { update(); }
    // Centres the view on an image-space rect, zooming in only when it would not fit.
    void revealImageRect(const QRectF &rect);

public slots:
    void zoomIn();
    void zoomOut();
    void zoomToFit();
    void zoomToActual();

signals:
    void zoomChanged(double zoom, bool fit);
    void hoverPixelChanged(const QString &text);
    void frameChanged(int layer);
    void playingChanged(bool playing);
    // Image-space position under the cursor (-1e9 when it left) and the image
    // point of a click (press + release without dragging).
    void imagePointHovered(const QPointF &imagePos);
    void imageClicked(const QPointF &imagePos);

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
    bool event(QEvent *event) override;

private:
    struct Crop {
        int surface = -1;
        int level = -1;
        QRect rect;              // level coordinates
        ImageOps::ViewParams params;
        QImage image;            // ARGB32_Premultiplied
    };
    struct VectorTile {
        QImage image;            // device pixels, premultiplied
        QRectF imageRect;        // image coordinates it covers
        double deviceScale = 0;
    };

    int currentSurfaceIndex() const;
    const QImage *currentSurface() const;
    ImagePyramidPtr pyramidFor(int surfaceIndex);
    void requestPyramid(int surfaceIndex);
    void applyZoom(double zoom, QPointF anchor, bool fit);
    void viewChanged();
    void updateMathGeometry();
    void updateHover(QPointF pos);
    void scheduleFrame();
    void scheduleVectorRender();
    void startVectorRender();
    void paintSurface(QPainter &p, const QImage *level, int levelIndex, double levelScaleX, double levelScaleY);
    QImage wholeLevel(int surfaceIndex, int levelIndex, const QImage &level);
    QRectF tileRect(int i, int j) const;
    QSizeF logicalSize() const;   // one tile, in view units at 100%
    QSizeF contentSize() const;   // logicalSize, x3 when tiled

    ImageDocumentPtr m_doc;
    quint64 m_docGeneration = 0;
    int m_layer = 0, m_face = 0, m_mip = 0;
    ImageOps::ViewParams m_params;
    bool m_tiled = false;
    bool m_checker = true;
    bool m_grid = true;
    QColor m_background, m_checkA, m_checkB;
    QPixmap m_checkTile;

    ImageViewMath m_math;
    bool m_fit = true;
    bool m_viewInitialised = false;

    QHash<int, ImagePyramidPtr> m_pyramids;
    QHash<int, bool> m_pyramidJobs;
    Crop m_crop;

    VectorTile m_vectorTile;
    QTimer m_vectorTimer;
    quint64 m_vectorGeneration = 0;

    QTimer m_frameTimer;
    bool m_playing = false;

    OverlayPainter m_overlay;
    QPointF m_pressPos;

    bool m_panning = false;
    QPointF m_lastPan;
    QPointF m_hoverPos;
    bool m_hoverValid = false;
};

#endif // IMAGECANVAS_H
