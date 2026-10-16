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
#include <QTemporaryDir>
#include <QtEndian>

#include "preview/atlas/AtlasDocument.h"
#include "preview/atlas/AtlasLoader.h"
#include "preview/tilemap/TileMapDocument.h"
#include "preview/tilemap/TileMapLoader.h"
#include "preview/tilemap/AssetFileUtil.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace {

const std::atomic_bool kNever{false};

QString writeFile(const QTemporaryDir &dir, const QString &name, const QByteArray &data)
{
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return {};
    f.write(data);
    return path;
}

// Two 16 x 16 tiles side by side: red, then green.
QString writeTiles(const QTemporaryDir &dir, const QString &name = QStringLiteral("tiles.png"))
{
    QImage img(32, 16, QImage::Format_ARGB32);
    img.fill(Qt::red);
    for (int y = 0; y < 16; ++y) {
        for (int x = 16; x < 32; ++x)
            img.setPixelColor(x, y, Qt::green);
    }
    const QString path = dir.filePath(name);
    img.save(path);
    return path;
}

// One 64 x 32 tile.
QString writeIsoTile(const QTemporaryDir &dir)
{
    QImage img(64, 32, QImage::Format_ARGB32);
    img.fill(Qt::blue);
    const QString path = dir.filePath(QStringLiteral("iso.png"));
    img.save(path);
    return path;
}

TileMapLoader::Result loadMap(const QTemporaryDir &dir, const QString &name, const QByteArray &data)
{
    return TileMapLoader::load(writeFile(dir, name, data), kNever);
}

QByteArray tilesetJson(const QString &image = QStringLiteral("tiles.png"))
{
    return QStringLiteral(R"({"firstgid":1,"name":"t","tilewidth":16,"tileheight":16,"columns":2,"tilecount":2,)"
                          R"("image":"%1","imagewidth":32,"imageheight":16})")
        .arg(image).toUtf8();
}

QByteArray orthoMap(const QByteArray &layerJson, const QByteArray &tileset, int w = 2, int h = 2)
{
    return QByteArray(R"({"type":"map","orientation":"orthogonal","renderorder":"right-down","width":)")
           + QByteArray::number(w) + R"(,"height":)" + QByteArray::number(h)
           + R"(,"tilewidth":16,"tileheight":16,"infinite":false,"tilesets":[)" + tileset + R"(],"layers":[)" + layerJson
           + "]}";
}

void put16(QByteArray &b, quint16 v) { char x[2]; qToLittleEndian(v, x); b.append(x, 2); }
void put32(QByteArray &b, quint32 v) { char x[4]; qToLittleEndian(v, x); b.append(x, 4); }

} // namespace

class Test2dFormats : public QObject
{
    Q_OBJECT

private slots:
    // ---------------------------------------------------------------- atlas
    void gdxAtlas();
    void spineAtlasBounds();
    void texturePackerJsonArray();
    void texturePackerJsonHash();
    void asepriteTags();
    void asepriteOverlappingTags();
    void starlingXml();
    void cocosPlist();
    void bmFontText();
    void bmFontXml();
    void bmFontBinary();
    void bitmapFontLayout();
    void atlasSniffers();
    void atlasLoadsPages();
    void atlasRejectsGarbage();
    void plistTooDeep();

    // ------------------------------------------------------------- tile maps
    void tmjOrthogonal();
    void tmjBase64Zlib();
    void tmjBase64Gzip();
    void tmxWithObjects();
    void tmjInfiniteChunks();
    void isometricPositions();
    void staggeredPositions();
    void hexagonalCells();
    void tilesetSheetWithAnimation();
    void externalTilesetTsx();
    void missingTilesetImage();
    void ldtkProject();
    void hitTestAndGrid();
    void gridIndexQuery();
    void gridIndexHugeExtent();
    void tileMapSniffers();
    void tileMapErrors();
    void tileMapCancel();

    // ------------------------------------------------------ crafted inputs
    void tiledCraftedSheetNumbers();
    void tiledHugeTileId();
    void ldtkCraftedSheetNumbers();
    void tmxTooDeep();
    void networkPathsRefused();
};

// ---------------------------------------------------------------------------
// Atlas
// ---------------------------------------------------------------------------

void Test2dFormats::gdxAtlas()
{
    const QByteArray text =
        "atlas.png\n"
        "size: 128,64\n"
        "format: RGBA8888\n"
        "filter: Linear,Linear\n"
        "repeat: none\n"
        "hero\n"
        "  rotate: false\n"
        "  xy: 2, 4\n"
        "  size: 16, 24\n"
        "  orig: 20, 30\n"
        "  offset: 1, 3\n"
        "  index: -1\n"
        "coin\n"
        "  rotate: true\n"
        "  xy: 40, 8\n"
        "  size: 10, 20\n"
        "  orig: 10, 20\n"
        "  offset: 0, 0\n"
        "  index: 2\n";
    QString error;
    const auto doc = AtlasLoader::parse(text, QStringLiteral("atlas"), QString(), &error);
    QVERIFY2(doc, qPrintable(error));
    QCOMPARE(doc->kind, Atlas::Document::Kind::Atlas);
    QCOMPARE(doc->pages.size(), size_t(1));
    QCOMPARE(doc->pages[0].file, QStringLiteral("atlas.png"));
    QCOMPARE(doc->pages[0].size, QSize(128, 64));
    QCOMPARE(doc->frames.size(), size_t(2));

    const Atlas::Frame &hero = doc->frames[0];
    QCOMPARE(hero.name, QStringLiteral("hero"));
    QCOMPARE(hero.rect, QRect(2, 4, 16, 24));
    QVERIFY(!hero.rotated);
    QCOMPARE(hero.sourceSize, QSize(20, 30));
    // The offset's y counts from the bottom: 30 - 24 - 3 = 3 from the top.
    QCOMPARE(hero.trim, QPoint(1, 3));

    const Atlas::Frame &coin = doc->frames[1];
    QVERIFY(coin.rotated);
    QCOMPARE(coin.rect, QRect(40, 8, 20, 10));   // stored turned: h x w
    QCOMPARE(coin.sourceSize, QSize(10, 20));
    QCOMPARE(coin.index, 2);
}

void Test2dFormats::spineAtlasBounds()
{
    const QByteArray text =
        "page.png\n"
        "size: 64, 64\n"
        "region\n"
        "  rotate: 90\n"
        "  bounds: 5, 6, 7, 8\n"
        "  offsets: 1, 2, 9, 10\n";
    QString error;
    const auto doc = AtlasLoader::parse(text, QString(), QString(), &error);
    QVERIFY2(doc, qPrintable(error));
    QCOMPARE(doc->frames.size(), size_t(1));
    const Atlas::Frame &f = doc->frames[0];
    QVERIFY(f.rotated);
    QCOMPARE(f.rect, QRect(5, 6, 8, 7));
    QCOMPARE(f.sourceSize, QSize(9, 10));
    QCOMPARE(f.trim, QPoint(1, 0));
}

void Test2dFormats::texturePackerJsonArray()
{
    const QByteArray json =
        R"({"frames":[{"filename":"a.png","frame":{"x":1,"y":2,"w":10,"h":20},"rotated":true,"trimmed":true,)"
        R"("spriteSourceSize":{"x":3,"y":4,"w":20,"h":10},"sourceSize":{"w":30,"h":40},"pivot":{"x":0.5,"y":0.25}}],)"
        R"("meta":{"app":"https://www.codeandweb.com/texturepacker","image":"sheet.png","size":{"w":64,"h":64}}})";
    QString error;
    const auto doc = AtlasLoader::parse(json, QStringLiteral("json"), QString(), &error);
    QVERIFY2(doc, qPrintable(error));
    QCOMPARE(doc->format, QStringLiteral("TexturePacker JSON"));
    QCOMPARE(doc->pages[0].file, QStringLiteral("sheet.png"));
    QCOMPARE(doc->pages[0].size, QSize(64, 64));
    const Atlas::Frame &f = doc->frames[0];
    QCOMPARE(f.name, QStringLiteral("a.png"));
    QCOMPARE(f.rect, QRect(1, 2, 10, 20));
    QVERIFY(f.rotated);
    QCOMPARE(f.trim, QPoint(3, 4));
    QCOMPARE(f.sourceSize, QSize(30, 40));
    QVERIFY(f.hasPivot);
    QCOMPARE(f.pivot, QPointF(0.5, 0.25));
}

