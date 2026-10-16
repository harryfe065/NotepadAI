#ifndef TILEMAPDOCUMENT_H
#define TILEMAPDOCUMENT_H

#include <QColor>
#include <QHash>
#include <QImage>
#include <QPoint>
#include <QPolygonF>
#include <QRect>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

// Format-neutral tile map, filled by the Tiled and LDtk loaders and painted by
// TileMapCanvas. Every tile is resolved at load time to a map-pixel rectangle,
// so orientation (orthogonal / isometric / staggered / hexagonal), render order
// and layer offsets never reach the canvas: it draws a list of instances.
namespace TileMap {

struct AnimFrame {
    int tile = 0;
    int durationMs = 100;
};

struct Animation {
    std::vector<AnimFrame> frames;
    int totalMs = 0;           // sum of the (>= 1 ms) frame durations, set at load
};

struct Tileset {
    QString name;
    int firstGid = 0;          // Tiled global id of tile 0
    int tileW = 0, tileH = 0;
    int margin = 0, spacing = 0;
    int columns = 0;
    int tileCount = 0;
    QPoint offset;             // Tiled <tileoffset>: drawing offset of every tile
    QImage image;            // the sheet (ARGB32_Premultiplied); null for a collection
    std::vector<QImage> tiles; // collection of images: one per tile id (null when absent)
    QHash<int, Animation> animations;   // keyed by local tile id; never empty
    int animationTotalMs(int tile) const
    {
        const auto it = animations.constFind(tile);
        return it != animations.constEnd() ? it->totalMs : 0;
    }

    bool isCollection() const { return image.isNull() && !tiles.empty(); }

    // Pixel rect of `id` inside image, or an empty rect. 64-bit maths: ids,
    // columns and spacing come from the file.
    QRect sheetRect(int id) const
    {
        if (columns <= 0 || id < 0 || image.isNull())
            return {};
        const qint64 col = id % columns, row = id / columns;
        const qint64 x = margin + col * (qint64(tileW) + spacing), y = margin + row * (qint64(tileH) + spacing);
        if (x < 0 || y < 0 || x + tileW > image.width() || y + tileH > image.height() || tileW <= 0 || tileH <= 0)
            return {};
        return QRect(int(x), int(y), tileW, tileH);
    }
    const QImage *collectionImage(int id) const
    {
        if (id < 0 || static_cast<size_t>(id) >= tiles.size() || tiles[static_cast<size_t>(id)].isNull())
            return nullptr;
        return &tiles[static_cast<size_t>(id)];
    }
};

enum InstanceFlag : uint8_t {
    FlipH = 1,
    FlipV = 2,
    FlipD = 4,       // diagonal (transpose), applied before H and V
    Animated = 8,    // drawn live at fine levels; coarse chunks bake its first frame
};

// One drawn tile: 20 bytes (fields ordered so there is no inner padding), so
// a million-tile map costs 20 MB.
struct Instance {
    float x = 0, y = 0;        // top-left, map pixels
    uint16_t w = 0, h = 0;     // drawn size
    int32_t tile = 0;          // local tile id, or the QRgb of a solid colour
    int16_t tileset = -1;      // index into Document::tilesets; -1 = solid colour
    uint8_t flags = 0;
};
static_assert(sizeof(Instance) == 20, "Instance is sized for 20 bytes");

// Instances of a layer (or a subset of them) bucketed by the cell of their
// top-left corner, 256 px or coarser so the table stays bounded, stored CSR
// style (two arrays, no per-cell allocation). A query returns the indices in
// ascending (= draw) order.
struct GridIndex {
    float cell = 256.0f;
    int cols = 0, rows = 0;
    float originX = 0, originY = 0;
    float maxW = 0, maxH = 0;
    std::vector<uint32_t> start;  // cols * rows + 1
    std::vector<uint32_t> items;

    void build(const std::vector<Instance> &instances);
    // Indexes only instances[subset[i]]; queries return those instance indices.
    void build(const std::vector<Instance> &instances, const std::vector<uint32_t> &subset);
    // Appends the indices of instances that may intersect `r`, sorted ascending.
    void query(const QRectF &r, std::vector<uint32_t> &out) const;
    // Upper bound of what query(r) would return, without copying: O(rows).
    size_t countCandidates(const QRectF &r) const;
};

// How map pixels map back to cells, for the hover read-out.
struct CellLayout {
    enum class Kind : uint8_t {
        None,         // no cells: Document::gridW / gridH (or none) decide
        Isometric,    // diamonds; (col, row) top vertex at ((col - row) * w / 2 + originX, (col + row) * h / 2)
        Staggered,    // Tiled staggered and hexagonal (hexSide > 0)
    };
    Kind kind = Kind::None;
    int tileW = 0, tileH = 0;
    double originX = 0;
    int hexSide = 0;
    bool staggerX = false;     // staggeraxis == "x"
    bool staggerEven = false;  // staggerindex == "even"
};

struct Object {
    enum class Shape : uint8_t { Rect, Ellipse, Point, Polygon, Polyline, Text, Tile };
    Shape shape = Shape::Rect;
    QString name;
    QString type;
    QPointF pos;               // map pixels (top-left of the box; the vertex origin for polygons)
    QSizeF size;
    double rotation = 0;       // degrees, about pos
    QPolygonF points;          // relative to pos
    QString text;
    Instance tile;             // Shape::Tile
    QColor color;              // invalid = layer default
    bool visible = true;
};

struct Layer {
    enum class Kind : uint8_t { Tiles, Objects, Image };
    Kind kind = Kind::Tiles;
    QString name;
    QString group;             // layers sharing it toggle together (LDtk: one per level)
    bool visible = true;
    double opacity = 1.0;
    QColor color;              // object outline colour, when the format names one

    std::vector<Instance> instances;
    std::vector<uint32_t> animated;   // indices of Animated instances
    GridIndex grid;
    GridIndex animatedGrid;           // the animated ones only (live drawing culls with it)

    QVector<Object> objects;

    QImage image;
    QPointF imagePos;
};

struct Level {
    QString name;
    QRectF rect;
    QColor background;
};

struct Document {
    QString format;            // "Tiled map", "LDtk project", ...
    QString detail;            // "orthogonal · 100 × 60 tiles · 16 × 16 px"
    bool tilesetSheet = false; // a bare tileset: tiles laid out in sheet order
    QRectF bounds;             // everything drawn, map pixels
    QColor background;         // invalid = theme base
    int gridW = 0, gridH = 0;  // grid overlay cell, 0 = none
    CellLayout cells;          // non-orthogonal Tiled maps: hover cell projection
    bool orthogonal = true;

    std::vector<Tileset> tilesets;
    std::vector<Layer> layers;
    std::vector<Level> levels;
    QStringList warnings;
    qint64 fileSize = 0;
    qint64 tileCount = 0;      // instances over all layers
    bool hasAnimations = false;
    bool hasObjects = false;
};

using DocumentPtr = std::shared_ptr<const Document>;

// "layer · tile 12 (tileset)" for the info line; empty when nothing is there.
QString describeInstance(const Document &doc, const Layer &layer, const Instance &inst);

// Topmost visible tile at `mapPos` over the enabled layers, or nullptr.
const Instance *hitTest(const Document &doc, const std::vector<bool> &layerOn, const QPointF &mapPos,
                        const Layer **layerOut);

// Cell under `mapPos` (Document::cells, else the gridW x gridH grid); false
// when the document has no cells.
bool cellAt(const Document &doc, const QPointF &mapPos, QPoint *cell);

} // namespace TileMap

#endif // TILEMAPDOCUMENT_H
