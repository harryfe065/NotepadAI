// FontFile (add-media-preview): sfnt / TTC / WOFF containers and the tables the
// font preview shows. Fonts are synthesised here with a minimal sfnt writer,
// so the expected values are the ones written, not ones recomputed.

#include "FontFile.h"
#include "UnicodeBlocks.h"

#include <QtEndian>
#include <QtTest>

#include <map>

namespace {

void be16(QByteArray &b, quint16 v) { char c[2]; qToBigEndian(v, c); b.append(c, 2); }
void be32(QByteArray &b, quint32 v) { char c[4]; qToBigEndian(v, c); b.append(c, 4); }

QByteArray utf16be(const QString &s)
{
    QByteArray out;
    for (QChar c : s)
        be16(out, c.unicode());
    return out;
}

QByteArray nameTable(const std::map<int, QString> &names)
{
    QByteArray records, strings;
    for (const auto &[id, text] : names) {
        const QByteArray encoded = utf16be(text);
        be16(records, 3);       // Windows
        be16(records, 1);       // Unicode BMP
        be16(records, 0x409);   // English (US)
        be16(records, quint16(id));
        be16(records, quint16(encoded.size()));
        be16(records, quint16(strings.size()));
        strings += encoded;
    }
    // A Mac Roman family name that must lose to the Windows one.
    be16(records, 1);
    be16(records, 0);
    be16(records, 0);
    be16(records, 1);
    be16(records, 3);
    be16(records, quint16(strings.size()));
    strings += "Mac";
    QByteArray t;
    be16(t, 0);
    be16(t, quint16(names.size() + 1));
    be16(t, quint16(6 + records.size()));
    return t + records + strings;
}

QByteArray maxpTable(quint16 glyphs)
{
    QByteArray t;
    be32(t, 0x00005000);
    be16(t, glyphs);
    return t;
}

QByteArray headTable(quint16 unitsPerEm)
{
    QByteArray t(54, '\0');
    qToBigEndian<quint32>(0x5F0F3CF5, t.data() + 12);
    qToBigEndian<quint16>(unitsPerEm, t.data() + 18);
    return t;
}

QByteArray os2Table(quint16 weight)
{
    QByteArray t(78, '\0');
    qToBigEndian<quint16>(weight, t.data() + 4);
    return t;
}

// Format 4: 'A'..'C' -> 1..3 through idDelta, 'x'..'y' -> 7, 9 through the
// glyph array (idRangeOffset), then the 0xFFFF terminator segment.
QByteArray cmapFormat4()
{
    QByteArray s;
    const int segCount = 3;
    be16(s, 4);
    be16(s, 0);                         // length (patched)
    be16(s, 0);
    be16(s, segCount * 2);
    be16(s, 4); be16(s, 1); be16(s, 2); // search fields (unused)
    be16(s, 'C'); be16(s, 'y'); be16(s, 0xFFFF);        // end codes
    be16(s, 0);                                         // reserved pad
    be16(s, 'A'); be16(s, 'x'); be16(s, 0xFFFF);        // start codes
    be16(s, quint16(1 - 'A')); be16(s, 0); be16(s, 1);  // deltas
    // idRangeOffset: segment 1 points at the glyph array right after the
    // offsets: from its own slot that is 2 slots (4 bytes) further on.
    be16(s, 0); be16(s, 4); be16(s, 0);
    be16(s, 7); be16(s, 9);                             // glyph array for 'x', 'y'
    qToBigEndian<quint16>(quint16(s.size()), s.data() + 2);
    return s;
}

QByteArray cmapFormat12(quint32 first, quint32 last, quint32 glyph)
{
    QByteArray s;
    be16(s, 12);
    be16(s, 0);
    be32(s, 16 + 12);
    be32(s, 0);
    be32(s, 1);
    be32(s, first); be32(s, last); be32(s, glyph);
    return s;
}

QByteArray cmapTable(const QList<std::tuple<int, int, QByteArray>> &subtables)
{
    QByteArray t;
    be16(t, 0);
    be16(t, quint16(subtables.size()));
    quint32 offset = 4 + quint32(subtables.size()) * 8;
    QByteArray bodies;
    for (const auto &[platform, encoding, body] : subtables) {
        be16(t, quint16(platform));
        be16(t, quint16(encoding));
        be32(t, offset + quint32(bodies.size()));
        bodies += body;
    }
    return t + bodies;
}

QByteArray fvarTable()
{
    QByteArray t;
    be16(t, 1); be16(t, 0);
    be16(t, 16);                // axes array offset
    be16(t, 2);                 // reserved
    be16(t, 1);                 // axis count
    be16(t, 20);                // axis size
    be16(t, 0); be16(t, 0);     // instances
    t += "wght";
    be32(t, 100u << 16);
    be32(t, 400u << 16);
    be32(t, 900u << 16);
    be16(t, 0);
    be16(t, 256);
    return t;
}

using Tables = std::map<QByteArray, QByteArray>;   // sorted by tag, as sfnt wants

// Table directory; the table bodies are appended to *bodies, which the file
// holds at offset `bodiesAt`, 4-byte aligned.
QByteArray directory(const Tables &tables, quint32 version, quint32 bodiesAt, QByteArray *bodies)
{
    QByteArray dir;
    be32(dir, version);
    be16(dir, quint16(tables.size()));
    be16(dir, 0); be16(dir, 0); be16(dir, 0);
    quint32 offset = bodiesAt + quint32(bodies->size());
    for (const auto &[tag, body] : tables) {
        dir += tag;
        be32(dir, 0);
        be32(dir, offset);
        be32(dir, quint32(body.size()));
        QByteArray padded = body;
        while (padded.size() % 4)
            padded.append('\0');
        *bodies += padded;
        offset += quint32(padded.size());
    }
    return dir;
}

QByteArray sfnt(const Tables &tables, quint32 version = 0x00010000)
{
    QByteArray bodies;
    const QByteArray dir = directory(tables, version, 12 + quint32(tables.size()) * 16, &bodies);
    return dir + bodies;
}

Tables basicTables(const QString &family, quint16 weight)
{
    return {{"name", nameTable({{1, family}, {2, QStringLiteral("Regular")}, {5, QStringLiteral("Version 1.002")},
                                {9, QStringLiteral("A. Designer")}})},
            {"maxp", maxpTable(12)},
            {"head", headTable(2048)},
            {"OS/2", os2Table(weight)},
            {"cmap", cmapTable({{3, 1, cmapFormat4()}})},
            {"glyf", QByteArray(4, '\0')}};
}

} // namespace

