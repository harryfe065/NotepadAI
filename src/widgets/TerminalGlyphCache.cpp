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

#include "TerminalGlyphCache.h"

#include <QGlyphRun>
#include <QImage>
#include <QPainter>
#include <QTextLayout>

#include <algorithm>
#include <cstring>

namespace {

constexpr int kMaxCachedCodepoints = 1 << 16;

int toUtf16(char32_t cp, QChar *out)
{
    if (QChar::requiresSurrogates(cp)) {
        out[0] = QChar(QChar::highSurrogate(cp));
        out[1] = QChar(QChar::lowSurrogate(cp));
        return 2;
    }
    out[0] = QChar(static_cast<char16_t>(cp));
    return 1;
}

} // namespace

void TerminalGlyphCache::setFont(const QFont &font)
{
    for (int style = 0; style < 4; ++style) {
        QFont f = font;
        f.setBold((style & TerminalFontBold) != 0);
        f.setItalic((style & TerminalFontItalic) != 0);
        m_fonts[style] = f;
    }
    m_raw.clear();
    m_rawDevice.clear();
    for (const QFont &f : m_fonts) {
        m_raw.push_back(QRawFont::fromFont(f));
    }
    for (auto &table : m_ascii) {
        for (Ref &ref : table) {
            ref = Ref{};
        }
    }
    m_map.clear();
    m_clusters.clear();
    // Slot numbers in tile keys refer to the old fonts.
    releaseTiles();
}

uint16_t TerminalGlyphCache::slotFor(const QRawFont &raw)
{
    for (std::size_t i = 0; i < m_raw.size(); ++i) {
        if (m_raw[i] == raw) {
            return static_cast<uint16_t>(i);
        }
    }
    m_raw.push_back(raw);
    return static_cast<uint16_t>(m_raw.size() - 1);
}

TerminalGlyphCache::Ref TerminalGlyphCache::resolve(char32_t cp, uint8_t style)
{
    QChar utf16[2];
    const int len = toUtf16(cp, utf16);

    const QRawFont &primary = m_raw[style];
    quint32 indexes[2] = {0, 0};
    int count = 2;
    if (primary.isValid() && primary.glyphIndexesForChars(utf16, len, indexes, &count)
        && count >= 1 && indexes[0] != 0) {
        return Ref{indexes[0], style, true};
    }

    // Not in the primary font: let Qt pick the fallback, once per codepoint.
    QTextLayout layout(QString(utf16, len), m_fonts[style]);
    layout.setCacheEnabled(false);
    layout.beginLayout();
    layout.createLine();
    layout.endLayout();
    const QList<QGlyphRun> runs = layout.glyphRuns();
    for (const QGlyphRun &run : runs) {
        const QList<quint32> glyphs = run.glyphIndexes();
        if (!glyphs.isEmpty() && glyphs.first() != 0) {
            return Ref{glyphs.first(), slotFor(run.rawFont()), true};
        }
    }
    // Nothing has it: the primary's .notdef box keeps the cell visible.
    return Ref{indexes[0], style, true};
}

TerminalGlyphCache::Ref TerminalGlyphCache::lookup(char32_t cp, uint8_t style)
{
    style &= 3;
    if (cp < 128) {
        Ref &ref = m_ascii[style][cp];
        if (!ref.valid) {
            ref = resolve(cp, style);
        }
        return ref;
    }
    const quint64 key = (static_cast<quint64>(cp) << 2) | style;
    const auto it = m_map.constFind(key);
    if (it != m_map.constEnd()) {
        return it.value();
    }
    const Ref ref = resolve(cp, style);
    if (m_map.size() >= kMaxCachedCodepoints) {
        m_map.clear();
    }
    m_map.insert(key, ref);
    return ref;
}

void TerminalGlyphCache::flush(QPainter &p, const QRawFont &font, qreal originX, qreal baselineY)
{
    if (m_indexes.empty()) return;
    QGlyphRun run;
    run.setRawFont(font);
    // Zero-copy: the run borrows our scratch buffers for the draw call.
    run.setRawData(m_indexes.data(), m_positions.data(), static_cast<int>(m_indexes.size()));
    p.drawGlyphRun(QPointF(originX, baselineY), run);
    m_indexes.clear();
    m_positions.clear();
}

const QRawFont &TerminalGlyphCache::deviceFont(uint16_t slot)
{
    if (m_rawDevice.size() < m_raw.size()) {
        const std::size_t from = m_rawDevice.size();
        m_rawDevice.resize(m_raw.size());
        for (std::size_t i = from; i < m_raw.size(); ++i) {
            m_rawDevice[i] = m_raw[i];
            if (m_tileDpr != 1) {
                m_rawDevice[i].setPixelSize(m_raw[i].pixelSize() * m_tileDpr);
            }
        }
    }
    return m_rawDevice[slot];
}

