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

#include "TerminalGlyphCache.h"

#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QImage>
#include <QPainter>
#include <QRawFont>

#include <cmath>
#include <vector>

namespace {

struct Result {
    int differing = 0;
    int maxDiff = 0;
};

// Draws `text` once through the tile path and once glyph by glyph through Qt
// (the same device-sized raw font, unscaled painter), on the same
// background, and compares the pixels.
Result compareRow(const QFont &font, qreal dpr, const QString &text, uint8_t style, QRgb fg, QRgb bg)
{
    TerminalGlyphCache cache;
    cache.setFont(font);
    const QFontMetricsF fm(cache.font(style));
    const int cellW = static_cast<int>(std::ceil(fm.horizontalAdvance(QLatin1Char('M')) * dpr - 1e-3));
    const qreal fontHeight = fm.ascent() + fm.descent();
    const int cellH = static_cast<int>(std::lround(fontHeight * 1.2 * dpr));
    const qreal baseline = std::round((fm.ascent() + (cellH / dpr - fontHeight) / 2) * dpr) / dpr;
    cache.setTileGeometry(cellW, cellH, baseline, dpr);

    const int cols = static_cast<int>(text.size());
    const int x0 = cellW;
    const int y0 = cellH;
    QImage got((cols + 2) * cellW, 3 * cellH, QImage::Format_RGB32);
    QImage ref(got.size(), QImage::Format_RGB32);
    got.fill(bg);
    ref.fill(bg);

    std::vector<TerminalRowLayout::Glyph> glyphs;
    for (int i = 0; i < cols; ++i) {
        if (text.at(i) != QLatin1Char(' ')) glyphs.push_back({i, text.at(i).unicode(), 1});
    }
    std::vector<QRgb> cellBg(static_cast<std::size_t>(cols) + 1, bg);
    std::vector<uint8_t> row(static_cast<std::size_t>(cols) + 2), below(row.size());
    TerminalGlyphCache::Touched touched{row.data(), below.data(), cols};
    TerminalGlyphCache::Frame frame{reinterpret_cast<uint32_t *>(got.bits()), got.bytesPerLine() / 4, got.width(),
                                    got.height()};
    {
        QPainter p(&got);
        cache.beginFrame();
        cache.blitRun(p, frame, glyphs.data(), static_cast<int>(glyphs.size()), style, fg, cellBg.data(), touched,
                      x0, y0);
    }

    QRawFont raw = QRawFont::fromFont(cache.font(style));
    raw.setPixelSize(raw.pixelSize() * dpr);
    const int baselineDevice = static_cast<int>(std::lround(baseline * dpr));
    {
        QPainter p(&ref);
        p.setPen(QColor::fromRgb(fg));
        for (const auto &g : glyphs) {
            const QList<quint32> index = raw.glyphIndexesForString(QString(QChar(static_cast<char16_t>(g.cp))));
            QGlyphRun run;
            run.setRawFont(raw);
            run.setGlyphIndexes(index);
            run.setPositions({QPointF(0, 0)});
            p.drawGlyphRun(QPointF(x0 + g.col * cellW, y0 + baselineDevice), run);
        }
    }

    Result r;
    for (int y = 0; y < got.height(); ++y) {
        const auto *a = reinterpret_cast<const QRgb *>(ref.constScanLine(y));
        const auto *b = reinterpret_cast<const QRgb *>(got.constScanLine(y));
        for (int x = 0; x < got.width(); ++x) {
            if (a[x] == b[x]) continue;
            ++r.differing;
            r.maxDiff = std::max(r.maxDiff, std::abs(qRed(a[x]) - qRed(b[x])) + std::abs(qGreen(a[x]) - qGreen(b[x]))
                                                + std::abs(qBlue(a[x]) - qBlue(b[x])));
        }
    }
    return r;
}

QFont monoFont()
{
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    f.setPointSize(10);
    return f;
}

} // namespace

class TestTerminalGlyphTiles : public QObject
{
    Q_OBJECT

private slots:
    void tilesMatchQt_data();
    void tilesMatchQt();
    void budgetSpentStillDrawsEverything();
};