class TestFontFile : public QObject
{
    Q_OBJECT

private slots:
    void singleSfntTables()
    {
        Tables tables = basicTables(QStringLiteral("Test Sans"), 700);
        tables["fvar"] = fvarTable();
        tables["GSUB"] = QByteArray(10, '\0');
        const QByteArray bytes = sfnt(tables);
        QVERIFY(FontFile::looksLikeFont(bytes));

        const FontFile::Document doc = FontFile::parse(bytes);
        QVERIFY(doc.isValid());
        QVERIFY(doc.container == FontFile::Container::Sfnt);
        QCOMPARE(doc.faces.size(), 1);
        const FontFile::Face &f = doc.faces.first();
        QCOMPARE(f.family, QStringLiteral("Test Sans"));
        QCOMPARE(f.subfamily, QStringLiteral("Regular"));
        QCOMPARE(f.version, QStringLiteral("Version 1.002"));
        QCOMPARE(f.designer, QStringLiteral("A. Designer"));
        QCOMPARE(f.numGlyphs, 12);
        QCOMPARE(f.unitsPerEm, 2048);
        QCOMPARE(f.weightClass, 700);
        QCOMPARE(f.outlines, QStringLiteral("TrueType"));
        QVERIFY(f.hasShaping);
        QCOMPARE(f.axes.size(), 1);
        QCOMPARE(f.axes[0].tag, QStringLiteral("wght"));
        QCOMPARE(f.axes[0].min, 100.0);
        QCOMPARE(f.axes[0].def, 400.0);
        QCOMPARE(f.axes[0].max, 900.0);
        // The single sfnt is handed to QRawFont as is.
        QVERIFY(FontFile::faceData(doc, 0).constData() == bytes.constData());
    }

