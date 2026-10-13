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

#include <QtTest>
#include <QVector>

#include "ScrollbackBuffer.h"

#include <vterm.h>

#include <cstring>


// Helper: build a line of cells whose first character codepoint is the row tag
// and whose column index is encoded in attrs.bold so we can detect padding.
static QVector<VTermScreenCell> makeLine(int cols, uint32_t tag)
{
    QVector<VTermScreenCell> row(cols);
    for (int i = 0; i < cols; ++i) {
        std::memset(&row[i], 0, sizeof(VTermScreenCell));
        row[i].chars[0] = tag + static_cast<uint32_t>(i);
        row[i].width = 1;
    }
    return row;
}


class TestScrollbackBuffer : public QObject
{
    Q_OBJECT

private slots:
    void empty_sizeAndIsEmpty();
    void pushLine_appendsToBottom();
    void pushLine_overCapacity_dropsOldest();
    void popLine_takesMostRecent();
    void popLine_emptyReturnsFalse();
    void popLine_intoNarrowerCols_truncates();
    void popLine_intoWiderCols_padsBlankCells();
    void lineAt_outOfRange_returnsFalse();
    void lineAt_returnsByHistoryIndex();
    void lineAt_intoWiderCols_padsBlankCells();
    void lineAt_intoNarrowerCols_truncates();
    void clear_emptiesBuffer();
    void setMaxLines_shrinks_dropsOldest();
    void pushLine_returnsEvictionCount();

    void wrapsFlag_roundTrips();
    void styles_roundTripExactly();
    void clusters_roundTrip();
    void trailingBlanks_trimmedButPaddedOnRead();
    void ringWraps_keepsContentInOrder();
    void popAfterWrap_restoresNewestFirst();
    void manyDistinctStyles_compactsAndStaysCorrect();
    void blankLineFlood_staysConstantMemory();
    void memory_isCompactForTypicalOutput();
    void clear_releasesMemory();

    void chunks_roundTripAcrossBoundaries();
    void chunks_evictionAcrossChunks();
    void snapshot_unaffectedByLaterChanges();
};


void TestScrollbackBuffer::empty_sizeAndIsEmpty()
{
    ScrollbackBuffer sb(100);
    QCOMPARE(sb.size(), 0);
    QVERIFY(sb.isEmpty());
    QCOMPARE(sb.maxLines(), 100);
}

void TestScrollbackBuffer::pushLine_appendsToBottom()
{
    ScrollbackBuffer sb(100);
    const auto line0 = makeLine(5, 100);
    const auto line1 = makeLine(5, 200);
    sb.pushLine(5, line0.constData());
    sb.pushLine(5, line1.constData());
    QCOMPARE(sb.size(), 2);
    QVERIFY(!sb.isEmpty());

    QVector<VTermScreenCell> out(5);
    QVERIFY(sb.lineAt(0, 5, out.data()));
    QCOMPARE(out[0].chars[0], 100u);
    QCOMPARE(out[2].chars[0], 102u);

    QVERIFY(sb.lineAt(1, 5, out.data()));
    QCOMPARE(out[0].chars[0], 200u);
    QCOMPARE(out[4].chars[0], 204u);
}

void TestScrollbackBuffer::pushLine_overCapacity_dropsOldest()
{
    ScrollbackBuffer sb(3);
    for (uint32_t i = 0; i < 5; ++i) {
        const auto row = makeLine(2, 1000u + i * 10);
        sb.pushLine(2, row.constData());
    }
    QCOMPARE(sb.size(), 3);

    // Oldest retained line should be the third push (tag 1020), then 1030, then 1040.
    QVector<VTermScreenCell> out(2);
    QVERIFY(sb.lineAt(0, 2, out.data()));
    QCOMPARE(out[0].chars[0], 1020u);
    QVERIFY(sb.lineAt(2, 2, out.data()));
    QCOMPARE(out[0].chars[0], 1040u);
}

void TestScrollbackBuffer::popLine_takesMostRecent()
{
    ScrollbackBuffer sb(10);
    sb.pushLine(3, makeLine(3, 10).constData());
    sb.pushLine(3, makeLine(3, 20).constData());

    QVector<VTermScreenCell> out(3);
    QVERIFY(sb.popLine(3, out.data()));
    QCOMPARE(out[0].chars[0], 20u);
    QCOMPARE(sb.size(), 1);

    QVERIFY(sb.popLine(3, out.data()));
    QCOMPARE(out[0].chars[0], 10u);
    QCOMPARE(sb.size(), 0);
}

