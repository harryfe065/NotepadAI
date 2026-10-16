#include "TileMapDocument.h"

#include <QObject>

namespace TileMap {

namespace {

constexpr double kMinCell = 256.0;
constexpr double kMaxCells = double(qint64(1) << 22);   // 4M cells = 16 MB of starts

// Cell of a coordinate already divided by the cell size, clamped to [0, n - 1]
// in floating point first: NaN goes to 0 and nothing overflows an int.
int clampCell(double v, int n)
{
    if (v >= double(n - 1))
        return n - 1;
    return v > 0 ? int(v) : 0;
}

// Shared by both build() overloads: `count` entries, entry k being the
// instance at index `at(k)`.
template<class IndexAt>
void buildGrid(GridIndex &g, const std::vector<Instance> &instances, size_t count, IndexAt at)
{
    g.cols = g.rows = 0;
    g.start.clear();
    g.items.clear();
    g.maxW = g.maxH = 0;
    g.originX = g.originY = 0;
    g.cell = float(kMinCell);

    // Bounds over finite corners only (loaders drop the rest; the index must
    // still never loop or overflow on them).
    double minX = 0, minY = 0, maxX = 0, maxY = 0;
    bool any = false;
    for (size_t k = 0; k < count; ++k) {
        const Instance &in = instances[at(k)];
        g.maxW = std::max(g.maxW, float(in.w));
        g.maxH = std::max(g.maxH, float(in.h));
        if (!std::isfinite(in.x) || !std::isfinite(in.y))
            continue;
        if (!any) {
            minX = maxX = in.x;
            minY = maxY = in.y;
            any = true;
            continue;
        }
        minX = std::min(minX, double(in.x));
        minY = std::min(minY, double(in.y));
        maxX = std::max(maxX, double(in.x));
        maxY = std::max(maxY, double(in.y));
    }
    if (count == 0)
        return;
    g.originX = float(minX);
    g.originY = float(minY);

    // Grow the cell until the table is bounded (huge sparse worlds). In double:
    // the spans are finite, so this ends, and the counts fit an int once it does.
    double cell = kMinCell, cols = 1, rows = 1;
    for (;;) {
        cols = std::floor((maxX - minX) / cell) + 1.0;
        rows = std::floor((maxY - minY) / cell) + 1.0;
        if (cols * rows <= kMaxCells)
            break;
        cell *= 2.0;
    }
    g.cell = float(cell);   // a power of two: exact in float
    g.cols = int(cols);
    g.rows = int(rows);

    const size_t cellCount = size_t(g.cols) * size_t(g.rows);
    g.start.assign(cellCount + 1, 0);
    auto cellOf = [&g](const Instance &in) {
        const int cx = clampCell((double(in.x) - g.originX) / g.cell, g.cols);
        const int cy = clampCell((double(in.y) - g.originY) / g.cell, g.rows);
        return size_t(cy) * size_t(g.cols) + size_t(cx);
    };
    for (size_t k = 0; k < count; ++k)
        ++g.start[cellOf(instances[at(k)]) + 1];
    for (size_t i = 0; i < cellCount; ++i)
        g.start[i + 1] += g.start[i];

    g.items.resize(count);
    std::vector<uint32_t> fill(g.start.begin(), g.start.end() - 1);
    for (size_t k = 0; k < count; ++k) {
        const size_t i = at(k);
        g.items[fill[cellOf(instances[i])]++] = uint32_t(i);   // ascending within a cell
    }
}

} // namespace

void GridIndex::build(const std::vector<Instance> &instances)
{
    buildGrid(*this, instances, instances.size(), [](size_t k) { return k; });
}

void GridIndex::build(const std::vector<Instance> &instances, const std::vector<uint32_t> &subset)
{
    // `subset` is ascending (finalizeLayer fills it in order), so cells stay sorted.
    buildGrid(*this, instances, subset.size(), [&subset](size_t k) { return size_t(subset[k]); });
}

namespace {

// Cell range of a query, or false when it misses the indexed area.
bool queryRange(const GridIndex &g, const QRectF &r, int &cx0, int &cy0, int &cx1, int &cy1)
{
    if (g.items.empty())
        return false;
    // An instance is stored under its top-left cell and reaches right / down
    // by at most maxW / maxH, so widen the query to the left / up.
    const double x0 = r.left() - g.maxW, y0 = r.top() - g.maxH;
    const double x1 = r.right(), y1 = r.bottom();
    if (!(x1 >= g.originX && y1 >= g.originY && x0 <= g.originX + double(g.cols) * g.cell
          && y0 <= g.originY + double(g.rows) * g.cell))
        return false;
    cx0 = clampCell(std::floor((x0 - g.originX) / g.cell), g.cols);
    cy0 = clampCell(std::floor((y0 - g.originY) / g.cell), g.rows);
    cx1 = clampCell(std::floor((x1 - g.originX) / g.cell), g.cols);
    cy1 = clampCell(std::floor((y1 - g.originY) / g.cell), g.rows);
    return true;
}

} // namespace

void GridIndex::query(const QRectF &r, std::vector<uint32_t> &out) const
{
    int cx0, cy0, cx1, cy1;
    if (!queryRange(*this, r, cx0, cy0, cx1, cy1))
        return;
    const size_t first = out.size();
    for (int cy = cy0; cy <= cy1; ++cy) {
        const size_t row = size_t(cy) * size_t(cols);
        const uint32_t b = start[row + size_t(cx0)];
        // Cells of one row are contiguous: one range per row.
        const uint32_t e = start[row + size_t(cx1) + 1];
        out.insert(out.end(), items.begin() + b, items.begin() + e);
    }
    std::sort(out.begin() + std::ptrdiff_t(first), out.end());
}

size_t GridIndex::countCandidates(const QRectF &r) const
{
    int cx0, cy0, cx1, cy1;
    if (!queryRange(*this, r, cx0, cy0, cx1, cy1))
        return 0;
    size_t n = 0;
    for (int cy = cy0; cy <= cy1; ++cy) {
        const size_t row = size_t(cy) * size_t(cols);
        n += start[row + size_t(cx1) + 1] - start[row + size_t(cx0)];
    }
    return n;
}

QString describeInstance(const Document &doc, const Layer &layer, const Instance &inst)
{
    if (inst.tileset < 0) {
        const QColor c = QColor::fromRgba(QRgb(inst.tile));
        return QObject::tr("%1 · %2").arg(layer.name, c.name());
    }
    const Tileset &ts = doc.tilesets[static_cast<size_t>(inst.tileset)];
    if (ts.firstGid > 0 && !doc.tilesetSheet)
        return QObject::tr("%1 · tile %2 (gid %3, %4)")
            .arg(layer.name).arg(inst.tile).arg(ts.firstGid + inst.tile).arg(ts.name);
    return QObject::tr("%1 · tile %2 (%3)").arg(layer.name).arg(inst.tile).arg(ts.name);
}

const Instance *hitTest(const Document &doc, const std::vector<bool> &layerOn, const QPointF &mapPos,
                        const Layer **layerOut)
{
    static thread_local std::vector<uint32_t> scratch;
    for (size_t li = doc.layers.size(); li-- > 0;) {
        const Layer &layer = doc.layers[li];
        if (layer.kind != Layer::Kind::Tiles || (li < layerOn.size() && !layerOn[li]))
            continue;
        scratch.clear();
        layer.grid.query(QRectF(mapPos, QSizeF(0.001, 0.001)), scratch);
        for (size_t k = scratch.size(); k-- > 0;) {
            const Instance &in = layer.instances[scratch[k]];
            if (mapPos.x() >= in.x && mapPos.x() < in.x + in.w && mapPos.y() >= in.y && mapPos.y() < in.y + in.h) {
                if (layerOut)
                    *layerOut = &layer;
                return &in;
            }
        }
    }
    return nullptr;
}

namespace {

int floorToInt(double v)
{
    v = std::floor(v);
    if (!(v > -2e9))
        return v < 0 ? -2'000'000'000 : 0;   // NaN -> 0
    return v < 2e9 ? int(v) : 2'000'000'000;
}

// Tiled staggered / hexagonal cells, placed exactly like TiledLoader's
// imageTopLeft() places a full-height tile.
struct StaggerGeometry {
    double tw, th, sideX = 0, sideY = 0, offX, offY, colW, rowH;
    bool staggerX, staggerEven;

