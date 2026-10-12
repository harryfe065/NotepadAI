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

#ifndef SCROLLBACKBUFFER_H
#define SCROLLBACKBUFFER_H

#include <vterm.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// Bounded history of terminal lines; sb_pushline writes here, sb_popline reads.
//
// Storage is compact and contiguous: every cell is 8 bytes (codepoint + interned
// style index + width), lines drop their trailing default blanks, and lines live
// in one ring-shaped cell arena indexed by a ring of line headers. Push, pop and
// eviction are O(1) amortised and never allocate per line. Styles (attrs + fg +
// bg) and multi-codepoint clusters are interned in side tables that are
// compacted when they outgrow the live history.
class ScrollbackBuffer
{
public:
    explicit ScrollbackBuffer(int maxLines = 10000);

    int  maxLines() const { return m_maxLines; }
    // Returns the number of lines evicted by the resize (0 if none).
    int  setMaxLines(int maxLines);

    int  size() const { return m_count; }
    bool isEmpty() const { return m_count == 0; }

    void clear();

    // Push a line onto the bottom (most recent) of the scrollback. `wrapsNext`
    // marks a soft-wrapped line that continues on the next one. Returns the
    // number of lines evicted by this call (0 in steady state, 1 at cap).
    int pushLine(int cols, const VTermScreenCell *cells, bool wrapsNext = false);

    // Pop the most recent line off the bottom. Writes up to `cols` cells into
    // `out`, padding with blank cells if the stored line was narrower.
    // Returns true if a line was popped, false if the buffer was empty.
    bool popLine(int cols, VTermScreenCell *out);

    // Fill `out` with the line at history index `index` (0 = oldest line still
    // in the buffer, size()-1 = most recent line). Padded/truncated to `cols`.
    // Returns true on success, false if index is out of range.
    bool lineAt(int index, int cols, VTermScreenCell *out) const;

    // True when the line at `index` soft-wraps into the next line.
    bool lineWraps(int index) const;

    // Bytes held by the arena, line ring and side tables (diagnostics/tests).
    std::size_t memoryBytes() const;

private:
    struct Cell {
        uint32_t ch;    // first codepoint, or cluster index when kCluster is set
        uint32_t meta;  // [0..23] style, [24..25] width, [26] cluster flag
    };
    static_assert(sizeof(Cell) == 8, "scrollback cells must stay 8 bytes");

    struct Line {
        uint32_t offset;   // first cell in m_cells
        uint16_t length;   // stored cells (trailing default blanks dropped)
        uint16_t flags;    // kWrapsNext
    };

    struct Style {
        uint32_t attrs;
        uint32_t fg;
        uint32_t bg;
        bool operator==(const Style &o) const { return attrs == o.attrs && fg == o.fg && bg == o.bg; }
    };

    using Cluster = std::array<uint32_t, VTERM_MAX_CHARS_PER_CELL>;

    static constexpr uint32_t kStyleMask = 0x00FFFFFFu;
    static constexpr uint32_t kWidthShift = 24;
    static constexpr uint32_t kCluster = 1u << 26;
    static constexpr uint16_t kWrapsNext = 1;

    const Line &lineRef(int index) const;
    void decodeLine(const Line &line, int cols, VTermScreenCell *out) const;
    void evictOldest();
    uint32_t reserveCells(uint32_t length);
    void growArena(uint32_t needed);
    uint32_t internStyle(const Style &style);
    uint32_t internCluster(const uint32_t *chars);
    void rehashStyles(std::size_t buckets);
    void compactTables();
    void relinearize(int newCapacity);

    int m_maxLines;

    // Line ring. Before the first eviction it simply grows (m_head stays 0).
    std::vector<Line> m_lines;
    int m_head = 0;
    int m_count = 0;

    // Cell arena ring. Live cells run from the oldest line's offset to m_tail,
    // possibly wrapping once to 0 (the unused tail end is skipped).
    std::vector<Cell> m_cells;
    uint32_t m_tail = 0;
    uint64_t m_used = 0;   // live cells across all lines

    // Interned styles: open-addressing table of indices into m_styles.
    std::vector<Style> m_styles;
    std::vector<uint32_t> m_styleSlots;
    std::size_t m_styleCompactAt = 1u << 16;
    Style m_lastStyle{};
    uint32_t m_lastStyleIndex = 0xFFFFFFFFu;

    std::vector<Cluster> m_clusters;
    std::size_t m_clusterCompactAt = 1u << 12;
};

#endif