void Test2dFormats::texturePackerJsonHash()
{
    const QByteArray json = R"({"frames":{"b.png":{"frame":{"x":0,"y":0,"w":8,"h":8}}},"meta":{"image":"x.png"}})";
    QString error;
    const auto doc = AtlasLoader::parse(json, QStringLiteral("json"), QString(), &error);
    QVERIFY2(doc, qPrintable(error));
    QCOMPARE(doc->frames.size(), size_t(1));
    QCOMPARE(doc->frames[0].name, QStringLiteral("b.png"));
    QCOMPARE(doc->frames[0].sourceSize, QSize(8, 8));
}

void Test2dFormats::asepriteTags()
{
    const QByteArray json =
        R"({"frames":[)"
        R"({"filename":"f 0","frame":{"x":0,"y":0,"w":8,"h":8}},)"
        R"({"filename":"f 1","frame":{"x":8,"y":0,"w":8,"h":8}},)"
        R"({"filename":"f 2","frame":{"x":16,"y":0,"w":8,"h":8}}],)"
        R"("meta":{"app":"http://www.aseprite.org/","image":"s.png","frameTags":[{"name":"walk","from":1,"to":2}]}})";
    QString error;
    const auto doc = AtlasLoader::parse(json, QStringLiteral("json"), QString(), &error);
    QVERIFY2(doc, qPrintable(error));
    QCOMPARE(doc->format, QStringLiteral("Aseprite sheet"));
    QVERIFY(doc->frames[0].tag.isEmpty());
    QCOMPARE(doc->frames[1].tag, QStringLiteral("walk"));
    QCOMPARE(doc->frames[2].tag, QStringLiteral("walk"));
}

void Test2dFormats::asepriteOverlappingTags()
{
    // A later tag wins where ranges overlap; out-of-range bounds are clamped.
    QByteArray frames;
    for (int i = 0; i < 6; ++i)
        frames += (i ? "," : "") + QStringLiteral(R"({"filename":"f%1","frame":{"x":%2,"y":0,"w":8,"h":8}})")
                                       .arg(i).arg(i * 8).toUtf8();
    const QByteArray json = R"({"frames":[)" + frames + R"(],"meta":{"app":"aseprite","image":"s.png","frameTags":[)"
                            R"({"name":"all","from":-5,"to":99},{"name":"mid","from":2,"to":3},{"name":"bad","from":4,"to":1}]}})";
    QString error;
    const auto doc = AtlasLoader::parse(json, QStringLiteral("json"), QString(), &error);
    QVERIFY2(doc, qPrintable(error));
    const QStringList expected{"all", "all", "mid", "mid", "all", "all"};
    for (int i = 0; i < 6; ++i)
        QCOMPARE(doc->frames[size_t(i)].tag, expected[i]);
}

void Test2dFormats::starlingXml()
{
    const QByteArray xml =
        R"(<TextureAtlas imagePath="atlas.png">)"
        R"(<SubTexture name="a" x="10" y="20" width="30" height="40" frameX="-2" frameY="-3" frameWidth="34" frameHeight="46"/>)"
        R"(<SubTexture name="r" x="0" y="0" width="8" height="16" rotated="true"/></TextureAtlas>)";
    QString error;
    const auto doc = AtlasLoader::parse(xml, QStringLiteral("xml"), QString(), &error);
    QVERIFY2(doc, qPrintable(error));
    QCOMPARE(doc->pages[0].file, QStringLiteral("atlas.png"));
    QCOMPARE(doc->frames.size(), size_t(2));
    QCOMPARE(doc->frames[0].rect, QRect(10, 20, 30, 40));
    QCOMPARE(doc->frames[0].trim, QPoint(2, 3));
    QCOMPARE(doc->frames[0].sourceSize, QSize(34, 46));
    QVERIFY(doc->frames[1].rotated);
    QCOMPARE(doc->frames[1].rect, QRect(0, 0, 16, 8));
    QCOMPARE(doc->frames[1].sourceSize, QSize(8, 16));
}

void Test2dFormats::cocosPlist()
{
    const QByteArray plist =
        R"(<?xml version="1.0" encoding="UTF-8"?><plist version="1.0"><dict><key>frames</key><dict>)"
        R"(<key>x.png</key><dict><key>textureRect</key><string>{{4,8},{16,32}}</string>)"
        R"(<key>textureRotated</key><true/><key>spriteSourceSize</key><string>{16,32}</string>)"
        R"(<key>spriteOffset</key><string>{0,0}</string><key>spriteSize</key><string>{16,32}</string></dict></dict>)"
        R"(<key>metadata</key><dict><key>format</key><integer>3</integer><key>textureFileName</key><string>t.png</string>)"
        R"(<key>size</key><string>{128,128}</string></dict></dict></plist>)";
    QString error;
    const auto doc = AtlasLoader::parse(plist, QStringLiteral("plist"), QString(), &error);
    QVERIFY2(doc, qPrintable(error));
    QCOMPARE(doc->format, QStringLiteral("Cocos2d plist"));
    QCOMPARE(doc->pages[0].file, QStringLiteral("t.png"));
    QCOMPARE(doc->pages[0].size, QSize(128, 128));
    QCOMPARE(doc->frames.size(), size_t(1));
    QCOMPARE(doc->frames[0].name, QStringLiteral("x.png"));
    QVERIFY(doc->frames[0].rotated);
    QCOMPARE(doc->frames[0].rect, QRect(4, 8, 32, 16));
    QCOMPARE(doc->frames[0].trim, QPoint(0, 0));
}

namespace {

QByteArray bmFontSample()
{
    return "info face=\"Test Font\" size=32 bold=0 italic=0\n"
           "common lineHeight=36 base=29 scaleW=256 scaleH=256 pages=1 packed=0\n"
           "page id=0 file=\"font_0.png\"\n"
           "chars count=3\n"
           "char id=65 x=10 y=20 width=12 height=14 xoffset=1 yoffset=2 xadvance=13 page=0 chnl=15\n"
           "char id=66 x=30 y=20 width=10 height=14 xoffset=0 yoffset=2 xadvance=11 page=0 chnl=15\n"
           "char id=32 x=0 y=0 width=0 height=0 xoffset=0 yoffset=0 xadvance=4 page=0 chnl=15\n"
           "kernings count=1\n"
           "kerning first=65 second=66 amount=-2\n";
}

} // namespace

void Test2dFormats::bmFontText()
{
    QString error;
    const auto doc = AtlasLoader::parse(::bmFontSample(), QStringLiteral("fnt"), QString(), &error);
    QVERIFY2(doc, qPrintable(error));
    QCOMPARE(doc->kind, Atlas::Document::Kind::BitmapFont);
    QCOMPARE(doc->face, QStringLiteral("Test Font"));
    QCOMPARE(doc->fontSize, 32);
    QCOMPARE(doc->lineHeight, 36);
    QCOMPARE(doc->base, 29);
    QCOMPARE(doc->pages[0].file, QStringLiteral("font_0.png"));
    QCOMPARE(doc->glyphs.size(), size_t(3));
    QCOMPARE(doc->glyphs[0].rect, QRect(10, 20, 12, 14));
    QCOMPARE(doc->glyphs[0].xoffset, 1);
    QCOMPARE(doc->glyphs[0].xadvance, 13);
    QCOMPARE(doc->kerningAmount.value((quint64(65) << 32) | 66, 0), -2);
    QVERIFY(doc->glyphIndex.contains(66));
}