    void cmapFormat4DeltasAndGlyphArray()
    {
        const FontFile::Document doc = FontFile::parse(sfnt(basicTables(QStringLiteral("F"), 400)));
        QVERIFY(doc.isValid());
        const auto &cmap = doc.faces.first().cmap;
        const std::vector<std::pair<char32_t, quint32>> expected = {
            {U'A', 1}, {U'B', 2}, {U'C', 3}, {U'x', 7}, {U'y', 9}};
        QVERIFY(cmap == expected);
    }

    void cmapPrefersFullRepertoire()
    {
        Tables tables = basicTables(QStringLiteral("Emoji"), 400);
        tables["cmap"] = cmapTable({{3, 1, cmapFormat4()}, {3, 10, cmapFormat12(0x1F600, 0x1F602, 40)}});
        const FontFile::Document doc = FontFile::parse(sfnt(tables));
        const auto &cmap = doc.faces.first().cmap;
        QCOMPARE(cmap.size(), size_t(3));
        QCOMPARE(cmap[0].first, char32_t(0x1F600));
        QCOMPARE(cmap[2].second, quint32(42));
    }

    void cffOutlines()
    {
        Tables tables = basicTables(QStringLiteral("C"), 400);
        tables.erase("glyf");
        tables["CFF "] = QByteArray(8, '\0');
        const FontFile::Document doc = FontFile::parse(sfnt(tables, FontFile::tag("OTTO")));
        QVERIFY(doc.isValid());
        QCOMPARE(doc.faces.first().outlines, QStringLiteral("CFF"));
    }

    void collectionFacesBecomeStandalone()
    {
        // Two faces sharing one 'glyf' table, as collections do.
        Tables a = basicTables(QStringLiteral("Family A"), 400);
        Tables b = basicTables(QStringLiteral("Family B"), 700);
        const quint32 header = 12 + 2 * 4;
        // Lay out both directories first, then every table body.
        const quint32 dirA = header, dirB = dirA + 12 + quint32(a.size()) * 16;
        const quint32 bodyStart = dirB + 12 + quint32(b.size()) * 16;
        QByteArray bodiesA, bodiesB;
        const QByteArray directoryA = directory(a, 0x00010000, bodyStart, &bodiesA);
        const QByteArray directoryB = directory(b, 0x00010000, bodyStart + quint32(bodiesA.size()), &bodiesB);
        QByteArray ttc("ttcf");
        be32(ttc, 0x00010000);
        be32(ttc, 2);
        be32(ttc, dirA);
        be32(ttc, dirB);
        ttc += directoryA + directoryB + bodiesA + bodiesB;
        QVERIFY(FontFile::looksLikeFont(ttc));

        const FontFile::Document doc = FontFile::parse(ttc);
        QVERIFY(doc.container == FontFile::Container::Collection);
        QCOMPARE(doc.faces.size(), 2);
        QCOMPARE(doc.faces[0].family, QStringLiteral("Family A"));
        QCOMPARE(doc.faces[1].family, QStringLiteral("Family B"));
        QCOMPARE(doc.faces[1].weightClass, 700);

        // The rebuilt face parses as a plain font with the same contents.
        const QByteArray face = FontFile::faceData(doc, 1);
        QVERIFY(face.size() < ttc.size());
        const FontFile::Document single = FontFile::parse(face);
        QVERIFY(single.container == FontFile::Container::Sfnt);
        QCOMPARE(single.faces.first().family, QStringLiteral("Family B"));
        QVERIFY(single.faces.first().cmap == doc.faces[1].cmap);
        QVERIFY(FontFile::faceData(doc, 2).isEmpty());
    }