void TestScrollbackBuffer::popLine_emptyReturnsFalse()
{
    ScrollbackBuffer sb(10);
    QVector<VTermScreenCell> out(3);
    QVERIFY(!sb.popLine(3, out.data()));
}

void TestScrollbackBuffer::popLine_intoNarrowerCols_truncates()
{
    ScrollbackBuffer sb(10);
    sb.pushLine(8, makeLine(8, 1).constData()); // chars 1..8 (tag 1)

    QVector<VTermScreenCell> out(3);
    QVERIFY(sb.popLine(3, out.data()));
    QCOMPARE(out[0].chars[0], 1u);
    QCOMPARE(out[1].chars[0], 2u);
    QCOMPARE(out[2].chars[0], 3u);
}

void TestScrollbackBuffer::popLine_intoWiderCols_padsBlankCells()
{
    ScrollbackBuffer sb(10);
    sb.pushLine(3, makeLine(3, 1).constData()); // chars 1..3

    QVector<VTermScreenCell> out(6);
    // pre-fill out with sentinel so we can detect that pad cells were actually written.
    for (int i = 0; i < 6; ++i) { out[i].chars[0] = 0xdeadbeef; }
    QVERIFY(sb.popLine(6, out.data()));
    QCOMPARE(out[0].chars[0], 1u);
    QCOMPARE(out[2].chars[0], 3u);
    QCOMPARE(out[3].chars[0], 0u); // blanked
    QCOMPARE(out[5].chars[0], 0u); // blanked
}

void TestScrollbackBuffer::lineAt_outOfRange_returnsFalse()
{
    ScrollbackBuffer sb(10);
    sb.pushLine(3, makeLine(3, 1).constData());
    QVector<VTermScreenCell> out(3);
    QVERIFY(!sb.lineAt(-1, 3, out.data()));
    QVERIFY(!sb.lineAt(1, 3, out.data())); // only index 0 exists
    QVERIFY(!sb.lineAt(100, 3, out.data()));
}

void TestScrollbackBuffer::lineAt_returnsByHistoryIndex()
{
    ScrollbackBuffer sb(10);
    for (uint32_t i = 0; i < 4; ++i) {
        sb.pushLine(2, makeLine(2, 100u + i * 10).constData());
    }
    QVector<VTermScreenCell> out(2);
    QVERIFY(sb.lineAt(0, 2, out.data()));
    QCOMPARE(out[0].chars[0], 100u);
    QVERIFY(sb.lineAt(3, 2, out.data()));
    QCOMPARE(out[0].chars[0], 130u);
}

void TestScrollbackBuffer::lineAt_intoWiderCols_padsBlankCells()
{
    ScrollbackBuffer sb(10);
    sb.pushLine(2, makeLine(2, 7).constData()); // chars 7..8

    QVector<VTermScreenCell> out(5);
    for (int i = 0; i < 5; ++i) { out[i].chars[0] = 0xcafef00d; }
    QVERIFY(sb.lineAt(0, 5, out.data()));
    QCOMPARE(out[0].chars[0], 7u);
    QCOMPARE(out[1].chars[0], 8u);
    QCOMPARE(out[2].chars[0], 0u);
    QCOMPARE(out[4].chars[0], 0u);
}

void TestScrollbackBuffer::lineAt_intoNarrowerCols_truncates()
{
    ScrollbackBuffer sb(10);
    sb.pushLine(6, makeLine(6, 1).constData()); // chars 1..6

    QVector<VTermScreenCell> out(2);
    QVERIFY(sb.lineAt(0, 2, out.data()));
    QCOMPARE(out[0].chars[0], 1u);
    QCOMPARE(out[1].chars[0], 2u);
}

void TestScrollbackBuffer::clear_emptiesBuffer()
{
    ScrollbackBuffer sb(10);
    sb.pushLine(2, makeLine(2, 1).constData());
    sb.pushLine(2, makeLine(2, 2).constData());
    QCOMPARE(sb.size(), 2);
    sb.clear();
    QCOMPARE(sb.size(), 0);
    QVERIFY(sb.isEmpty());
}