void Test2dFormats::bmFontXml()
{
    const QByteArray xml =
        R"(<?xml version="1.0"?><font><info face="X" size="-16"/>)"
        R"(<common lineHeight="20" base="16" scaleW="128" scaleH="128" pages="1"/>)"
        R"(<pages><page id="0" file="x_0.png"/></pages>)"
        R"(<chars count="1"><char id="97" x="1" y="2" width="3" height="4" xoffset="0" yoffset="1" xadvance="5" page="0" chnl="15"/></chars>)"
        R"(<kernings count="0"/></font>)";
    QString error;
    const auto doc = AtlasLoader::parse(xml, QStringLiteral("fnt"), QString(), &error);
    QVERIFY2(doc, qPrintable(error));
    QCOMPARE(doc->kind, Atlas::Document::Kind::BitmapFont);
    QCOMPARE(doc->face, QStringLiteral("X"));
    QCOMPARE(doc->fontSize, 16);
    QCOMPARE(doc->lineHeight, 20);
    QCOMPARE(doc->glyphs.size(), size_t(1));
    QCOMPARE(doc->glyphs[0].id, quint32(97));
    QCOMPARE(doc->pages[0].file, QStringLiteral("x_0.png"));
}

void Test2dFormats::bmFontBinary()
{
    QByteArray b("BMF", 3);
    b.append(char(3));

    // Block 1: info. 14 bytes of fields, then the null-terminated face name.
    QByteArray info;
    put16(info, 24);
    info.append(QByteArray(12, '\0'));
    info.append("Bin\0", 4);
    b.append(char(1));
    put32(b, quint32(info.size()));
    b.append(info);

    // Block 2: common (15 bytes).
    QByteArray common;
    put16(common, 30); put16(common, 24); put16(common, 128); put16(common, 128); put16(common, 1);
    common.append(QByteArray(5, '\0'));
    b.append(char(2));
    put32(b, quint32(common.size()));
    b.append(common);

    // Block 3: page names.
    const QByteArray pages("p_0.png\0", 8);
    b.append(char(3));
    put32(b, quint32(pages.size()));
    b.append(pages);

    // Block 4: one char (20 bytes).
    QByteArray chars;
    put32(chars, 66);
    put16(chars, 5); put16(chars, 6); put16(chars, 7); put16(chars, 8);
    put16(chars, quint16(qint16(-1))); put16(chars, 2); put16(chars, 9);
    chars.append(char(0)); chars.append(char(15));
    b.append(char(4));
    put32(b, quint32(chars.size()));
    b.append(chars);

    // Block 5: one kerning pair (10 bytes).
    QByteArray kern;
    put32(kern, 66); put32(kern, 67); put16(kern, quint16(qint16(-3)));
    b.append(char(5));
    put32(b, quint32(kern.size()));
    b.append(kern);

    QCOMPARE(chars.size(), 20);
    QString error;
    const auto doc = AtlasLoader::parse(b, QStringLiteral("fnt"), QString(), &error);
    QVERIFY2(doc, qPrintable(error));
    QCOMPARE(doc->format, QStringLiteral("BMFont (binary)"));
    QCOMPARE(doc->face, QStringLiteral("Bin"));
    QCOMPARE(doc->fontSize, 24);
    QCOMPARE(doc->lineHeight, 30);
    QCOMPARE(doc->base, 24);
    QCOMPARE(doc->pages[0].file, QStringLiteral("p_0.png"));
    QCOMPARE(doc->glyphs.size(), size_t(1));
    QCOMPARE(doc->glyphs[0].id, quint32(66));
    QCOMPARE(doc->glyphs[0].rect, QRect(5, 6, 7, 8));
    QCOMPARE(doc->glyphs[0].xoffset, -1);
    QCOMPARE(doc->glyphs[0].xadvance, 9);
    QCOMPARE(doc->kerningAmount.value((quint64(66) << 32) | 67, 0), -3);
}

void Test2dFormats::bitmapFontLayout()
{
    QString error;
    const auto doc = AtlasLoader::parse(::bmFontSample(), QStringLiteral("fnt"), QString(), &error);
    QVERIFY2(doc, qPrintable(error));
    QImage page(256, 256, QImage::Format_ARGB32_Premultiplied);
    page.fill(Qt::white);
    const std::vector<QImage> pages{page};

    // A then B: A advances 13, kerning -2 puts B at 11; 11 + 11 = 22 wide (+2).
    const QImage ab = Atlas::renderBitmapFont(*doc, pages, QStringLiteral("AB"), 0);
    QVERIFY(!ab.isNull());
    QCOMPARE(ab.width(), 24);
    QCOMPARE(ab.height(), 36);

    // Wrapping at the space: two lines.
    const QImage wrapped = Atlas::renderBitmapFont(*doc, pages, QStringLiteral("AA AA"), 30);
    QVERIFY(!wrapped.isNull());
    QCOMPARE(wrapped.height(), 72);

    // A newline always breaks; unknown characters are skipped.
    const QImage lines = Atlas::renderBitmapFont(*doc, pages, QStringLiteral("A\nB"), 0);
    QCOMPARE(lines.height(), 72);
    QVERIFY(Atlas::renderBitmapFont(*doc, pages, QStringLiteral("ZZZ"), 0).isNull());
    QVERIFY(Atlas::renderBitmapFont(*doc, pages, QString(), 0).isNull());
}

void Test2dFormats::atlasSniffers()
{
    QVERIFY(AtlasLoader::looksLikeBmFont("info face=\"A\" size=1\ncommon"));
    QVERIFY(AtlasLoader::looksLikeBmFont(QByteArray("BMF\x03\x01", 5)));
    QVERIFY(AtlasLoader::looksLikeBmFont("<?xml version=\"1.0\"?><font><info face=\"x\"/><common lineHeight=\"1\"/>"));
    QVERIFY(!AtlasLoader::looksLikeBmFont("hello world"));
    QVERIFY(!AtlasLoader::looksLikeBmFont("BMP header"));

    QVERIFY(AtlasLoader::looksLikeAtlasContent(R"({"frames":[{"filename":"a","frame":{"x":0}}],)"));
    QVERIFY(AtlasLoader::looksLikeAtlasContent(R"({"textures":[{"image":"a.png","sprites":[]}]})"));
    QVERIFY(AtlasLoader::looksLikeAtlasContent("<TextureAtlas imagePath=\"a.png\">"));
    QVERIFY(AtlasLoader::looksLikeAtlasContent("<?xml?><plist><dict><key>frames</key><dict>"));
    QVERIFY(AtlasLoader::looksLikeAtlasContent("hero.png\nsize: 64,64\nformat: RGBA8888\n"));
    QVERIFY(!AtlasLoader::looksLikeAtlasContent(R"({"name":"package","version":"1"})"));
    QVERIFY(!AtlasLoader::looksLikeAtlasContent("<html><body></body></html>"));
    QVERIFY(!AtlasLoader::looksLikeAtlasContent("notes.png\nare nice\n"));
    QVERIFY(!AtlasLoader::looksLikeAtlasContent(QByteArray()));
}

void Test2dFormats::atlasLoadsPages()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeTiles(dir, QStringLiteral("sheet.png"));
    const QString good = writeFile(dir, QStringLiteral("good.json"),
                                   R"({"frames":[{"filename":"a","frame":{"x":0,"y":0,"w":16,"h":16}}],"meta":{"image":"sheet.png"}})");
    AtlasLoader::Result r = AtlasLoader::load(good, kNever);
    QVERIFY2(r.doc, qPrintable(r.error));
    QCOMPARE(r.images.size(), qsizetype(1));
    QVERIFY(r.images[0]);
    QCOMPARE(r.images[0]->width, 32);
    QVERIFY(r.doc->warnings.isEmpty());
    QVERIFY(!r.doc->pages[0].path.isEmpty());

    const QString bad = writeFile(dir, QStringLiteral("bad.json"),
                                  R"({"frames":[{"filename":"a","frame":{"x":0,"y":0,"w":16,"h":16}}],"meta":{"image":"nope.png"}})");
    r = AtlasLoader::load(bad, kNever);
    QVERIFY(r.doc);
    QVERIFY(!r.images[0]);
    QVERIFY(!r.doc->warnings.isEmpty());

    // A page with another extension next to the atlas is found.
    const QString swapped = writeFile(dir, QStringLiteral("swapped.json"),
                                      R"({"frames":[{"filename":"a","frame":{"x":0,"y":0,"w":16,"h":16}}],"meta":{"image":"sheet.webp"}})");
    r = AtlasLoader::load(swapped, kNever);
    QVERIFY(r.doc);
    QVERIFY(r.images[0]);
}