    void woffInflatesToSfnt()
    {
        const Tables tables = basicTables(QStringLiteral("Web Font"), 300);
        QByteArray dir, data;
        const quint32 dataStart = 44 + quint32(tables.size()) * 20;
        bool compressedOne = false;
        for (const auto &[tag, body] : tables) {
            QByteArray stored = body;
            // zlib-compress the tables that shrink (qCompress adds a 4-byte size prefix).
            const QByteArray z = qCompress(body).mid(4);
            if (z.size() < body.size()) {
                stored = z;
                compressedOne = true;
            }
            dir += tag;
            be32(dir, dataStart + quint32(data.size()));
            be32(dir, quint32(stored.size()));
            be32(dir, quint32(body.size()));
            be32(dir, 0);
            data += stored;
            while (data.size() % 4)
                data.append('\0');
        }
        QVERIFY(compressedOne);
        QByteArray woff("wOFF");
        be32(woff, 0x00010000);
        be32(woff, quint32(44 + dir.size() + data.size()));
        be16(woff, quint16(tables.size()));
        be16(woff, 0);
        woff += QByteArray(44 - woff.size(), '\0');
        woff += dir + data;
        QVERIFY(FontFile::looksLikeFont(woff));

        const FontFile::Document doc = FontFile::parse(woff);
        QVERIFY(doc.isValid());
        QVERIFY(doc.container == FontFile::Container::Woff);
        QCOMPARE(doc.faces.first().family, QStringLiteral("Web Font"));
        QCOMPARE(doc.faces.first().weightClass, 300);
        QCOMPARE(doc.faces.first().cmap.size(), size_t(5));
        QVERIFY(FontFile::looksLikeFont(FontFile::faceData(doc, 0)));
    }

    void rejectsWhatItCannotRead()
    {
        QVERIFY(!FontFile::parse(QByteArray("not a font at all")).isValid());
        QVERIFY(!FontFile::parse(QByteArray()).isValid());

        QByteArray woff2("wOF2");
        woff2 += QByteArray(60, '\0');
        const FontFile::Document w2 = FontFile::parse(woff2);
        QVERIFY(!w2.isValid());
        QVERIFY(w2.container == FontFile::Container::Woff2);
        QVERIFY(!w2.error.isEmpty());
        QVERIFY(!FontFile::looksLikeFont(woff2));

        // A directory entry pointing past the end drops that table only.
        QByteArray bytes = sfnt(basicTables(QStringLiteral("Cut"), 400));
        const FontFile::Document whole = FontFile::parse(bytes);
        bytes.chop(8);                           // clips the last table ('name')
        const FontFile::Document cut = FontFile::parse(bytes);
        QVERIFY(cut.isValid());
        QCOMPARE(cut.faces.first().numGlyphs, whole.faces.first().numGlyphs);
        QVERIFY(cut.faces.first().family.isEmpty());

        // Bare 00 01 00 00 followed by text is not a font.
        QVERIFY(!FontFile::looksLikeFont(QByteArray("\x00\x01\x00\x00hello there", 15)));
    }

    void corruptCmapStaysInBounds()
    {
        Tables tables = basicTables(QStringLiteral("Bad"), 400);
        QByteArray bad = cmapFormat4();
        qToBigEndian<quint16>(0xFFFE, bad.data() + 6);     // segCountX2 far past the table
        tables["cmap"] = cmapTable({{3, 1, bad}});
        const FontFile::Document doc = FontFile::parse(sfnt(tables));
        QVERIFY(doc.isValid());
        QVERIFY(doc.faces.first().cmap.empty());
    }

    void unicodeBlocksSortedAndFound()
    {
        const auto &blocks = UnicodeBlocks::all();
        for (size_t i = 1; i < blocks.size(); ++i)
            QVERIFY2(blocks[i - 1].last < blocks[i].first, blocks[i].name);
        QCOMPARE(QString::fromLatin1(blocks[size_t(UnicodeBlocks::indexOf('A'))].name), QStringLiteral("Basic Latin"));
        QCOMPARE(QString::fromLatin1(blocks[size_t(UnicodeBlocks::indexOf(0x1F600))].name), QStringLiteral("Emoticons"));
        QCOMPARE(QString::fromLatin1(blocks[size_t(UnicodeBlocks::indexOf(0x1F320))].name),
                 QStringLiteral("Miscellaneous Symbols and Pictographs"));
        QCOMPARE(UnicodeBlocks::indexOf(0x0010), -1);
        QCOMPARE(UnicodeBlocks::indexOf(0x0800), -1);    // between Arabic and Devanagari
        QCOMPARE(UnicodeBlocks::indexOf(0x10FFFF), -1);
    }
};

QTEST_GUILESS_MAIN(TestFontFile)
#include "test_font_file.moc"
