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

#include <cstdint>
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

    // A shaped multi-codepoint cluster at one cell.
    void drawCluster(QPainter &p, const TerminalRowLayout::Cluster &cluster,
                     qreal originX, qreal baselineY, qreal cellW);

private:
    struct Ref {
        uint32_t glyph = 0;
        uint16_t font = 0;
        bool valid = false;
    };

    Ref lookup(char32_t cp, uint8_t style);
    Ref resolve(char32_t cp, uint8_t style);
    uint16_t slotFor(const QRawFont &raw);
    void flush(QPainter &p, uint16_t font, qreal originX, qreal baselineY);

    QFont m_fonts[4];
    std::vector<QRawFont> m_raw;   // [0..3] primary variants, then fallbacks
    Ref m_ascii[4][128] = {};
    QHash<quint64, Ref> m_map;
    // Multi-codepoint clusters are shaped once (QTextLayout) and replayed.
    struct ShapedCluster {
        QList<QGlyphRun> runs;
        qreal ascent = 0;
    };
    QHash<QString, ShapedCluster> m_clusters;

    std::vector<quint32> m_indexes;
    std::vector<QPointF> m_positions;
};

#endif
