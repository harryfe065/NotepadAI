#include "TileMapLoaderInternal.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QObject>
#include <QSet>
#include <QXmlStreamReader>

#include <cmath>

// Tiled (https://doc.mapeditor.org/en/stable/reference/json-map-format/).
//
// TMX / TSX are read into a small element tree and converted to the TMJ / TSJ
// JSON shape, so one builder serves all four file types.
namespace TileMapLoader::detail {

using namespace TileMap;

namespace {

constexpr uint32_t kFlipH = 0x80000000u;
constexpr uint32_t kFlipV = 0x40000000u;
constexpr uint32_t kFlipD = 0x20000000u;
constexpr uint32_t kGidMask = 0x0FFFFFFFu;
constexpr int kPollMask = 0xFFFF;

// ---------------------------------------------------------------- TMX → JSON

struct XNode {
    QString name;
    QVector<QPair<QString, QString>> attrs;
    QString text;
    std::vector<XNode> kids;

    QString attr(const char *key) const
    {
        for (const auto &a : attrs) {
            if (a.first == QLatin1String(key))
                return a.second;
        }
        return {};
    }
};

void readNode(QXmlStreamReader &xml, XNode &n)
{
    n.name = xml.name().toString();
    for (const QXmlStreamAttribute &a : xml.attributes())
        n.attrs.append({a.name().toString(), a.value().toString()});
    while (!xml.atEnd()) {
        switch (xml.readNext()) {
        case QXmlStreamReader::StartElement:
            n.kids.emplace_back();
            readNode(xml, n.kids.back());
            break;
        case QXmlStreamReader::Characters:
            n.text += xml.text();
            break;
        case QXmlStreamReader::EndElement:
            return;
        default:
            break;
        }
    }
}

QJsonValue coerce(const QString &key, const QString &value)
{
    static const QSet<QString> stringKeys = {
        QStringLiteral("name"),        QStringLiteral("class"),       QStringLiteral("type"),
        QStringLiteral("source"),      QStringLiteral("image"),       QStringLiteral("orientation"),
        QStringLiteral("renderorder"), QStringLiteral("encoding"),    QStringLiteral("compression"),
        QStringLiteral("staggeraxis"), QStringLiteral("staggerindex"), QStringLiteral("backgroundcolor"),
        QStringLiteral("tintcolor"),   QStringLiteral("color"),       QStringLiteral("trans"),
        QStringLiteral("text"),        QStringLiteral("points"),      QStringLiteral("version"),
        QStringLiteral("tiledversion"), QStringLiteral("format"),
    };
    if (!stringKeys.contains(key)) {
        bool ok = false;
        const double d = value.toDouble(&ok);
        if (ok)
            return d;
    }
    return value;
}

QJsonObject attrsToJson(const XNode &n)
{
    QJsonObject o;
    for (const auto &a : n.attrs)
        o.insert(a.first, coerce(a.first, a.second));
    return o;
}

QJsonArray pointsToJson(const QString &points)
{
    QJsonArray out;
    const QStringList pairs = points.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString &p : pairs) {
        const QStringList xy = p.split(QLatin1Char(','));
        if (xy.size() == 2)
            out.append(QJsonObject{{QStringLiteral("x"), xy[0].toDouble()}, {QStringLiteral("y"), xy[1].toDouble()}});
    }
    return out;
}

QJsonObject tilesetToJson(const XNode &n)
{
    QJsonObject o = attrsToJson(n);
    QJsonArray tiles;
    for (const XNode &k : n.kids) {
        if (k.name == QLatin1String("image")) {
            o.insert(QStringLiteral("image"), k.attr("source"));
            o.insert(QStringLiteral("imagewidth"), k.attr("width").toDouble());
            o.insert(QStringLiteral("imageheight"), k.attr("height").toDouble());
        } else if (k.name == QLatin1String("tileoffset")) {
            o.insert(QStringLiteral("tileoffset"), attrsToJson(k));
        } else if (k.name == QLatin1String("tile")) {
            QJsonObject t = attrsToJson(k);
            for (const XNode &tk : k.kids) {
                if (tk.name == QLatin1String("image")) {
                    t.insert(QStringLiteral("image"), tk.attr("source"));
                    t.insert(QStringLiteral("imagewidth"), tk.attr("width").toDouble());
                    t.insert(QStringLiteral("imageheight"), tk.attr("height").toDouble());
                } else if (tk.name == QLatin1String("animation")) {
                    QJsonArray frames;
                    for (const XNode &f : tk.kids) {
                        frames.append(QJsonObject{{QStringLiteral("tileid"), f.attr("tileid").toDouble()},
                                                  {QStringLiteral("duration"), f.attr("duration").toDouble()}});
                    }
                    t.insert(QStringLiteral("animation"), frames);
                }
            }
            tiles.append(t);
        }
    }
    if (!tiles.isEmpty())
        o.insert(QStringLiteral("tiles"), tiles);
    return o;
}

QJsonObject objectToJson(const XNode &n)
{
    QJsonObject o = attrsToJson(n);
    for (const XNode &k : n.kids) {
        if (k.name == QLatin1String("ellipse"))
            o.insert(QStringLiteral("ellipse"), true);
        else if (k.name == QLatin1String("point"))
            o.insert(QStringLiteral("point"), true);
        else if (k.name == QLatin1String("polygon"))
            o.insert(QStringLiteral("polygon"), pointsToJson(k.attr("points")));
        else if (k.name == QLatin1String("polyline"))
            o.insert(QStringLiteral("polyline"), pointsToJson(k.attr("points")));
        else if (k.name == QLatin1String("text")) {
            QJsonObject t = attrsToJson(k);
            t.insert(QStringLiteral("text"), k.text);
            o.insert(QStringLiteral("text"), t);
        }
    }
    return o;
}

// `data` element (or one `chunk` of it) into the TMJ "data" value.
void dataToJson(const XNode &data, const QString &encoding, QJsonObject &out)
{
    if (encoding.isEmpty()) {
        QJsonArray gids;
        for (const XNode &t : data.kids) {
            if (t.name == QLatin1String("tile"))
                gids.append(t.attr("gid").toDouble());
        }
        out.insert(QStringLiteral("data"), gids);
    } else {
        out.insert(QStringLiteral("data"), data.text.trimmed());
    }
}

QJsonObject layerToJson(const XNode &n);

QJsonArray layersToJson(const XNode &parent)
{
    QJsonArray layers;
    for (const XNode &k : parent.kids) {
        if (k.name == QLatin1String("layer") || k.name == QLatin1String("objectgroup")
            || k.name == QLatin1String("imagelayer") || k.name == QLatin1String("group"))
            layers.append(layerToJson(k));
    }
    return layers;
}

QJsonObject layerToJson(const XNode &n)
{
    QJsonObject o = attrsToJson(n);
    if (n.name == QLatin1String("layer")) {
        o.insert(QStringLiteral("type"), QStringLiteral("tilelayer"));
        for (const XNode &k : n.kids) {
            if (k.name != QLatin1String("data"))
                continue;
            const QString encoding = k.attr("encoding");
            o.insert(QStringLiteral("encoding"), encoding);
            o.insert(QStringLiteral("compression"), k.attr("compression"));
            QJsonArray chunks;
            for (const XNode &c : k.kids) {
                if (c.name == QLatin1String("chunk")) {
                    QJsonObject co = attrsToJson(c);
                    dataToJson(c, encoding, co);
                    chunks.append(co);
                }
            }
            if (!chunks.isEmpty())
                o.insert(QStringLiteral("chunks"), chunks);
            else
                dataToJson(k, encoding, o);
        }
    } else if (n.name == QLatin1String("objectgroup")) {
        o.insert(QStringLiteral("type"), QStringLiteral("objectgroup"));
        QJsonArray objects;
        for (const XNode &k : n.kids) {
            if (k.name == QLatin1String("object"))
                objects.append(objectToJson(k));
        }
        o.insert(QStringLiteral("objects"), objects);
    } else if (n.name == QLatin1String("imagelayer")) {
        o.insert(QStringLiteral("type"), QStringLiteral("imagelayer"));
        for (const XNode &k : n.kids) {
            if (k.name == QLatin1String("image"))
                o.insert(QStringLiteral("image"), k.attr("source"));
        }
    } else {
        o.insert(QStringLiteral("type"), QStringLiteral("group"));
        o.insert(QStringLiteral("layers"), layersToJson(n));
    }
    return o;
}

QJsonObject mapToJson(const XNode &map)
{
    QJsonObject o = attrsToJson(map);
    o.insert(QStringLiteral("type"), QStringLiteral("map"));
    QJsonArray tilesets;
    for (const XNode &k : map.kids) {
        if (k.name == QLatin1String("tileset"))
            tilesets.append(tilesetToJson(k));
    }
    o.insert(QStringLiteral("tilesets"), tilesets);
    o.insert(QStringLiteral("layers"), layersToJson(map));
    return o;
}

// -------------------------------------------------------------------- builder

struct MapInfo {
    QString orientation = QStringLiteral("orthogonal");
    int width = 0, height = 0;      // tiles
    int tileW = 0, tileH = 0;
    bool infinite = false;
    QString renderOrder = QStringLiteral("right-down");
    int hexSide = 0;
    bool staggerX = false;          // staggeraxis == "x"
    bool staggerEven = false;       // staggerindex == "even"
    bool isOrtho() const { return orientation == QLatin1String("orthogonal"); }
    bool isIso() const { return orientation == QLatin1String("isometric"); }
    bool isStaggered() const { return orientation == QLatin1String("staggered"); }
    bool isHex() const { return orientation == QLatin1String("hexagonal"); }
};

class Builder
{
public:
    Builder(Context &ctx, Document &doc) : m_ctx(ctx), m_doc(doc) {}

