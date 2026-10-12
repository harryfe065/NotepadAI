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
#include <QPainter>
#include <QTextLayout>

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
    for (const QFont &f : m_fonts) {
        m_raw.push_back(QRawFont::fromFont(f));
    }
    for (auto &table : m_ascii) {
        for (Ref &ref : table) {
            ref = Ref{};
        }
    }
    m_map.clear();
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

void TerminalGlyphCache::flush(QPainter &p, uint16_t font, qreal originX, qreal baselineY)
{
    if (m_indexes.empty()) return;
    QGlyphRun run;
    run.setRawFont(m_raw[font]);
    // Zero-copy: the run borrows our scratch buffers for the draw call.
    run.setRawData(m_indexes.data(), m_positions.data(), static_cast<int>(m_indexes.size()));
    p.drawGlyphRun(QPointF(originX, baselineY), run);
    m_indexes.clear();
    m_positions.clear();
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
            flush(p, current, originX, baselineY);
        }
        current = ref.font;
        m_indexes.push_back(ref.glyph);
        m_positions.emplace_back(glyphs[i].col * cellW, 0.0);
    }
    flush(p, current, originX, baselineY);
}

void TerminalGlyphCache::drawCluster(QPainter &p, const TerminalRowLayout::Cluster &cluster,
                                     qreal originX, qreal baselineY, qreal cellW)
{
    int n = 0;
    while (n < VTERM_MAX_CHARS_PER_CELL && cluster.chars[n]) ++n;
    if (n == 0) return;
    p.setPen(QColor::fromRgb(cluster.fg));
    p.setFont(m_fonts[cluster.font & 3]);
    p.drawText(QPointF(originX + cluster.col * cellW, baselineY),
               QString::fromUcs4(cluster.chars, n));
}
