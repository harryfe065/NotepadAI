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

#include "ScrollbackBuffer.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>

namespace {

constexpr uint32_t kInitialArenaCells = 4096;
constexpr uint32_t kNoIndex = std::numeric_limits<uint32_t>::max();

uint32_t packAttrs(const VTermScreenCellAttrs &a)
{
    return  static_cast<uint32_t>(a.bold)
         | (static_cast<uint32_t>(a.underline) << 1)
         | (static_cast<uint32_t>(a.italic)    << 3)
         | (static_cast<uint32_t>(a.blink)     << 4)
         | (static_cast<uint32_t>(a.reverse)   << 5)
         | (static_cast<uint32_t>(a.conceal)   << 6)
         | (static_cast<uint32_t>(a.strike)    << 7)
         | (static_cast<uint32_t>(a.font)      << 8)
         | (static_cast<uint32_t>(a.dwl)       << 12)
         | (static_cast<uint32_t>(a.dhl)       << 13)
         | (static_cast<uint32_t>(a.small)     << 15)
         | (static_cast<uint32_t>(a.baseline)  << 16)
         | (static_cast<uint32_t>(a.dim)       << 18);
}

void unpackAttrs(uint32_t v, VTermScreenCellAttrs &a)
{
    a.bold      = v & 1u;
    a.underline = (v >> 1) & 3u;
    a.italic    = (v >> 3) & 1u;
    a.blink     = (v >> 4) & 1u;
    a.reverse   = (v >> 5) & 1u;
    a.conceal   = (v >> 6) & 1u;
    a.strike    = (v >> 7) & 1u;
    a.font      = (v >> 8) & 15u;
    a.dwl       = (v >> 12) & 1u;
    a.dhl       = (v >> 13) & 3u;
    a.small     = (v >> 15) & 1u;
    a.baseline  = (v >> 16) & 3u;
    a.dim       = (v >> 18) & 1u;
}

// Only the bytes the colour type makes meaningful take part, so equal colours
// always intern to the same style.
uint32_t packColor(const VTermColor &c)
{
    if (VTERM_COLOR_IS_INDEXED(&c)) {
        return c.type | (static_cast<uint32_t>(c.indexed.idx) << 8);
    }
    return  c.type
         | (static_cast<uint32_t>(c.rgb.red)   << 8)
         | (static_cast<uint32_t>(c.rgb.green) << 16)
         | (static_cast<uint32_t>(c.rgb.blue)  << 24);
}

VTermColor unpackColor(uint32_t v)
{
    VTermColor c;
    std::memset(&c, 0, sizeof(c));
    c.type = static_cast<uint8_t>(v & 0xFFu);
    if (VTERM_COLOR_IS_INDEXED(&c)) {
        c.indexed.idx = static_cast<uint8_t>((v >> 8) & 0xFFu);
    } else {
        c.rgb.red   = static_cast<uint8_t>((v >> 8) & 0xFFu);
        c.rgb.green = static_cast<uint8_t>((v >> 16) & 0xFFu);
        c.rgb.blue  = static_cast<uint8_t>((v >> 24) & 0xFFu);
    }
    return c;
}

void blankCell(VTermScreenCell *cell)
{
    std::memset(cell, 0, sizeof(*cell));
    cell->width = 1;
    cell->fg.type = VTERM_COLOR_DEFAULT_FG;
    cell->bg.type = VTERM_COLOR_DEFAULT_BG;
}

// Underline, reverse and strike are visible even on a blank cell.
bool isTrimmableBlank(const VTermScreenCell &cell)
{
    return cell.chars[0] == 0
        && VTERM_COLOR_IS_DEFAULT_BG(&cell.bg)
        && cell.attrs.underline == 0 && !cell.attrs.reverse && !cell.attrs.strike;
}

// attrs, fg and bg sit back to back in VTermScreenCell, so a run of cells in
// one style is detected with a single 12-byte compare instead of re-packing.
constexpr std::size_t kStyleBytes = sizeof(VTermScreenCellAttrs) + 2 * sizeof(VTermColor);
constexpr bool kStyleBytesContiguous =
    offsetof(VTermScreenCell, fg) == offsetof(VTermScreenCell, attrs) + sizeof(VTermScreenCellAttrs)
    && offsetof(VTermScreenCell, bg) == offsetof(VTermScreenCell, fg) + sizeof(VTermColor);

bool sameStyleBytes(const VTermScreenCell &a, const VTermScreenCell &b)
{
    if constexpr (kStyleBytesContiguous && kStyleBytes == 12) {
        // Raw bytes (padding bits included): a mismatch only costs a re-pack.
        uint64_t a0 = 0;
        uint64_t b0 = 0;
        uint32_t a1 = 0;
        uint32_t b1 = 0;
        const auto *pa = reinterpret_cast<const unsigned char *>(&a) + offsetof(VTermScreenCell, attrs);
        const auto *pb = reinterpret_cast<const unsigned char *>(&b) + offsetof(VTermScreenCell, attrs);
        std::memcpy(&a0, pa, sizeof(a0));
        std::memcpy(&b0, pb, sizeof(b0));
        std::memcpy(&a1, pa + sizeof(a0), sizeof(a1));
        std::memcpy(&b1, pb + sizeof(b0), sizeof(b1));
        return a0 == b0 && a1 == b1;
    } else {
        return false;
    }
}

uint32_t hashStyle(uint32_t attrs, uint32_t fg, uint32_t bg)
{
    uint64_t h = (static_cast<uint64_t>(fg) << 32) | bg;
    h ^= static_cast<uint64_t>(attrs) * 0x9E3779B97F4A7C15ull;
    h ^= h >> 31;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 29;
    return static_cast<uint32_t>(h);
}

} // namespace

