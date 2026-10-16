#include "TileMapLoaderInternal.h"

#include "AssetFileUtil.h"
#include "ImageDecoder.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QObject>

#include "miniz.h"

#include <cmath>
#include <cstring>
#include <exception>
#include <vector>

namespace TileMapLoader {

namespace detail {

QString Context::resolve(const QString &dir, const QString &ref)
{
    if (AssetFileUtil::isNetworkPath(ref)) {
        warnings << QObject::tr("Skipped a network path: %1").arg(ref);
        return {};
    }
    return QDir(dir).absoluteFilePath(ref);
}

QImage Context::loadImage(const QString &absPath)
{
    if (absPath.isEmpty())
        return {};
    const auto it = m_images.constFind(absPath);
    if (it != m_images.constEnd())
        return it.value();

    QImage out;
    if (!QFileInfo::exists(absPath)) {
        warnings << QObject::tr("Image not found: %1").arg(QDir::toNativeSeparators(absPath));
    } else {
        static const std::atomic_bool never{false};
        const ImageDecoder::Result r = ImageDecoder::decode(absPath, cancel ? *cancel : never);
        if (r.cancelled) {
            // The caller sees cancelled() and abandons the load.
        } else if (!r.doc || r.doc->surfaces.empty()) {
            warnings << QObject::tr("Could not read image %1: %2")
                            .arg(QFileInfo(absPath).fileName(), r.error);
        } else {
            out = r.doc->surfaces.front();
            if (out.format() != QImage::Format_ARGB32_Premultiplied)
                out = out.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        }
    }
    m_images.insert(absPath, out);
    return out;
}

QByteArray Context::readFile(const QString &absPath)
{
    if (absPath.isEmpty())
        return {};
    QFile f(absPath);
    if (!f.open(QIODevice::ReadOnly) || f.size() > kMaxFileBytes)
        return {};
    return f.readAll();
}

bool inflateExact(const QByteArray &src, bool gzip, size_t expected, QByteArray *out)
{
    if (expected == 0 || expected > (size_t(1) << 31))
        return false;
    const auto *p = reinterpret_cast<const unsigned char *>(src.constData());
    size_t n = size_t(src.size());
    int flags = TINFL_FLAG_PARSE_ZLIB_HEADER;
    if (gzip) {
        if (n < 18 || p[0] != 0x1f || p[1] != 0x8b || p[2] != 8)
            return false;
        const unsigned char fl = p[3];
        size_t off = 10;
        if (fl & 4) {
            if (off + 2 > n) return false;
            off += 2 + size_t(p[off]) + (size_t(p[off + 1]) << 8);
        }
        for (const unsigned char bit : {8, 16}) {
            if (fl & bit) {
                while (off < n && p[off] != 0) ++off;
                ++off;
            }
        }
        if (fl & 2) off += 2;
        if (off + 8 > n) return false;
        p += off;
        n -= off + 8;           // CRC32 + ISIZE trailer
        flags = 0;              // raw deflate
    }
    out->resize(qsizetype(expected));
    const size_t got = tinfl_decompress_mem_to_mem(out->data(), expected, p, n, flags);
    return got == expected;
}

void finalizeLayer(TileMap::Layer &layer)
{
    // NaN / huge offsets from the file would poison the bounds and the view.
    std::erase_if(layer.instances, [](const TileMap::Instance &in) {
        return !(std::abs(in.x) <= kMaxCoord && std::abs(in.y) <= kMaxCoord);
    });
    layer.animated.clear();
    for (size_t i = 0; i < layer.instances.size(); ++i) {
        if (layer.instances[i].flags & TileMap::Animated)
            layer.animated.push_back(uint32_t(i));
    }
    layer.grid.build(layer.instances);
    layer.animatedGrid.build(layer.instances, layer.animated);
}

QRectF contentBounds(const TileMap::Document &doc)
{
    QRectF bounds;
    bool have = false;
    auto add = [&](QRectF r) {
        r = r.normalized();
        bounds = have ? bounds.united(r) : r;
        have = true;
    };
    for (const TileMap::Layer &layer : doc.layers) {
        if (layer.kind == TileMap::Layer::Kind::Tiles && !layer.instances.empty()) {
            float minX = layer.instances[0].x, minY = layer.instances[0].y;
            float maxX = minX, maxY = minY;
            for (const TileMap::Instance &in : layer.instances) {
                minX = std::min(minX, in.x);
                minY = std::min(minY, in.y);
                maxX = std::max(maxX, in.x + in.w);
                maxY = std::max(maxY, in.y + in.h);
            }
            add(QRectF(QPointF(minX, minY), QPointF(maxX, maxY)));
        } else if (layer.kind == TileMap::Layer::Kind::Image && !layer.image.isNull()) {
            add(QRectF(layer.imagePos, QSizeF(layer.image.size())));
        } else if (layer.kind == TileMap::Layer::Kind::Objects) {
            for (const TileMap::Object &o : layer.objects) {
                QRectF r(o.pos, o.size);
                if (!o.points.isEmpty())
                    r = o.points.translated(o.pos).boundingRect();
                if (o.shape == TileMap::Object::Shape::Tile)
                    r = QRectF(o.tile.x, o.tile.y, o.tile.w, o.tile.h);
                add(r);
            }
        }
    }
    for (const TileMap::Level &level : doc.levels)
        add(level.rect);
    return bounds;
}

double jnum(const QJsonObject &o, const char *key, double def)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isDouble()) return v.toDouble();
    if (v.isString()) {
        bool ok = false;
        const double d = v.toString().toDouble(&ok);
        return ok ? d : def;
    }
    if (v.isBool()) return v.toBool() ? 1.0 : 0.0;
    return def;
}

