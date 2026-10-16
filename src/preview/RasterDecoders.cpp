#include "RasterDecoders.h"

#include "stb_image.h"
#include "tinyexr.h"

#include <QFloat16>
#include <QObject>

#include <algorithm>
#include <climits>
#include <cstring>
#include <memory>

namespace RasterDecoders {

namespace {

const stbi_uc *bytes(QByteArrayView data) { return reinterpret_cast<const stbi_uc *>(data.data()); }

QString stbError()
{
    const char *reason = stbi_failure_reason();
    return reason ? QObject::tr("The image could not be decoded (%1).").arg(QString::fromLatin1(reason))
                  : QObject::tr("The image could not be decoded.");
}

QString formatName(Kind kind)
{
    switch (kind) {
    case Kind::Tga: return QStringLiteral("TGA");
    case Kind::Psd: return QStringLiteral("PSD");
    case Kind::Radiance: return QStringLiteral("HDR");
    case Kind::Exr: return QStringLiteral("EXR");
    }
    return QString();
}

// --- stb: TGA, PSD, Radiance ---------------------------------------------------------

qint64 estimateStb(Kind kind, QByteArrayView data)
{
    if (data.size() > INT_MAX)
        return -1;
    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(bytes(data), int(data.size()), &w, &h, &comp))
        return -1;
    return qint64(w) * h * (kind == Kind::Radiance ? 8 : 4);
}

ImageDecoder::Result decodeStb(Kind kind, QByteArrayView data, const std::atomic_bool &cancel)
{
    if (data.size() > INT_MAX)
        return ImageDecoder::failed(QObject::tr("Files over 2 GB are not supported for this format."));
    const int len = int(data.size());
    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(bytes(data), len, &w, &h, &comp))
        return ImageDecoder::failed(stbError());
    const bool hdr = kind == Kind::Radiance;
    const qint64 needed = qint64(w) * h * (hdr ? 8 : 4);
    if (needed > ImageDecoder::kMaxDecodedBytes)
        return ImageDecoder::tooLarge(needed);
    const bool sixteen = kind == Kind::Psd && stbi_is_16_bit_from_memory(bytes(data), len);

    auto doc = std::make_shared<ImageDocument>();
    doc->format = formatName(kind);
    doc->fileSize = data.size();
    doc->hasAlpha = comp == 2 || comp == 4;

    QImage surface;
    if (hdr) {
        float *rgba = stbi_loadf_from_memory(bytes(data), len, &w, &h, &comp, 4);
        if (!rgba)
            return ImageDecoder::failed(stbError());
        const std::unique_ptr<float, void (*)(void *)> owner(rgba, stbi_image_free);
        if (cancel.load(std::memory_order_relaxed))
            return ImageDecoder::cancelled();
        surface = QImage(w, h, QImage::Format_RGBA16FPx4);
        if (surface.isNull())
            return ImageDecoder::tooLarge(needed);
        ImageOps::parallelRows(h, qint64(w) * h, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                qFloatToFloat16(reinterpret_cast<qfloat16 *>(surface.scanLine(y)), rgba + qint64(y) * w * 4,
                                qsizetype(w) * 4);
        });
        doc->isHdr = true;
        doc->hasAlpha = false;
        doc->detail = QObject::tr("Radiance RGBE");
    } else {
        stbi_uc *rgba = stbi_load_from_memory(bytes(data), len, &w, &h, &comp, 4);
        if (!rgba)
            return ImageDecoder::failed(stbError());
        if (cancel.load(std::memory_order_relaxed)) {
            stbi_image_free(rgba);
            return ImageDecoder::cancelled();
        }
        // Wrap without a copy; the one conversion pass (SIMD swizzle in Qt)
        // writes the straight ARGB32 surface and frees stb's buffer after.
        const QImage wrapped(rgba, w, h, qsizetype(w) * 4, QImage::Format_RGBA8888, stbi_image_free, rgba);
        surface = wrapped.convertToFormat(QImage::Format_ARGB32);
        if (surface.isNull())
            return ImageDecoder::tooLarge(needed);
        const QString depth = sixteen ? QObject::tr("16-bit") : QObject::tr("8-bit");
        const QString channels = comp == 1 ? QObject::tr("gray") : comp == 2 ? QObject::tr("gray + alpha")
                               : comp == 3 ? QObject::tr("RGB") : QObject::tr("RGBA");
        doc->detail = kind == Kind::Psd ? QObject::tr("%1 %2 composite").arg(depth, channels)
                                        : QObject::tr("%1 %2").arg(depth, channels);
    }
    doc->width = w;
    doc->height = h;
    doc->surfaces.push_back(std::move(surface));

    ImageDecoder::Result r;
    r.pyramid = ImageOps::buildPyramid(doc->surfaces[0]);
    r.doc = std::move(doc);
    return r;
}