void Test2dFormats::atlasRejectsGarbage()
{
    QString error;
    QVERIFY(!AtlasLoader::parse("{ not json", QString(), QString(), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!AtlasLoader::parse("<html></html>", QString(), QString(), &error));
    QVERIFY(!AtlasLoader::parse(QByteArray(), QString(), QString(), &error));
    QVERIFY(!AtlasLoader::parse("just some words\nwithout structure\n", QString(), QString(), &error));
    QVERIFY(!AtlasLoader::parse("BMF", QString(), QString(), &error));
}

void Test2dFormats::plistTooDeep()
{
    // Nesting deep enough to overflow a recursive reader's stack is refused.
    constexpr int kDepth = 200000;
    const QByteArray plist = "<?xml version=\"1.0\"?><plist><dict><key>frames</key>" + QByteArray("<array>").repeated(kDepth)
                             + QByteArray("</array>").repeated(kDepth) + "</dict></plist>";
    QString error;
    QVERIFY(!AtlasLoader::parse(plist, QStringLiteral("plist"), QString(), &error));
    QVERIFY(!error.isEmpty());
}

// ---------------------------------------------------------------------------
// Tile maps
// ---------------------------------------------------------------------------

void Test2dFormats::tmjOrthogonal()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeTiles(dir);
    const auto r = loadMap(dir, QStringLiteral("map.tmj"),
        orthoMap(R"({"type":"tilelayer","name":"ground","width":3,"height":2,"data":[1,2,0,2147483649,0,2]})",
                 tilesetJson(), 3, 2));
    QVERIFY2(r.doc, qPrintable(r.error));
    const TileMap::Document &doc = *r.doc;
    QCOMPARE(doc.format, QStringLiteral("Tiled map"));
    QCOMPARE(doc.layers.size(), size_t(1));
    QCOMPARE(doc.tilesets.size(), size_t(1));
    QCOMPARE(doc.tilesets[0].columns, 2);
    QCOMPARE(doc.gridW, 16);
    QVERIFY(doc.warnings.isEmpty());

    const auto &in = doc.layers[0].instances;
    QCOMPARE(in.size(), size_t(4));
    QCOMPARE(in[0].x, 0.0f);
    QCOMPARE(in[0].y, 0.0f);
    QCOMPARE(in[0].tile, 0);
    QCOMPARE(in[0].w, uint16_t(16));
    QCOMPARE(in[1].x, 16.0f);
    QCOMPARE(in[1].tile, 1);
    QVERIFY(in[2].flags & TileMap::FlipH);      // gid 0x80000001: tile 0, flipped
    QCOMPARE(in[2].tile, 0);
    QCOMPARE(in[2].y, 16.0f);
    QCOMPARE(in[3].x, 32.0f);
    QCOMPARE(in[3].y, 16.0f);
    QCOMPARE(in[3].tile, 1);
    QCOMPARE(doc.tileCount, qint64(4));
    QCOMPARE(doc.bounds, QRectF(0, 0, 48, 32));

    // The layer's grid index finds tiles near a point, in draw order.
    std::vector<uint32_t> hits;
    doc.layers[0].grid.query(QRectF(0, 0, 10, 10), hits);
    QVERIFY(!hits.empty());
    QVERIFY(std::is_sorted(hits.begin(), hits.end()));
    QVERIFY(std::find(hits.begin(), hits.end(), 0u) != hits.end());
}

namespace {

QByteArray gidBytes(const std::vector<quint32> &gids)
{
    QByteArray raw;
    for (const quint32 g : gids)
        put32(raw, g);
    return raw;
}

} // namespace

void Test2dFormats::tmjBase64Zlib()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeTiles(dir);
    const QByteArray stream = qCompress(gidBytes({1, 2, 2, 1}), 9).mid(4);   // drop Qt's length prefix: a zlib stream
    const auto r = loadMap(dir, QStringLiteral("z.tmj"),
        orthoMap(R"({"type":"tilelayer","name":"z","width":2,"height":2,"encoding":"base64","compression":"zlib","data":")"
                     + stream.toBase64() + R"("})", tilesetJson()));
    QVERIFY2(r.doc, qPrintable(r.error));
    QCOMPARE(r.doc->layers[0].instances.size(), size_t(4));
    QCOMPARE(r.doc->layers[0].instances[1].tile, 1);
    QCOMPARE(r.doc->layers[0].instances[3].tile, 0);

    // Uncompressed base64.
    const auto plain = loadMap(dir, QStringLiteral("p.tmj"),
        orthoMap(R"({"type":"tilelayer","name":"p","width":2,"height":2,"encoding":"base64","data":")"
                     + gidBytes({2, 2, 2, 2}).toBase64() + R"("})", tilesetJson()));
    QVERIFY2(plain.doc, qPrintable(plain.error));
    QCOMPARE(plain.doc->layers[0].instances.size(), size_t(4));
}

void Test2dFormats::tmjBase64Gzip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeTiles(dir);
    const QByteArray z = qCompress(gidBytes({1, 1, 2, 2}), 9);   // 4-byte length, zlib header, deflate, adler32
    QByteArray gz;
    const unsigned char header[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff};
    gz.append(reinterpret_cast<const char *>(header), 10);
    gz.append(z.mid(6, z.size() - 6 - 4));
    gz.append(QByteArray(8, '\0'));   // CRC32 + ISIZE (not checked)
    const auto r = loadMap(dir, QStringLiteral("g.tmj"),
        orthoMap(R"({"type":"tilelayer","name":"g","width":2,"height":2,"encoding":"base64","compression":"gzip","data":")"
                     + gz.toBase64() + R"("})", tilesetJson()));
    QVERIFY2(r.doc, qPrintable(r.error));
    QVERIFY(r.doc->warnings.isEmpty());
    QCOMPARE(r.doc->layers[0].instances.size(), size_t(4));
    QCOMPARE(r.doc->layers[0].instances[2].tile, 1);

    // zstd is reported, not guessed at.
    const auto zstd = loadMap(dir, QStringLiteral("s.tmj"),
        orthoMap(R"({"type":"tilelayer","name":"s","width":2,"height":2,"encoding":"base64","compression":"zstd","data":"AAAA"})",
                 tilesetJson()));
    QVERIFY(zstd.doc);
    QVERIFY(zstd.doc->layers[0].instances.empty());
    QVERIFY(!zstd.doc->warnings.isEmpty());
}

void Test2dFormats::tmxWithObjects()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeTiles(dir);
    const QByteArray tmx =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<map version=\"1.10\" orientation=\"orthogonal\" renderorder=\"right-down\" width=\"2\" height=\"2\""
        " tilewidth=\"16\" tileheight=\"16\" infinite=\"0\">\n"
        " <tileset firstgid=\"1\" name=\"t\" tilewidth=\"16\" tileheight=\"16\" tilecount=\"2\" columns=\"2\">"
        "<image source=\"tiles.png\" width=\"32\" height=\"16\"/></tileset>\n"
        " <layer id=\"1\" name=\"g\" width=\"2\" height=\"2\"><data encoding=\"csv\">1,2,\n2,1</data></layer>\n"
        " <objectgroup name=\"obj\" color=\"#ff0000\">"
        "<object id=\"1\" name=\"spawn\" x=\"8\" y=\"8\" width=\"16\" height=\"16\"/>"
        "<object id=\"2\" name=\"p\" x=\"0\" y=\"0\"><point/></object>"
        "<object id=\"3\" x=\"4\" y=\"4\"><polygon points=\"0,0 10,0 10,10\"/></object>"
        "</objectgroup>\n"
        "</map>\n";
    const auto r = loadMap(dir, QStringLiteral("m.tmx"), tmx);
    QVERIFY2(r.doc, qPrintable(r.error));
    QCOMPARE(r.doc->layers.size(), size_t(2));
    QCOMPARE(r.doc->layers[0].instances.size(), size_t(4));
    QCOMPARE(r.doc->layers[0].instances[1].tile, 1);
    QCOMPARE(r.doc->layers[0].instances[2].tile, 1);

    const TileMap::Layer &objects = r.doc->layers[1];
    QCOMPARE(objects.kind, TileMap::Layer::Kind::Objects);
    QCOMPARE(objects.color, QColor("#ff0000"));
    QVERIFY(r.doc->hasObjects);
    QCOMPARE(objects.objects.size(), qsizetype(3));
    QCOMPARE(objects.objects[0].name, QStringLiteral("spawn"));
    QCOMPARE(objects.objects[0].shape, TileMap::Object::Shape::Rect);
    QCOMPARE(objects.objects[0].pos, QPointF(8, 8));
    QCOMPARE(objects.objects[0].size, QSizeF(16, 16));
    QCOMPARE(objects.objects[1].shape, TileMap::Object::Shape::Point);
    QCOMPARE(objects.objects[2].shape, TileMap::Object::Shape::Polygon);
    QCOMPARE(objects.objects[2].points.size(), qsizetype(3));
    QCOMPARE(objects.objects[2].pos, QPointF(4, 4));
}