void TestTerminalGlyphTiles::tilesMatchQt_data()
{
    QTest::addColumn<qreal>("dpr");
    QTest::addColumn<int>("style");
    QTest::newRow("100%") << 1.0 << int(TerminalFontRegular);
    QTest::newRow("150%") << 1.5 << int(TerminalFontRegular);
    QTest::newRow("200%") << 2.0 << int(TerminalFontRegular);
    QTest::newRow("italic 150%") << 1.5 << int(TerminalFontItalic);
    QTest::newRow("bold italic 100%") << 1.0 << int(TerminalFontBold | TerminalFontItalic);
}

// Tiles are Qt's own pixels: glyphs inside their cells are copied verbatim,
// glyphs that spill are merged (exact over plain background). Only pixels
// where two glyphs overlap may differ, and then only slightly.
void TestTerminalGlyphTiles::tilesMatchQt()
{
    QFETCH(qreal, dpr);
    QFETCH(int, style);
    const QFont font = monoFont();
    if (!QRawFont::fromFont(font).isValid()) QSKIP("no usable fixed-pitch font on this platform");

    const QString text = QStringLiteral("fn main() { let x = a_b[0] + y; } // WMgjpqy@#%");
    const Result dark = compareRow(font, dpr, text, uint8_t(style), qRgb(0xD4, 0xD4, 0xD4), qRgb(0x1E, 0x1E, 0x1E));
    const Result light = compareRow(font, dpr, text, uint8_t(style), qRgb(0, 0, 0), qRgb(0xFF, 0xFF, 0xFF));
    for (const Result &r : {dark, light}) {
        QVERIFY2(r.differing <= text.size() * 4,
                 qPrintable(QStringLiteral("%1 pixels differ").arg(r.differing)));
        QVERIFY2(r.maxDiff <= 96, qPrintable(QStringLiteral("max channel-sum diff %1").arg(r.maxDiff)));
    }
    if (style == TerminalFontRegular) {
        // Upright text never overlaps itself; at most a faint antialiasing
        // fringe outside a cell is clipped (TerminalGlyphCache::spillFor).
        QVERIFY2(dark.differing <= 4, qPrintable(QStringLiteral("%1 pixels differ").arg(dark.differing)));
    }
}

// A frame whose tile budget is spent draws the rest through Qt: every glyph
// still reaches the frame.
void TestTerminalGlyphTiles::budgetSpentStillDrawsEverything()
{
    const QFont font = monoFont();
    if (!QRawFont::fromFont(font).isValid()) QSKIP("no usable fixed-pitch font on this platform");
    TerminalGlyphCache cache;
    cache.setFont(font);
    const int cellW = 8;
    const int cellH = 18;
    cache.setTileGeometry(cellW, cellH, 13, 1.0);
    const int cols = 600;
    QImage img(cols * cellW, cellH, QImage::Format_RGB32);
    img.fill(Qt::black);
    TerminalGlyphCache::Frame frame{reinterpret_cast<uint32_t *>(img.bits()), img.bytesPerLine() / 4, img.width(),
                                    img.height()};
    std::vector<QRgb> cellBg(cols + 1, qRgb(0, 0, 0));
    std::vector<uint8_t> row(cols + 2), below(cols + 2);
    TerminalGlyphCache::Touched touched{row.data(), below.data(), cols};
    QPainter p(&img);
    cache.beginFrame();
    // 600 distinct colours: far more tiles than one frame may create.
    for (int i = 0; i < cols; ++i) {
        const TerminalRowLayout::Glyph g{i, U'#', 1};
        cache.blitRun(p, frame, &g, 1, TerminalFontRegular, qRgb(255, i % 256, (i * 7) % 256), cellBg.data(), touched,
                      0, 0);
    }
    p.end();
    for (int i = 0; i < cols; ++i) {
        bool lit = false;
        for (int y = 0; y < cellH && !lit; ++y) {
            const auto *line = reinterpret_cast<const QRgb *>(img.constScanLine(y)) + i * cellW;
            for (int x = 0; x < cellW && !lit; ++x) lit = qRed(line[x]) > 64;
        }
        QVERIFY2(lit, qPrintable(QStringLiteral("cell %1 is empty").arg(i)));
    }
}

QTEST_MAIN(TestTerminalGlyphTiles)

#include "test_terminal_glyph_tiles.moc"
