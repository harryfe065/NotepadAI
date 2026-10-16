// Hex view (add-hex-preview): the page cache, offset / pattern parsing, copy
// formats, the data inspector and the chunked search. Bytes come from memory
// through the same reader seam the file reader plugs into.

#include "HexFormat.h"
#include "HexSearch.h"
#include "HexSource.h"

#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <cstring>

namespace {

// A reader over `data` that counts its calls.
HexSource::Reader memoryReader(const QByteArray &data, int *calls)
{
    return [data, calls](qint64 offset, char *out, qint64 len) -> qint64 {
        if (calls)
            ++*calls;
        if (offset < 0 || offset >= data.size())
            return 0;
        const qint64 n = std::min<qint64>(len, data.size() - offset);
        std::memcpy(out, data.constData() + offset, size_t(n));
        return n;
    };
}

qint64 offset(const QString &text, qint64 cursor)
{
    return HexFormat::parseOffset(text, cursor).value_or(-1);
}

QByteArray pattern(int size)
{
    QByteArray b(size, '\0');
    for (int i = 0; i < size; ++i)
        b[i] = char((i * 7 + 3) & 0xFF);
    return b;
}

} // namespace

class TestHexView : public QObject
{
    Q_OBJECT

private slots:
    // ---- HexSource ----

    void readsAcrossPagesWithOneCall()
    {
        const QByteArray data = pattern(int(HexSource::kPageBytes * 3));
        int calls = 0;
        HexSource source;
        source.reset(memoryReader(data, &calls), data.size());
        QByteArray out(100, '\0');
        // Straddles the border of pages 0 and 1: both missing, one contiguous read.
        QCOMPARE(source.read(HexSource::kPageBytes - 50, out.data(), 100), qint64(100));
        QCOMPARE(out, data.mid(int(HexSource::kPageBytes - 50), 100));
        QCOMPARE(calls, 1);
        // Served from the cache.
        QCOMPARE(source.read(10, out.data(), 100), qint64(100));
        QCOMPARE(out, data.mid(10, 100));
        QCOMPARE(calls, 1);
    }

    void readClipsAtEnd()
    {
        const QByteArray data = pattern(1000);
        HexSource source;
        source.reset(memoryReader(data, nullptr), data.size());
        QByteArray out(64, 'x');
        QCOMPARE(source.read(990, out.data(), 64), qint64(10));
        QCOMPARE(out.left(10), data.right(10));
        QCOMPARE(source.read(1000, out.data(), 64), qint64(0));
        QCOMPARE(source.read(-5, out.data(), 64), qint64(0));
    }

    void cacheEvictsLeastRecentlyUsed()
    {
        const QByteArray data = pattern(int(HexSource::kPageBytes * (HexSource::kMaxPages + 2)));
        int calls = 0;
        HexSource source;
        source.reset(memoryReader(data, &calls), data.size());
        char byte = 0;
        for (int page = 0; page < HexSource::kMaxPages; ++page)
            source.read(page * HexSource::kPageBytes, &byte, 1);
        QCOMPARE(calls, HexSource::kMaxPages);
        source.read(0, &byte, 1);                                         // page 0 is now recent
        source.read(HexSource::kMaxPages * HexSource::kPageBytes, &byte, 1);  // evicts page 1
        QCOMPARE(calls, HexSource::kMaxPages + 1);
        source.read(0, &byte, 1);
        QCOMPARE(calls, HexSource::kMaxPages + 1);
        source.read(HexSource::kPageBytes, &byte, 1);
        QCOMPARE(calls, HexSource::kMaxPages + 2);
        QCOMPARE(byte, data[int(HexSource::kPageBytes)]);
    }

    void resetDropsTheCache()
    {
        int calls = 0;
        HexSource source;
        source.reset(memoryReader(QByteArray("abcdef"), &calls), 6);
        char c = 0;
        source.read(0, &c, 1);
        source.reset(memoryReader(QByteArray("xyz"), &calls), 3);
        source.read(0, &c, 1);
        QCOMPARE(c, 'x');
        QCOMPARE(calls, 2);
        QCOMPARE(source.size(), qint64(3));
    }

