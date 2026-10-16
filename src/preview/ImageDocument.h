#ifndef IMAGEDOCUMENT_H
#define IMAGEDOCUMENT_H

#include <QByteArray>
#include <QImage>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>
#include <vector>

// A decoded image file: one or more 2D surfaces addressed by
// (layer, face, mip). Layers double as animation frames (with delays), faces
// as cubemap sides, mips as stored mip levels (DDS/KTX).
//
// LDR surfaces are QImage::Format_ARGB32 with STRAIGHT alpha, so channel views
// can show colour under alpha 0 (packed masks). HDR surfaces are
// Format_RGBA16FPx4, linear, straight alpha. Immutable once published; shared
// across threads through ImageDocumentPtr.
struct ImageDocument
{
    QString format;    // "PNG", "DDS", ...
    QString detail;    // "8-bit RGBA", "BC7 sRGB", ...
    qint64 fileSize = 0;
    int width = 0;
    int height = 0;
    int mipCount = 1;
    int faceCount = 1;
    int layerCount = 1;
    bool hasAlpha = false;
    bool isHdr = false;
    bool isAnimation = false;     // layers are frames
    QVector<int> frameDelaysMs;   // per layer, when isAnimation
    QStringList faceNames;        // per face, when faceCount > 1
    QStringList layerNames;       // optional: array/volume slice names; for animations the frame's tags

    // Vector source (SVG): surfaces[0] is a raster at rasterScale × the
    // intrinsic size; the canvas re-rasterises the visible part when zoomed
    // past it.
    QByteArray vectorData;
    double rasterScale = 1.0;

    std::vector<QImage> surfaces; // (layer * faceCount + face) * mipCount + mip

    int surfaceIndex(int layer, int face, int mip) const
    {
        return (layer * faceCount + face) * mipCount + mip;
    }
    const QImage &surface(int layer, int face, int mip) const
    {
        return surfaces[static_cast<size_t>(surfaceIndex(layer, face, mip))];
    }
    bool isVector() const { return !vectorData.isEmpty(); }
    qint64 decodedBytes() const;
};

using ImageDocumentPtr = std::shared_ptr<const ImageDocument>;

// Halving levels of one surface: levels[0] is the surface itself (shared, no
// copy), each next level is a 2x2 box filter of the previous one, alpha-
// weighted so transparent texels do not darken edges. Stops at 32 px.
struct ImagePyramid
{
    std::vector<QImage> levels;
};

using ImagePyramidPtr = std::shared_ptr<const ImagePyramid>;

namespace ImageOps {

enum class Channel : quint8 { Rgba, Rgb, R, G, B, A };

struct ViewParams {
    Channel channel = Channel::Rgba;
    float exposure = 0.0f; // EV stops, HDR only
};

// Runs fn(y0, y1) over [0, rows) split across the global pool when `pixels`
// is large (>= 1M), else inline. The caller takes part, so it is safe to call
// from a pool thread. For decoders that fill rows independently.
void parallelRows(int rows, qint64 pixels, const std::function<void(int, int)> &fn);

// Box-filter halving of an ARGB32 (straight) or RGBA16FPx4 image.
QImage halve(const QImage &src);
ImagePyramidPtr buildPyramid(const QImage &level0);

// Converts `rect` of a surface level into a premultiplied ARGB32 image ready to
// paint: applies the channel view, and for HDR the exposure and sRGB encode.
// Work is proportional to rect's area only.
QImage renderCrop(const QImage &level, const QRect &rect, const ViewParams &params);

// Pixel value at (x, y) of an LDR or HDR surface, as display text.
QString describePixel(const QImage &surface, int x, int y);

} // namespace ImageOps

#endif // IMAGEDOCUMENT_H