void Test2dFormats::tmjInfiniteChunks()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeTiles(dir);
    const QByteArray json =
        R"({"type":"map","orientation":"orthogonal","width":0,"height":0,"tilewidth":16,"tileheight":16,"infinite":true,"tilesets":[)"
        + tilesetJson() +
        R"(],"layers":[{"type":"tilelayer","name":"c","chunks":[)"
        R"({"x":0,"y":0,"width":2,"height":1,"data":[2,0]},)"
        R"({"x":-2,"y":0,"width":2,"height":1,"data":[1,1]}]}]})";
    const auto r = loadMap(dir, QStringLiteral("inf.tmj"), json);
    QVERIFY2(r.doc, qPrintable(r.error));
    const auto &in = r.doc->layers[0].instances;
    QCOMPARE(in.size(), size_t(3));
    // Chunks are read in reading order whatever order the file lists them.
    QCOMPARE(in[0].x, -32.0f);
    QCOMPARE(in[1].x, -16.0f);
    QCOMPARE(in[2].x, 0.0f);
    QCOMPARE(in[2].tile, 1);
    QVERIFY(r.doc->bounds.left() <= -32.0);
}

void Test2dFormats::isometricPositions()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeIsoTile(dir);
    const QByteArray json =
        R"({"type":"map","orientation":"isometric","width":2,"height":2,"tilewidth":64,"tileheight":32,"infinite":false,)"
        R"("tilesets":[{"firstgid":1,"name":"iso","tilewidth":64,"tileheight":32,"columns":1,"tilecount":1,"image":"iso.png",)"
        R"("imagewidth":64,"imageheight":32}],)"
        R"("layers":[{"type":"tilelayer","name":"g","width":2,"height":2,"data":[1,1,1,1]}]})";
    const auto r = loadMap(dir, QStringLiteral("iso.tmj"), json);
    QVERIFY2(r.doc, qPrintable(r.error));
    QVERIFY(!r.doc->orthogonal);
    QCOMPARE(r.doc->gridW, 0);
    const auto &in = r.doc->layers[0].instances;
    QCOMPARE(in.size(), size_t(4));
    // originX = height * tw / 2 = 64; x = (col - row) * 32 + 64 - 32, y = (col + row) * 16.
    QCOMPARE(in[0].x, 32.0f); QCOMPARE(in[0].y, 0.0f);     // (0, 0)
    QCOMPARE(in[1].x, 64.0f); QCOMPARE(in[1].y, 16.0f);    // (1, 0)
    QCOMPARE(in[2].x, 0.0f);  QCOMPARE(in[2].y, 16.0f);    // (0, 1)
    QCOMPARE(in[3].x, 32.0f); QCOMPARE(in[3].y, 32.0f);    // (1, 1)

    // Hovering a tile's diamond names its cell, though there is no grid overlay.
    QCOMPARE(r.doc->cells.kind, TileMap::CellLayout::Kind::Isometric);
    const QPoint expected[] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    for (int k = 0; k < 4; ++k) {
        QPoint cell;
        QVERIFY(TileMap::cellAt(*r.doc, QPointF(in[size_t(k)].x + 32, in[size_t(k)].y + 16), &cell));
        QCOMPARE(cell, expected[k]);
    }
}

void Test2dFormats::staggeredPositions()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeIsoTile(dir);
    const QByteArray json =
        R"({"type":"map","orientation":"staggered","staggeraxis":"y","staggerindex":"odd","width":3,"height":2,)"
        R"("tilewidth":64,"tileheight":32,"infinite":false,)"
        R"("tilesets":[{"firstgid":1,"name":"iso","tilewidth":64,"tileheight":32,"columns":1,"tilecount":1,"image":"iso.png",)"
        R"("imagewidth":64,"imageheight":32}],)"
        R"("layers":[{"type":"tilelayer","name":"g","width":3,"height":2,"data":[1,1,1,1,1,1]}]})";
    const auto r = loadMap(dir, QStringLiteral("stag.tmj"), json);
    QVERIFY2(r.doc, qPrintable(r.error));
    const auto &in = r.doc->layers[0].instances;
    QCOMPARE(in.size(), size_t(6));
    QCOMPARE(in[1].x, 64.0f); QCOMPARE(in[1].y, 0.0f);     // row 0 is not staggered
    QCOMPARE(in[3].x, 32.0f); QCOMPARE(in[3].y, 16.0f);    // row 1 is: shifted half a tile, half a row down
    QCOMPARE(in[5].x, 160.0f); QCOMPARE(in[5].y, 16.0f);

    // The hover cell of each tile's diamond centre.
    QCOMPARE(r.doc->cells.kind, TileMap::CellLayout::Kind::Staggered);
    for (int k = 0; k < 6; ++k) {
        QPoint cell;
        QVERIFY(TileMap::cellAt(*r.doc, QPointF(in[size_t(k)].x + 32, in[size_t(k)].y + 16), &cell));
        QCOMPARE(cell, QPoint(k % 3, k / 3));
    }
    // A diamond's left vertex region belongs to the row above, not the box it sits in.
    QPoint corner;
    QVERIFY(TileMap::cellAt(*r.doc, QPointF(2, 2), &corner));
    QCOMPARE(corner, QPoint(-1, -1));
}

void Test2dFormats::hexagonalCells()
{
    // Pointy-top hexagons (stagger axis y, odd rows shifted): 32 x 32 boxes
    // with 16 px sides, rows 24 px apart, odd rows shifted 16 px right.
    TileMap::Document doc;
    doc.cells.kind = TileMap::CellLayout::Kind::Staggered;
    doc.cells.tileW = 32;
    doc.cells.tileH = 32;
    doc.cells.hexSide = 16;
    QPoint cell;
    QVERIFY(TileMap::cellAt(doc, QPointF(16, 16), &cell));
    QCOMPARE(cell, QPoint(0, 0));
    QVERIFY(TileMap::cellAt(doc, QPointF(32, 40), &cell));    // (0, 1): x 16 + 16, y 24 + 16
    QCOMPARE(cell, QPoint(0, 1));
    QVERIFY(TileMap::cellAt(doc, QPointF(96, 40), &cell));
    QCOMPARE(cell, QPoint(2, 1));
    QVERIFY(TileMap::cellAt(doc, QPointF(1, 1), &cell));      // the top-left corner is the hexagon above-left
    QCOMPARE(cell, QPoint(-1, -1));

    // Flat-top (stagger axis x).
    doc.cells.staggerX = true;
    QVERIFY(TileMap::cellAt(doc, QPointF(16, 16), &cell));
    QCOMPARE(cell, QPoint(0, 0));
    QVERIFY(TileMap::cellAt(doc, QPointF(40, 32), &cell));    // column 1 shifted down: x 24 + 16, y 16 + 16
    QCOMPARE(cell, QPoint(1, 0));

    // No cells at all: no coordinates.
    TileMap::Document none;
    QVERIFY(!TileMap::cellAt(none, QPointF(5, 5), &cell));
}