void TerminalGlyphCache::drawRun(QPainter &p, const TerminalRowLayout::Glyph *glyphs, int count,
                                 uint8_t style, QRgb fg, qreal originX, qreal baselineY, qreal cellW)
{
    if (count <= 0 || m_raw.empty()) return;
    p.setPen(QColor::fromRgb(fg));
    m_indexes.clear();
    m_positions.clear();
    uint16_t current = 0;
    for (int i = 0; i < count; ++i) {
        const Ref ref = lookup(glyphs[i].cp, style);
        if (ref.font != current && !m_indexes.empty()) {
            flush(p, m_raw[current], originX, baselineY);
        }
        current = ref.font;
        m_indexes.push_back(ref.glyph);
        m_positions.emplace_back(glyphs[i].col * cellW, 0.0);
    }
    flush(p, m_raw[current], originX, baselineY);
}

void TerminalGlyphCache::drawCluster(QPainter &p, const TerminalRowLayout::Cluster &cluster,
                                     qreal originX, qreal baselineY, qreal cellW)
{
    int n = 0;
    while (n < VTERM_MAX_CHARS_PER_CELL && cluster.chars[n]) ++n;
    if (n == 0) return;
    const uint8_t style = cluster.font & 3;
    // Key: the font variant, then the cluster's codepoints.
    QString key = QString::fromUcs4(cluster.chars, n);
    key.prepend(QChar(u'0' + style));
    auto it = m_clusters.constFind(key);
    if (it == m_clusters.constEnd()) {
        QTextLayout layout(key.mid(1), m_fonts[style]);
        layout.setCacheEnabled(false);
        layout.beginLayout();
        QTextLine line = layout.createLine();
        layout.endLayout();
        ShapedCluster shaped;
        shaped.runs = layout.glyphRuns();
        shaped.ascent = line.isValid() ? line.ascent() : 0;
        if (m_clusters.size() >= kMaxCachedCodepoints) {
            m_clusters.clear();
        }
        it = m_clusters.insert(key, shaped);
    }
    p.setPen(QColor::fromRgb(cluster.fg));
    const QPointF origin(originX + cluster.col * cellW, baselineY - it->ascent);
    for (const QGlyphRun &run : it->runs) {
        p.drawGlyphRun(origin, run);
    }
}

void TerminalGlyphCache::setTileGeometry(int cellW, int cellH, qreal baseline, qreal dpr)
{
    if (cellW == m_tileCellW && cellH == m_tileCellH && baseline == m_tileBaseline && dpr == m_tileDpr) {
        return;
    }
    m_tileCellW = cellW;
    m_tileCellH = cellH;
    m_tileBaseline = baseline;
    m_tileBaselineDevice = static_cast<int>(std::lround(baseline * dpr));
    m_tileDpr = dpr;
    m_rawDevice.clear();
    releaseTiles();
}

void TerminalGlyphCache::releaseTiles()
{
    m_tiles.clear();
    m_tiles.squeeze();
    m_tilePixels.clear();
    m_tilePixels.shrink_to_fit();
    m_spill.clear();
}

bool TerminalGlyphCache::Touched::any(int from, int to) const
{
    for (int c = std::max(from, -1); c < std::min(to, cols + 1); ++c) {
        if (row[c + 1]) return true;
    }
    return false;
}

void TerminalGlyphCache::Touched::mark(int from, int to)
{
    for (int c = std::max(from, -1); c < std::min(to, cols + 1); ++c) {
        row[c + 1] = 1;
    }
}

void TerminalGlyphCache::Touched::markBelow(int from, int to)
{
    for (int c = std::max(from, -1); c < std::min(to, cols + 1); ++c) {
        below[c + 1] = 1;
    }
}