// --- tinyexr: OpenEXR -------------------------------------------------------------------

struct ExrHeader {
    EXRVersion version{};
    EXRHeader header{};
    bool parsed = false;
    ExrHeader() { InitEXRHeader(&header); }
    ~ExrHeader()
    {
        if (parsed)
            FreeEXRHeader(&header);
    }
    ExrHeader(const ExrHeader &) = delete;
    ExrHeader &operator=(const ExrHeader &) = delete;
};

QString takeExrError(const char *err, const QString &fallback)
{
    if (!err)
        return fallback;
    const QString text = QObject::tr("The EXR could not be decoded (%1).").arg(QString::fromUtf8(err));
    FreeEXRErrorMessage(err);
    return text;
}

bool parseExr(QByteArrayView data, ExrHeader &h, QString *error)
{
    const auto *mem = reinterpret_cast<const unsigned char *>(data.data());
    const size_t size = size_t(data.size());
    if (ParseEXRVersionFromMemory(&h.version, mem, size) != TINYEXR_SUCCESS) {
        if (error) *error = QObject::tr("Not an OpenEXR file.");
        return false;
    }
    if (h.version.multipart || h.version.non_image) {
        if (error) *error = QObject::tr("Multi-part and deep EXR files are not supported.");
        return false;
    }
    const char *err = nullptr;
    if (ParseEXRHeaderFromMemory(&h.header, &h.version, mem, size, &err) != TINYEXR_SUCCESS) {
        const QString text = takeExrError(err, QObject::tr("The EXR header could not be read."));
        if (error) *error = text;
        return false;
    }
    h.parsed = true;
    return true;
}

int pixelBytes(int type) { return type == TINYEXR_PIXELTYPE_HALF ? 2 : 4; }

QString compressionName(int c)
{
    switch (c) {
    case TINYEXR_COMPRESSIONTYPE_NONE: return QStringLiteral("uncompressed");
    case TINYEXR_COMPRESSIONTYPE_RLE: return QStringLiteral("RLE");
    case TINYEXR_COMPRESSIONTYPE_ZIPS: return QStringLiteral("ZIPS");
    case TINYEXR_COMPRESSIONTYPE_ZIP: return QStringLiteral("ZIP");
    case TINYEXR_COMPRESSIONTYPE_PIZ: return QStringLiteral("PIZ");
    case 5: return QStringLiteral("PXR24");
    default: return QString();
    }
}

// R, G, B, A (and Y for luminance-only) channel indices: unprefixed names
// first, else the layer of the first "*.R" channel.
struct ExrChannels {
    int r = -1, g = -1, b = -1, a = -1;
    QString layer;
};

ExrChannels pickChannels(const EXRHeader &h)
{
    ExrChannels c;
    auto find = [&h](const QByteArray &name) {
        for (int i = 0; i < h.num_channels; ++i)
            if (name == h.channels[i].name) return i;
        return -1;
    };
    c.r = find("R");
    c.g = find("G");
    c.b = find("B");
    c.a = find("A");
    if (c.r < 0 && c.g < 0 && c.b < 0) {
        for (int i = 0; i < h.num_channels; ++i) {
            const QByteArray name(h.channels[i].name);
            if (name.endsWith(".R")) {
                const QByteArray prefix = name.left(name.size() - 1);
                c.r = i;
                c.g = find(prefix + 'G');
                c.b = find(prefix + 'B');
                c.a = find(prefix + 'A');
                c.layer = QString::fromUtf8(prefix.left(prefix.size() - 1));
                break;
            }
        }
    }
    if (c.r < 0 && c.g < 0 && c.b < 0) {
        int y = find("Y");
        if (y < 0 && h.num_channels > 0) y = 0;
        c.r = c.g = c.b = y;
    }
    return c;
}

