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
#include <QFloat16>
#include <QtEndian>

#include "preview/AsepriteDecoder.h"
#include "preview/DdsDecoder.h"
#include "preview/RasterDecoders.h"

namespace {

void put16(QByteArray &b, quint16 v)
{
    char x[2];
    qToLittleEndian(v, x);
    b.append(x, 2);
}

void put32(QByteArray &b, quint32 v)
{
    char x[4];
    qToLittleEndian(v, x);
    b.append(x, 4);
}

constexpr quint32 fourCC(char a, char b, char c, char d)
{
    return quint32(quint8(a)) | (quint32(quint8(b)) << 8) | (quint32(quint8(c)) << 16) | (quint32(quint8(d)) << 24);
}

struct DdsSpec {
    int width = 1, height = 1, mips = 1;
    quint32 pfFlags = 0x4; // DDPF_FOURCC
    quint32 fourcc = 0;
    quint32 bitCount = 0;
    quint32 masks[4] = {0, 0, 0, 0};
    quint32 caps2 = 0;
    // DX10
    bool dx10 = false;
    quint32 dxgi = 0, misc = 0, arraySize = 1;
};

QByteArray ddsHeader(const DdsSpec &s)
{
    QByteArray b("DDS ");
    put32(b, 124);
    put32(b, 0x1 | 0x2 | 0x4 | 0x1000 | (s.mips > 1 ? 0x20000 : 0));
    put32(b, quint32(s.height));
    put32(b, quint32(s.width));
    put32(b, 0); // pitch
    put32(b, 0); // depth
    put32(b, quint32(s.mips));
    b.append(44, '\0');
    put32(b, 32);
    put32(b, s.pfFlags);
    put32(b, s.dx10 ? fourCC('D', 'X', '1', '0') : s.fourcc);
    put32(b, s.bitCount);
    for (quint32 m : s.masks) put32(b, m);
    put32(b, 0x1000); // caps
    put32(b, s.caps2);
    b.append(12, '\0');
    if (s.dx10) {
        put32(b, s.dxgi);
        put32(b, 3); // TEXTURE2D
        put32(b, s.misc);
        put32(b, s.arraySize);
        put32(b, 0);
    }
    return b;
}

} // namespace

