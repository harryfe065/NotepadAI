#include "AsepriteDecoder.h"

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"

#include <QHash>
#include <QObject>
#include <QPainter>
#include <QtEndian>

#include <algorithm>
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
    return straight.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

bool inflateTo(const uchar *src, qsizetype srcLen, QByteArray &out, qsizetype expected)
{
    out.resize(expected);
    mz_ulong len = mz_ulong(expected);
    const int rc = mz_uncompress(reinterpret_cast<unsigned char *>(out.data()), &len, src, mz_ulong(srcLen));
    // A cel stream may end a few bytes short of a full image in old files;
    // accept that (rest stays zero) but not a corrupt stream.
    if (rc != MZ_OK && rc != MZ_BUF_ERROR)
        return false;
    if (qsizetype(len) < expected)
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
    // Hue/Saturation/Color/Luminosity, Subtract, Divide: no raster-engine
    // equivalent; drawn as Normal.
    default: return QPainter::CompositionMode_SourceOver;
    }
}

// Tilemap cel to an image: tiles blitted with their x/y/diagonal flips.
QImage renderTilemap(const quint32 *ids, int cols, int rows, const Tileset &ts, quint32 idMask, quint32 xFlip,
                     quint32 yFlip, quint32 dFlip)
{
    const int tw = ts.tileW, th = ts.tileH;
    QImage out(cols * tw, rows * th, QImage::Format_ARGB32_Premultiplied);
    if (out.isNull()) return out;
    out.fill(0);
    for (int ty = 0; ty < rows; ++ty) {
        for (int tx = 0; tx < cols; ++tx) {
            const quint32 v = ids[qsizetype(ty) * cols + tx];
            const quint32 id = v & idMask;
            if ((ts.zeroIsEmpty && id == 0) || (!ts.zeroIsEmpty && v == 0xffffffffu) || int(id) >= ts.count)
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
    return qint64(h.width) * h.height * 4 * h.frames;
}

ImageDecoder::Result decode(QByteArrayView data, const std::atomic_bool &cancel)
{
    Header hdr;
    if (!readHeader(data, hdr))
        return ImageDecoder::failed(QObject::tr("Not an Aseprite file."));
    const qint64 needed = qint64(hdr.width) * hdr.height * 4 * hdr.frames;
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
                Tileset ts;
                ts.count = int(ch.u32());
                ts.tileW = ch.u16();
                ts.tileH = ch.u16();
                ch.s16();
                ch.skip(14);
                ch.string();
                ts.zeroIsEmpty = (flags & 4) != 0;
                if (flags & 1) ch.skip(8);
                if ((flags & 2) && ts.tileW > 0 && ts.tileH > 0 && ts.count > 0) {
                    const quint32 len = ch.u32();
                    const qint64 tileBytes = qint64(ts.tileW) * ts.tileH * ts.count * bytesPerPixel(hdr.depth);
                    if (ch.need(len) && tileBytes <= ImageDecoder::kMaxDecodedBytes
                        && inflateTo(base + ch.pos, len, scratch, tileBytes))
                        ts.tiles = toImage(reinterpret_cast<const uchar *>(scratch.constData()), ts.tileW,
                                           ts.tileH * ts.count, hdr.depth, palette, hdr.transparentIndex, false);
                }
                if (!ts.tiles.isNull())
                    tilesets.insert(id, ts);
                break;
            }
            case kChunkTags: {
                const quint16 count = ch.u16();
                ch.skip(8);
                for (quint16 t = 0; t < count && ch.ok; ++t) {
                    const int from = ch.u16(), to = ch.u16();
                    ch.u8();
                    ch.u16();
                    ch.skip(6 + 3 + 1);
                    const QString name = ch.string();
                    for (int f = from; f <= to && f < hdr.frames; ++f)
                        frameTags[f] += frameTags[f].isEmpty() ? name : QStringLiteral(", ") + name;
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
                    const qint64 raw = qint64(w) * h * bytesPerPixel(hdr.depth);
                    if (!ch.ok || w == 0 || h == 0 || raw > ImageDecoder::kMaxDecodedBytes)
                        break;
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
                    const qint64 raw = qint64(cols) * rows * 4;
                    if (raw > ImageDecoder::kMaxDecodedBytes
                        || qint64(cols) * ts->tileW * rows * ts->tileH * 4 > ImageDecoder::kMaxDecodedBytes)
                        break;
                    if (!inflateTo(base + ch.pos, ch.size - ch.pos, scratch, raw)) break;
                    std::vector<quint32> ids(size_t(cols) * rows);
                    for (size_t i = 0; i < ids.size(); ++i)
                        ids[i] = qFromLittleEndian<quint32>(scratch.constData() + 4 * i);
                    cel.image = renderTilemap(ids.data(), cols, rows, *ts, idMask, xFlip, yFlip, dFlip);
                }
                if (!cel.image.isNull())
                    cels[size_t(frame)].push_back(std::move(cel));
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
                p.setOpacity(opacity / 255.0);
                p.setCompositionMode(compositionFor(layer.blendMode));
                p.drawImage(cel.x, cel.y, cel.image);
            }
        }
        doc->surfaces.push_back(canvas.convertToFormat(QImage::Format_ARGB32));
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