// Measured, not estimated: the glyph is rendered once, white on black, with
// two cells of margin around its own. A faint antialiasing fringe (one device
// pixel, under kFaintFringe on every channel) does not count: clipping it
// changes a few edge pixels by a barely visible amount, while treating it as
// spill would send most glyphs at fractional scales down the merge path.
TerminalGlyphCache::Spill TerminalGlyphCache::spillFor(quint32 glyph, uint16_t font, int width)
{
    const quint64 key = (static_cast<quint64>(glyph) << 24) | (static_cast<quint64>(font) << 8)
                        | static_cast<quint64>(width);
    const auto it = m_spill.constFind(key);
    if (it != m_spill.constEnd()) return it.value();

    const int w = m_tileCellW * width;
    const int h = m_tileCellH;
    const int m = 2 * std::max(m_tileCellW, m_tileCellH);
    QImage probe(w + 2 * m, h + 2 * m, QImage::Format_RGB32);
    probe.fill(Qt::black);
    {
        QPainter p(&probe);
        p.setPen(Qt::white);
        QGlyphRun run;
        run.setRawFont(deviceFont(font));
        const QPointF origin(0, 0);
        run.setRawData(&glyph, &origin, 1);
        p.drawGlyphRun(QPointF(m, m + m_tileBaselineDevice), run);
    }
    constexpr int kFaintFringe = 0x50;
    int minX = probe.width();
    int maxX = -1;
    int minY = probe.height();
    int maxY = -1;
    for (int y = 0; y < probe.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(probe.constScanLine(y));
        for (int x = 0; x < probe.width(); ++x) {
            const int peak = std::max({qRed(line[x]), qGreen(line[x]), qBlue(line[x])});
            // Within one pixel of the cell only a strong pixel counts.
            const bool nearCell = x >= m - 1 && x <= m + w && y >= m - 1 && y <= m + h;
            if (peak > (nearCell ? kFaintFringe : 0)) {
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
        }
    }
    Spill s;
    if (maxX >= 0) {
        const auto cells = [this](int pixels) {
            return static_cast<int8_t>(std::clamp((pixels + m_tileCellW - 1) / m_tileCellW, 0, 2));
        };
        s.left = cells(m - minX);
        s.right = cells(maxX + 1 - (m + w));
        s.up = minY < m;
        s.down = maxY >= m + h;
    }
    if (m_spill.size() >= kMaxCachedCodepoints) m_spill.clear();
    m_spill.insert(key, s);
    return s;
}

bool TerminalGlyphCache::makeTile(const TileKey &key, quint32 glyph, uint16_t font, int width, QRgb fg, QRgb bg,
                                  Tile *out)
{
    const Spill s = spillFor(glyph, font, width);
    Tile tile;
    tile.extended = s.any();
    tile.x = static_cast<int16_t>(-s.left * m_tileCellW);
    tile.y = static_cast<int16_t>(s.up ? -m_tileCellH : 0);
    tile.width = static_cast<int16_t>((s.left + width + s.right) * m_tileCellW);
    tile.height = static_cast<int16_t>(m_tileCellH * (1 + (s.up ? 1 : 0) + (s.down ? 1 : 0)));

    const std::size_t pixels = static_cast<std::size_t>(tile.width) * tile.height;
    if (m_tilePixels.size() + pixels > kMaxTilePixels) {
        m_tiles.clear();
        m_tilePixels.clear();
    }
    QImage image(tile.width, tile.height, QImage::Format_RGB32);
    image.fill(bg);
    {
        QPainter p(&image);
        p.setPen(QColor::fromRgb(fg));
        QGlyphRun run;
        run.setRawFont(deviceFont(font));
        const QPointF origin(0, 0);
        run.setRawData(&glyph, &origin, 1);
        // At the cell origin, exactly like a direct draw into the frame.
        p.drawGlyphRun(QPointF(-tile.x, -tile.y + m_tileBaselineDevice), run);
    }
    tile.offset = static_cast<uint32_t>(m_tilePixels.size());
    m_tilePixels.resize(m_tilePixels.size() + pixels);
    uint32_t *dst = m_tilePixels.data() + tile.offset;
    for (int row = 0; row < tile.height; ++row) {
        std::memcpy(dst + static_cast<qsizetype>(row) * tile.width, image.constScanLine(row),
                    static_cast<std::size_t>(tile.width) * 4);
    }
    m_tiles.insert(key, tile);
    *out = tile;
    return true;
}

void TerminalGlyphCache::copyTile(const Frame &frame, const uint32_t *src, const Tile &tile, int x, int y)
{
    const int h = std::min<int>(tile.height, frame.height - y);
    uint32_t *dst = frame.bits + static_cast<qsizetype>(y) * frame.stride + x;
    for (int row = 0; row < h; ++row) {
        // Fixed 16-byte copies inline to vector moves; a sized memcpy call
        // per scanline cost more than the pixels.
        int k = 0;
        for (; k + 4 <= tile.width; k += 4) {
            std::memcpy(dst + k, src + k, 16);
        }
        for (; k < tile.width; ++k) {
            dst[k] = src[k];
        }
        dst += frame.stride;
        src += tile.width;
    }
}

void TerminalGlyphCache::mergeTile(const Frame &frame, const uint32_t *src, const Tile &tile, int x, int y,
                                   QRgb fg, QRgb bg)
{
    const uint32_t bgPixel = 0xFF000000u | bg;
    const int fromX = std::max(0, -x);
    const int toX = std::min<int>(tile.width, frame.width - x);
    const int fromY = std::max(0, -y);
    const int toY = std::min<int>(tile.height, frame.height - y);
    // Coverage of the glyph per channel, read back from the tile: (tile - bg)
    // / (fg - bg). A channel where fg and bg nearly agree says little, so
    // the strongest channel stands in for it.
    const int fgc[3] = {qRed(fg), qGreen(fg), qBlue(fg)};
    const int bgc[3] = {qRed(bg), qGreen(bg), qBlue(bg)};
    for (int row = fromY; row < toY; ++row) {
        const uint32_t *s = src + static_cast<qsizetype>(row) * tile.width;
        uint32_t *d = frame.bits + static_cast<qsizetype>(y + row) * frame.stride + x;
        for (int k = fromX; k < toX; ++k) {
            const uint32_t e = s[k];
            if (e == bgPixel) continue;          // the glyph left it alone
            const uint32_t under = d[k];
            if (under == bgPixel) {              // nothing else there: exact
                d[k] = e;
                continue;
            }
            const int ec[3] = {qRed(e), qGreen(e), qBlue(e)};
            const int uc[3] = {qRed(under), qGreen(under), qBlue(under)};
            int cov[3];
            int strongest = 0;
            int best = -1;
            for (int c = 0; c < 3; ++c) {
                const int span = fgc[c] - bgc[c];
                cov[c] = std::abs(span) >= 24 ? std::clamp((ec[c] - bgc[c]) * 256 / span, 0, 256) : -1;
                if (std::abs(span) > best) {
                    best = std::abs(span);
                    strongest = c;
                }
            }
            int out[3];
            for (int c = 0; c < 3; ++c) {
                const int a = cov[c] >= 0 ? cov[c] : std::max(cov[strongest], 0);
                out[c] = uc[c] + (((fgc[c] - uc[c]) * a) >> 8);
            }
            d[k] = qRgb(out[0], out[1], out[2]);
        }
    }
}

void TerminalGlyphCache::blitRun(QPainter &devicePainter, const Frame &frame,
                                 const TerminalRowLayout::Glyph *glyphs, int count, uint8_t style, QRgb fg,
                                 const QRgb *cellBg, Touched &touched, int x0, int y)
{
    if (count <= 0 || m_raw.empty() || m_tileCellW <= 0) return;
    for (int i = 0; i < count; ++i) {
        const TerminalRowLayout::Glyph &g = glyphs[i];
        const Ref ref = lookup(g.cp, style);
        const int width = std::clamp(g.width, 1, 2);
        const int x = x0 + g.col * m_tileCellW;
        const QRgb bg = cellBg[g.col];
        const TileKey key{(static_cast<quint64>(ref.glyph) << 24) | (static_cast<quint64>(ref.font) << 8)
                              | static_cast<quint64>(width),
                          (static_cast<quint64>(fg) << 32) | bg};
        Tile tile;
        bool have = false;
        const auto it = m_tiles.constFind(key);
        if (it != m_tiles.constEnd()) {
            tile = it.value();
            have = true;
        } else if (m_tileBudget > 0) {
            --m_tileBudget;
            have = makeTile(key, ref.glyph, ref.font, width, fg, bg, &tile);
        }

        if (!have) {
            // Budget spent: draw it through Qt, in order, like the merge would.
            const Spill s = spillFor(ref.glyph, ref.font, width);
            touched.mark(g.col + width, g.col + width + s.right);
            if (s.down) touched.markBelow(g.col - s.left, g.col + width + s.right);
            devicePainter.setPen(QColor::fromRgb(fg));
            m_indexes.assign(1, ref.glyph);
            m_positions.assign(1, QPointF(0, 0));
            flush(devicePainter, deviceFont(ref.font), x, y + m_tileBaselineDevice);
            continue;
        }

        const uint32_t *src = m_tilePixels.data() + tile.offset;
        const int tx = x + tile.x;
        const int ty = y + tile.y;
        const bool inside = tx >= 0 && ty >= 0 && tx + tile.width <= frame.width;
        if (!tile.extended && inside && !touched.any(g.col, g.col + width)) {
            copyTile(frame, src, tile, tx, ty);
            continue;
        }
        mergeTile(frame, src, tile, tx, ty, fg, bg);
        if (tile.extended) {
            const int right = (tile.x + tile.width) / m_tileCellW - width;
            const int left = -tile.x / m_tileCellW;
            touched.mark(g.col + width, g.col + width + right);
            if (tile.y + tile.height > m_tileCellH) touched.markBelow(g.col - left, g.col + width + right);
        }
    }
}