ScrollbackBuffer::ScrollbackBuffer(int maxLines)
    : m_maxLines(maxLines > 0 ? maxLines : 0)
{
}

int ScrollbackBuffer::setMaxLines(int maxLines)
{
    m_maxLines = maxLines > 0 ? maxLines : 0;
    int evicted = 0;
    while (m_count > m_maxLines) {
        evictOldest();
        ++evicted;
    }
    // A linear ring keeps the "push_back until full" invariant valid for the
    // new cap.
    relinearize(m_count);
    // A much smaller cap should give the memory back, not keep the old arena.
    if (m_cells.size() > std::max<std::size_t>(kInitialArenaCells, static_cast<std::size_t>(m_used) * 2)) {
        compactArena();
    }
    return evicted;
}

void ScrollbackBuffer::clear()
{
    std::vector<Line>().swap(m_lines);
    std::vector<Cell>().swap(m_cells);
    std::vector<Style>().swap(m_styles);
    std::vector<uint32_t>().swap(m_styleSlots);
    std::vector<Cluster>().swap(m_clusters);
    m_head = 0;
    m_count = 0;
    m_tail = 0;
    m_used = 0;
    m_styleCompactAt = 1u << 16;
    m_clusterCompactAt = 1u << 12;
    m_lastStyleIndex = kNoIndex;
}

const ScrollbackBuffer::Line &ScrollbackBuffer::lineRef(int index) const
{
    return m_lines[static_cast<std::size_t>((m_head + index) % static_cast<int>(m_lines.size()))];
}

void ScrollbackBuffer::evictOldest()
{
    if (m_count == 0) return;
    m_used -= lineRef(0).length;
    m_head = (m_head + 1) % static_cast<int>(m_lines.size());
    --m_count;
    if (m_count == 0) {
        m_head = 0;
        m_tail = 0;
        m_used = 0;
        m_lines.clear();
    }
}

void ScrollbackBuffer::relinearize(int newCapacity)
{
    std::vector<Line> lines;
    lines.reserve(static_cast<std::size_t>(std::max(newCapacity, m_count)));
    for (int i = 0; i < m_count; ++i) {
        lines.push_back(lineRef(i));
    }
    m_lines.swap(lines);
    m_head = 0;
}

void ScrollbackBuffer::compactArena()
{
    const auto target = std::max<uint64_t>(kInitialArenaCells, m_used + m_used / 2);
    std::vector<Cell> cells(static_cast<std::size_t>(target));
    uint32_t write = 0;
    for (int i = 0; i < m_count; ++i) {
        Line &line = m_lines[static_cast<std::size_t>((m_head + i) % static_cast<int>(m_lines.size()))];
        if (line.length > 0) {
            std::memcpy(&cells[write], &m_cells[line.offset], line.length * sizeof(Cell));
        }
        line.offset = write;
        write += line.length;
    }
    m_cells.swap(cells);
    m_tail = write;
}

void ScrollbackBuffer::growArena(uint32_t needed)
{
    // 1.5x growth keeps pushes amortised O(1) while bounding the arena to
    // about 1.5x the live cells (the free gap only has to fit one line).
    const uint64_t live = m_used + needed;
    const auto cap = static_cast<uint64_t>(m_cells.size());
    const uint64_t want = std::max<uint64_t>({
        kInitialArenaCells,
        cap + cap / 2,
        live + live / 2,
    });

    std::vector<Cell> cells(static_cast<std::size_t>(want));
    uint32_t write = 0;
    for (int i = 0; i < m_count; ++i) {
        Line &line = m_lines[static_cast<std::size_t>((m_head + i) % static_cast<int>(m_lines.size()))];
        if (line.length > 0) {
            std::memcpy(&cells[write], &m_cells[line.offset], line.length * sizeof(Cell));
        }
        line.offset = write;
        write += line.length;
    }
    m_cells.swap(cells);
    m_tail = write;
}