    bool buildMap(const QJsonObject &root);
    bool buildTilesetSheet(const QJsonObject &root);

private:
    void addTileset(const QJsonObject &t, const QString &dir);
    void buildTileset(const QJsonObject &t, const QString &dir, int firstGid);
    int tilesetFor(uint32_t gid);
    bool decodeTileData(const QJsonValue &data, const QString &encoding, const QString &compression,
                        size_t count, std::vector<uint32_t> &out);

    void addLayers(const QJsonArray &layers, const QString &prefix, QPointF offset, double opacity, bool visible);
    void addTileLayer(const QJsonObject &l, Layer &layer, QPointF offset);
    void emitTiles(Layer &layer, const std::vector<uint32_t> &gids, int x0, int y0, int w, int h, QPointF offset);
    void addObjects(const QJsonObject &l, Layer &layer, QPointF offset);
    void projectObject(Object &o, const QJsonObject &src, double x, double y, double w, double h);
    QPointF isoProject(double x, double y) const;
    QPointF imageTopLeft(int col, int row, int w, int h) const;
    bool staggered(int index) const { return ((index & 1) != 0) != m_info.staggerEven; }

    Context &m_ctx;
    Document &m_doc;
    MapInfo m_info;
    double m_isoOriginX = 0;
    std::vector<std::pair<int, int>> m_lookup;   // (firstGid, tileset index), ascending
    int m_lastTs = -1;
};

QColor parseColor(const QString &s)
{
    return s.isEmpty() ? QColor() : QColor(s);
}

void Builder::buildTileset(const QJsonObject &t, const QString &dir, int firstGid)
{
    Tileset ts;
    ts.firstGid = firstGid;
    ts.name = jstr(t, "name");
    ts.tileW = jint(t, "tilewidth");
    ts.tileH = jint(t, "tileheight");
    ts.margin = jint(t, "margin");
    ts.spacing = jint(t, "spacing");
    ts.columns = jint(t, "columns");
    ts.tileCount = jint(t, "tilecount");
    const QJsonObject off = t.value(QLatin1String("tileoffset")).toObject();
    ts.offset = QPoint(jint(off, "x"), jint(off, "y"));

    const QString imageRel = jstr(t, "image");
    if (!imageRel.isEmpty()) {
        ts.image = m_ctx.loadImage(QDir(dir).absoluteFilePath(imageRel));
        if (!ts.image.isNull() && ts.tileW > 0 && ts.tileH > 0) {
            if (ts.columns <= 0)
                ts.columns = std::max(1, (ts.image.width() - 2 * ts.margin + ts.spacing) / (ts.tileW + ts.spacing));
            const int rows = std::max(1, (ts.image.height() - 2 * ts.margin + ts.spacing) / (ts.tileH + ts.spacing));
            if (ts.tileCount <= 0)
                ts.tileCount = ts.columns * rows;
        }
    }

    const QJsonArray tiles = t.value(QLatin1String("tiles")).toArray();
    for (const QJsonValue &tv : tiles) {
        const QJsonObject to = tv.toObject();
        const int id = jint(to, "id", -1);
        if (id < 0)
            continue;
        const QJsonArray anim = to.value(QLatin1String("animation")).toArray();
        if (!anim.isEmpty()) {
            std::vector<AnimFrame> frames;
            for (const QJsonValue &fv : anim) {
                const QJsonObject fo = fv.toObject();
                frames.push_back({jint(fo, "tileid"), std::max(1, jint(fo, "duration", 100))});
            }
            ts.animations.insert(id, std::move(frames));
        }
        const QString tileImage = jstr(to, "image");
        if (!tileImage.isEmpty() && imageRel.isEmpty()) {
            QImage img = m_ctx.loadImage(QDir(dir).absoluteFilePath(tileImage));
            if (img.isNull())
                continue;
            const int sw = jint(to, "width"), sh = jint(to, "height");
            if (sw > 0 && sh > 0)
                img = img.copy(jint(to, "x"), jint(to, "y"), sw, sh);
            if (ts.tiles.size() <= static_cast<size_t>(id))
                ts.tiles.resize(static_cast<size_t>(id) + 1);
            ts.tileW = std::max(ts.tileW, img.width());
            ts.tileH = std::max(ts.tileH, img.height());
            ts.tiles[static_cast<size_t>(id)] = std::move(img);
        }
    }
    if (!ts.tiles.empty())
        ts.tileCount = std::max(ts.tileCount, int(ts.tiles.size()));
    if (ts.tileW <= 0) ts.tileW = std::max(1, m_info.tileW);
    if (ts.tileH <= 0) ts.tileH = std::max(1, m_info.tileH);

    if (m_doc.tilesets.size() >= 30000)
        return;
    m_lookup.emplace_back(firstGid, int(m_doc.tilesets.size()));
    m_doc.tilesets.push_back(std::move(ts));
}

void Builder::addTileset(const QJsonObject &t, const QString &dir)
{
    const int firstGid = jint(t, "firstgid", 1);
    const QString source = jstr(t, "source");
    if (source.isEmpty()) {
        buildTileset(t, dir, firstGid);
        return;
    }
    const QString path = QDir(dir).absoluteFilePath(source);
    const QByteArray data = m_ctx.readFile(path);
    QJsonObject json;
    if (!data.isEmpty()) {
        if (QFileInfo(path).suffix().compare(QLatin1String("tsx"), Qt::CaseInsensitive) == 0) {
            QXmlStreamReader xml(data);
            XNode root;
            while (!xml.atEnd() && xml.readNext() != QXmlStreamReader::StartElement) {}
            if (!xml.atEnd()) {
                readNode(xml, root);
                json = tilesetToJson(root);
            }
        } else {
            json = QJsonDocument::fromJson(data).object();
        }
    }
    if (json.isEmpty()) {
        m_ctx.warnings << QObject::tr("Tileset not found: %1").arg(QDir::toNativeSeparators(path));
        // A placeholder keeps the tiles visible as grey squares.
        Tileset ts;
        ts.firstGid = firstGid;
        ts.name = QFileInfo(source).completeBaseName();
        ts.tileW = std::max(1, m_info.tileW);
        ts.tileH = std::max(1, m_info.tileH);
        m_lookup.emplace_back(firstGid, int(m_doc.tilesets.size()));
        m_doc.tilesets.push_back(std::move(ts));
        return;
    }
    buildTileset(json, QFileInfo(path).absolutePath(), firstGid);
}

int Builder::tilesetFor(uint32_t gid)
{
    if (m_lastTs >= 0) {
        const auto &cur = m_lookup[static_cast<size_t>(m_lastTs)];
        const bool beforeNext = static_cast<size_t>(m_lastTs) + 1 >= m_lookup.size()
                                || int(gid) < m_lookup[static_cast<size_t>(m_lastTs) + 1].first;
        if (int(gid) >= cur.first && beforeNext)
            return cur.second;
    }
    // Last tileset whose firstGid <= gid.
    auto it = std::upper_bound(m_lookup.begin(), m_lookup.end(), int(gid),
                               [](int g, const std::pair<int, int> &e) { return g < e.first; });
    if (it == m_lookup.begin())
        return -1;
    --it;
    m_lastTs = int(it - m_lookup.begin());
    return it->second;
}

bool Builder::decodeTileData(const QJsonValue &data, const QString &encoding, const QString &compression,
                             size_t count, std::vector<uint32_t> &out)
{
    out.assign(count, 0);
    if (data.isArray()) {
        const QJsonArray arr = data.toArray();
        const size_t n = std::min(count, size_t(arr.size()));
        for (size_t i = 0; i < n; ++i)
            out[i] = uint32_t(arr.at(qsizetype(i)).toDouble());
        return true;
    }
    if (!data.isString())
        return count == 0;
    const QString text = data.toString();

    if (encoding.isEmpty() || encoding == QLatin1String("csv")) {
        const QByteArray csv = text.toLatin1();
        const char *p = csv.constData();
        const char *end = p + csv.size();
        size_t i = 0;
        while (p < end && i < count) {
            while (p < end && (*p < '0' || *p > '9'))
                ++p;
            if (p >= end)
                break;
            uint64_t v = 0;
            while (p < end && *p >= '0' && *p <= '9')
                v = v * 10 + uint64_t(*p++ - '0');
            out[i++] = uint32_t(v);
        }
        return true;
    }

    if (encoding != QLatin1String("base64")) {
        m_ctx.warnings << QObject::tr("Unsupported tile data encoding: %1").arg(encoding);
        return false;
    }
    QByteArray raw = QByteArray::fromBase64(text.toLatin1());
    if (compression == QLatin1String("zlib") || compression == QLatin1String("gzip")) {
        QByteArray inflated;
        if (!inflateExact(raw, compression == QLatin1String("gzip"), count * 4, &inflated)) {
            m_ctx.warnings << QObject::tr("A tile layer's compressed data is corrupt.");
            return false;
        }
        raw = std::move(inflated);
    } else if (!compression.isEmpty()) {
        m_ctx.warnings << QObject::tr("Tile data compressed with %1 is not supported.").arg(compression);
        return false;
    }
    if (size_t(raw.size()) < count * 4) {
        m_ctx.warnings << QObject::tr("A tile layer has less data than its size says.");
    }
    const size_t n = std::min(count, size_t(raw.size()) / 4);
    const auto *b = reinterpret_cast<const unsigned char *>(raw.constData());
    for (size_t i = 0; i < n; ++i)
        out[i] = uint32_t(b[i * 4]) | (uint32_t(b[i * 4 + 1]) << 8) | (uint32_t(b[i * 4 + 2]) << 16)
                 | (uint32_t(b[i * 4 + 3]) << 24);
    return true;
}

QPointF Builder::imageTopLeft(int col, int row, int w, int h) const
{
    const double tw = m_info.tileW, th = m_info.tileH;
    if (m_info.isIso()) {
        // (col, row) maps to the top vertex of the diamond; the image sits on
        // its bottom edge, left-aligned to the cell.
        return {(col - row) * tw / 2.0 + m_isoOriginX - tw / 2.0, (col + row) * th / 2.0 + th - h};
    }
    if (m_info.isStaggered() || m_info.isHex()) {
        double sideX = 0, sideY = 0;
        if (m_info.isHex()) {
            if (m_info.staggerX) sideX = m_info.hexSide;
            else sideY = m_info.hexSide;
        }
        const double offX = (tw - sideX) / 2.0, offY = (th - sideY) / 2.0;
        const double columnWidth = offX + sideX, rowHeight = offY + sideY;
        double px, py;
        if (m_info.staggerX) {
            px = col * columnWidth;
            py = row * (th + sideY) + (staggered(col) ? rowHeight : 0.0);
        } else {
            px = col * (tw + sideX) + (staggered(row) ? columnWidth : 0.0);
            py = row * rowHeight;
        }
        return {px, py + th - h};
    }
    return {col * tw, (row + 1) * th - h};
}

void Builder::emitTiles(Layer &layer, const std::vector<uint32_t> &gids, int x0, int y0, int w, int h,
                        QPointF offset)
{
    const bool ortho = m_info.isOrtho();
    const bool leftward = ortho && m_info.renderOrder.startsWith(QLatin1String("left"));
    const bool upward = ortho && m_info.renderOrder.endsWith(QLatin1String("up"));
    const size_t before = layer.instances.size();
    layer.instances.reserve(before + gids.size() / 2);

    int poll = 0;
    for (int r = 0; r < h; ++r) {
        const int row = upward ? h - 1 - r : r;
        for (int c = 0; c < w; ++c) {
            const int col = leftward ? w - 1 - c : c;
            const uint32_t raw = gids[size_t(row) * size_t(w) + size_t(col)];
            const uint32_t gid = raw & kGidMask;
            if (gid == 0)
                continue;
            if ((++poll & kPollMask) == 0 && m_ctx.cancelled()) {
                m_ctx.instances += qint64(layer.instances.size() - before);
                return;
            }
            const int tsi = tilesetFor(gid);
            if (tsi < 0)
                continue;
            const Tileset &ts = m_doc.tilesets[static_cast<size_t>(tsi)];
            const int local = int(gid) - ts.firstGid;

            Instance in;
            in.tileset = int16_t(tsi);
            in.tile = local;
            if (ts.image.isNull() && ts.tiles.empty()) {
                // Missing sheet: grey placeholder cell.
                in.tileset = -1;
                in.tile = int32_t(0xA0808080u);
                in.w = uint16_t(ts.tileW);
                in.h = uint16_t(ts.tileH);
            } else if (ts.isCollection()) {
                const QImage *img = ts.collectionImage(local);
                if (!img)
                    continue;
                in.w = uint16_t(std::min(img->width(), 65535));
                in.h = uint16_t(std::min(img->height(), 65535));
            } else {
                if (ts.sheetRect(local).isEmpty())
                    continue;
                in.w = uint16_t(ts.tileW);
                in.h = uint16_t(ts.tileH);
            }
            const QPointF p = imageTopLeft(x0 + col, y0 + row, in.w, in.h);
            in.x = float(p.x() + offset.x() + ts.offset.x());
            in.y = float(p.y() + offset.y() + ts.offset.y());
            if (raw & kFlipH) in.flags |= FlipH;
            if (raw & kFlipV) in.flags |= FlipV;
            if (raw & kFlipD) in.flags |= FlipD;
            if (in.tileset >= 0 && ts.animations.contains(local))
                in.flags |= Animated;
            layer.instances.push_back(in);
        }
    }
    m_ctx.instances += qint64(layer.instances.size() - before);
}

void Builder::addTileLayer(const QJsonObject &l, Layer &layer, QPointF offset)
{
    const QString encoding = jstr(l, "encoding");
    const QString compression = jstr(l, "compression");
    std::vector<uint32_t> gids;

    const QJsonArray chunks = l.value(QLatin1String("chunks")).toArray();
    if (!chunks.isEmpty()) {
        // Chunks in reading order, so overlapping tiles stack like Tiled's.
        std::vector<QJsonObject> sorted;
        sorted.reserve(size_t(chunks.size()));
        for (const QJsonValue &cv : chunks)
            sorted.push_back(cv.toObject());
        std::stable_sort(sorted.begin(), sorted.end(), [](const QJsonObject &a, const QJsonObject &b) {
            const int ay = jint(a, "y"), by = jint(b, "y");
            return ay != by ? ay < by : jint(a, "x") < jint(b, "x");
        });
        for (const QJsonObject &c : sorted) {
            if (m_ctx.cancelled() || m_ctx.instances > kMaxInstances)
                return;
            const int w = jint(c, "width"), h = jint(c, "height");
            if (w <= 0 || h <= 0 || qint64(w) * h > (qint64(1) << 24))
                continue;
            if (!decodeTileData(c.value(QLatin1String("data")), encoding, compression, size_t(w) * size_t(h), gids))
                continue;
            emitTiles(layer, gids, jint(c, "x"), jint(c, "y"), w, h, offset);
        }
        return;
    }

    const int w = jint(l, "width", m_info.width), h = jint(l, "height", m_info.height);
    if (w <= 0 || h <= 0)
        return;
    if (qint64(w) * h > kMaxInstances * 2) {
        m_ctx.warnings << QObject::tr("Layer \"%1\" is too large to show.").arg(layer.name);
        return;
    }
    if (!decodeTileData(l.value(QLatin1String("data")), encoding, compression, size_t(w) * size_t(h), gids))
        return;
    emitTiles(layer, gids, jint(l, "startx"), jint(l, "starty"), w, h, offset);
}

QPointF Builder::isoProject(double x, double y) const
{
    const double th = std::max(1, m_info.tileH);
    const double tx = x / th, ty = y / th;
    return {(tx - ty) * m_info.tileW / 2.0 + m_isoOriginX, (tx + ty) * m_info.tileH / 2.0};
}

void Builder::addObjects(const QJsonObject &l, Layer &layer, QPointF offset)
{
    layer.color = parseColor(jstr(l, "color"));
    const QJsonArray objs = l.value(QLatin1String("objects")).toArray();
    layer.objects.reserve(objs.size());
    for (const QJsonValue &ov : objs) {
        const QJsonObject o = ov.toObject();
        Object obj;
        obj.name = jstr(o, "name");
        obj.type = jstr(o, "type");
        if (obj.type.isEmpty())
            obj.type = jstr(o, "class");
        obj.visible = jflag(o, "visible", true);
        obj.rotation = jnum(o, "rotation");
        const double x = jnum(o, "x"), y = jnum(o, "y");
        double w = jnum(o, "width"), h = jnum(o, "height");

        const uint32_t rawGid = uint32_t(jnum(o, "gid"));
        if (rawGid & kGidMask) {
            const int tsi = tilesetFor(rawGid & kGidMask);
            if (tsi >= 0) {
                const Tileset &ts = m_doc.tilesets[static_cast<size_t>(tsi)];
                const int local = int(rawGid & kGidMask) - ts.firstGid;
                if (w <= 0) w = ts.tileW;
                if (h <= 0) h = ts.tileH;
                obj.shape = Object::Shape::Tile;
                obj.tile.tileset = int16_t(tsi);
                obj.tile.tile = local;
                obj.tile.w = uint16_t(std::clamp(w, 1.0, 65535.0));
                obj.tile.h = uint16_t(std::clamp(h, 1.0, 65535.0));
                if (ts.image.isNull() && ts.tiles.empty()) {
                    obj.tile.tileset = -1;
                    obj.tile.tile = int32_t(0xA0808080u);
                }
                if (rawGid & kFlipH) obj.tile.flags |= FlipH;
                if (rawGid & kFlipV) obj.tile.flags |= FlipV;
                QPointF tl;
                if (m_info.isIso()) {
                    const QPointF p = isoProject(x, y);
                    tl = QPointF(p.x() - w / 2.0, p.y() - h);
                } else {
                    tl = QPointF(x, y - h);
                }
                obj.tile.x = float(tl.x() + offset.x());
                obj.tile.y = float(tl.y() + offset.y());
                obj.pos = QPointF(obj.tile.x, obj.tile.y);
                obj.size = QSizeF(w, h);
                layer.objects.append(std::move(obj));
                continue;
            }
        }
        projectObject(obj, o, x, y, w, h);
        obj.pos += offset;
        layer.objects.append(std::move(obj));
    }
}

void Builder::projectObject(Object &obj, const QJsonObject &o, double x, double y, double w, double h)
{
    auto points = [](const QJsonArray &a) {
        QPolygonF poly;
        poly.reserve(a.size());
        for (const QJsonValue &v : a) {
            const QJsonObject po = v.toObject();
            poly << QPointF(jnum(po, "x"), jnum(po, "y"));
        }
        return poly;
    };
    const bool iso = m_info.isIso();

    if (o.contains(QLatin1String("point")) && o.value(QLatin1String("point")).toBool()) {
        obj.shape = Object::Shape::Point;
        obj.pos = iso ? isoProject(x, y) : QPointF(x, y);
        return;
    }
    if (o.contains(QLatin1String("polygon")) || o.contains(QLatin1String("polyline"))) {
        const bool closed = o.contains(QLatin1String("polygon"));
        obj.shape = closed ? Object::Shape::Polygon : Object::Shape::Polyline;
        QPolygonF rel = points(o.value(QLatin1String(closed ? "polygon" : "polyline")).toArray());
        if (iso) {
            QPolygonF abs;
            for (const QPointF &p : rel)
                abs << isoProject(x + p.x(), y + p.y());
            obj.pos = QPointF(0, 0);
            obj.points = abs;
            obj.rotation = 0;
        } else {
            obj.pos = QPointF(x, y);
            obj.points = rel;
        }
        return;
    }
    if (o.contains(QLatin1String("text"))) {
        obj.shape = Object::Shape::Text;
        obj.text = jstr(o.value(QLatin1String("text")).toObject(), "text");
        obj.pos = QPointF(x, y);
        obj.size = QSizeF(w, h);
        return;
    }
    const bool ellipse = o.value(QLatin1String("ellipse")).toBool();
    obj.shape = ellipse ? Object::Shape::Ellipse : Object::Shape::Rect;
    if (iso) {
        // A box on the ground plane projects to a diamond.
        QPolygonF poly;
        if (ellipse) {
            for (int i = 0; i < 32; ++i) {
                const double a = i * 2.0 * 3.14159265358979323846 / 32.0;
                poly << isoProject(x + w / 2.0 + std::cos(a) * w / 2.0, y + h / 2.0 + std::sin(a) * h / 2.0);
            }
        } else {
            poly << isoProject(x, y) << isoProject(x + w, y) << isoProject(x + w, y + h) << isoProject(x, y + h);
        }
        obj.shape = Object::Shape::Polygon;
        obj.pos = QPointF(0, 0);
        obj.points = poly;
        obj.rotation = 0;
        return;
    }
    obj.pos = QPointF(x, y);
    obj.size = QSizeF(w, h);
}

void Builder::addLayers(const QJsonArray &layers, const QString &prefix, QPointF offset, double opacity, bool visible)
{
    for (const QJsonValue &lv : layers) {
        if (m_ctx.cancelled() || m_ctx.instances > kMaxInstances)
            return;
        const QJsonObject l = lv.toObject();
        const QString type = jstr(l, "type");
        const QString name = jstr(l, "name");
        const QPointF layerOffset = offset + QPointF(jnum(l, "offsetx"), jnum(l, "offsety"));
        const double layerOpacity = opacity * jnum(l, "opacity", 1.0);
        const bool layerVisible = visible && jflag(l, "visible", true);

        if (type == QLatin1String("group")) {
            addLayers(l.value(QLatin1String("layers")).toArray(), prefix + name + QLatin1Char('/'), layerOffset,
                      layerOpacity, layerVisible);
            continue;
        }

        Layer layer;
        layer.name = prefix + name;
        layer.group = layer.name;
        layer.visible = layerVisible;
        layer.opacity = layerOpacity;
        if (type == QLatin1String("tilelayer")) {
            layer.kind = Layer::Kind::Tiles;
            addTileLayer(l, layer, layerOffset);
            finalizeLayer(layer);
        } else if (type == QLatin1String("objectgroup")) {
            layer.kind = Layer::Kind::Objects;
            addObjects(l, layer, layerOffset);
        } else if (type == QLatin1String("imagelayer")) {
            layer.kind = Layer::Kind::Image;
            const QString image = jstr(l, "image");
            if (!image.isEmpty()) {
                layer.image = m_ctx.loadImage(QDir(m_ctx.baseDir).absoluteFilePath(image));
                layer.imagePos = layerOffset + QPointF(jnum(l, "x"), jnum(l, "y"));
            }
        } else {
            continue;
        }
        m_doc.layers.push_back(std::move(layer));
    }
}

bool Builder::buildMap(const QJsonObject &root)
{
    m_info.orientation = jstr(root, "orientation");
    if (m_info.orientation.isEmpty())
        m_info.orientation = QStringLiteral("orthogonal");
    m_info.width = jint(root, "width");
    m_info.height = jint(root, "height");
    m_info.tileW = std::max(1, jint(root, "tilewidth", 16));
    m_info.tileH = std::max(1, jint(root, "tileheight", 16));
    m_info.infinite = jflag(root, "infinite", false);
    if (const QString ro = jstr(root, "renderorder"); !ro.isEmpty())
        m_info.renderOrder = ro;
    m_info.hexSide = jint(root, "hexsidelength");
    m_info.staggerX = jstr(root, "staggeraxis") == QLatin1String("x");
    m_info.staggerEven = jstr(root, "staggerindex") == QLatin1String("even");
    m_isoOriginX = (m_info.isIso() && !m_info.infinite) ? m_info.height * m_info.tileW / 2.0 : 0.0;

    const QJsonArray tilesets = root.value(QLatin1String("tilesets")).toArray();
    for (const QJsonValue &tv : tilesets)
        addTileset(tv.toObject(), m_ctx.baseDir);
    std::sort(m_lookup.begin(), m_lookup.end());

    addLayers(root.value(QLatin1String("layers")).toArray(), QString(), QPointF(0, 0), 1.0, true);
    if (m_ctx.cancelled())
        return false;
    if (m_ctx.instances > kMaxInstances) {
        m_ctx.error = QObject::tr("This map has more than %1 million tiles.").arg(kMaxInstances / 1'000'000);
        return false;
    }

    m_doc.format = QStringLiteral("Tiled map");
    m_doc.background = parseColor(jstr(root, "backgroundcolor"));
    m_doc.orthogonal = m_info.isOrtho();
    if (m_info.isOrtho()) {
        m_doc.gridW = m_info.tileW;
        m_doc.gridH = m_info.tileH;
    }
    QString size = m_info.infinite ? QObject::tr("infinite")
                                   : QObject::tr("%1 × %2 tiles").arg(m_info.width).arg(m_info.height);
    m_doc.detail = QStringLiteral("%1 · %2 · %3 × %4 px")
                       .arg(m_info.orientation, size).arg(m_info.tileW).arg(m_info.tileH);

    // Finite orthogonal maps are exactly their grid, even when empty or when
    // tall tiles stick out of it.
    if (m_info.isOrtho() && !m_info.infinite && m_info.width > 0 && m_info.height > 0) {
        Level frame;
        frame.rect = QRectF(0, 0, double(m_info.width) * m_info.tileW, double(m_info.height) * m_info.tileH);
        m_doc.levels.push_back(frame);
    }
    return true;
}

bool Builder::buildTilesetSheet(const QJsonObject &root)
{
    m_info.tileW = std::max(1, jint(root, "tilewidth", 16));
    m_info.tileH = std::max(1, jint(root, "tileheight", 16));
    buildTileset(root, m_ctx.baseDir, 0);
    if (m_doc.tilesets.empty())
        return false;
    Tileset &ts = m_doc.tilesets.front();

    Layer layer;
    layer.name = ts.name.isEmpty() ? QObject::tr("Tileset") : ts.name;
    layer.group = layer.name;
    const int count = ts.tileCount;
    if (qint64(count) > kMaxInstances) {
        m_ctx.error = QObject::tr("This tileset is too large to show.");
        return false;
    }
    const int cols = ts.columns > 0 ? ts.columns : std::max(1, int(std::ceil(std::sqrt(double(count)))));
    layer.instances.reserve(size_t(std::max(0, count)));
    for (int id = 0; id < count; ++id) {
        Instance in;
        in.tileset = 0;
        in.tile = id;
        if (ts.isCollection()) {
            const QImage *img = ts.collectionImage(id);
            if (!img)
                continue;
            in.w = uint16_t(std::min(img->width(), 65535));
            in.h = uint16_t(std::min(img->height(), 65535));
        } else if (!ts.image.isNull()) {
            if (ts.sheetRect(id).isEmpty())
                continue;
            in.w = uint16_t(ts.tileW);
            in.h = uint16_t(ts.tileH);
        } else {
            in.tileset = -1;
            in.tile = int32_t(0xA0808080u);
            in.w = uint16_t(ts.tileW);
            in.h = uint16_t(ts.tileH);
        }
        // Laid out in sheet order, without the sheet's margin and spacing.
        in.x = float((id % cols) * ts.tileW);
        in.y = float((id / cols) * ts.tileH);
        if (in.tileset >= 0 && ts.animations.contains(id))
            in.flags |= Animated;
        layer.instances.push_back(in);
    }
    finalizeLayer(layer);
    m_doc.layers.push_back(std::move(layer));

    m_doc.format = QStringLiteral("Tiled tileset");
    m_doc.tilesetSheet = true;
    m_doc.gridW = ts.tileW;
    m_doc.gridH = ts.tileH;
    m_doc.detail = QObject::tr("%n tile(s) · %1 × %2 px", nullptr, count).arg(ts.tileW).arg(ts.tileH);
    return !m_ctx.cancelled();
}

} // namespace