void Test2dFormats::tilesetSheetWithAnimation()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeTiles(dir);
    const QByteArray json =
        R"({"name":"ts","tilewidth":16,"tileheight":16,"columns":2,"tilecount":2,"image":"tiles.png","imagewidth":32,"imageheight":16,)"
        R"("tiles":[{"id":0,"animation":[{"tileid":0,"duration":100},{"tileid":1,"duration":200}]}]})";
    const auto r = loadMap(dir, QStringLiteral("set.tsj"), json);
    QVERIFY2(r.doc, qPrintable(r.error));
    QVERIFY(r.doc->tilesetSheet);
    QCOMPARE(r.doc->format, QStringLiteral("Tiled tileset"));
    QCOMPARE(r.doc->layers.size(), size_t(1));
    const auto &in = r.doc->layers[0].instances;
    QCOMPARE(in.size(), size_t(2));
    QCOMPARE(in[1].x, 16.0f);
    QVERIFY(in[0].flags & TileMap::Animated);
    QVERIFY(!(in[1].flags & TileMap::Animated));
    QVERIFY(r.doc->hasAnimations);
    QCOMPARE(r.doc->layers[0].animated.size(), size_t(1));
    QCOMPARE(r.doc->tilesets[0].animations.value(0).frames.size(), size_t(2));
    QCOMPARE(r.doc->tilesets[0].animationTotalMs(0), 300);
    QCOMPARE(r.doc->tilesets[0].animationTotalMs(1), 0);
    // The animated-tile index finds it for the canvas's live culling.
    std::vector<uint32_t> live;
    r.doc->layers[0].animatedGrid.query(QRectF(0, 0, 4, 4), live);
    QCOMPARE(live.size(), size_t(1));
    QCOMPARE(live[0], 0u);
    QCOMPARE(r.doc->layers[0].animatedGrid.countCandidates(QRectF(0, 0, 4, 4)), size_t(1));
}

void Test2dFormats::externalTilesetTsx()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeTiles(dir);
    writeFile(dir, QStringLiteral("ext.tsx"),
              "<?xml version=\"1.0\"?><tileset version=\"1.10\" name=\"ext\" tilewidth=\"16\" tileheight=\"16\" tilecount=\"2\" columns=\"2\">"
              "<image source=\"tiles.png\" width=\"32\" height=\"16\"/>"
              "<tile id=\"1\"><animation><frame tileid=\"1\" duration=\"50\"/><frame tileid=\"0\" duration=\"50\"/></animation></tile>"
              "</tileset>");
    const auto r = loadMap(dir, QStringLiteral("e.tmj"),
        orthoMap(R"({"type":"tilelayer","name":"g","width":2,"height":1,"data":[1,2]})",
                 R"({"firstgid":1,"source":"ext.tsx"})", 2, 1));
    QVERIFY2(r.doc, qPrintable(r.error));
    QVERIFY(r.doc->warnings.isEmpty());
    QCOMPARE(r.doc->tilesets.size(), size_t(1));
    QCOMPARE(r.doc->tilesets[0].name, QStringLiteral("ext"));
    QCOMPARE(r.doc->tilesets[0].tileCount, 2);
    QCOMPARE(r.doc->layers[0].instances.size(), size_t(2));
    QVERIFY(r.doc->layers[0].instances[1].flags & TileMap::Animated);

    // A tileset file that is not there leaves grey placeholders and a warning.
    const auto missing = loadMap(dir, QStringLiteral("e2.tmj"),
        orthoMap(R"({"type":"tilelayer","name":"g","width":2,"height":1,"data":[1,2]})",
                 R"({"firstgid":1,"source":"gone.tsx"})", 2, 1));
    QVERIFY(missing.doc);
    QVERIFY(!missing.doc->warnings.isEmpty());
    QCOMPARE(missing.doc->layers[0].instances.size(), size_t(2));
    QCOMPARE(missing.doc->layers[0].instances[0].tileset, int16_t(-1));
}

void Test2dFormats::missingTilesetImage()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto r = loadMap(dir, QStringLiteral("m.tmj"),
        orthoMap(R"({"type":"tilelayer","name":"g","width":2,"height":2,"data":[1,2,1,2]})",
                 tilesetJson(QStringLiteral("nope.png"))));
    QVERIFY2(r.doc, qPrintable(r.error));
    QVERIFY(!r.doc->warnings.isEmpty());
    const auto &in = r.doc->layers[0].instances;
    QCOMPARE(in.size(), size_t(4));
    QCOMPARE(in[0].tileset, int16_t(-1));
    QCOMPARE(uint32_t(in[0].tile), 0xA0808080u);
    QCOMPARE(in[0].w, uint16_t(16));
}

void Test2dFormats::ldtkProject()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeTiles(dir);
    const QByteArray json =
        R"({"__header__":{"fileType":"LDtk Project JSON"},"jsonVersion":"1.5.3","defaultGridSize":16,"bgColor":"#40465B",)"
        R"("worldLayout":"Free","defs":{"layers":[{"uid":1,"identifier":"Collisions","intGridValues":[{"value":1,"identifier":"wall","color":"#FF0000"}]}],)"
        R"("tilesets":[{"uid":10,"identifier":"T","relPath":"tiles.png","pxWid":32,"pxHei":16,"tileGridSize":16,"spacing":0,"padding":0}],"entities":[]},)"
        R"("levels":[{"identifier":"L0","worldX":100,"worldY":50,"pxWid":64,"pxHei":32,"__bgColor":"#112233","layerInstances":[)"
        R"({"__identifier":"Entities","__type":"Entities","__cWid":4,"__cHei":2,"__gridSize":16,"__pxTotalOffsetX":0,"__pxTotalOffsetY":0,"layerDefUid":2,)"
        R"("entityInstances":[{"__identifier":"Hero","__pivot":[0.5,1],"__smartColor":"#00FF00","px":[16,32],"width":16,"height":32}]},)"
        R"({"__identifier":"Collisions","__type":"IntGrid","__cWid":4,"__cHei":2,"__gridSize":16,"__pxTotalOffsetX":0,"__pxTotalOffsetY":0,"layerDefUid":1,)"
        R"("intGridCsv":[0,1,0,0,0,0,0,1],"autoLayerTiles":[]},)"
        R"({"__identifier":"Tiles","__type":"Tiles","__cWid":4,"__cHei":2,"__gridSize":16,"__tilesetDefUid":10,"__pxTotalOffsetX":0,"__pxTotalOffsetY":0,"layerDefUid":3,)"
        R"("gridTiles":[{"px":[0,0],"src":[16,0],"f":1,"t":1},{"px":[32,16],"src":[0,0],"f":0,"t":0}]}]}]})";
    const auto r = loadMap(dir, QStringLiteral("p.ldtk"), json);
    QVERIFY2(r.doc, qPrintable(r.error));
    const TileMap::Document &doc = *r.doc;
    QCOMPARE(doc.format, QStringLiteral("LDtk project"));
    QCOMPARE(doc.background, QColor("#40465B"));
    QCOMPARE(doc.gridW, 16);
    QCOMPARE(doc.levels.size(), size_t(1));
    QCOMPARE(doc.levels[0].rect, QRectF(100, 50, 64, 32));
    QCOMPARE(doc.levels[0].background, QColor("#112233"));

    // The file lists the top-most layer first; the document is bottom-up.
    QCOMPARE(doc.layers.size(), size_t(3));
    QCOMPARE(doc.layers[0].name, QStringLiteral("Tiles"));
    QCOMPARE(doc.layers[1].name, QStringLiteral("Collisions"));
    QCOMPARE(doc.layers[2].name, QStringLiteral("Entities"));

    const auto &tiles = doc.layers[0].instances;
    QCOMPARE(tiles.size(), size_t(2));
    QCOMPARE(tiles[0].x, 100.0f);
    QCOMPARE(tiles[0].y, 50.0f);
    QCOMPARE(tiles[0].tile, 1);                    // src (16, 0) in a 2-column sheet
    QVERIFY(tiles[0].flags & TileMap::FlipH);
    QCOMPARE(tiles[1].x, 132.0f);
    QCOMPARE(tiles[1].y, 66.0f);
    QCOMPARE(tiles[1].tile, 0);

    const auto &walls = doc.layers[1].instances;
    QCOMPARE(walls.size(), size_t(2));
    QCOMPARE(walls[0].tileset, int16_t(-1));
    QCOMPARE(uint32_t(walls[0].tile), 0xA0FF0000u);
    QCOMPARE(walls[0].x, 116.0f);
    QCOMPARE(walls[1].x, 148.0f);
    QCOMPARE(walls[1].y, 66.0f);

    QCOMPARE(doc.layers[2].kind, TileMap::Layer::Kind::Objects);
    QCOMPARE(doc.layers[2].objects.size(), qsizetype(1));
    const TileMap::Object &hero = doc.layers[2].objects[0];
    QCOMPARE(hero.name, QStringLiteral("Hero"));
    QCOMPARE(hero.pos, QPointF(108, 50));          // px minus the pivot * size
    QCOMPARE(hero.size, QSizeF(16, 32));
    QCOMPARE(hero.color, QColor("#00FF00"));
}