    void fileReaderKeepsNoHandle()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("blob.bin"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("0123456789");
        }
        const HexSource::Reader read = HexSource::fileReader(path);
        char out[4];
        QCOMPARE(read(3, out, 4), qint64(4));
        QCOMPARE(QByteArray(out, 4), QByteArray("3456"));
        // No handle survives the read: the file can be deleted (Windows refuses
        // while one is open) and a later read fails cleanly.
        QVERIFY(QFile::remove(path));
        QCOMPARE(read(0, out, 4), qint64(-1));
    }

    // ---- HexFormat ----

    void offsetDigitsGrowPast4G()
    {
        QCOMPARE(HexFormat::offsetDigits(0), 8);
        QCOMPARE(HexFormat::offsetDigits(0xFFFFFFFFLL + 1), 8);
        QCOMPARE(HexFormat::offsetDigits(0xFFFFFFFFLL + 2), 9);
        QCOMPARE(HexFormat::offsetDigits(qint64(1) << 40), 10);
    }

    void parsesOffsets()
    {
        QCOMPARE(offset(QStringLiteral("0x1000"), 0), qint64(4096));
        QCOMPARE(offset(QStringLiteral("4096"), 0), qint64(4096));
        QCOMPARE(offset(QStringLiteral("1f"), 0), qint64(31));
        QCOMPARE(offset(QStringLiteral(" +16 "), 100), qint64(116));
        QCOMPARE(offset(QStringLiteral("-0x10"), 100), qint64(84));
        QCOMPARE(offset(QStringLiteral("-200"), 100), qint64(0));
        QCOMPARE(offset(QStringLiteral("zz"), 0), qint64(-1));
        QCOMPARE(offset(QString(), 0), qint64(-1));
    }

    void parsesPatterns()
    {
        QByteArray out;
        QString error;
        QVERIFY(HexFormat::parsePattern(QStringLiteral("50 4B 03 04"), true, &out, &error));
        QCOMPARE(out, QByteArray("PK\x03\x04", 4));
        QVERIFY(HexFormat::parsePattern(QStringLiteral("0x50,0x4b \\x03 04"), true, &out, &error));
        QCOMPARE(out, QByteArray("PK\x03\x04", 4));
        QVERIFY(!HexFormat::parsePattern(QStringLiteral("50 4"), true, &out, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!HexFormat::parsePattern(QStringLiteral("5G"), true, &out, &error));
        QVERIFY(!HexFormat::parsePattern(QString(), true, &out, &error));
        QVERIFY(HexFormat::parsePattern(QStringLiteral("héllo"), false, &out, &error));
        QCOMPARE(out, QStringLiteral("héllo").toUtf8());
    }

    void copyFormats()
    {
        const QByteArray bytes("MZ\x90\x00\tA\x7F", 7);
        QCOMPARE(HexFormat::toHex(bytes), QStringLiteral("4D 5A 90 00 09 41 7F"));
        QCOMPARE(HexFormat::toText(bytes), QStringLiteral("MZ..\tA."));
        QCOMPARE(HexFormat::toCArray(QByteArray("\x01\x02", 2)), QStringLiteral("0x01, 0x02"));
        const QString longArray = HexFormat::toCArray(QByteArray(17, '\xAB'));
        QCOMPARE(longArray.count(QLatin1Char('\n')), 1);
    }

    void byteClasses()
    {
        QVERIFY(HexFormat::classify(0x00) == HexFormat::ByteClass::Null);
        QVERIFY(HexFormat::classify('A') == HexFormat::ByteClass::Printable);
        QVERIFY(HexFormat::classify(' ') == HexFormat::ByteClass::Printable);
        QVERIFY(HexFormat::classify(0x0A) == HexFormat::ByteClass::Control);
        QVERIFY(HexFormat::classify(0x7F) == HexFormat::ByteClass::Control);
        QVERIFY(HexFormat::classify(0x80) == HexFormat::ByteClass::High);
    }

    void rowText()
    {
        const QByteArray row("AB\x00\xFF", 4);
        // Hex column: two digits per byte, one space between bytes, an extra
        // space every 8 bytes; bytes past the end are blank.
        QCOMPARE(HexFormat::hexColumn(row, 8), QStringLiteral("41 42 00 FF            "));
        QCOMPARE(HexFormat::asciiColumn(row, 8), QStringLiteral("AB..    "));
        QCOMPARE(HexFormat::hexColumnWidth(16), 16 * 3 - 1 + 1);
    }

    void inspector()
    {
        const QByteArray one("\x00\x00\x80\x3F", 4);
        const QString text = HexFormat::inspector(one);
        QVERIFY2(text.contains(QStringLiteral("f32 1")), qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("u32 1065353216")), qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("u16 0")), qPrintable(text));
        QVERIFY2(!text.contains(QStringLiteral("u64")), qPrintable(text));   // only 4 bytes left
        QCOMPARE(HexFormat::inspector(QByteArray("\xFF", 1)), QStringLiteral("u8 255  ·  i8 -1"));
        QCOMPARE(HexFormat::inspector(QByteArray()), QString());
    }

    // ---- HexSearch ----

    void findsForwardAcrossChunkBorders()
    {
        QByteArray data(100, 'x');
        data.replace(5, 4, "PK\x03\x04");
        data.replace(50, 4, "PK\x03\x04");
        std::atomic_bool cancel{false};
        const auto read = memoryReader(data, nullptr);
        const QByteArray pk("PK\x03\x04", 4);
        // A chunk of 7 bytes puts both matches across chunk borders.
        QCOMPARE(HexSearch::find(read, data.size(), pk, 0, data.size(), false, cancel, 7), qint64(5));
        QCOMPARE(HexSearch::find(read, data.size(), pk, 6, data.size(), false, cancel, 7), qint64(50));
        QCOMPARE(HexSearch::find(read, data.size(), pk, 51, data.size(), false, cancel, 7), qint64(-1));
        // A match must start before `to`, but may end past it.
        QCOMPARE(HexSearch::find(read, data.size(), pk, 0, 6, false, cancel, 7), qint64(5));
        QCOMPARE(HexSearch::find(read, data.size(), pk, 0, 5, false, cancel, 7), qint64(-1));
    }

    void findsBackward()
    {
        QByteArray data(100, 'x');
        data.replace(5, 3, "abc");
        data.replace(60, 3, "abc");
        std::atomic_bool cancel{false};
        const auto read = memoryReader(data, nullptr);
        QCOMPARE(HexSearch::find(read, data.size(), "abc", 0, data.size(), true, cancel, 7), qint64(60));
        QCOMPARE(HexSearch::find(read, data.size(), "abc", 0, 60, true, cancel, 7), qint64(5));
        QCOMPARE(HexSearch::find(read, data.size(), "abc", 0, 5, true, cancel, 7), qint64(-1));
        QCOMPARE(HexSearch::find(read, data.size(), "abc", 0, 61, true, cancel, 4), qint64(60));
    }

    void findHonoursCancel()
    {
        const QByteArray data(1000, 'x');
        std::atomic_bool cancel{true};
        QCOMPARE(HexSearch::find(memoryReader(data, nullptr), data.size(), "y", 0, data.size(), false, cancel, 16),
                 qint64(HexSearch::kCancelled));
    }

    void findEmptyPatternNeverMatches()
    {
        std::atomic_bool cancel{false};
        QCOMPARE(HexSearch::find(memoryReader("abc", nullptr), 3, QByteArray(), 0, 3, false, cancel), qint64(-1));
    }
};

QTEST_GUILESS_MAIN(TestHexView)
#include "test_hex_view.moc"
