#ifndef IMAGEVIEWMATH_H
#define IMAGEVIEWMATH_H

#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSizeF>

#include <algorithm>
#include <cmath>

// Pure zoom/pan math of the image canvas (logical pixels). `center` is the
// image coordinate shown at the viewport centre; `zoom` is view px per image px.
struct ImageViewMath
{
    static constexpr double kMinZoom = 1.0 / 64.0;
    static constexpr double kMaxZoom = 256.0;

    QSizeF viewport;
    QSizeF image;
    double zoom = 1.0;
    QPointF center;

    // Scale that fits the whole image, never below kMinZoom.
    static double fitZoom(QSizeF viewport, QSizeF image)
    {
        if (image.isEmpty() || viewport.isEmpty())
            return 1.0;
        return std::clamp(std::min(viewport.width() / image.width(), viewport.height() / image.height()),
                          kMinZoom, kMaxZoom);
    }

    // First-open zoom: fit when the image is larger than the viewport, else the
    // largest whole zoom that still fits (crisp pixel art), at most 8x.
    static double initialZoom(QSizeF viewport, QSizeF image)
    {
        const double fit = fitZoom(viewport, image);
        if (fit < 1.0)
            return fit;
        return std::clamp(std::floor(fit), 1.0, 8.0);
    }

    QPointF imageToView(QPointF p) const
    {
        return {(p.x() - center.x()) * zoom + viewport.width() / 2.0,
                (p.y() - center.y()) * zoom + viewport.height() / 2.0};
    }

    QPointF viewToImage(QPointF v) const
    {
        return {(v.x() - viewport.width() / 2.0) / zoom + center.x(),
                (v.y() - viewport.height() / 2.0) / zoom + center.y()};
    }

    QRectF imageRectInView() const
    {
        return {imageToView({0, 0}), QSizeF(image.width() * zoom, image.height() * zoom)};
    }

    // Zoom to `newZoom` keeping the image point under `anchor` (view coords) fixed.
    void zoomAt(QPointF anchor, double newZoom)
    {
        newZoom = std::clamp(newZoom, kMinZoom, kMaxZoom);
        const QPointF fixed = viewToImage(anchor);
        zoom = newZoom;
        center = {fixed.x() - (anchor.x() - viewport.width() / 2.0) / zoom,
                  fixed.y() - (anchor.y() - viewport.height() / 2.0) / zoom};
        clampCenter();
    }

    void panBy(QPointF viewDelta)
    {
        center -= QPointF(viewDelta.x() / zoom, viewDelta.y() / zoom);
        clampCenter();
    }

    // On an axis the image overflows, the view never scrolls past its edge; on
    // an axis where it fits, it stays centred.
    void clampCenter()
    {
        auto clampAxis = [this](double c, double imageLen, double viewLen) {
            const double shown = viewLen / zoom;
            if (imageLen <= shown)
                return imageLen / 2.0;
            const double half = shown / 2.0;
            return std::clamp(c, half, imageLen - half);
        };
        center.setX(clampAxis(center.x(), image.width(), viewport.width()));
        center.setY(clampAxis(center.y(), image.height(), viewport.height()));
    }

    // Source rect (image px) visible in the viewport, clipped to the image.
    QRectF visibleImageRect() const
    {
        const QPointF tl = viewToImage({0, 0});
        const QPointF br = viewToImage({viewport.width(), viewport.height()});
        return QRectF(tl, br).intersected(QRectF(QPointF(0, 0), image));
    }

    // Pyramid level for a device scale (device px per image px): the smallest
    // level that still has at least one texel per device pixel.
    static int levelFor(double deviceScale, int levelCount)
    {
        if (deviceScale >= 1.0 || levelCount <= 1)
            return 0;
        const int level = static_cast<int>(std::floor(std::log2(1.0 / deviceScale)));
        return std::clamp(level, 0, levelCount - 1);
    }
};

#endif // IMAGEVIEWMATH_H
