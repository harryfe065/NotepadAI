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

#include <QImage>
#include <QPainter>

#include "TerminalBoxDrawing.h"

namespace {

constexpr int kW = 10;
constexpr int kH = 24; // a 1.2 line-height row: font glyphs would leave gaps

QImage render(char32_t cp, qreal dpr = 1.0)
{
    QImage img(static_cast<int>(kW * dpr), static_cast<int>(kH * dpr), QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::black);
    QPainter p(&img);
    TerminalBoxDrawing::paint(p, QRectF(0, 0, kW, kH), cp, Qt::white, 1.0, dpr);
    p.end();
    return img;
}

bool lit(const QImage &img, int x, int y)
{
    return qRed(img.pixel(x, y)) > 128;
}

int litCount(const QImage &img)
{
    int n = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            n += lit(img, x, y) ? 1 : 0;
    return n;
}

} // namespace

class TestTerminalBoxDrawing : public QObject
{
    Q_OBJECT

private slots:
    void ranges_areProcedural();
    void arms_tableMatchesNames();
    void horizontal_touchesBothEdges();
    void vertical_touchesTopAndBottom();
    void cross_isConnectedAtCentre();
    void doubleCorner_hasTwoStrokesPerArm();
    void fullBlock_fillsCell();
    void halfBlocks_splitExactly();
    void powerlineTriangle_fillsLeftEdge();
    void fractionalScale_stillTiles();
};

void TestTerminalBoxDrawing::ranges_areProcedural()
{
    QVERIFY(TerminalBoxDrawing::isProcedural(0x2500));
    QVERIFY(TerminalBoxDrawing::isProcedural(0x257F));
    QVERIFY(TerminalBoxDrawing::isProcedural(0x2588));
    QVERIFY(TerminalBoxDrawing::isProcedural(0x259F));
    QVERIFY(TerminalBoxDrawing::isProcedural(0xE0B0));
    QVERIFY(TerminalBoxDrawing::isProcedural(0xE0B7));
    QVERIFY(!TerminalBoxDrawing::isProcedural(0x24FF));
    QVERIFY(!TerminalBoxDrawing::isProcedural(0x25A0));
    QVERIFY(!TerminalBoxDrawing::isProcedural(U'a'));
}

void TestTerminalBoxDrawing::arms_tableMatchesNames()
{
    TerminalBoxDrawing::Arms a;
    QVERIFY(TerminalBoxDrawing::armsFor(0x253C, &a)); // ┼
    QCOMPARE(a.left + a.right + a.up + a.down, 4);
    QVERIFY(TerminalBoxDrawing::armsFor(0x256C, &a)); // ╬
    QCOMPARE(a.left, 3);
    QCOMPARE(a.down, 3);
    QVERIFY(TerminalBoxDrawing::armsFor(0x2517, &a)); // ┗ heavy up and right
    QCOMPARE(a.right, 2);
    QCOMPARE(a.up, 2);
    QCOMPARE(a.left, 0);
    QCOMPARE(a.down, 0);
    QVERIFY(TerminalBoxDrawing::armsFor(0x2555, &a)); // ╕ double left, single down
    QCOMPARE(a.left, 3);
    QCOMPARE(a.down, 1);
    QVERIFY(TerminalBoxDrawing::armsFor(0x257D, &a)); // ╽ light up, heavy down
    QCOMPARE(a.up, 1);
    QCOMPARE(a.down, 2);
    QVERIFY(!TerminalBoxDrawing::armsFor(0x256D, &a)); // ╭ is an arc
    QVERIFY(!TerminalBoxDrawing::armsFor(0x2504, &a)); // ┄ is a dash
}

void TestTerminalBoxDrawing::horizontal_touchesBothEdges()
{
    const QImage img = render(0x2500);
    const int y = kH / 2;
    bool found = false;
    for (int dy = -2; dy <= 2 && !found; ++dy) {
        found = lit(img, 0, y + dy) && lit(img, kW - 1, y + dy);
    }
    QVERIFY(found);
    // Thin: one light stroke, not a bar.
    QVERIFY(litCount(img) <= kW * 2);
}

void TestTerminalBoxDrawing::vertical_touchesTopAndBottom()
{
    const QImage img = render(0x2502);
    bool found = false;
    for (int x = 0; x < kW && !found; ++x) {
        found = lit(img, x, 0) && lit(img, x, kH - 1);
    }
    QVERIFY(found);
}

void TestTerminalBoxDrawing::cross_isConnectedAtCentre()
{
    const QImage img = render(0x253C);
    bool any = false;
    for (int x = 0; x < kW; ++x) any |= lit(img, x, 0);
    QVERIFY(any);
    any = false;
    for (int y = 0; y < kH; ++y) any |= lit(img, 0, y);
    QVERIFY(any);
    // Centre pixel is covered (no gap where arms meet).
    bool centre = false;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
            centre |= lit(img, kW / 2 + dx, kH / 2 + dy);
    QVERIFY(centre);
}

void TestTerminalBoxDrawing::doubleCorner_hasTwoStrokesPerArm()
{
    const QImage img = render(0x2554); // ╔
    // Right edge column: two separate lit runs (the two horizontal strokes).
    int runs = 0;
    bool prev = false;
    for (int y = 0; y < kH; ++y) {
        const bool now = lit(img, kW - 1, y);
        if (now && !prev) ++runs;
        prev = now;
    }
    QCOMPARE(runs, 2);
    // Bottom edge row: two separate lit runs (the two vertical strokes).
    runs = 0;
    prev = false;
    for (int x = 0; x < kW; ++x) {
        const bool now = lit(img, x, kH - 1);
        if (now && !prev) ++runs;
        prev = now;
    }
    QCOMPARE(runs, 2);
}

void TestTerminalBoxDrawing::fullBlock_fillsCell()
{
    QCOMPARE(litCount(render(0x2588)), kW * kH);
}

void TestTerminalBoxDrawing::halfBlocks_splitExactly()
{
    const QImage upper = render(0x2580);
    const QImage lower = render(0x2584);
    QCOMPARE(litCount(upper) + litCount(lower), kW * kH);
    QVERIFY(lit(upper, 0, 0));
    QVERIFY(!lit(upper, 0, kH - 1));
    QVERIFY(lit(lower, 0, kH - 1));
}

void TestTerminalBoxDrawing::powerlineTriangle_fillsLeftEdge()
{
    const QImage img = render(0xE0B0);
    for (int y = 1; y < kH - 1; ++y) {
        QVERIFY2(lit(img, 0, y), qPrintable(QStringLiteral("row %1").arg(y)));
    }
    QVERIFY(!lit(img, kW - 1, 0));
}

void TestTerminalBoxDrawing::fractionalScale_stillTiles()
{
    const qreal dpr = 1.5;
    const QImage img = render(0x2502, dpr);
    bool found = false;
    for (int x = 0; x < img.width() && !found; ++x) {
        found = lit(img, x, 0) && lit(img, x, img.height() - 1);
    }
    QVERIFY(found);
    QCOMPARE(litCount(render(0x2588, dpr)), img.width() * img.height());
}

QTEST_MAIN(TestTerminalBoxDrawing)

#include "test_terminal_box_drawing.moc"