void Test2dFormats::hitTestAndGrid()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeTiles(dir);
    const auto r = loadMap(dir, QStringLiteral("map.tmj"),
        orthoMap(R"({"type":"tilelayer","name":"ground","width":3,"height":2,"data":[1,2,0,1,0,2]})",
                 tilesetJson(), 3, 2));
    QVERIFY2(r.doc, qPrintable(r.error));
    const std::vector<bool> on{true};
    const TileMap::Layer *layer = nullptr;
    const TileMap::Instance *hit = TileMap::hitTest(*r.doc, on, QPointF(20, 5), &layer);
    QVERIFY(hit);
    QCOMPARE(hit->tile, 1);
    QCOMPARE(layer, &r.doc->layers[0]);
    QVERIFY(!TileMap::hitTest(*r.doc, on, QPointF(40, 5), nullptr));          // the empty cell
    QVERIFY(!TileMap::hitTest(*r.doc, on, QPointF(-5, -5), nullptr));         // outside the map
    QVERIFY(!TileMap::hitTest(*r.doc, std::vector<bool>{false}, QPointF(20, 5), nullptr));
    QVERIFY(TileMap::describeInstance(*r.doc, *layer, *hit).contains(QStringLiteral("ground")));
}

void Test2dFormats::gridIndexQuery()
{
    std::vector<TileMap::Instance> instances;
    for (int i = 0; i < 2000; ++i) {
        TileMap::Instance in;
        in.x = float((i % 100) * 32);
        in.y = float((i / 100) * 32);
        in.w = in.h = 32;
        instances.push_back(in);
    }
    TileMap::GridIndex grid;
    grid.build(instances);

    const QRectF query(500, 300, 120, 90);
    std::vector<uint32_t> hits;
    grid.query(query, hits);
    QVERIFY(std::is_sorted(hits.begin(), hits.end()));
    QVERIFY(std::adjacent_find(hits.begin(), hits.end()) == hits.end());   // no duplicates
    for (size_t i = 0; i < instances.size(); ++i) {
        const QRectF r(instances[i].x, instances[i].y, instances[i].w, instances[i].h);
        if (r.intersects(query))
            QVERIFY2(std::binary_search(hits.begin(), hits.end(), uint32_t(i)), qPrintable(QString::number(i)));
    }
    // Far outside the indexed area nothing comes back.
    std::vector<uint32_t> none;
    grid.query(QRectF(-5000, -5000, 10, 10), none);
    QVERIFY(none.empty());
    grid.query(QRectF(90000, 0, 10, 10), none);
    QVERIFY(none.empty());

    TileMap::GridIndex empty;
    empty.build({});
    empty.query(QRectF(0, 0, 100, 100), none);
    QVERIFY(none.empty());
}

void Test2dFormats::gridIndexHugeExtent()
{
    // Extents near the float range (and a NaN) neither overflow the cell
    // counts nor throw: the cells grow until the table is bounded.
    std::vector<TileMap::Instance> instances(4);
    instances[0].x = -3e38f;
    instances[1].x = 3e38f;
    instances[1].y = 3e38f;
    instances[2].x = std::numeric_limits<float>::quiet_NaN();
    instances[3].x = 5.0f;
    for (TileMap::Instance &in : instances)
        in.w = in.h = 16;
    TileMap::GridIndex grid;
    grid.build(instances);
    QVERIFY(grid.cols >= 1 && grid.rows >= 1);
    QVERIFY(qint64(grid.cols) * grid.rows <= (qint64(1) << 22));
    std::vector<uint32_t> hits;
    grid.query(QRectF(0, -1, 10, 2), hits);
    QVERIFY(std::find(hits.begin(), hits.end(), 3u) != hits.end());
    QVERIFY(std::is_sorted(hits.begin(), hits.end()));
    grid.query(QRectF(std::numeric_limits<double>::quiet_NaN(), 0, 1, 1), hits);   // no crash
    QVERIFY(grid.countCandidates(QRectF(-1e300, -1e300, 2e300, 2e300)) <= instances.size());
}

void Test2dFormats::tileMapSniffers()
{
    QVERIFY(TileMapLoader::looksLikeTiledJson(R"({ "compressionlevel":-1, "height":10, "tiledversion":"1.10.2" })"));
    QVERIFY(TileMapLoader::looksLikeTiledJson(R"({"layers":[],"tilewidth":16})"));
    QVERIFY(!TileMapLoader::looksLikeTiledJson(R"({"name":"x","version":"1.0.0"})"));
    QVERIFY(TileMapLoader::looksLikeTiledXml("<?xml version=\"1.0\"?><map version=\"1.10\" orientation=\"orthogonal\" width=\"2\">"));
    QVERIFY(TileMapLoader::looksLikeTiledXml("<tileset version=\"1.10\" name=\"t\" tilewidth=\"16\">"));
    // Translation Memory eXchange files share the .tmx suffix.
    QVERIFY(!TileMapLoader::looksLikeTiledXml("<?xml version=\"1.0\"?><tmx version=\"1.4\"><header creationtool=\"x\"/>"));
    QVERIFY(!TileMapLoader::looksLikeTiledXml("<map name=\"imagemap\"><area/></map>"));
    QVERIFY(TileMapLoader::looksLikeLdtk(R"({"__header__":{"fileType":"LDtk Project JSON"},"jsonVersion":"1.5"})"));
    QVERIFY(!TileMapLoader::looksLikeLdtk(R"({"tiledversion":"1"})"));
    QVERIFY(TileMapLoader::looksLikeTileMap(R"({"tiledversion":"1.10"})"));
    QVERIFY(!TileMapLoader::looksLikeTileMap("plain text"));
}

void Test2dFormats::tileMapErrors()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(!loadMap(dir, QStringLiteral("a.tmj"), "{ broken").doc);
    QVERIFY(!loadMap(dir, QStringLiteral("a.tmj"), "{ broken").error.isEmpty());
    QVERIFY(!loadMap(dir, QStringLiteral("b.tmj"), R"({"hello":"world"})").doc);
    QVERIFY(!loadMap(dir, QStringLiteral("c.tmx"), "<notamap/>").doc);
    QVERIFY(!loadMap(dir, QStringLiteral("d.tmx"), "not xml at all").doc);
    QVERIFY(!loadMap(dir, QStringLiteral("e.ldtk"), R"({"__header__":{},"levels":[]})").doc);
    QVERIFY(!loadMap(dir, QStringLiteral("f.tmj"), QByteArray()).doc);
    QVERIFY(!TileMapLoader::load(dir.filePath(QStringLiteral("missing.tmj")), kNever).doc);
}