    explicit StaggerGeometry(const CellLayout &c)
        : tw(std::max(1, c.tileW)), th(std::max(1, c.tileH)), staggerX(c.staggerX), staggerEven(c.staggerEven)
    {
        if (c.staggerX)
            sideX = std::max(0, c.hexSide);
        else
            sideY = std::max(0, c.hexSide);
        offX = (tw - sideX) / 2.0;
        offY = (th - sideY) / 2.0;
        colW = std::max(0.5, offX + sideX);
        rowH = std::max(0.5, offY + sideY);
    }
    bool shifted(int i) const { return ((i & 1) != 0) != staggerEven; }
    QPointF topLeft(int col, int row) const
    {
        if (staggerX)
            return {col * colW, row * (th + sideY) + (shifted(col) ? rowH : 0.0)};
        return {col * (tw + sideX) + (shifted(row) ? colW : 0.0), row * rowH};
    }
    // Inside the cell's diamond (no side) or hexagon, `p` relative to its box.
    bool contains(const QPointF &p) const
    {
        const double dx = std::abs(p.x() - tw / 2.0), dy = std::abs(p.y() - th / 2.0);
        if (staggerX)
            return dy <= th / 2.0 && dx <= sideX / 2.0 + offX * (1.0 - dy / (th / 2.0));
        return dx <= tw / 2.0 && dy <= sideY / 2.0 + offY * (1.0 - dx / (tw / 2.0));
    }
};

} // namespace

bool cellAt(const Document &doc, const QPointF &mapPos, QPoint *cell)
{
    const CellLayout &c = doc.cells;
    switch (c.kind) {
    case CellLayout::Kind::None:
        if (doc.gridW <= 0 || doc.gridH <= 0)
            return false;
        *cell = QPoint(floorToInt(mapPos.x() / doc.gridW), floorToInt(mapPos.y() / doc.gridH));
        return true;
    case CellLayout::Kind::Isometric: {
        // Inverse of the diamond placement: x and y are the half-sums.
        const double tx = (mapPos.x() - c.originX) / std::max(1, c.tileW);
        const double ty = mapPos.y() / std::max(1, c.tileH);
        *cell = QPoint(floorToInt(ty + tx), floorToInt(ty - tx));
        return true;
    }
    case CellLayout::Kind::Staggered: {
        const StaggerGeometry g(c);
        // The strip the point falls in gives a cell at most one step off;
        // the shape test over its neighbours settles it.
        int col, row;
        if (g.staggerX) {
            col = floorToInt(mapPos.x() / g.colW);
            row = floorToInt((mapPos.y() - (g.shifted(col) ? g.rowH : 0.0)) / (g.th + g.sideY));
        } else {
            row = floorToInt(mapPos.y() / g.rowH);
            col = floorToInt((mapPos.x() - (g.shifted(row) ? g.colW : 0.0)) / (g.tw + g.sideX));
        }
        static constexpr int kOrder[9][2] = {{0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
        for (const auto &d : kOrder) {
            const int cc = col + d[0], rr = row + d[1];
            if (g.contains(mapPos - g.topLeft(cc, rr))) {
                *cell = QPoint(cc, rr);
                return true;
            }
        }
        *cell = QPoint(col, row);
        return true;
    }
    }
    return false;
}

} // namespace TileMap