void TestScrollbackBuffer::setMaxLines_shrinks_dropsOldest()
{
    ScrollbackBuffer sb(10);
    for (uint32_t i = 0; i < 6; ++i) {
        sb.pushLine(2, makeLine(2, 100u + i * 10).constData());
    }
    QCOMPARE(sb.size(), 6);

    sb.setMaxLines(3);
    QCOMPARE(sb.size(), 3);
    QCOMPARE(sb.maxLines(), 3);

    // Oldest remaining should be the 4th push (tag 130), most recent the 6th (tag 150).
    QVector<VTermScreenCell> out(2);
    QVERIFY(sb.lineAt(0, 2, out.data()));
    QCOMPARE(out[0].chars[0], 130u);
    QVERIFY(sb.lineAt(2, 2, out.data()));
    QCOMPARE(out[0].chars[0], 150u);
}


void TestScrollbackBuffer::pushLine_returnsEvictionCount()
{
    ScrollbackBuffer sb(3);
    const auto row = makeLine(2, 1u);
    QCOMPARE(sb.pushLine(2, row.constData()), 0);
    QCOMPARE(sb.pushLine(2, row.constData()), 0);
    QCOMPARE(sb.pushLine(2, row.constData()), 0); // now at cap
    QCOMPARE(sb.pushLine(2, row.constData()), 1); // evicts oldest
    QCOMPARE(sb.size(), 3);
    QCOMPARE(sb.pushLine(2, row.constData()), 1);
    QCOMPARE(sb.size(), 3);

    QCOMPARE(sb.setMaxLines(1), 2);
    QCOMPARE(sb.size(), 1);
    QCOMPARE(sb.setMaxLines(10), 0);
}


static VTermScreenCell textCell(uint32_t ch)
{
    VTermScreenCell c;
    std::memset(&c, 0, sizeof(c));
    c.chars[0] = ch;
    c.width = 1;
    c.fg.type = VTERM_COLOR_DEFAULT_FG;
    c.bg.type = VTERM_COLOR_DEFAULT_BG;
    return c;
}

// A line of `len` visible cells tagged by `tag`, padded with default blanks.
static QVector<VTermScreenCell> textLine(int cols, int len, uint32_t tag)
{
    QVector<VTermScreenCell> row(cols);
    for (int i = 0; i < cols; ++i) {
        row[i] = textCell(i < len ? tag + static_cast<uint32_t>(i) : 0);
    }
    return row;
}

void TestScrollbackBuffer::wrapsFlag_roundTrips()
{
    ScrollbackBuffer sb(10);
    sb.pushLine(3, makeLine(3, 1).constData(), true);
    sb.pushLine(3, makeLine(3, 2).constData(), false);
    QVERIFY(sb.lineWraps(0));
    QVERIFY(!sb.lineWraps(1));
    QVERIFY(!sb.lineWraps(2));
}

void TestScrollbackBuffer::styles_roundTripExactly()
{
    QVector<VTermScreenCell> row(4);
    for (auto &c : row) c = textCell('x');
    row[0].attrs.bold = 1;
    row[0].attrs.underline = VTERM_UNDERLINE_CURLY;
    row[1].attrs.dim = 1;
    row[1].attrs.italic = 1;
    row[1].fg.type = VTERM_COLOR_INDEXED;
    row[1].fg.indexed.idx = 202;
    row[2].attrs.reverse = 1;
    row[2].attrs.strike = 1;
    row[2].bg.type = VTERM_COLOR_RGB;
    row[2].bg.rgb.red = 10;
    row[2].bg.rgb.green = 20;
    row[2].bg.rgb.blue = 30;
    row[3].width = 2;

    ScrollbackBuffer sb(10);
    sb.pushLine(4, row.constData());
    QVector<VTermScreenCell> out(4);
    QVERIFY(sb.lineAt(0, 4, out.data()));
    QCOMPARE(out[0].attrs.bold, 1u);
    QCOMPARE(out[0].attrs.underline, static_cast<unsigned>(VTERM_UNDERLINE_CURLY));
    QCOMPARE(out[1].attrs.dim, 1u);
    QCOMPARE(out[1].attrs.italic, 1u);
    QVERIFY(VTERM_COLOR_IS_INDEXED(&out[1].fg));
    QCOMPARE(static_cast<int>(out[1].fg.indexed.idx), 202);
    QCOMPARE(out[2].attrs.reverse, 1u);
    QCOMPARE(out[2].attrs.strike, 1u);
    QVERIFY(VTERM_COLOR_IS_RGB(&out[2].bg));
    QCOMPARE(static_cast<int>(out[2].bg.rgb.red), 10);
    QCOMPARE(static_cast<int>(out[2].bg.rgb.green), 20);
    QCOMPARE(static_cast<int>(out[2].bg.rgb.blue), 30);
    QVERIFY(VTERM_COLOR_IS_DEFAULT_FG(&out[3].fg));
    QCOMPARE(static_cast<int>(out[3].width), 2);
}

