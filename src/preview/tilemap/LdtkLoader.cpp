#include "TileMapLoaderInternal.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QObject>

#include <cmath>

// LDtk (https://ldtk.io/json/). Every level's layers of one name are merged
// into a single layer in world coordinates, so the layer toggles stay few even
// for a world of hundreds of levels.
namespace TileMapLoader::detail {

using namespace TileMap;

namespace {

constexpr int kPollMask = 0xFFFF;

struct TilesetDef {
    int index = -1;       // into Document::tilesets
    int grid = 16;
    int padding = 0;
    int spacing = 0;
    int columns = 1;
    bool loaded = false;
};

QColor colorOf(const QJsonValue &v)
{
    return v.isString() ? QColor(v.toString()) : QColor();
}

class LdtkBuilder
{
public:
    LdtkBuilder(Context &ctx, Document &doc) : m_ctx(ctx), m_doc(doc) {}
    bool build(const QJsonObject &root, bool singleLevelFile);

private:
    void readDefs(const QJsonObject &defs);
    void addLevel(const QJsonObject &level, QPointF origin);
    Layer &layerFor(const QString &name, Layer::Kind kind);
    void addTiles(Layer &layer, const QJsonArray &tiles, const TilesetDef &td, int tilesetIndex, QPointF origin,
                  QPointF offset);
    void addIntGrid(Layer &layer, const QJsonObject &li, int layerUid, QPointF origin, QPointF offset);
    void addEntities(Layer &layer, const QJsonObject &li, QPointF origin, QPointF offset);

