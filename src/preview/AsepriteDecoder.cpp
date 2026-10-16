#include "AsepriteDecoder.h"

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"

#include <QHash>
#include <QObject>
#include <QPainter>
#include <QtEndian>

#include <algorithm>
#include <limits>
#include <vector>

// File format: https://github.com/aseprite/aseprite/blob/main/docs/ase-file-specs.md
namespace AsepriteDecoder {

namespace {

constexpr quint16 kFileMagic = 0xA5E0;
constexpr quint16 kFrameMagic = 0xF1FA;
constexpr int kHeaderSize = 128;

constexpr quint16 kChunkOldPalette = 0x0004;
constexpr quint16 kChunkOldPalette64 = 0x0011;
constexpr quint16 kChunkLayer = 0x2004;
constexpr quint16 kChunkCel = 0x2005;
constexpr quint16 kChunkTags = 0x2018;
constexpr quint16 kChunkPalette = 0x2019;
constexpr quint16 kChunkTileset = 0x2023;

constexpr quint32 kHeaderLayerOpacityValid = 1;
constexpr quint32 kHeaderGroupOpacityValid = 2;
constexpr quint32 kHeaderLayerUuid = 4;

constexpr quint16 kLayerVisible = 1;
constexpr quint16 kLayerBackground = 8;
constexpr quint16 kLayerReference = 64; // tracing aid, never exported

// Tag names are shown in the frame label: one name is cut at this length and
// all frame labels together stop growing past kMaxTagChars.
constexpr int kMaxTagNameChars = 256;
constexpr qint64 kMaxTagChars = qint64(1) << 20;

static_assert(quint64(ImageDecoder::kMaxDecodedBytes) <= std::numeric_limits<mz_ulong>::max(),
              "inflate lengths must fit miniz's length type");

using ImageDecoder::addSat;
using ImageDecoder::mulSat;

// Bounds-checked little-endian reader; any overrun clears `ok` and reads 0.
struct Reader {
    const uchar *p = nullptr;
    qsizetype size = 0;
    qsizetype pos = 0;
    bool ok = true;