void Test2dFormats::tileMapCancel()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeTiles(dir);
    const QString path = writeFile(dir, QStringLiteral("c.tmj"),
        orthoMap(R"({"type":"tilelayer","name":"g","width":2,"height":2,"data":[1,2,1,2]})", tilesetJson()));
    const std::atomic_bool cancel{true};
    const auto r = TileMapLoader::load(path, cancel);
    QVERIFY(r.cancelled);
    QVERIFY(!r.doc);
}

// ---------------------------------------------------------------------------
// Crafted inputs
// ---------------------------------------------------------------------------

void Test2dFormats::tiledCraftedSheetNumbers()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeTiles(dir);
    // Negative spacing cancelling the tile width used to divide by zero; a
    // made-up tile count used to size per-tile caches.
    const QByteArray tileset =
        R"({"firstgid":1,"name":"t","tilewidth":16,"tileheight":16,"spacing":-16,"margin":-4,"columns":2000000000,)"
        R"("tilecount":2000000000,"image":"tiles.png"})";
    const auto r = loadMap(dir, QStringLiteral("n.tmj"),
        orthoMap(R"({"type":"tilelayer","name":"g","width":2,"height":1,"offsetx":1e300,"data":[1,2]},)"
                 R"({"type":"tilelayer","name":"h","width":2,"height":1,"data":[1,2]})", tileset, 2, 1));
    QVERIFY2(r.doc, qPrintable(r.error));
    const TileMap::Tileset &ts = r.doc->tilesets[0];
    QCOMPARE(ts.spacing, 0);
    QCOMPARE(ts.margin, 0);
    QCOMPARE(ts.columns, 2);          // from the image, like Tiled
    QCOMPARE(ts.tileCount, 2);        // what the 32 x 16 sheet holds
    // A layer pushed beyond any sane coordinate is dropped, not a broken view.
    QVERIFY(r.doc->layers[0].instances.empty());
    QCOMPARE(r.doc->layers[1].instances.size(), size_t(2));
    QVERIFY(std::isfinite(r.doc->bounds.width()) && r.doc->bounds.width() < 1e6);
}

void Test2dFormats::tiledHugeTileId()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeTiles(dir);
    // A collection tile id near INT_MAX must not size the image table.
    const QByteArray json =
        R"({"name":"c","tilewidth":16,"tileheight":16,"tilecount":2,"columns":0,"tiles":[)"
        R"({"id":2000000000,"image":"tiles.png"},{"id":1,"image":"tiles.png","x":16,"y":0,"width":16,"height":16}]})";
    const auto r = loadMap(dir, QStringLiteral("c.tsj"), json);
    QVERIFY2(r.doc, qPrintable(r.error));
    QCOMPARE(r.doc->tilesets[0].tiles.size(), size_t(2));
    QCOMPARE(r.doc->tilesets[0].tiles[1].size(), QSize(16, 16));
    QVERIFY(!r.doc->warnings.isEmpty());
    QCOMPARE(r.doc->layers[0].instances.size(), size_t(1));
}

void Test2dFormats::ldtkCraftedSheetNumbers()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeTiles(dir);
    // grid 1 + spacing -1 used to divide by zero.
    const QByteArray json =
        R"({"__header__":{},"jsonVersion":"1.5.3","defaultGridSize":16,"worldLayout":"Free","defs":{"layers":[],)"
        R"("tilesets":[{"uid":10,"identifier":"T","relPath":"tiles.png","tileGridSize":1,"spacing":-1,"padding":-7}],"entities":[]},)"
        R"("levels":[{"identifier":"L0","worldX":0,"worldY":0,"pxWid":32,"pxHei":16,"layerInstances":[)"
        R"({"__identifier":"Tiles","__type":"Tiles","__gridSize":1,"__tilesetDefUid":10,"__pxTotalOffsetX":0,"__pxTotalOffsetY":0,)"
        R"("gridTiles":[{"px":[0,0],"src":[3,2],"f":0},{"px":[1,0],"src":[-5,1e300],"f":0}]}]}]})";
    const auto r = loadMap(dir, QStringLiteral("p.ldtk"), json);
    QVERIFY2(r.doc, qPrintable(r.error));
    const auto &tiles = r.doc->layers[0].instances;
    QCOMPARE(tiles.size(), size_t(1));      // the out-of-sheet source is skipped
    QCOMPARE(tiles[0].tile, 2 * 32 + 3);
}

void Test2dFormats::tmxTooDeep()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    constexpr int kDepth = 200000;
    const QByteArray tmx = "<map orientation=\"orthogonal\" width=\"1\" height=\"1\" tilewidth=\"16\" tileheight=\"16\">"
                           + QByteArray("<group>").repeated(kDepth) + QByteArray("</group>").repeated(kDepth) + "</map>";
    const auto r = loadMap(dir, QStringLiteral("deep.tmx"), tmx);
    QVERIFY(!r.doc);
    QVERIFY(!r.error.isEmpty());

    // Within the limit, nested groups still load (and name their layers by path).
    const QByteArray ok = "<map orientation=\"orthogonal\" width=\"1\" height=\"1\" tilewidth=\"16\" tileheight=\"16\">"
                          "<group name=\"a\"><group name=\"b\"><objectgroup name=\"o\"/></group></group></map>";
    const auto shallow = loadMap(dir, QStringLiteral("ok.tmx"), ok);
    QVERIFY2(shallow.doc, qPrintable(shallow.error));
    QCOMPARE(shallow.doc->layers.size(), size_t(1));
    QCOMPARE(shallow.doc->layers[0].name, QStringLiteral("a/b/o"));
}

void Test2dFormats::networkPathsRefused()
{
    QVERIFY(AssetFileUtil::isNetworkPath(u"\\\\host\\share\\a.png"));
    QVERIFY(AssetFileUtil::isNetworkPath(u"//host/share/a.png"));
    QVERIFY(AssetFileUtil::isNetworkPath(u"\\/host/a.png"));
    QVERIFY(AssetFileUtil::isNetworkPath(u"\\\\?\\UNC\\host\\a.png"));
    QVERIFY(!AssetFileUtil::isNetworkPath(u"/usr/share/a.png"));
    QVERIFY(!AssetFileUtil::isNetworkPath(u"C:\\a.png"));
    QVERIFY(!AssetFileUtil::isNetworkPath(u"tiles/a.png"));
    QVERIFY(!AssetFileUtil::isNetworkPath(u""));

    // A tile map's tileset image on a share: refused with a warning, tiles
    // become placeholders.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto r = loadMap(dir, QStringLiteral("unc.tmj"),
        orthoMap(R"({"type":"tilelayer","name":"g","width":2,"height":2,"data":[1,2,1,2]})",
                 tilesetJson(QStringLiteral("//nohost.invalid/share/tiles.png"))));
    QVERIFY2(r.doc, qPrintable(r.error));
    QCOMPARE(r.doc->layers[0].instances[0].tileset, int16_t(-1));
    QVERIFY(r.doc->warnings.join(QLatin1Char('\n')).contains(QStringLiteral("network")));
    QVERIFY(!r.doc->warnings.join(QLatin1Char('\n')).contains(QStringLiteral("not found")));

    // An external tileset on a share is refused the same way.
    const auto ext = loadMap(dir, QStringLiteral("unc2.tmj"),
        orthoMap(R"({"type":"tilelayer","name":"g","width":1,"height":1,"data":[1]})",
                 R"({"firstgid":1,"source":"\\\\nohost.invalid\\share\\t.tsx"})", 1, 1));
    QVERIFY2(ext.doc, qPrintable(ext.error));
    QVERIFY(ext.doc->warnings.join(QLatin1Char('\n')).contains(QStringLiteral("network")));

    // An atlas page on a share is treated as missing.
    QString error;
    const auto atlas = AtlasLoader::parse(
        R"({"frames":[{"filename":"a","frame":{"x":0,"y":0,"w":8,"h":8}}],"meta":{"image":"\\\\nohost.invalid\\share\\a.png"}})",
        QStringLiteral("json"), dir.path(), &error);
    QVERIFY2(atlas, qPrintable(error));
    QVERIFY(atlas->pages[0].path.isEmpty());
}

QTEST_GUILESS_MAIN(Test2dFormats)
#include "test_2d_formats.moc"