void TestScrollbackBuffer::clusters_roundTrip()
{
    QVector<VTermScreenCell> row(2);
    row[0] = textCell('e');
    row[0].chars[1] = 0x0301; // combining acute
    row[0].chars[2] = 0;
    row[1] = textCell('z');

    ScrollbackBuffer sb(10);
    sb.pushLine(2, row.constData());
    QVector<VTermScreenCell> out(2);
    QVERIFY(sb.lineAt(0, 2, out.data()));
    QCOMPARE(out[0].chars[0], static_cast<uint32_t>('e'));
    QCOMPARE(out[0].chars[1], 0x0301u);
    QCOMPARE(out[0].chars[2], 0u);
    QCOMPARE(out[1].chars[0], static_cast<uint32_t>('z'));
    QCOMPARE(out[1].chars[1], 0u);
}

void TestScrollbackBuffer::trailingBlanks_trimmedButPaddedOnRead()
{
    ScrollbackBuffer sb(10);
    sb.pushLine(80, textLine(80, 3, 'a').constData());
    QVector<VTermScreenCell> out(80);
    for (auto &c : out) c.chars[0] = 0xdeadbeef;
    QVERIFY(sb.lineAt(0, 80, out.data()));
    QCOMPARE(out[2].chars[0], static_cast<uint32_t>('c'));
    for (int i = 3; i < 80; ++i) {
        QCOMPARE(out[i].chars[0], 0u);
        QVERIFY(VTERM_COLOR_IS_DEFAULT_BG(&out[i].bg));
        QCOMPARE(static_cast<int>(out[i].width), 1);
    }

    // A coloured blank is content and must survive.
    auto coloured = textLine(5, 1, 'q');
    coloured[4].bg.type = VTERM_COLOR_INDEXED;
    coloured[4].bg.indexed.idx = 4;
    sb.pushLine(5, coloured.constData());
    QVector<VTermScreenCell> out2(5);
    QVERIFY(sb.lineAt(1, 5, out2.data()));
    QVERIFY(VTERM_COLOR_IS_INDEXED(&out2[4].bg));
    QCOMPARE(static_cast<int>(out2[4].bg.indexed.idx), 4);
}

void TestScrollbackBuffer::ringWraps_keepsContentInOrder()
{
    // Varying lengths force the cell arena to wrap and skip its tail many times.
    ScrollbackBuffer sb(50);
    const int cols = 120;
    for (uint32_t i = 0; i < 5000; ++i) {
        const int len = static_cast<int>((i * 37) % cols) + 1;
        sb.pushLine(cols, textLine(cols, len, 100000u * (i + 1)).constData());
    }
    QCOMPARE(sb.size(), 50);
    QVector<VTermScreenCell> out(cols);
    for (int k = 0; k < 50; ++k) {
        const uint32_t i = 5000 - 50 + static_cast<uint32_t>(k);
        const int len = static_cast<int>((i * 37) % cols) + 1;
        QVERIFY(sb.lineAt(k, cols, out.data()));
        QCOMPARE(out[0].chars[0], 100000u * (i + 1));
        QCOMPARE(out[len - 1].chars[0], 100000u * (i + 1) + static_cast<uint32_t>(len - 1));
        if (len < cols) {
            QCOMPARE(out[len].chars[0], 0u);
        }
    }
}

void TestScrollbackBuffer::popAfterWrap_restoresNewestFirst()
{
    ScrollbackBuffer sb(8);
    const int cols = 40;
    for (uint32_t i = 0; i < 100; ++i) {
        sb.pushLine(cols, textLine(cols, static_cast<int>(i % cols) + 1, 1000u * (i + 1)).constData());
    }
    QVector<VTermScreenCell> out(cols);
    for (uint32_t i = 99; i >= 92; --i) {
        QVERIFY(sb.popLine(cols, out.data()));
        QCOMPARE(out[0].chars[0], 1000u * (i + 1));
    }
    QVERIFY(sb.isEmpty());
    // The ring is reusable after draining.
    sb.pushLine(cols, textLine(cols, 4, 7).constData());
    QVERIFY(sb.lineAt(0, cols, out.data()));
    QCOMPARE(out[3].chars[0], 10u);
}