// Finds `length` contiguous free cells after the newest line, wrapping to the
// arena start when the end is too short. Offsets stay monotonic in ring order,
// so the oldest line's offset is always where live cells begin.
uint32_t ScrollbackBuffer::reserveCells(uint32_t length)
{
    if (length == 0) {
        return m_tail;
    }
    if (m_count == 0) {
        m_tail = 0;
    }

    const auto cap = static_cast<uint32_t>(m_cells.size());
    if (cap > 0) {
        const uint32_t head = m_count > 0 ? lineRef(0).offset : m_tail;
        // tail == head is either an empty arena or a full one; m_used decides.
        if (m_tail > head || (m_tail == head && m_used == 0)) {
            if (cap - m_tail >= length) {
                const uint32_t at = m_tail;
                m_tail += length;
                return at;
            }
            if (head >= length) {
                m_tail = length;
                return 0;
            }
        } else if (m_tail < head && head - m_tail >= length) {
            const uint32_t at = m_tail;
            m_tail += length;
            return at;
        }
    }

    growArena(length);
    const uint32_t at = m_tail;
    m_tail += length;
    return at;
}

void ScrollbackBuffer::rehashStyles(std::size_t buckets)
{
    m_styleSlots.assign(buckets, 0);
    const std::size_t mask = buckets - 1;
    for (std::size_t i = 0; i < m_styles.size(); ++i) {
        const Style &s = m_styles[i];
        std::size_t slot = hashStyle(s.attrs, s.fg, s.bg) & mask;
        while (m_styleSlots[slot] != 0) {
            slot = (slot + 1) & mask;
        }
        m_styleSlots[slot] = static_cast<uint32_t>(i + 1);
    }
}

uint32_t ScrollbackBuffer::internStyle(const Style &style)
{
    if (m_lastStyleIndex != kNoIndex && style == m_lastStyle) {
        return m_lastStyleIndex;
    }

    if (m_styleSlots.empty()) {
        rehashStyles(256);
    }
    std::size_t mask = m_styleSlots.size() - 1;
    std::size_t slot = hashStyle(style.attrs, style.fg, style.bg) & mask;
    while (const uint32_t entry = m_styleSlots[slot]) {
        if (m_styles[entry - 1] == style) {
            m_lastStyle = style;
            m_lastStyleIndex = entry - 1;
            return entry - 1;
        }
        slot = (slot + 1) & mask;
    }

    if (m_styles.size() > kStyleMask) {
        // Unreachable outside pathological input: compaction keeps the live
        // set far below 2^24. Fall back to the first style rather than corrupt.
        return 0;
    }

    const auto index = static_cast<uint32_t>(m_styles.size());
    m_styles.push_back(style);
    m_styleSlots[slot] = index + 1;
    if (m_styles.size() * 2 > m_styleSlots.size()) {
        rehashStyles(m_styleSlots.size() * 2);
    }
    m_lastStyle = style;
    m_lastStyleIndex = index;
    return index;
}

uint32_t ScrollbackBuffer::internCluster(const uint32_t *chars)
{
    Cluster cluster{};
    for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && chars[i]; ++i) {
        cluster[static_cast<std::size_t>(i)] = chars[i];
    }
    m_clusters.push_back(cluster);
    return static_cast<uint32_t>(m_clusters.size() - 1);
}

// Drops styles and clusters no live cell references and renumbers the rest.
// Runs only after the tables doubled past the live set, so it is amortised.
void ScrollbackBuffer::compactTables()
{
    std::vector<uint32_t> styleRemap(m_styles.size(), kNoIndex);
    std::vector<uint32_t> clusterRemap(m_clusters.size(), kNoIndex);
    std::vector<Style> styles;
    std::vector<Cluster> clusters;

    for (int i = 0; i < m_count; ++i) {
        const Line &line = lineRef(i);
        Cell *cell = m_cells.data() + line.offset;
        for (uint32_t c = 0; c < line.length; ++c, ++cell) {
            const uint32_t s = cell->meta & kStyleMask;
            if (styleRemap[s] == kNoIndex) {
                styleRemap[s] = static_cast<uint32_t>(styles.size());
                styles.push_back(m_styles[s]);
            }
            cell->meta = (cell->meta & ~kStyleMask) | styleRemap[s];
            if (cell->meta & kCluster) {
                const uint32_t k = cell->ch;
                if (clusterRemap[k] == kNoIndex) {
                    clusterRemap[k] = static_cast<uint32_t>(clusters.size());
                    clusters.push_back(m_clusters[k]);
                }
                cell->ch = clusterRemap[k];
            }
        }
    }

    m_styles.swap(styles);
    m_clusters.swap(clusters);
    std::size_t buckets = 256;
    while (buckets < m_styles.size() * 2) buckets *= 2;
    rehashStyles(buckets);
    m_lastStyleIndex = kNoIndex;
    m_styleCompactAt = std::max<std::size_t>(1u << 16, m_styles.size() * 2);
    m_clusterCompactAt = std::max<std::size_t>(1u << 12, m_clusters.size() * 2);
}