    bool need(qsizetype n)
    {
        if (!ok || n < 0 || pos + n > size) {
            ok = false;
            return false;
        }
        return true;
    }
    quint8 u8() { return need(1) ? p[pos++] : 0; }
    quint16 u16()
    {
        if (!need(2)) return 0;
        const quint16 v = qFromLittleEndian<quint16>(p + pos);
        pos += 2;
        return v;
    }
    qint16 s16() { return qint16(u16()); }
    quint32 u32()
    {
        if (!need(4)) return 0;
        const quint32 v = qFromLittleEndian<quint32>(p + pos);
        pos += 4;
        return v;
    }
    void skip(qsizetype n)
    {
        if (need(n)) pos += n;
    }
    QString string()
    {
        const quint16 len = u16();
        if (!need(len)) return QString();
        const QString s = QString::fromUtf8(reinterpret_cast<const char *>(p + pos), len);
        pos += len;
        return s;
    }
};

struct Header {
    quint16 frames = 0;
    int width = 0, height = 0;
    int depth = 32; // bits per pixel: 32 RGBA, 16 gray+alpha, 8 indexed
    quint32 flags = 0;
    quint8 transparentIndex = 0;
};

bool readHeader(QByteArrayView data, Header &h)
{
    if (data.size() < kHeaderSize)
        return false;
    Reader r{reinterpret_cast<const uchar *>(data.data()), data.size()};
    r.u32();
    if (r.u16() != kFileMagic)
        return false;
    h.frames = r.u16();
    h.width = r.u16();
    h.height = r.u16();
    h.depth = r.u16();
    h.flags = r.u32();
    r.u16(); // speed (deprecated)
    r.skip(8);
    h.transparentIndex = r.u8();
    return r.ok && h.width > 0 && h.height > 0 && h.frames > 0 && (h.depth == 32 || h.depth == 16 || h.depth == 8);
}

// The composed frames (ARGB32, all of them); 16-bit fields, cannot overflow.
qint64 framesBytes(const Header &h)
{
    return qint64(h.width) * h.height * 4 * h.frames;
}

struct Layer {
    quint16 flags = 0;
    quint16 type = 0; // 0 image, 1 group, 2 tilemap
    quint16 childLevel = 0;
    quint16 blendMode = 0;
    quint8 opacity = 255;
    quint32 tileset = 0;
    int parent = -1;
    bool effectiveVisible = true;
    int effectiveOpacity = 255;
};

struct Cel {
    QImage image; // ARGB32_Premultiplied
    int x = 0, y = 0;
    int opacity = 255;
    int zIndex = 0;
    int layer = 0;
};

struct Tileset {
    int tileW = 0, tileH = 0, count = 0;
    bool zeroIsEmpty = true;
    QImage tiles; // ARGB32_Premultiplied, tileW x (tileH * count)
};

int bytesPerPixel(int depth) { return depth / 8; }

// Raw sprite pixels (depth-dependent) to premultiplied ARGB32.
QImage toImage(const uchar *src, int w, int h, int depth, const std::vector<quint32> &palette,
               int transparentIndex, bool background)
{
    QImage straight(w, h, QImage::Format_ARGB32);
    if (straight.isNull()) return straight;
    for (int y = 0; y < h; ++y) {
        auto *d = reinterpret_cast<quint32 *>(straight.scanLine(y));
        if (depth == 32) {
            const uchar *s = src + qsizetype(y) * w * 4;
            for (int x = 0; x < w; ++x, s += 4)
                d[x] = (quint32(s[3]) << 24) | (quint32(s[0]) << 16) | (quint32(s[1]) << 8) | s[2];
        } else if (depth == 16) {
            const uchar *s = src + qsizetype(y) * w * 2;
            for (int x = 0; x < w; ++x, s += 2)
                d[x] = (quint32(s[1]) << 24) | (quint32(s[0]) << 16) | (quint32(s[0]) << 8) | s[0];
        } else {
            const uchar *s = src + qsizetype(y) * w;
            for (int x = 0; x < w; ++x) {
                const int i = s[x];
                if (i == transparentIndex && !background)
                    d[x] = 0;
                else
                    d[x] = i < int(palette.size()) ? palette[size_t(i)] : 0xff000000u;
            }
        }
    }
    // In place: no second full-size buffer.
    return std::move(straight).convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

// Inflates exactly `expected` bytes into `out`. Callers check `expected`
// against the memory budget; anything non-positive or past the cap (so past
// miniz's length type) is refused rather than truncated.
bool inflateTo(const uchar *src, qsizetype srcLen, QByteArray &out, qint64 expected)
{
    if (expected <= 0 || expected > ImageDecoder::kMaxDecodedBytes || srcLen < 0
        || quint64(srcLen) > std::numeric_limits<mz_ulong>::max())
        return false;
    out.resize(qsizetype(expected));
    if (out.size() != expected)
        return false;
    mz_ulong len = mz_ulong(out.size());
    const int rc = mz_uncompress(reinterpret_cast<unsigned char *>(out.data()), &len, src, mz_ulong(srcLen));
    // A cel stream may end a few bytes short of a full image in old files;
    // accept that (rest stays zero) but not a corrupt stream.
    if (rc != MZ_OK && rc != MZ_BUF_ERROR)
        return false;
    if (qsizetype(len) < out.size())
        std::fill(out.begin() + qsizetype(len), out.end(), '\0');
    return true;
}

QPainter::CompositionMode compositionFor(quint16 blend)
{
    switch (blend) {
    case 1: return QPainter::CompositionMode_Multiply;
    case 2: return QPainter::CompositionMode_Screen;
    case 3: return QPainter::CompositionMode_Overlay;
    case 4: return QPainter::CompositionMode_Darken;
    case 5: return QPainter::CompositionMode_Lighten;
    case 6: return QPainter::CompositionMode_ColorDodge;
    case 7: return QPainter::CompositionMode_ColorBurn;
    case 8: return QPainter::CompositionMode_HardLight;
    case 9: return QPainter::CompositionMode_SoftLight;
    case 10: return QPainter::CompositionMode_Difference;
    case 11: return QPainter::CompositionMode_Exclusion;
    case 16: return QPainter::CompositionMode_Plus;
    // Subtract and Divide are drawn by blendSeparable; Hue / Saturation /
    // Color / Luminosity (non-separable) have no raster-engine equivalent and
    // are drawn as Normal.
    default: return QPainter::CompositionMode_SourceOver;
    }
}

constexpr quint16 kBlendSubtract = 17;
constexpr quint16 kBlendDivide = 18;

// Separable blend modes QPainter lacks (W3C compositing: the blended colour
// B(Cb, Cs) mixes in by the backdrop alpha, then source-over). `canvas` and
// `src` are premultiplied ARGB32.
void blendSeparable(QImage &canvas, const QImage &src, int ox, int oy, int opacity, quint16 mode)
{
    const QRect area = QRect(ox, oy, src.width(), src.height()) & canvas.rect();
    for (int y = area.top(); y <= area.bottom(); ++y) {
        auto *d = reinterpret_cast<quint32 *>(canvas.scanLine(y));
        const auto *s = reinterpret_cast<const quint32 *>(src.constScanLine(y - oy));
        for (int x = area.left(); x <= area.right(); ++x) {
            const QRgb sp = s[x - ox];
            const int sa = qAlpha(sp) * opacity / 255;
            if (sa == 0)
                continue;
            const QRgb dp = d[x];
            const int da = qAlpha(dp);
            int out[3];
            for (int ch = 0; ch < 3; ++ch) {
                const int shift = 16 - ch * 8;
                // Unpremultiplied channels, 0..255.
                const int cs = qAlpha(sp) ? int((sp >> shift) & 0xFF) * 255 / qAlpha(sp) : 0;
                const int cb = da ? int((dp >> shift) & 0xFF) * 255 / da : 0;
                const int b = mode == kBlendSubtract ? std::max(0, cb - cs)
                                                     : (cs == 0 ? 255 : std::min(255, cb * 255 / cs));
                const int mixed = ((255 - da) * cs + da * b) / 255;         // Cs' = (1-ab)Cs + ab B
                // Source-over in premultiplied form.
                out[ch] = (mixed * sa + int((dp >> shift) & 0xFF) * (255 - sa)) / 255;
            }
            const int a = sa + da * (255 - sa) / 255;
            d[x] = qRgba(out[0], out[1], out[2], a);
        }
    }
}

// Tilemap cel to an image: tiles blitted with their x/y/diagonal flips.
// `ids` is the inflated little-endian 32-bit tile array (cols x rows); the
// caller has checked that the output fits the memory budget.
QImage renderTilemap(const uchar *ids, int cols, int rows, const Tileset &ts, quint32 idMask, quint32 xFlip,
                     quint32 yFlip, quint32 dFlip)
{
    const int tw = ts.tileW, th = ts.tileH;
    QImage out(cols * tw, rows * th, QImage::Format_ARGB32_Premultiplied);
    if (out.isNull()) return out;
    out.fill(0);
    for (int ty = 0; ty < rows; ++ty) {
        for (int tx = 0; tx < cols; ++tx) {
            const quint32 v = qFromLittleEndian<quint32>(ids + 4 * (qsizetype(ty) * cols + tx));
            const quint32 id = v & idMask;
            if ((ts.zeroIsEmpty && id == 0) || (!ts.zeroIsEmpty && v == 0xffffffffu) || id >= quint32(ts.count))
                continue;
            const bool fx = v & xFlip, fy = v & yFlip, fd = v & dFlip;
            for (int y = 0; y < th; ++y) {
                auto *d = reinterpret_cast<quint32 *>(out.scanLine(ty * th + y)) + tx * tw;
                for (int x = 0; x < tw; ++x) {
                    int sx = x, sy = y;
                    if (fd) std::swap(sx, sy);
                    if (fx) sx = tw - 1 - sx;
                    if (fy) sy = th - 1 - sy;
                    if (sx >= tw || sy >= th) continue; // non-square tiles under a diagonal flip
                    d[x] = reinterpret_cast<const quint32 *>(ts.tiles.constScanLine(int(id) * th + sy))[sx];
                }
            }
        }
    }
    return out;
}

} // namespace

bool looksLikeAseprite(QByteArrayView head)
{
    return head.size() >= 6 && qFromLittleEndian<quint16>(head.data() + 4) == kFileMagic;
}

qint64 estimate(QByteArrayView data)
{
    Header h;
    if (!readHeader(data, h))
        return -1;
    return framesBytes(h);
}

ImageDecoder::Result decode(QByteArrayView data, const std::atomic_bool &cancel)
{
    Header hdr;
    if (!readHeader(data, hdr))
        return ImageDecoder::failed(QObject::tr("Not an Aseprite file."));
    const qint64 needed = framesBytes(hdr);
    if (needed > ImageDecoder::kMaxDecodedBytes)
        return ImageDecoder::tooLarge(needed);

    const auto *base = reinterpret_cast<const uchar *>(data.data());
    std::vector<quint32> palette(256, 0xff000000u);
    std::vector<Layer> layers;
    QHash<quint32, Tileset> tilesets;
    std::vector<std::vector<Cel>> cels(hdr.frames);
    QVector<int> durations(hdr.frames, 100);
    QStringList frameTags; // per frame: the tags it belongs to
    for (int f = 0; f < hdr.frames; ++f)
        frameTags << QString();
    QByteArray scratch;
    // Held until every frame is composed, against the one cap: the finished
    // frames plus every decoded cel and tileset image (4 bytes a pixel).
    qint64 celBytes = needed;
    // Peak once one more image is decoded: what is held, the inflate buffer
    // (it keeps its largest size) and the new image. Saturating.
    auto peakWith = [&](qint64 inflated, qint64 decoded) {
        return addSat(addSat(celBytes, std::max<qint64>(scratch.capacity(), inflated)), decoded);
    };
    qint64 tagChars = 0;
    bool tagsSeen = false;

    auto findCel = [&cels](int frame, int layer) -> const Cel * {
        if (frame < 0 || frame >= int(cels.size())) return nullptr;
        for (const Cel &c : cels[size_t(frame)])
            if (c.layer == layer) return &c;
        return nullptr;
    };

    qsizetype pos = kHeaderSize;
    for (int frame = 0; frame < hdr.frames; ++frame) {
        if (cancel.load(std::memory_order_relaxed))
            return ImageDecoder::cancelled();
        Reader fr{base, data.size(), pos};
        const quint32 frameBytes = fr.u32();
        if (fr.u16() != kFrameMagic || !fr.ok || frameBytes < 16 || pos + qsizetype(frameBytes) > data.size())
            return ImageDecoder::failed(QObject::tr("The Aseprite file is damaged (frame %1).").arg(frame + 1));
        const quint16 oldChunks = fr.u16();
        durations[frame] = fr.u16();
        fr.skip(2);
        const quint32 newChunks = fr.u32();
        const quint32 chunkCount = newChunks ? newChunks : oldChunks;
        const qsizetype frameEnd = pos + frameBytes;
        qsizetype cpos = fr.pos;

        for (quint32 c = 0; c < chunkCount && cpos + 6 <= frameEnd; ++c) {
            Reader ch{base, frameEnd, cpos};
            const quint32 chunkSize = ch.u32();
            const quint16 type = ch.u16();
            if (chunkSize < 6 || cpos + qsizetype(chunkSize) > frameEnd)
                break;
            ch.size = cpos + chunkSize; // reads stay inside this chunk

            switch (type) {
            case kChunkOldPalette:
            case kChunkOldPalette64: {
                const quint16 packets = ch.u16();
                int index = 0;
                for (quint16 p = 0; p < packets && ch.ok; ++p) {
                    index += ch.u8();
                    int count = ch.u8();
                    if (count == 0) count = 256;
                    for (int i = 0; i < count && ch.ok; ++i, ++index) {
                        int r = ch.u8(), g = ch.u8(), b = ch.u8();
                        if (type == kChunkOldPalette64) {
                            r = r * 255 / 63;
                            g = g * 255 / 63;
                            b = b * 255 / 63;
                        }
                        if (index < 256)
                            palette[size_t(index)] = 0xff000000u | (quint32(r) << 16) | (quint32(g) << 8) | quint32(b);
                    }
                }
                break;
            }
            case kChunkPalette: {
                const quint32 newSize = ch.u32();
                const quint32 first = ch.u32(), last = ch.u32();
                ch.skip(8);
                if (newSize > palette.size() && newSize <= 65536)
                    palette.resize(newSize, 0xff000000u);
                for (quint32 i = first; i <= last && ch.ok; ++i) {
                    const quint16 flags = ch.u16();
                    const quint32 r = ch.u8(), g = ch.u8(), b = ch.u8(), a = ch.u8();
                    if (flags & 1) ch.string();
                    if (i < palette.size())
                        palette[i] = (a << 24) | (r << 16) | (g << 8) | b;
                }
                break;
            }
            case kChunkLayer: {
                Layer l;
                l.flags = ch.u16();
                l.type = ch.u16();
                l.childLevel = ch.u16();
                ch.u16();
                ch.u16();
                l.blendMode = ch.u16();
                l.opacity = ch.u8();
                ch.skip(3);
                ch.string();
                if (l.type == 2) l.tileset = ch.u32();
                if (hdr.flags & kHeaderLayerUuid) ch.skip(16);
                // Parent: the nearest earlier layer one level up.
                for (int i = int(layers.size()) - 1; i >= 0; --i) {
                    if (layers[size_t(i)].childLevel + 1 == l.childLevel) {
                        l.parent = i;
                        break;
                    }
                    if (layers[size_t(i)].childLevel < l.childLevel) break;
                }
                const bool layerOpacity = (hdr.flags & kHeaderLayerOpacityValid) != 0;
                const bool groupOpacity = (hdr.flags & kHeaderGroupOpacityValid) != 0;
                l.effectiveVisible = (l.flags & kLayerVisible) && !(l.flags & kLayerReference);
                l.effectiveOpacity = (layerOpacity && (l.type != 1 || groupOpacity)) ? l.opacity : 255;
                if (l.parent >= 0) {
                    const Layer &p = layers[size_t(l.parent)];
                    l.effectiveVisible = l.effectiveVisible && p.effectiveVisible;
                    // Groups composite separately in Aseprite; folding their
                    // opacity into the children is the flat approximation.
                    l.effectiveOpacity = l.effectiveOpacity * p.effectiveOpacity / 255;
                }
                layers.push_back(l);
                break;
            }
            case kChunkTileset: {
                const quint32 id = ch.u32();
                const quint32 flags = ch.u32();
                const quint32 count = ch.u32();
                Tileset ts;
                ts.tileW = ch.u16();
                ts.tileH = ch.u16();
                ch.s16();
                ch.skip(14);
                ch.string();
                ts.zeroIsEmpty = (flags & 4) != 0;
                if (flags & 1) ch.skip(8);
                if (!(flags & 2) || !ch.ok || ts.tileW == 0 || ts.tileH == 0 || count == 0)
                    break;
                const quint32 len = ch.u32();
                // The sheet is tileW x (tileH * count). Within the budget the
                // decoded size bounds every factor, so the sheet height fits
                // an int (and QImage) once the check below passes.
                const qint64 sheetH = qint64(ts.tileH) * count; // 16 x 32 bits
                const qint64 pixels = mulSat(ts.tileW, sheetH);
                const qint64 peak = peakWith(mulSat(pixels, bytesPerPixel(hdr.depth)), mulSat(pixels, 4));
                if (peak > ImageDecoder::kMaxDecodedBytes)
                    return ImageDecoder::tooLarge(peak);
                ts.count = int(count);
                if (!ch.need(len) || !inflateTo(base + ch.pos, len, scratch, pixels * bytesPerPixel(hdr.depth)))
                    break;
                ts.tiles = toImage(reinterpret_cast<const uchar *>(scratch.constData()), ts.tileW, int(sheetH),
                                   hdr.depth, palette, hdr.transparentIndex, false);
                if (ts.tiles.isNull())
                    return ImageDecoder::tooLarge(peak);
                if (const auto old = tilesets.constFind(id); old != tilesets.constEnd())
                    celBytes -= old->tiles.sizeInBytes();
                celBytes += ts.tiles.sizeInBytes();
                tilesets.insert(id, ts);
                break;
            }
            case kChunkTags: {
                // Aseprite writes the tags once (first frame); a file repeating
                // them in every frame would only multiply the labels.
                if (tagsSeen)
                    break;
                tagsSeen = true;
                const quint16 count = ch.u16();
                ch.skip(8);
                for (quint16 t = 0; t < count && ch.ok && tagChars < kMaxTagChars; ++t) {
                    const int from = ch.u16(), to = ch.u16();
                    ch.u8();
                    ch.u16();
                    ch.skip(6 + 3 + 1);
                    const QString name = ch.string().left(kMaxTagNameChars);
                    if (name.isEmpty())
                        continue;
                    // Every pass counts at least 3 chars, so the budget also
                    // bounds the loop (a tag can span 65535 frames).
                    const int last = std::min(to, int(hdr.frames) - 1);
                    for (int f = from; f <= last && tagChars < kMaxTagChars; ++f) {
                        QString &label = frameTags[f];
                        if (!label.isEmpty())
                            label += QStringLiteral(", ");
                        label += name;
                        tagChars += name.size() + 2;
                    }
                }
                break;
            }
            case kChunkCel: {
                Cel cel;
                cel.layer = ch.u16();
                cel.x = ch.s16();
                cel.y = ch.s16();
                cel.opacity = ch.u8();
                const quint16 celType = ch.u16();
                cel.zIndex = ch.s16();
                ch.skip(5);
                if (cel.layer >= int(layers.size()))
                    break;
                const Layer &layer = layers[size_t(cel.layer)];
                const bool background = (layer.flags & kLayerBackground) != 0;
                if (celType == 0 || celType == 2) {
                    const int w = ch.u16(), h = ch.u16();
                    if (!ch.ok || w == 0 || h == 0)
                        break;
                    const qint64 raw = qint64(w) * h * bytesPerPixel(hdr.depth); // 16 x 16 bits
                    const qint64 peak = peakWith(celType == 2 ? raw : 0, qint64(w) * h * 4);
                    if (peak > ImageDecoder::kMaxDecodedBytes)
                        return ImageDecoder::tooLarge(peak);
                    const uchar *pixels = nullptr;
                    if (celType == 0) {
                        if (!ch.need(raw)) break;
                        pixels = base + ch.pos;
                    } else {
                        if (!inflateTo(base + ch.pos, ch.size - ch.pos, scratch, raw)) break;
                        pixels = reinterpret_cast<const uchar *>(scratch.constData());
                    }
                    cel.image = toImage(pixels, w, h, hdr.depth, palette, hdr.transparentIndex, background);
                } else if (celType == 1) {
                    const int linked = ch.u16();
                    if (const Cel *src = findCel(linked, cel.layer)) {
                        const int z = cel.zIndex;
                        cel = *src; // shares the image (implicitly shared QImage)
                        cel.zIndex = z;
                    }
                } else if (celType == 3) {
                    const int cols = ch.u16(), rows = ch.u16();
                    const int bits = ch.u16();
                    const quint32 idMask = ch.u32(), xFlip = ch.u32(), yFlip = ch.u32(), dFlip = ch.u32();
                    ch.skip(10);
                    const auto ts = tilesets.constFind(layer.tileset);
                    if (!ch.ok || bits != 32 || cols == 0 || rows == 0 || ts == tilesets.constEnd())
                        break;
                    const qint64 raw = qint64(cols) * rows * 4; // 16 x 16 bits
                    // Each side is 16 x 16 bits; their product is not.
                    const qint64 decoded = mulSat(mulSat(qint64(cols) * ts->tileW, qint64(rows) * ts->tileH), 4);
                    const qint64 peak = peakWith(raw, decoded);
                    if (peak > ImageDecoder::kMaxDecodedBytes)
                        return ImageDecoder::tooLarge(peak);
                    if (!inflateTo(base + ch.pos, ch.size - ch.pos, scratch, raw)) break;
                    cel.image = renderTilemap(reinterpret_cast<const uchar *>(scratch.constData()), cols, rows, *ts,
                                              idMask, xFlip, yFlip, dFlip);
                }
                if (!cel.image.isNull()) {
                    // Linked cels share their source's image; count the others.
                    if (celType != 1) {
                        celBytes += cel.image.sizeInBytes();
                        if (celBytes > ImageDecoder::kMaxDecodedBytes)
                            return ImageDecoder::tooLarge(celBytes);
                    }
                    cels[size_t(frame)].push_back(std::move(cel));
                }
                break;
            }
            default:
                break;
            }
            cpos += chunkSize;
        }
        pos = frameEnd;
    }

    auto doc = std::make_shared<ImageDocument>();
    doc->format = QStringLiteral("Aseprite");
    doc->detail = hdr.depth == 32 ? QObject::tr("RGBA") : hdr.depth == 16 ? QObject::tr("grayscale")
                                                                          : QObject::tr("indexed");
    if (layers.size() > 0)
        doc->detail += QObject::tr(" · %n layer(s)", nullptr, int(layers.size()));
    doc->fileSize = data.size();
    doc->width = hdr.width;
    doc->height = hdr.height;
    doc->hasAlpha = true;
    doc->surfaces.reserve(hdr.frames);

    for (int frame = 0; frame < hdr.frames; ++frame) {
        if (cancel.load(std::memory_order_relaxed))
            return ImageDecoder::cancelled();
        std::vector<Cel> &list = cels[size_t(frame)];
        // Draw order: layer index + z-index, ties by z-index (spec note 5).
        std::stable_sort(list.begin(), list.end(), [](const Cel &a, const Cel &b) {
            const int oa = a.layer + a.zIndex, ob = b.layer + b.zIndex;
            return oa != ob ? oa < ob : a.zIndex < b.zIndex;
        });
        QImage canvas(hdr.width, hdr.height, QImage::Format_ARGB32_Premultiplied);
        if (canvas.isNull())
            return ImageDecoder::tooLarge(needed);
        canvas.fill(0);
        {
            QPainter p(&canvas);
            for (const Cel &cel : list) {
                const Layer &layer = layers[size_t(cel.layer)];
                if (!layer.effectiveVisible || layer.type == 1)
                    continue;
                const int opacity = cel.opacity * layer.effectiveOpacity / 255;
                if (opacity == 0)
                    continue;
                if (layer.blendMode == kBlendSubtract || layer.blendMode == kBlendDivide) {
                    p.end();
                    blendSeparable(canvas, cel.image.convertToFormat(QImage::Format_ARGB32_Premultiplied), cel.x,
                                   cel.y, opacity, layer.blendMode);
                    p.begin(&canvas);
                    continue;
                }
                p.setOpacity(opacity / 255.0);
                p.setCompositionMode(compositionFor(layer.blendMode));
                p.drawImage(cel.x, cel.y, cel.image);
            }
        }
        doc->surfaces.push_back(std::move(canvas).convertToFormat(QImage::Format_ARGB32)); // in place
    }

    doc->layerCount = hdr.frames;
    doc->isAnimation = hdr.frames > 1;
    if (doc->isAnimation) {
        doc->frameDelaysMs = durations;
        doc->layerNames = frameTags;
    }

    ImageDecoder::Result r;
    if (!doc->isAnimation)
        r.pyramid = ImageOps::buildPyramid(doc->surfaces[0]);
    r.doc = std::move(doc);
    return r;
}

} // namespace AsepriteDecoder