    Context &m_ctx;
    Document &m_doc;
    QHash<int, TilesetDef> m_tilesetByUid;
    QHash<int, QHash<int, QColor>> m_intColors;   // layer def uid -> value -> colour
    QHash<int, QColor> m_entityColors;
    QHash<QString, int> m_layerByName;
    QString m_layout;
    QColor m_rootBg;
};

void LdtkBuilder::readDefs(const QJsonObject &defs)
{
    const QJsonArray tilesets = defs.value(QLatin1String("tilesets")).toArray();
    for (const QJsonValue &tv : tilesets) {
        const QJsonObject t = tv.toObject();
        const int uid = jint(t, "uid", -1);
        const QString rel = jstr(t, "relPath");
        TilesetDef td;
        td.grid = std::max(1, jint(t, "tileGridSize", 16));
        td.padding = jint(t, "padding");
        td.spacing = jint(t, "spacing");

        Tileset ts;
        ts.name = jstr(t, "identifier");
        ts.tileW = ts.tileH = td.grid;
        ts.margin = td.padding;
        ts.spacing = td.spacing;
        int pxW = jint(t, "pxWid");
        int pxH = jint(t, "pxHei");
        if (!rel.isEmpty()) {
            ts.image = m_ctx.loadImage(QDir(m_ctx.baseDir).absoluteFilePath(rel));
            td.loaded = !ts.image.isNull();
            if (td.loaded) {
                pxW = ts.image.width();
                pxH = ts.image.height();
            }
        } else {
            m_ctx.warnings << QObject::tr("Tileset \"%1\" is embedded in the project and cannot be shown.").arg(ts.name);
        }
        td.columns = std::max(1, (pxW - 2 * td.padding + td.spacing) / (td.grid + td.spacing));
        ts.columns = td.columns;
        ts.tileCount = td.columns * std::max(1, (pxH - 2 * td.padding + td.spacing) / (td.grid + td.spacing));
        td.index = int(m_doc.tilesets.size());
        m_doc.tilesets.push_back(std::move(ts));
        m_tilesetByUid.insert(uid, td);
    }

    const QJsonArray layers = defs.value(QLatin1String("layers")).toArray();
    for (const QJsonValue &lv : layers) {
        const QJsonObject l = lv.toObject();
        QHash<int, QColor> colors;
        for (const QJsonValue &vv : l.value(QLatin1String("intGridValues")).toArray()) {
            const QJsonObject vo = vv.toObject();
            colors.insert(jint(vo, "value"), colorOf(vo.value(QLatin1String("color"))));
        }
        m_intColors.insert(jint(l, "uid", -1), colors);
    }
    for (const QJsonValue &ev : defs.value(QLatin1String("entities")).toArray()) {
        const QJsonObject e = ev.toObject();
        m_entityColors.insert(jint(e, "uid", -1), colorOf(e.value(QLatin1String("color"))));
    }
}

Layer &LdtkBuilder::layerFor(const QString &name, Layer::Kind kind)
{
    const auto it = m_layerByName.constFind(name);
    if (it != m_layerByName.constEnd())
        return m_doc.layers[static_cast<size_t>(it.value())];
    Layer layer;
    layer.kind = kind;
    layer.name = name;
    layer.group = name;
    m_layerByName.insert(name, int(m_doc.layers.size()));
    m_doc.layers.push_back(std::move(layer));
    return m_doc.layers.back();
}

void LdtkBuilder::addTiles(Layer &layer, const QJsonArray &tiles, const TilesetDef &td, int tilesetIndex,
                           QPointF origin, QPointF offset)
{
    layer.instances.reserve(layer.instances.size() + size_t(tiles.size()));
    int poll = 0;
    for (const QJsonValue &tv : tiles) {
        if ((++poll & kPollMask) == 0 && m_ctx.cancelled())
            return;
        const QJsonObject t = tv.toObject();
        const QJsonArray px = t.value(QLatin1String("px")).toArray();
        const QJsonArray src = t.value(QLatin1String("src")).toArray();
        if (px.size() < 2 || src.size() < 2)
            continue;
        Instance in;
        in.w = in.h = uint16_t(td.grid);
        in.x = float(origin.x() + offset.x() + px.at(0).toDouble());
        in.y = float(origin.y() + offset.y() + px.at(1).toDouble());
        if (td.loaded && tilesetIndex >= 0) {
            const int col = (int(src.at(0).toDouble()) - td.padding) / (td.grid + td.spacing);
            const int row = (int(src.at(1).toDouble()) - td.padding) / (td.grid + td.spacing);
            const Tileset &ts = m_doc.tilesets[static_cast<size_t>(tilesetIndex)];
            in.tileset = int16_t(tilesetIndex);
            in.tile = row * td.columns + col;
            if (ts.sheetRect(in.tile).isEmpty())
                continue;
        } else {
            in.tileset = -1;
            in.tile = int32_t(0xA0808080u);
        }
        const int f = jint(t, "f");
        if (f & 1) in.flags |= FlipH;
        if (f & 2) in.flags |= FlipV;
        layer.instances.push_back(in);
    }
    m_ctx.instances += tiles.size();
}

void LdtkBuilder::addIntGrid(Layer &layer, const QJsonObject &li, int layerUid, QPointF origin, QPointF offset)
{
    const QJsonArray csv = li.value(QLatin1String("intGridCsv")).toArray();
    const int cw = jint(li, "__cWid"), grid = std::max(1, jint(li, "__gridSize", 16));
    if (cw <= 0)
        return;
    const QHash<int, QColor> colors = m_intColors.value(layerUid);
    for (qsizetype i = 0; i < csv.size(); ++i) {
        const int v = csv.at(i).toInt();
        if (v == 0)
            continue;
        QColor c = colors.value(v);
        if (!c.isValid())
            c = QColor(255, 255, 255);
        c.setAlpha(0xA0);
        Instance in;
        in.tileset = -1;
        in.tile = int32_t(c.rgba());
        in.w = in.h = uint16_t(grid);
        in.x = float(origin.x() + offset.x() + double(i % cw) * grid);
        in.y = float(origin.y() + offset.y() + double(i / cw) * grid);
        layer.instances.push_back(in);
    }
    m_ctx.instances += csv.size();
}

void LdtkBuilder::addEntities(Layer &layer, const QJsonObject &li, QPointF origin, QPointF offset)
{
    for (const QJsonValue &ev : li.value(QLatin1String("entityInstances")).toArray()) {
        const QJsonObject e = ev.toObject();
        const QJsonArray px = e.value(QLatin1String("px")).toArray();
        if (px.size() < 2)
            continue;
        Object o;
        o.name = jstr(e, "__identifier");
        const double w = jnum(e, "width"), h = jnum(e, "height");
        const QJsonArray pivot = e.value(QLatin1String("__pivot")).toArray();
        const double pvx = pivot.size() > 0 ? pivot.at(0).toDouble() : 0.0;
        const double pvy = pivot.size() > 1 ? pivot.at(1).toDouble() : 0.0;
        o.pos = QPointF(origin.x() + offset.x() + px.at(0).toDouble() - pvx * w,
                        origin.y() + offset.y() + px.at(1).toDouble() - pvy * h);
        o.size = QSizeF(w, h);
        o.shape = Object::Shape::Rect;
        o.color = colorOf(e.value(QLatin1String("__smartColor")));
        if (!o.color.isValid())
            o.color = m_entityColors.value(jint(e, "defUid", -1));
        layer.objects.append(std::move(o));
    }
}

void LdtkBuilder::addLevel(const QJsonObject &levelIn, QPointF origin)
{
    QJsonObject level = levelIn;
    const QString external = jstr(level, "externalRelPath");
    if (level.value(QLatin1String("layerInstances")).isNull() && !external.isEmpty()) {
        const QString path = QDir(m_ctx.baseDir).absoluteFilePath(external);
        const QJsonObject loaded = QJsonDocument::fromJson(m_ctx.readFile(path)).object();
        if (loaded.isEmpty())
            m_ctx.warnings << QObject::tr("Level file not found: %1").arg(QDir::toNativeSeparators(path));
        else
            level = loaded;
    }

    Level frame;
    frame.name = jstr(level, "identifier");
    frame.rect = QRectF(origin, QSizeF(jnum(level, "pxWid"), jnum(level, "pxHei")));
    frame.background = colorOf(level.value(QLatin1String("__bgColor")));
    if (!frame.background.isValid())
        frame.background = m_rootBg;
    m_doc.levels.push_back(frame);

    const QString bgRel = jstr(level, "bgRelPath");
    if (!bgRel.isEmpty()) {
        const QImage img = m_ctx.loadImage(QDir(m_ctx.baseDir).absoluteFilePath(bgRel));
        if (!img.isNull()) {
            Layer &bg = layerFor(QObject::tr("Level backgrounds"), Layer::Kind::Image);
            // One image per level: stash as extra objects would lose the pixels,
            // so only the first background image is kept.
            if (bg.image.isNull()) {
                bg.image = img;
                bg.imagePos = origin;
            }
        }
    }

    const QJsonArray instances = level.value(QLatin1String("layerInstances")).toArray();
    // The file lists the top-most layer first; the document draws bottom first.
    for (qsizetype i = instances.size(); i-- > 0;) {
        if (m_ctx.cancelled() || m_ctx.instances > kMaxInstances)
            return;
        const QJsonObject li = instances.at(i).toObject();
        const QString type = jstr(li, "__type");
        const QString name = jstr(li, "__identifier");
        const QPointF offset(jnum(li, "__pxTotalOffsetX"), jnum(li, "__pxTotalOffsetY"));
        const int layerUid = jint(li, "layerDefUid", -1);

        if (type == QLatin1String("Entities")) {
            Layer &layer = layerFor(name, Layer::Kind::Objects);
            addEntities(layer, li, origin, offset);
            continue;
        }

        const int tsUid = jint(li, "__tilesetDefUid", -1);
        const auto tdIt = m_tilesetByUid.constFind(tsUid);
        const QJsonArray autoTiles = li.value(QLatin1String("autoLayerTiles")).toArray();
        const QJsonArray gridTiles = li.value(QLatin1String("gridTiles")).toArray();
        const bool hasTiles = !autoTiles.isEmpty() || !gridTiles.isEmpty();

        Layer &layer = layerFor(name, Layer::Kind::Tiles);
        layer.visible = layer.visible && jflag(li, "visible", true);
        layer.opacity = jnum(li, "__opacity", 1.0);
        if (hasTiles) {
            const TilesetDef td = tdIt != m_tilesetByUid.constEnd() ? tdIt.value() : TilesetDef();
            if (!autoTiles.isEmpty())
                addTiles(layer, autoTiles, td, td.index, origin, offset);
            if (!gridTiles.isEmpty())
                addTiles(layer, gridTiles, td, td.index, origin, offset);
        } else if (type == QLatin1String("IntGrid")) {
            addIntGrid(layer, li, layerUid, origin, offset);
        }
    }
}

bool LdtkBuilder::build(const QJsonObject &root, bool singleLevelFile)
{
    m_rootBg = colorOf(root.value(QLatin1String("bgColor")));
    readDefs(root.value(QLatin1String("defs")).toObject());

    // Levels: top-level array, or the worlds of a multi-world project.
    std::vector<QJsonObject> levels;
    m_layout = jstr(root, "worldLayout");
    if (singleLevelFile) {
        levels.push_back(root);
    } else {
        for (const QJsonValue &lv : root.value(QLatin1String("levels")).toArray())
            levels.push_back(lv.toObject());
        if (levels.empty()) {
            for (const QJsonValue &wv : root.value(QLatin1String("worlds")).toArray()) {
                const QJsonObject w = wv.toObject();
                if (m_layout.isEmpty())
                    m_layout = jstr(w, "worldLayout");
                for (const QJsonValue &lv : w.value(QLatin1String("levels")).toArray())
                    levels.push_back(lv.toObject());
            }
        }
    }
    if (levels.empty() && !singleLevelFile) {
        m_ctx.error = QObject::tr("This LDtk project has no levels.");
        return false;
    }
    if (singleLevelFile)
        m_ctx.warnings << QObject::tr("This is a single level file; open the project's .ldtk file to see its tilesets.");

    const bool linear = m_layout.startsWith(QLatin1String("Linear"));
    const bool horizontal = m_layout == QLatin1String("LinearHorizontal");
    double cursor = 0;
    for (const QJsonObject &level : levels) {
        if (m_ctx.cancelled())
            return false;
        QPointF origin(jnum(level, "worldX"), jnum(level, "worldY"));
        if (linear && !level.value(QLatin1String("worldX")).isDouble()) {
            origin = horizontal ? QPointF(cursor, 0) : QPointF(0, cursor);
        }
        cursor += (horizontal ? jnum(level, "pxWid") : jnum(level, "pxHei")) + 16;
        addLevel(level, origin);
        if (m_ctx.instances > kMaxInstances) {
            m_ctx.error = QObject::tr("This project has more than %1 million tiles.").arg(kMaxInstances / 1'000'000);
            return false;
        }
    }

    for (Layer &layer : m_doc.layers) {
        if (layer.kind == Layer::Kind::Tiles)
            finalizeLayer(layer);
    }

    const int defaultGrid = jint(root, "defaultGridSize", 16);
    m_doc.format = singleLevelFile ? QStringLiteral("LDtk level") : QStringLiteral("LDtk project");
    m_doc.background = m_rootBg;
    m_doc.gridW = m_doc.gridH = defaultGrid;
    m_doc.detail = QObject::tr("%n level(s) · %1 layers · grid %2 px", nullptr, int(levels.size()))
                       .arg(int(m_doc.layers.size())).arg(defaultGrid);
    return !m_ctx.cancelled();
}

} // namespace

DocPtr loadLdtk(const QByteArray &data, const QString &suffixLower, Context &ctx)
{
    QJsonParseError err;
    const QJsonDocument json = QJsonDocument::fromJson(data, &err);
    if (!json.isObject()) {
        ctx.error = QObject::tr("The file is not valid JSON: %1").arg(err.errorString());
        return nullptr;
    }
    const QJsonObject root = json.object();
    const bool singleLevel = suffixLower == QLatin1String("ldtkl")
                             || (root.contains(QLatin1String("layerInstances")) && !root.contains(QLatin1String("levels"))
                                 && !root.contains(QLatin1String("worlds")));
    auto doc = std::make_unique<Document>();
    LdtkBuilder builder(ctx, *doc);
    return builder.build(root, singleLevel) ? std::move(doc) : nullptr;
}

} // namespace TileMapLoader::detail