int jint(const QJsonObject &o, const char *key, int def)
{
    return int(std::lround(jnum(o, key, def)));
}

QString jstr(const QJsonObject &o, const char *key)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isString()) return v.toString();
    if (v.isDouble()) return QString::number(v.toDouble());
    return {};
}

bool jflag(const QJsonObject &o, const char *key, bool def)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isBool()) return v.toBool();
    if (v.isDouble()) return v.toDouble() != 0.0;
    if (v.isString()) {
        const QString s = v.toString().toLower();
        return s == QLatin1String("1") || s == QLatin1String("true");
    }
    return def;
}

} // namespace detail

namespace {

QString suffixOf(const QString &path)
{
    return QFileInfo(path).suffix().toLower();
}

using AssetFileUtil::trimmedHead;

} // namespace

bool looksLikeTiledJson(QByteArrayView head)
{
    head = trimmedHead(head);
    if (head.isEmpty() || head[0] != '{')
        return false;
    // Every Tiled export carries these near the top (key order varies, and the
    // head may cut the file anywhere, so look for either of the two stable ones).
    return head.indexOf(QByteArrayView("\"tiledversion\"")) >= 0
           || (head.indexOf(QByteArrayView("\"tilewidth\"")) >= 0 && head.indexOf(QByteArrayView("\"layers\"")) >= 0)
           || (head.indexOf(QByteArrayView("\"tilewidth\"")) >= 0 && head.indexOf(QByteArrayView("\"tilecount\"")) >= 0);
}

bool looksLikeTiledXml(QByteArrayView head)
{
    head = trimmedHead(head);
    if (head.isEmpty() || head[0] != '<')
        return false;
    const auto has = [head](const char *needle) { return head.indexOf(QByteArrayView(needle)) >= 0; };
    return (has("<map ") && (has("orientation=") || has("tilewidth="))) || (has("<tileset") && has("tilewidth="));
}

bool looksLikeTileMap(QByteArrayView head)
{
    return looksLikeTiledJson(head) || looksLikeTiledXml(head) || looksLikeLdtk(head);
}

bool looksLikeLdtk(QByteArrayView head)
{
    head = trimmedHead(head);
    return !head.isEmpty() && head[0] == '{'
           && (head.indexOf(QByteArrayView("\"__header__\"")) >= 0 || head.indexOf(QByteArrayView("\"jsonVersion\"")) >= 0);
}

Result loadFromBytes(const QByteArray &data, const QString &suffixLower, const QString &baseDir,
                     const std::atomic_bool &cancel)
{
    Result result;
    detail::Context ctx;
    ctx.baseDir = baseDir;
    ctx.cancel = &cancel;

    detail::DocPtr doc;
    try {
        if (suffixLower == QLatin1String("ldtk") || suffixLower == QLatin1String("ldtkl")
            || (suffixLower != QLatin1String("tmx") && suffixLower != QLatin1String("tsx") && looksLikeLdtk(data.left(4096))))
            doc = detail::loadLdtk(data, suffixLower, ctx);
        else
            doc = detail::loadTiled(data, suffixLower, ctx);
    } catch (const std::bad_alloc &) {
        result.error = QObject::tr("Not enough memory to load this map.");
        return result;
    } catch (const std::exception &) {
        // std::length_error and friends from a size the file made up.
        result.error = QObject::tr("This map is too large to load.");
        return result;
    }

    if (ctx.cancelled()) {
        result.cancelled = true;
        return result;
    }
    if (!doc) {
        result.error = ctx.error.isEmpty() ? QObject::tr("The file is not a valid tile map.") : ctx.error;
        return result;
    }
    doc->warnings = ctx.warnings;
    doc->warnings.removeDuplicates();
    doc->fileSize = data.size();
    for (const TileMap::Layer &layer : doc->layers) {
        doc->tileCount += qint64(layer.instances.size());
        doc->hasObjects = doc->hasObjects || (layer.kind == TileMap::Layer::Kind::Objects && !layer.objects.isEmpty());
        doc->hasAnimations = doc->hasAnimations || !layer.animated.empty();
    }
    doc->bounds = detail::contentBounds(*doc);
    if (doc->bounds.width() <= 0 || doc->bounds.height() <= 0)
        doc->bounds = QRectF(0, 0, std::max(1, doc->gridW) * 8.0, std::max(1, doc->gridH) * 8.0);
    result.doc = std::move(doc);
    return result;
}

Result load(const QString &path, const std::atomic_bool &cancel)
{
    Result result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = file.errorString();
        return result;
    }
    if (file.size() > kMaxFileBytes) {
        result.error = QObject::tr("The file is larger than the %1 MB limit.").arg(kMaxFileBytes >> 20);
        return result;
    }
    const QByteArray data = file.readAll();
    if (cancel.load(std::memory_order_relaxed)) {
        result.cancelled = true;
        return result;
    }
    return loadFromBytes(data, suffixOf(path), QFileInfo(path).absolutePath(), cancel);
}

} // namespace TileMapLoader
