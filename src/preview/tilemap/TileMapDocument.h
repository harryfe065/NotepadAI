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
    QHash<int, std::vector<AnimFrame>> animations;
    int animationTotalMs(int tile) const
    {
        int total = 0;
        for (const AnimFrame &f : animations.value(tile))
            total += std::max(1, f.durationMs);
        return total;
    }

    bool isCollection() const { return image.isNull() && !tiles.empty(); }

    // Pixel rect of `id` inside image, or an empty rect.
    QRect sheetRect(int id) const
    {
        if (columns <= 0 || id < 0 || image.isNull())
            return {};
        const int col = id % columns, row = id / columns;
        const QRect r(margin + col * (tileW + spacing), margin + row * (tileH + spacing), tileW, tileH);
        return image.rect().contains(r) ? r : QRect();
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
    Animated = 8,    // drawn live by the canvas, not baked into chunks
};

// One drawn tile: 20 bytes, so a million-tile map costs 20 MB.
struct Instance {
    float x = 0, y = 0;        // top-left, map pixels
    uint16_t w = 0, h = 0;     // drawn size
    int16_t tileset = -1;      // index into Document::tilesets; -1 = solid colour
    int32_t tile = 0;          // local tile id, or the QRgb of a solid colour
    uint8_t flags = 0;
};

// Instances of a layer bucketed by the 256 px cell of their top-left corner,
// stored CSR style (two arrays, no per-cell allocation). A query returns the
// indices in ascending (= draw) order.
struct GridIndex {
    float cell = 256.0f;
    int cols = 0, rows = 0;
    float originX = 0, originY = 0;
    float maxW = 0, maxH = 0;
    std::vector<uint32_t> start;  // cols * rows + 1
    std::vector<uint32_t> items;

    void build(const std::vector<Instance> &instances);
    // Appends the indices of instances that may intersect `r`, sorted ascending.
    void query(const QRectF &r, std::vector<uint32_t> &out) const;
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

} // namespace TileMap

#endif // TILEMAPDOCUMENT_H
