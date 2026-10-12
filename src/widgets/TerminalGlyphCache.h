/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Notepad Next is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Notepad Next.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef TERMINALGLYPHCACHE_H
#define TERMINALGLYPHCACHE_H

#include "TerminalRowLayout.h"

#include <QFont>
#include <QGlyphRun>
#include <QList>
#include <QHash>
#include <QPointF>
#include <QRawFont>
#include <QRgb>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

class QPainter;

// Maps (codepoint, font variant) to a glyph in a QRawFont and draws runs of
// them pinned to the cell grid. The primary font's cmap answers almost every
// lookup (ASCII hits a flat table); a codepoint the primary font lacks is
// resolved once through Qt's font fallback and cached. A run is one
// QGlyphRun per font, so text never re-shapes and fallback glyphs cannot
// shift the columns after them.
class TerminalGlyphCache
{
public:
    void setFont(const QFont &font);
    const QFont &font(uint8_t style) const { return m_fonts[style & 3]; }

    // Draws `count` glyphs; glyph i sits at originX + glyphs[i].col * cellW.
    void drawRun(QPainter &p, const TerminalRowLayout::Glyph *glyphs, int count,
                 uint8_t style, QRgb fg, qreal originX, qreal baselineY, qreal cellW);

    // A shaped multi-codepoint cluster at one cell (logical coordinates).
    void drawCluster(QPainter &p, const TerminalRowLayout::Cluster &cluster,
                     qreal originX, qreal baselineY, qreal cellW);
    // The same in device pixels through an unscaled painter (the frame path):
    // the cell sits at x0 + col * cellW with its baseline at `baselineY`.
    void drawClusterDevice(QPainter &devicePainter, const TerminalRowLayout::Cluster &cluster, int x0,
                           int baselineY);

    // --- Tiles -------------------------------------------------------------
    // Qt blends every glyph through its raster engine (~1 us each at 100%,
    // 6-10 us through a scaled painter), so a full 200x60 frame cost 40-90 ms.
    // Instead each (glyph, font, colour, background) is drawn by Qt once onto
    // a tile and from then on copied into the frame (~50 ns per glyph).
    // Tiles are rendered in device pixels with raw fonts sized for the
    // device, through unscaled painters: Qt's cached glyph path at any scale.
    //
    // A glyph that stays inside its cell gets an opaque cell-sized tile and
    // is copied as is: pixel-identical to Qt. One that reaches outside (italic
    // overhang, wide fallback glyphs) gets an extended tile covering the cells
    // it reaches, merged in: pixels it leaves at the background are skipped,
    // pixels drawn onto untouched background are copied (identical to Qt);
    // only where two glyphs overlap is the colour blended by estimate.

    // Opaque 32-bit pixels in device pixels (the widget's own frame, or its
    // part of the window's backing store). Rows outside [clipTop, clipBottom)
    // are never written.
    struct Frame {
        uint32_t *bits = nullptr;
        qsizetype stride = 0;   // pixels per scanline
        int width = 0;
        int height = 0;
        int clipTop = 0;
        int clipBottom = std::numeric_limits<int>::max();
        int top() const { return std::max(0, clipTop); }
        int bottom() const { return std::min(height, clipBottom); }
    };
    // Cell size and baseline in device pixels; tiles are dropped when any of
    // them changes.
    void setTileGeometry(int cellW, int cellH, qreal baseline, qreal dpr);
    // Cells of the row being painted, and of the row below, that a glyph
    // already reached from outside: an opaque tile copied there would erase
    // those pixels, so glyphs in them are merged instead.
    struct Touched {
        uint8_t *row = nullptr;    // cols + 2 entries, index = col + 1
        uint8_t *below = nullptr;
        int cols = 0;
        bool any(int from, int to) const;
        void mark(int from, int to);
        void markBelow(int from, int to);
    };
    // Call once per paint: bounds the tiles created in one frame, so output
    // with ever-changing colours (rainbow text) degrades to drawing through Qt
    // instead of rasterising thousands of tiles.
    void beginFrame() { m_tileBudget = kTileBudgetPerFrame; }
    // A row is drawn in two passes, so the caller only fills the cells no
    // opaque tile will cover (writing those pixels twice was a third of a
    // frame). planRun resolves each glyph of a run, in column order, to its
    // tile and decides copy (opaque) or merge, marking the cells a merged
    // glyph reaches; drawPlan then draws the row's plan in the same order.
    // Cells sit at device position (x0 + col * cellW, y); `cellBg[col]` is
    // each cell's background.
    // A tile's pixels in m_tilePixels; `x`/`y` place its top-left corner
    // relative to the cell (negative when it reaches left of or above it).
    struct Tile {
        uint32_t offset = 0;
        int16_t x = 0;
        int16_t y = 0;
        int16_t width = 0;
        int16_t height = 0;
        // The pixels the glyph changed, [inkLeft, inkRight) x [inkTop,
        // inkBottom) within the tile: a merge never visits the rest (most of
        // an extended tile is background).
        int16_t inkLeft = 0;
        int16_t inkTop = 0;
        int16_t inkRight = 0;
        int16_t inkBottom = 0;
        bool extended = false;
    };
    // One glyph of a planned row. Without a tile (budget spent) it is drawn
    // through Qt.
    struct Placement {
        Tile tile;
        int col = 0;
        int width = 1;
        quint32 glyph = 0;
        uint16_t font = 0;
        QRgb fg = 0;
        QRgb bg = 0;
        bool hasTile = false;
        bool copy = false;   // opaque copy; else merged (or drawn by Qt)
    };
    void planRun(const Frame &frame, const TerminalRowLayout::Glyph *glyphs, int count, uint8_t style, QRgb fg,
                 const QRgb *cellBg, Touched &touched, int x0, int y, std::vector<Placement> &plan);
    // `devicePainter` paints the same frame unscaled; it draws the glyphs
    // left over once the frame's tile budget is spent.
    void drawPlan(QPainter &devicePainter, const Frame &frame, const std::vector<Placement> &plan, int x0, int y);
    // Both passes for one run, onto an already filled background.
    void blitRun(QPainter &devicePainter, const Frame &frame, const TerminalRowLayout::Glyph *glyphs,
                 int count, uint8_t style, QRgb fg, const QRgb *cellBg, Touched &touched, int x0, int y);
    // Frees every tile (hidden terminals keep no pixels).
    void releaseTiles();

private:
    static constexpr int kTileBudgetPerFrame = 384;
    static constexpr std::size_t kMaxTilePixels = (4u << 20) / sizeof(uint32_t);   // 4 MiB