DocPtr loadTiled(const QByteArray &data, const QString &suffixLower, Context &ctx)
{
    QJsonObject root;
    bool isMapKind = true;

    if (suffixLower == QLatin1String("tmx") || suffixLower == QLatin1String("tsx")) {
        QXmlStreamReader xml(data);
        XNode node;
        while (!xml.atEnd() && xml.readNext() != QXmlStreamReader::StartElement) {}
        if (xml.atEnd() || xml.hasError()) {
            ctx.error = QObject::tr("The file is not valid XML: %1").arg(xml.errorString());
            return nullptr;
        }
        readNode(xml, node);
        if (xml.hasError()) {
            ctx.error = QObject::tr("The file is not valid XML: %1").arg(xml.errorString());
            return nullptr;
        }
        if (node.name == QLatin1String("map")) {
            root = mapToJson(node);
        } else if (node.name == QLatin1String("tileset")) {
            root = tilesetToJson(node);
            isMapKind = false;
        } else {
            ctx.error = QObject::tr("This is not a Tiled map or tileset.");
            return nullptr;
        }
    } else {
        QJsonParseError err;
        const QJsonDocument json = QJsonDocument::fromJson(data, &err);
        if (!json.isObject()) {
            ctx.error = QObject::tr("The file is not valid JSON: %1").arg(err.errorString());
            return nullptr;
        }
        root = json.object();
        isMapKind = root.contains(QLatin1String("layers")) || jstr(root, "type") == QLatin1String("map");
        if (!isMapKind && !root.contains(QLatin1String("tilewidth"))) {
            ctx.error = QObject::tr("This is not a Tiled map or tileset.");
            return nullptr;
        }
    }

    auto doc = std::make_unique<Document>();
    Builder builder(ctx, *doc);
    const bool ok = isMapKind ? builder.buildMap(root) : builder.buildTilesetSheet(root);
    return ok ? std::move(doc) : nullptr;
}

} // namespace TileMapLoader::detail
