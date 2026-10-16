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

#include "preview/ImageDocument.h"
#include "preview/ImageViewMath.h"

class TestImageViewMath : public QObject
{
    Q_OBJECT

private slots:
    void fitZoomUsesTighterAxis()
    {
        QCOMPARE(ImageViewMath::fitZoom({800, 600}, {1600, 600}), 0.5);
        QCOMPARE(ImageViewMath::fitZoom({800, 600}, {100, 300}), 2.0);
        QCOMPARE(ImageViewMath::fitZoom({800, 600}, {0, 0}), 1.0);
    }

    void initialZoomIsWholeForSmallImages()
    {
        // 16x16 sprite in 800x600: largest whole zoom capped at 8.
        QCOMPARE(ImageViewMath::initialZoom({800, 600}, {16, 16}), 8.0);
        // 300x200 fits 2.66x: floor to 2.
        QCOMPARE(ImageViewMath::initialZoom({800, 600}, {300, 200}), 2.0);
        // Larger than the viewport: fit.
        QCOMPARE(ImageViewMath::initialZoom({800, 600}, {1600, 1200}), 0.5);
    }

    void viewImageRoundTrip()
    {
        ImageViewMath m{{800, 600}, {1000, 1000}, 2.5, {400, 300}};
        const QPointF p(123.25, 456.5);
        const QPointF back = m.viewToImage(m.imageToView(p));
        QVERIFY(qAbs(back.x() - p.x()) < 1e-9);
        QVERIFY(qAbs(back.y() - p.y()) < 1e-9);
    }

    void zoomAtKeepsAnchorFixed()
    {
        ImageViewMath m{{800, 600}, {4000, 4000}, 1.0, {2000, 2000}};
        const QPointF anchor(100, 50);
        const QPointF before = m.viewToImage(anchor);
        m.zoomAt(anchor, 4.0);
        const QPointF after = m.viewToImage(anchor);
        QVERIFY(qAbs(before.x() - after.x()) < 1e-9);
        QVERIFY(qAbs(before.y() - after.y()) < 1e-9);
        QCOMPARE(m.zoom, 4.0);
    }

    void zoomIsClamped()
    {
        ImageViewMath m{{800, 600}, {100, 100}, 1.0, {50, 50}};
        m.zoomAt({400, 300}, 1e6);
        QCOMPARE(m.zoom, ImageViewMath::kMaxZoom);
        m.zoomAt({400, 300}, 1e-9);
        QCOMPARE(m.zoom, ImageViewMath::kMinZoom);
    }

    void smallImageStaysCentred()
    {
        ImageViewMath m{{800, 600}, {100, 100}, 1.0, {50, 50}};
        m.panBy({300, -200});
        QCOMPARE(m.center, QPointF(50, 50));
    }

    void panStopsAtImageEdge()
    {
        ImageViewMath m{{800, 600}, {2000, 2000}, 1.0, {1000, 1000}};
        m.panBy({100000, 100000});
        QCOMPARE(m.center, QPointF(400, 300));
        QCOMPARE(m.visibleImageRect(), QRectF(0, 0, 800, 600));
        m.panBy({-100000, -100000});
        QCOMPARE(m.center, QPointF(1600, 1700));
    }

    void levelForPicksSmallestSufficientLevel()
    {
        QCOMPARE(ImageViewMath::levelFor(2.0, 6), 0);
        QCOMPARE(ImageViewMath::levelFor(1.0, 6), 0);
        QCOMPARE(ImageViewMath::levelFor(0.5, 6), 1);
        QCOMPARE(ImageViewMath::levelFor(0.3, 6), 1);
        QCOMPARE(ImageViewMath::levelFor(0.25, 6), 2);
        QCOMPARE(ImageViewMath::levelFor(1.0 / 1024, 6), 5);
        QCOMPARE(ImageViewMath::levelFor(0.1, 1), 0);
    }

    // Transparent texels must not darken their opaque neighbours.
    void halveIsAlphaWeighted()
    {
        QImage src(2, 2, QImage::Format_ARGB32);
        src.setPixel(0, 0, qRgba(255, 0, 0, 255));
        src.setPixel(1, 0, qRgba(0, 0, 0, 0));
        src.setPixel(0, 1, qRgba(0, 0, 0, 0));
        src.setPixel(1, 1, qRgba(0, 0, 0, 0));
        const QImage dst = ImageOps::halve(src);
        QCOMPARE(dst.size(), QSize(1, 1));
        const QRgb px = dst.pixel(0, 0);
        QCOMPARE(qRed(px), 255);
        QCOMPARE(qAlpha(px), 64);
    }

    void halveOpaqueAverages()
    {
        QImage src(2, 2, QImage::Format_ARGB32);
        src.setPixel(0, 0, qRgba(0, 0, 0, 255));
        src.setPixel(1, 0, qRgba(255, 255, 255, 255));
        src.setPixel(0, 1, qRgba(0, 0, 0, 255));
        src.setPixel(1, 1, qRgba(255, 255, 255, 255));
        const QRgb px = ImageOps::halve(src).pixel(0, 0);
        QCOMPARE(qRed(px), 128);
        QCOMPARE(qAlpha(px), 255);
    }

    void pyramidStopsAtSmallLevel()
    {
        QImage src(256, 64, QImage::Format_ARGB32);
        src.fill(Qt::white);
        const ImagePyramidPtr p = ImageOps::buildPyramid(src);
        QVERIFY(p);
        QVERIFY(!p->levels.empty());
        QCOMPARE(std::max(p->levels.back().width(), p->levels.back().height()) <= 32, true);
    }

    void renderCropCoversOnlyRect()
    {
        QImage src(64, 64, QImage::Format_ARGB32);
        src.fill(qRgba(10, 20, 30, 255));
        const QImage crop = ImageOps::renderCrop(src, QRect(8, 8, 16, 4), {});
        QCOMPARE(crop.size(), QSize(16, 4));
        QCOMPARE(crop.format(), QImage::Format_ARGB32_Premultiplied);
        ImageOps::ViewParams red;
        red.channel = ImageOps::Channel::R;
        const QRgb px = ImageOps::renderCrop(src, QRect(0, 0, 1, 1), red).pixel(0, 0);
        QCOMPARE(qRed(px), 10);
        QCOMPARE(qGreen(px), 10);
        QCOMPARE(qBlue(px), 10);
    }
};

QTEST_APPLESS_MAIN(TestImageViewMath)
#include "test_image_view_math.moc"
