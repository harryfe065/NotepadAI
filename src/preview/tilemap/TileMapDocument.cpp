#include "TileMapDocument.h"

#include <QObject>

namespace TileMap {

namespace {

constexpr float kMinCell = 256.0f;
constexpr qint64 kMaxCells = qint64(1) << 22;   // 4M cells = 16 MB of starts

} // namespace

void GridIndex::build(const std::vector<Instance> &instances)
{
    cols = rows = 0;
    start.clear();
    items.clear();
    maxW = maxH = 0;
    if (instances.empty())
        return;

    float minX = instances[0].x, minY = instances[0].y, maxX = minX, maxY = minY;
    for (const Instance &in : instances) {
        minX = std::min(minX, in.x);
        minY = std::min(minY, in.y);
        maxX = std::max(maxX, in.x);
        maxY = std::max(maxY, in.y);
        maxW = std::max(maxW, float(in.w));
        maxH = std::max(maxH, float(in.h));
    }
    originX = minX;
    originY = minY;

    // Grow the cell until the table is bounded (huge sparse worlds).
    cell = kMinCell;
    for (;;) {
        cols = int((maxX - minX) / cell) + 1;
        rows = int((maxY - minY) / cell) + 1;
        if (qint64(cols) * rows <= kMaxCells)
            break;
        cell *= 2;
    }

    const size_t cellCount = size_t(cols) * size_t(rows);
    start.assign(cellCount + 1, 0);
    auto cellOf = [this](const Instance &in) {
        const int cx = std::clamp(int((in.x - originX) / cell), 0, cols - 1);
        const int cy = std::clamp(int((in.y - originY) / cell), 0, rows - 1);
        return size_t(cy) * size_t(cols) + size_t(cx);
    };
    for (const Instance &in : instances)
        ++start[cellOf(in) + 1];
    for (size_t i = 0; i < cellCount; ++i)
        start[i + 1] += start[i];

    items.resize(instances.size());
    std::vector<uint32_t> fill(start.begin(), start.end() - 1);
    for (size_t i = 0; i < instances.size(); ++i)
        items[fill[cellOf(instances[i])]++] = uint32_t(i);   // ascending within a cell
}

void GridIndex::query(const QRectF &r, std::vector<uint32_t> &out) const
{
    if (items.empty())
        return;
    // An instance is stored under its top-left cell and reaches right / down
    // by at most maxW / maxH, so widen the query to the left / up.
    const double x0 = r.left() - maxW, y0 = r.top() - maxH;
    const double x1 = r.right(), y1 = r.bottom();
    if (x1 < originX || y1 < originY || x0 > originX + double(cols) * cell || y0 > originY + double(rows) * cell)
        return;
    const int cx0 = std::clamp(int(std::floor((x0 - originX) / cell)), 0, cols - 1);
    const int cy0 = std::clamp(int(std::floor((y0 - originY) / cell)), 0, rows - 1);
    const int cx1 = std::clamp(int(std::floor((x1 - originX) / cell)), 0, cols - 1);
    const int cy1 = std::clamp(int(std::floor((y1 - originY) / cell)), 0, rows - 1);
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

} // namespace TileMap