int ScrollbackBuffer::pushLine(int cols, const VTermScreenCell *cells, bool wrapsNext)
{
    if (m_maxLines <= 0 || cols <= 0 || cells == nullptr) {
        return 0;
    }

    int length = std::min(cols, static_cast<int>(std::numeric_limits<uint16_t>::max()));
    while (length > 0 && isTrimmableBlank(cells[length - 1])) {
        --length;
    }

    int evicted = 0;
    if (m_count >= m_maxLines) {
        evictOldest();
        evicted = 1;
    }

    // Compact before encoding so the indices written below stay valid.
    if (m_styles.size() >= m_styleCompactAt || m_clusters.size() >= m_clusterCompactAt) {
        compactTables();
    }

    const uint32_t offset = reserveCells(static_cast<uint32_t>(length));
    Cell *out = m_cells.data() + offset;
    uint32_t styleIndex = 0;
    for (int i = 0; i < length; ++i) {
        const VTermScreenCell &src = cells[i];
        if (i == 0 || !sameStyleBytes(src, cells[i - 1])) {
            styleIndex = internStyle(Style{packAttrs(src.attrs), packColor(src.fg), packColor(src.bg)});
        }
        uint32_t meta = styleIndex
                      | (static_cast<uint32_t>(src.width & 3) << kWidthShift);
        uint32_t ch = src.chars[0];
        if (ch != 0 && ch != kTerminalWideSpacer && src.chars[1] != 0) {
            ch = internCluster(src.chars);
            meta |= kCluster;
        }
        out[i] = Cell{ch, meta};
    }

    m_used += static_cast<uint64_t>(length);
    const Line line{offset, static_cast<uint16_t>(length),
                    static_cast<uint16_t>(wrapsNext ? kWrapsNext : 0)};
    if (m_count == static_cast<int>(m_lines.size())) {
        m_lines.push_back(line);
    } else {
        m_lines[static_cast<std::size_t>((m_head + m_count) % static_cast<int>(m_lines.size()))] = line;
    }
    ++m_count;
    return evicted;
}

void ScrollbackBuffer::decodeLine(const Line &line, int cols, VTermScreenCell *out) const
{
    const int copy = std::min(cols, static_cast<int>(line.length));
    const Cell *cell = m_cells.data() + line.offset;
    for (int i = 0; i < copy; ++i, ++cell) {
        VTermScreenCell &dst = out[i];
        std::memset(&dst, 0, sizeof(dst));
        if (cell->meta & kCluster) {
            const Cluster &cluster = m_clusters[cell->ch];
            std::copy(cluster.begin(), cluster.end(), dst.chars);
        } else {
            dst.chars[0] = cell->ch;
        }
        dst.width = static_cast<char>((cell->meta >> kWidthShift) & 3u);
        const Style &style = m_styles[cell->meta & kStyleMask];
        unpackAttrs(style.attrs, dst.attrs);
        dst.fg = unpackColor(style.fg);
        dst.bg = unpackColor(style.bg);
    }
    for (int i = copy; i < cols; ++i) {
        blankCell(&out[i]);
    }
}

bool ScrollbackBuffer::popLine(int cols, VTermScreenCell *out)
{
    if (m_count == 0 || cols <= 0 || out == nullptr) {
        return false;
    }
    const Line line = lineRef(m_count - 1);
    decodeLine(line, cols, out);
    --m_count;
    m_used -= line.length;
    // The newest line ends at m_tail, so its offset is the new tail.
    m_tail = line.offset;
    if (m_count == 0) {
        m_head = 0;
        m_tail = 0;
        m_used = 0;
        m_lines.clear();
    }
    return true;
}

bool ScrollbackBuffer::lineAt(int index, int cols, VTermScreenCell *out) const
{
    if (index < 0 || index >= m_count || cols <= 0 || out == nullptr) {
        return false;
    }
    decodeLine(lineRef(index), cols, out);
    return true;
}

bool ScrollbackBuffer::lineWraps(int index) const
{
    if (index < 0 || index >= m_count) return false;
    return (lineRef(index).flags & kWrapsNext) != 0;
}

std::size_t ScrollbackBuffer::memoryBytes() const
{
    return m_lines.capacity() * sizeof(Line)
         + m_cells.capacity() * sizeof(Cell)
         + m_styles.capacity() * sizeof(Style)
         + m_styleSlots.capacity() * sizeof(uint32_t)
         + m_clusters.capacity() * sizeof(Cluster);
}