qint64 estimateExr(QByteArrayView data)
{
    ExrHeader h;
    if (!parseExr(data, h, nullptr))
        return -1;
    const qint64 w = qint64(h.header.data_window.max_x) - h.header.data_window.min_x + 1;
    const qint64 hh = qint64(h.header.data_window.max_y) - h.header.data_window.min_y + 1;
    return w > 0 && hh > 0 ? w * hh * 8 : -1;
}

inline quint16 toHalfBits(const unsigned char *plane, int type, qsizetype index)
{
    if (type == TINYEXR_PIXELTYPE_HALF)
        return reinterpret_cast<const quint16 *>(plane)[index];
    float f;
    if (type == TINYEXR_PIXELTYPE_FLOAT)
        f = reinterpret_cast<const float *>(plane)[index];
    else
        f = float(reinterpret_cast<const quint32 *>(plane)[index]);
    const qfloat16 h(f);
    quint16 bits;
    std::memcpy(&bits, &h, 2);
    return bits;
}

ImageDecoder::Result decodeExr(QByteArrayView data, const std::atomic_bool &cancel)
{
    ExrHeader h;
    QString error;
    if (!parseExr(data, h, &error))
        return ImageDecoder::failed(error);
    const EXRHeader &hdr = h.header;
    const int w = hdr.data_window.max_x - hdr.data_window.min_x + 1;
    const int height = hdr.data_window.max_y - hdr.data_window.min_y + 1;
    if (w <= 0 || height <= 0)
        return ImageDecoder::failed(QObject::tr("The EXR has an empty data window."));

    // Our RGBA16F surface plus tinyexr's planar copy of every channel.
    const qint64 needed = qint64(w) * height * 8;
    qint64 planar = 0;
    for (int i = 0; i < hdr.num_channels; ++i)
        planar += qint64(w) * height * pixelBytes(hdr.pixel_types[i]);
    if (needed > ImageDecoder::kMaxDecodedBytes || needed + planar > 2 * ImageDecoder::kMaxDecodedBytes)
        return ImageDecoder::tooLarge(needed + planar);
    if (cancel.load(std::memory_order_relaxed))
        return ImageDecoder::cancelled();

    // Keep stored pixel types: HALF stays HALF (no float copy at all).
    for (int i = 0; i < hdr.num_channels; ++i)
        hdr.requested_pixel_types[i] = hdr.pixel_types[i];

    EXRImage image;
    InitEXRImage(&image);
    const char *err = nullptr;
    if (LoadEXRImageFromMemory(&image, &h.header, reinterpret_cast<const unsigned char *>(data.data()),
                               size_t(data.size()), &err) != TINYEXR_SUCCESS)
        return ImageDecoder::failed(takeExrError(err, QObject::tr("The EXR could not be decoded.")));
    const std::unique_ptr<EXRImage, int (*)(EXRImage *)> imageOwner(&image, FreeEXRImage);
    if (cancel.load(std::memory_order_relaxed))
        return ImageDecoder::cancelled();

    const ExrChannels ch = pickChannels(hdr);
    QImage surface(image.width, image.height, QImage::Format_RGBA16FPx4);
    if (surface.isNull())
        return ImageDecoder::tooLarge(needed);
    const int types[4] = {ch.r >= 0 ? hdr.requested_pixel_types[ch.r] : 0,
                          ch.g >= 0 ? hdr.requested_pixel_types[ch.g] : 0,
                          ch.b >= 0 ? hdr.requested_pixel_types[ch.b] : 0,
                          ch.a >= 0 ? hdr.requested_pixel_types[ch.a] : 0};
    const int idx[4] = {ch.r, ch.g, ch.b, ch.a};
    constexpr quint16 kOne = 0x3c00, kZero = 0;

    // Writes `count` pixels of row `y` from planes at plane index `src0`.
    auto writeRun = [&](unsigned char **planes, qsizetype src0, int y, int x0, int count) {
        auto *d = reinterpret_cast<quint16 *>(surface.scanLine(y)) + 4 * qsizetype(x0);
        for (int c = 0; c < 4; ++c) {
            if (idx[c] < 0) {
                const quint16 fill = c == 3 ? kOne : kZero;
                for (int x = 0; x < count; ++x) d[4 * x + c] = fill;
                continue;
            }
            const unsigned char *plane = planes[idx[c]];
            if (types[c] == TINYEXR_PIXELTYPE_HALF) {
                const auto *s = reinterpret_cast<const quint16 *>(plane) + src0;
                for (int x = 0; x < count; ++x) d[4 * x + c] = s[x];
            } else {
                for (int x = 0; x < count; ++x) d[4 * x + c] = toHalfBits(plane, types[c], src0 + x);
            }
        }
    };

    if (image.tiles) {
        const int tw = hdr.tile_size_x, th = hdr.tile_size_y;
        ImageOps::parallelRows(image.num_tiles, qint64(w) * height, [&](int t0, int t1) {
            for (int t = t0; t < t1; ++t) {
                const EXRTile &tile = image.tiles[t];
                if (tile.level_x != 0 || tile.level_y != 0) continue;
                const int px = tile.offset_x * tw, py = tile.offset_y * th;
                const int cols = std::min(tile.width, image.width - px);
                for (int y = 0; y < tile.height && py + y < image.height; ++y)
                    if (cols > 0) writeRun(tile.images, qsizetype(y) * tw, py + y, px, cols);
            }
        });
    } else if (image.images) {
        ImageOps::parallelRows(image.height, qint64(w) * height, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                writeRun(image.images, qsizetype(y) * image.width, y, 0, image.width);
        });
    } else {
        return ImageDecoder::failed(QObject::tr("The EXR has no image data."));
    }

    auto doc = std::make_shared<ImageDocument>();
    doc->format = QStringLiteral("EXR");
    const bool anyFloat = std::any_of(std::begin(types), std::end(types),
                                      [](int t) { return t == TINYEXR_PIXELTYPE_FLOAT; });
    QStringList detail;
    detail << (anyFloat ? QObject::tr("32-bit float") : QObject::tr("16-bit float"));
    if (!ch.layer.isEmpty()) detail << QObject::tr("layer %1").arg(ch.layer);
    if (hdr.tiled) detail << QObject::tr("tiled");
    const QString comp = compressionName(hdr.compression_type);
    if (!comp.isEmpty()) detail << comp;
    if (hdr.num_channels > 4) detail << QObject::tr("%1 channels").arg(hdr.num_channels);
    doc->detail = detail.join(QStringLiteral(" · "));
    doc->fileSize = data.size();
    doc->width = image.width;
    doc->height = image.height;
    doc->isHdr = true;
    doc->hasAlpha = ch.a >= 0;
    doc->surfaces.push_back(std::move(surface));

    ImageDecoder::Result r;
    r.pyramid = ImageOps::buildPyramid(doc->surfaces[0]);
    r.doc = std::move(doc);
    return r;
}

} // namespace

bool looksLikePsd(QByteArrayView head)
{
    return head.size() >= 6 && head.startsWith("8BPS") && head[4] == 0 && head[5] == 1;
}

bool looksLikeRadiance(QByteArrayView head)
{
    return head.startsWith("#?RADIANCE") || head.startsWith("#?RGBE");
}

bool looksLikeExr(QByteArrayView head)
{
    return head.size() >= 4 && head.startsWith("\x76\x2f\x31\x01");
}

qint64 estimate(Kind kind, QByteArrayView data)
{
    return kind == Kind::Exr ? estimateExr(data) : estimateStb(kind, data);
}

ImageDecoder::Result decode(Kind kind, QByteArrayView data, const std::atomic_bool &cancel)
{
    return kind == Kind::Exr ? decodeExr(data, cancel) : decodeStb(kind, data, cancel);
}

} // namespace RasterDecoders