void TestScrollbackBuffer::manyDistinctStyles_compactsAndStaysCorrect()
{
    // A truecolour gradient interns a new style per cell; old ones must be
    // dropped as lines are evicted, and survivors must keep their colours.
    ScrollbackBuffer sb(100);
    const int cols = 200;
    QVector<VTermScreenCell> row(cols);
    uint32_t serial = 0;
    for (int line = 0; line < 2000; ++line) {
        for (int c = 0; c < cols; ++c, ++serial) {
            row[c] = textCell('#');
            row[c].fg.type = VTERM_COLOR_RGB;
            row[c].fg.rgb.red = static_cast<uint8_t>(serial & 0xFF);
            row[c].fg.rgb.green = static_cast<uint8_t>((serial >> 8) & 0xFF);
            row[c].fg.rgb.blue = static_cast<uint8_t>((serial >> 16) & 0xFF);
        }
        sb.pushLine(cols, row.constData());
    }
    QVector<VTermScreenCell> out(cols);
    QVERIFY(sb.lineAt(99, cols, out.data()));
    const uint32_t last = serial - 1;
    QCOMPARE(static_cast<uint32_t>(out[cols - 1].fg.rgb.red), last & 0xFF);
    QCOMPARE(static_cast<uint32_t>(out[cols - 1].fg.rgb.green), (last >> 8) & 0xFF);
    QCOMPARE(static_cast<uint32_t>(out[cols - 1].fg.rgb.blue), (last >> 16) & 0xFF);
    QVERIFY(sb.lineAt(0, cols, out.data()));
    const uint32_t first = (2000 - 100) * cols;
    QCOMPARE(static_cast<uint32_t>(out[0].fg.rgb.red), first & 0xFF);
    QCOMPARE(static_cast<uint32_t>(out[0].fg.rgb.green), (first >> 8) & 0xFF);
    // Live styles are 100 * 200; tables stay within a small multiple of that.
    QVERIFY2(sb.memoryBytes() < 8u * 1024 * 1024,
             qPrintable(QStringLiteral("memoryBytes=%1").arg(sb.memoryBytes())));
}

void TestScrollbackBuffer::blankLineFlood_staysConstantMemory()
{
    ScrollbackBuffer sb(1000);
    const auto blank = textLine(200, 0, 0);
    for (int i = 0; i < 100000; ++i) {
        sb.pushLine(200, blank.constData());
    }
    QCOMPARE(sb.size(), 1000);
    QVERIFY2(sb.memoryBytes() < 64u * 1024,
             qPrintable(QStringLiteral("memoryBytes=%1").arg(sb.memoryBytes())));
}

void TestScrollbackBuffer::memory_isCompactForTypicalOutput()
{
    // 10k lines of 60 visible chars in a 200-column terminal: 8 bytes per
    // stored cell plus a 8-byte header, versus ~40 bytes per padded cell before.
    ScrollbackBuffer sb(10000);
    const auto line = textLine(200, 60, 'a');
    for (int i = 0; i < 10000; ++i) {
        sb.pushLine(200, line.constData());
    }
    const std::size_t stored = 10000u * (60u * 8u + 8u);
    QVERIFY2(sb.memoryBytes() <= stored * 3,
             qPrintable(QStringLiteral("memoryBytes=%1").arg(sb.memoryBytes())));
}

void TestScrollbackBuffer::clear_releasesMemory()
{
    ScrollbackBuffer sb(100);
    for (int i = 0; i < 100; ++i) {
        sb.pushLine(80, textLine(80, 80, 'a').constData());
    }
    QVERIFY(sb.memoryBytes() > 0);
    sb.clear();
    QCOMPARE(sb.memoryBytes(), static_cast<std::size_t>(0));
}