class TestTextureDecoders : public QObject
{
    Q_OBJECT

private slots:
    void ddsLegacyBgraWithMips()
    {
        DdsSpec s;
        s.width = 2;
        s.height = 2;
        s.mips = 2;
        s.pfFlags = 0x40 | 0x1; // RGB | ALPHAPIXELS
        s.bitCount = 32;
        s.masks[0] = 0x00ff0000;
        s.masks[1] = 0x0000ff00;
        s.masks[2] = 0x000000ff;
        s.masks[3] = 0xff000000;
        QByteArray f = ddsHeader(s);
        // mip 0: 2x2, mip 1: 1x1 (B G R A bytes)
        const char mip0[16] = {'\x10', '\x20', '\x30', '\x40', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        f.append(mip0, 16);
        f.append("\x01\x02\x03\x04", 4);
        QVERIFY(DdsDecoder::looksLikeDds(f));
        QCOMPARE(DdsDecoder::estimate(f), qint64(2 * 2 * 4 + 1 * 4));
        std::atomic_bool cancel{false};
        const auto r = DdsDecoder::decode(f, cancel);
        QVERIFY2(r.doc, qPrintable(r.error));
        QCOMPARE(r.doc->mipCount, 2);
        QCOMPARE(int(r.doc->surfaces.size()), 2);
        const QRgb px = r.doc->surface(0, 0, 0).pixel(0, 0);
        QCOMPARE(qRed(px), 0x30);
        QCOMPARE(qGreen(px), 0x20);
        QCOMPARE(qBlue(px), 0x10);
        QCOMPARE(qAlpha(px), 0x40);
        QCOMPARE(r.doc->surface(0, 0, 1).size(), QSize(1, 1));
    }

    void ddsBc1SolidRed()
    {
        DdsSpec s;
        s.width = 4;
        s.height = 4;
        s.fourcc = fourCC('D', 'X', 'T', '1');
        QByteArray f = ddsHeader(s);
        put16(f, 0xF800); // color0 = pure red (565)
        put16(f, 0x0000); // color1 = black; c0 > c1 -> opaque 4-colour mode
        put32(f, 0);      // every texel index 0
        std::atomic_bool cancel{false};
        const auto r = DdsDecoder::decode(f, cancel);
        QVERIFY2(r.doc, qPrintable(r.error));
        QCOMPARE(r.doc->detail, QStringLiteral("BC1 (DXT1)"));
        const QImage &img = r.doc->surface(0, 0, 0);
        QCOMPARE(img.size(), QSize(4, 4));
        QCOMPARE(img.pixel(3, 3), qRgba(255, 0, 0, 255));
    }

    // Non-multiple-of-4 sizes clip the last block.
    void ddsBc1OddSize()
    {
        DdsSpec s;
        s.width = 5;
        s.height = 3;
        s.fourcc = fourCC('D', 'X', 'T', '1');
        QByteArray f = ddsHeader(s);
        for (int i = 0; i < 2; ++i) {
            put16(f, 0x001F); // blue
            put16(f, 0x0000);
            put32(f, 0);
        }
        std::atomic_bool cancel{false};
        const auto r = DdsDecoder::decode(f, cancel);
        QVERIFY2(r.doc, qPrintable(r.error));
        QCOMPARE(r.doc->surface(0, 0, 0).size(), QSize(5, 3));
        QCOMPARE(r.doc->surface(0, 0, 0).pixel(4, 2), qRgba(0, 0, 255, 255));
    }

    void ddsDx10Cubemap()
    {
        DdsSpec s;
        s.dx10 = true;
        s.dxgi = 28; // R8G8B8A8_UNORM
        s.misc = 0x4;
        QByteArray f = ddsHeader(s);
        for (int face = 0; face < 6; ++face) {
            const char px[4] = {char(face * 40), 0, 0, '\xff'};
            f.append(px, 4);
        }
        std::atomic_bool cancel{false};
        const auto r = DdsDecoder::decode(f, cancel);
        QVERIFY2(r.doc, qPrintable(r.error));
        QCOMPARE(r.doc->faceCount, 6);
        QCOMPARE(r.doc->faceNames.size(), 6);
        QCOMPARE(qRed(r.doc->surface(0, 5, 0).pixel(0, 0)), 200);
    }

    void ddsHalfFloatIsHdr()
    {
        DdsSpec s;
        s.dx10 = true;
        s.dxgi = 54; // R16_FLOAT
        QByteArray f = ddsHeader(s);
        put16(f, 0x3c00); // 1.0
        std::atomic_bool cancel{false};
        const auto r = DdsDecoder::decode(f, cancel);
        QVERIFY2(r.doc, qPrintable(r.error));
        QVERIFY(r.doc->isHdr);
        const QImage &img = r.doc->surface(0, 0, 0);
        QCOMPARE(img.format(), QImage::Format_RGBA16FPx4);
        const auto *h = reinterpret_cast<const qfloat16 *>(img.constScanLine(0));
        QCOMPARE(float(h[0]), 1.0f);
        QCOMPARE(float(h[1]), 1.0f);
        QCOMPARE(float(h[3]), 1.0f);
    }

    void ddsTruncatedFails()
    {
        DdsSpec s;
        s.width = 64;
        s.height = 64;
        s.fourcc = fourCC('D', 'X', 'T', '5');
        const QByteArray f = ddsHeader(s) + QByteArray(100, '\0');
        std::atomic_bool cancel{false};
        const auto r = DdsDecoder::decode(f, cancel);
        QVERIFY(!r.doc);
        QVERIFY(!r.error.isEmpty());
    }

    void ddsShortMipChainKeepsWhatIsThere()
    {
        DdsSpec s;
        s.width = 4;
        s.height = 4;
        s.mips = 3;
        s.fourcc = fourCC('D', 'X', 'T', '1');
        QByteArray f = ddsHeader(s);
        f.append(8, '\0'); // mip 0 only
        std::atomic_bool cancel{false};
        const auto r = DdsDecoder::decode(f, cancel);
        QVERIFY2(r.doc, qPrintable(r.error));
        QCOMPARE(r.doc->mipCount, 1);
    }

    void tgaTopLeftRgba()
    {
        QByteArray f;
        f.append(char(0));  // id length
        f.append(char(0));  // no colour map
        f.append(char(2));  // uncompressed true colour
        f.append(5, '\0');  // colour map spec
        put16(f, 0);
        put16(f, 0);
        put16(f, 2);
        put16(f, 2);
        f.append(char(32));
        f.append(char(0x28)); // top-left origin, 8 alpha bits
        const char px[16] = {'\x10', '\x20', '\x30', '\x80', 0, 0, 0, '\xff', 0, 0, 0, '\xff', 0, 0, 0, '\xff'};
        f.append(px, 16);
        std::atomic_bool cancel{false};
        QCOMPARE(RasterDecoders::estimate(RasterDecoders::Kind::Tga, f), qint64(16));
        const auto r = RasterDecoders::decode(RasterDecoders::Kind::Tga, f, cancel);
        QVERIFY2(r.doc, qPrintable(r.error));
        QCOMPARE(r.doc->surface(0, 0, 0).pixel(0, 0), qRgba(0x30, 0x20, 0x10, 0x80));
    }

    void magicSniffers()
    {
        QVERIFY(RasterDecoders::looksLikeExr(QByteArrayView("\x76\x2f\x31\x01\x02\0\0\0", 8)));
        QVERIFY(RasterDecoders::looksLikePsd(QByteArrayView("8BPS\0\x01", 6)));
        QVERIFY(!RasterDecoders::looksLikePsd(QByteArrayView("8BPS\0\x02", 6)));
        QVERIFY(RasterDecoders::looksLikeRadiance("#?RADIANCE\n"));
        QVERIFY(!DdsDecoder::looksLikeDds("DDS text"));
    }

    void asepriteRgbaRawCel()
    {
        const QByteArray f = buildAseprite(/*indexed=*/false);
        QVERIFY(AsepriteDecoder::looksLikeAseprite(f));
        QCOMPARE(AsepriteDecoder::estimate(f), qint64(2 * 2 * 4));
        std::atomic_bool cancel{false};
        const auto r = AsepriteDecoder::decode(f, cancel);
        QVERIFY2(r.doc, qPrintable(r.error));
        QCOMPARE(r.doc->surface(0, 0, 0).size(), QSize(2, 2));
        QCOMPARE(r.doc->surface(0, 0, 0).pixel(1, 1), qRgba(255, 0, 0, 255));
        QCOMPARE(qAlpha(r.doc->surface(0, 0, 0).pixel(0, 0)), 0);
    }

    void asepriteIndexedUsesPalette()
    {
        const QByteArray f = buildAseprite(/*indexed=*/true);
        std::atomic_bool cancel{false};
        const auto r = AsepriteDecoder::decode(f, cancel);
        QVERIFY2(r.doc, qPrintable(r.error));
        QCOMPARE(r.doc->surface(0, 0, 0).pixel(1, 1), qRgba(0, 255, 0, 255));
        QCOMPARE(qAlpha(r.doc->surface(0, 0, 0).pixel(0, 0)), 0); // transparent index 0
    }

private:
    // One 2x2 frame, one visible layer, one raw cel whose (1,1) pixel is red
    // (RGBA) or palette entry 1 = green (indexed, transparent index 0).
    static QByteArray buildAseprite(bool indexed)
    {
        QByteArray chunks;
        int chunkCount = 0;
        auto chunk = [&](quint16 type, const QByteArray &body) {
            put32(chunks, quint32(6 + body.size()));
            put16(chunks, type);
            chunks += body;
            ++chunkCount;
        };
        if (indexed) {
            QByteArray pal;
            put32(pal, 2);
            put32(pal, 0);
            put32(pal, 1);
            pal.append(8, '\0');
            put16(pal, 0); pal.append("\0\0\0\xff", 4);
            put16(pal, 0); pal.append("\0\xff\0\xff", 4);
            chunk(0x2019, pal);
        }
        QByteArray layer;
        put16(layer, 1); // visible
        put16(layer, 0); // image layer
        put16(layer, 0);
        put16(layer, 0);
        put16(layer, 0);
        put16(layer, 0); // normal
        layer.append(char(255));
        layer.append(3, '\0');
        put16(layer, 1);
        layer.append('L');
        chunk(0x2004, layer);

        QByteArray cel;
        put16(cel, 0);
        put16(cel, 0);
        put16(cel, 0);
        cel.append(char(255));
        put16(cel, 0); // raw
        put16(cel, 0); // z-index
        cel.append(5, '\0');
        put16(cel, 2);
        put16(cel, 2);
        if (indexed) {
            cel.append("\0\0\0\x01", 4);
        } else {
            cel.append(12, '\0');
            cel.append("\xff\0\0\xff", 4);
        }
        chunk(0x2005, cel);

        QByteArray frame;
        put32(frame, quint32(16 + chunks.size()));
        put16(frame, 0xF1FA);
        put16(frame, quint16(chunkCount));
        put16(frame, 100);
        frame.append(2, '\0');
        put32(frame, quint32(chunkCount));
        frame += chunks;

        QByteArray h;
        put32(h, quint32(128 + frame.size()));
        put16(h, 0xA5E0);
        put16(h, 1);
        put16(h, 2);
        put16(h, 2);
        put16(h, indexed ? 8 : 32);
        put32(h, 1); // layer opacity valid
        put16(h, 100);
        put32(h, 0);
        put32(h, 0);
        h.append(char(0)); // transparent index
        h.append(3, '\0');
        put16(h, indexed ? 2 : 0);
        h.append(char(1));
        h.append(char(1));
        h.append(8, '\0'); // grid x, y, width, height
        h.append(84, '\0');
        Q_ASSERT(h.size() == 128);
        return h + frame;
    }
};

QTEST_APPLESS_MAIN(TestTextureDecoders)
#include "test_texture_decoders.moc"