    struct TileKey {
        quint64 glyph;   // glyph << 24 | font slot << 8 | cell width
        quint64 colors;  // fg << 32 | bg
        bool operator==(const TileKey &o) const { return glyph == o.glyph && colors == o.colors; }
    };
    friend size_t qHash(const TileKey &k, size_t seed) { return qHashMulti(seed, k.glyph, k.colors); }
    // Whole cells a glyph's pixels (antialiasing included) reach beyond its
    // own.
    struct Spill {
        int8_t left = 0;
        int8_t right = 0;
        bool up = false;
        bool down = false;
        bool any() const { return left || right || up || down; }
    };

    Spill spillFor(quint32 glyph, uint16_t font, int width);
    static quint64 glyphKey(quint32 glyph, uint16_t font, int width)
    {
        return (static_cast<quint64>(glyph) << 24) | (static_cast<quint64>(font) << 8) | static_cast<quint64>(width);
    }
    Tile makeTile(const TileKey &key, quint32 glyph, uint16_t font, int width, QRgb fg, QRgb bg);
    static void copyTile(const Frame &frame, const uint32_t *src, const Tile &tile, int x, int y);
    static void mergeTile(const Frame &frame, const uint32_t *src, const Tile &tile, int x, int y,
                          QRgb fg, QRgb bg);

    struct Ref {
        uint32_t glyph = 0;
        uint16_t font = 0;
        bool valid = false;
    };

    Ref lookup(char32_t cp, uint8_t style);
    Ref resolve(char32_t cp, uint8_t style);
    uint16_t slotFor(const QRawFont &raw);
    void flush(QPainter &p, const QRawFont &font, qreal originX, qreal baselineY);
    const QRawFont &deviceFont(uint16_t slot);

    QFont m_fonts[4];
    std::vector<QRawFont> m_raw;   // [0..3] primary variants, then fallbacks
    std::vector<QRawFont> m_rawDevice;   // m_raw at the tile scale, built on demand
    Ref m_ascii[4][128] = {};
    QHash<quint64, Ref> m_map;
    // Multi-codepoint clusters are shaped once (QTextLayout) and replayed.
    struct ShapedCluster {
        QList<QGlyphRun> runs;
        qreal ascent = 0;
        QList<QGlyphRun> deviceRuns;   // runs at m_tileDpr, built on first use
        qreal deviceScale = 0;
    };
    ShapedCluster &shapedCluster(const TerminalRowLayout::Cluster &cluster);
    QHash<QString, ShapedCluster> m_clusters;

    std::vector<quint32> m_indexes;
    std::vector<QPointF> m_positions;

    QHash<TileKey, Tile> m_tiles;
    std::vector<uint32_t> m_tilePixels;
    QHash<quint64, Spill> m_spill;
    std::vector<Placement> m_runPlan;   // blitRun scratch
    int m_tileCellW = 0;
    int m_tileCellH = 0;
    qreal m_tileBaseline = 0;
    int m_tileBaselineDevice = 0;
    qreal m_tileDpr = 1;
    int m_tileBudget = kTileBudgetPerFrame;
};

#endif