// Lines are kept in chunks of kScrollbackChunkLines: indexing, wraps and pops
// must not notice the boundaries.
void TestScrollbackBuffer::chunks_roundTripAcrossBoundaries()
{
    ScrollbackBuffer sb(5000);
    const int cols = 4;
    const int n = kScrollbackChunkLines * 3 + 17;
    for (int i = 0; i < n; ++i) {
        sb.pushLine(cols, makeLine(cols, 10u * static_cast<uint32_t>(i)).constData(), i % 3 == 0);
    }
    QCOMPARE(sb.size(), n);
    QVector<VTermScreenCell> out(cols);
    for (int i = 0; i < n; ++i) {
        QVERIFY(sb.lineAt(i, cols, out.data()));
        QCOMPARE(out[0].chars[0], 10u * static_cast<uint32_t>(i));
        QCOMPARE(sb.lineWraps(i), i % 3 == 0);
    }
    // Pop back past two boundaries, newest first, then push again.
    const int popped = kScrollbackChunkLines * 2;
    for (int i = n - 1; i >= n - popped; --i) {
        QVERIFY(sb.popLine(cols, out.data()));
        QCOMPARE(out[0].chars[0], 10u * static_cast<uint32_t>(i));
    }
    QCOMPARE(sb.size(), n - popped);
    sb.pushLine(cols, makeLine(cols, 7).constData());
    QVERIFY(sb.lineAt(n - popped, cols, out.data()));
    QCOMPARE(out[0].chars[0], 7u);
    QVERIFY(sb.lineAt(n - popped - 1, cols, out.data()));
    QCOMPARE(out[0].chars[0], 10u * static_cast<uint32_t>(n - popped - 1));
}

void TestScrollbackBuffer::chunks_evictionAcrossChunks()
{
    ScrollbackBuffer sb(kScrollbackChunkLines + 100);
    const int cols = 4;
    int evicted = 0;
    for (int i = 0; i < 4000; ++i) {
        evicted += sb.pushLine(cols, makeLine(cols, 10u * static_cast<uint32_t>(i)).constData());
    }
    QCOMPARE(sb.size(), kScrollbackChunkLines + 100);
    QCOMPARE(evicted, 4000 - sb.size());
    QVector<VTermScreenCell> out(cols);
    QVERIFY(sb.lineAt(0, cols, out.data()));
    QCOMPARE(out[0].chars[0], 10u * static_cast<uint32_t>(4000 - sb.size()));
    QCOMPARE(sb.setMaxLines(10), kScrollbackChunkLines + 90);
    QVERIFY(sb.lineAt(0, cols, out.data()));
    QCOMPARE(out[0].chars[0], 10u * 3990u);
    // Draining past the evicted lines the oldest chunk still holds.
    for (int i = 3999; i >= 3990; --i) {
        QVERIFY(sb.popLine(cols, out.data()));
        QCOMPARE(out[0].chars[0], 10u * static_cast<uint32_t>(i));
    }
    QVERIFY(sb.isEmpty());
    QVERIFY(!sb.popLine(cols, out.data()));
    QCOMPARE(sb.memoryBytes(), static_cast<std::size_t>(0));
}

// A snapshot (what find-in-terminal's worker reads) keeps its lines whatever
// the terminal does afterwards.
void TestScrollbackBuffer::snapshot_unaffectedByLaterChanges()
{
    ScrollbackBuffer sb(1000);
    const int cols = 4;
    for (int i = 0; i < 1500; ++i) {
        sb.pushLine(cols, makeLine(cols, 10u * static_cast<uint32_t>(i)).constData());
    }
    const ScrollbackSnapshot snap = sb.snapshot();
    for (int i = 0; i < 300; ++i) {
        sb.pushLine(cols, makeLine(cols, 3u).constData());
    }
    QVector<VTermScreenCell> out(cols);
    for (int i = 0; i < 50; ++i) {
        sb.popLine(cols, out.data());
    }
    sb.setMaxLines(200);
    QVERIFY(sb.lineAt(199, cols, out.data()));
    QCOMPARE(out[0].chars[0], 3u);
    sb.clear();
    QCOMPARE(snap.size(), 1000);
    for (int i = 0; i < 1000; ++i) {
        QVERIFY(snap.lineAt(i, cols, out.data()));
        QCOMPARE(out[0].chars[0], 10u * static_cast<uint32_t>(500 + i));
    }
    QVERIFY(!snap.lineAt(1000, cols, out.data()));
}

QTEST_APPLESS_MAIN(TestScrollbackBuffer)

#include "test_scrollback_buffer.moc"
