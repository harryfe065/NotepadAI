#include "FontFile.h"

#include <QtEndian>

#include <algorithm>
#include <cstring>

namespace FontFile {

namespace {

constexpr quint32 kTrueType = 0x00010000;
constexpr qsizetype kMaxCmapEntries = 0x110000;

// Bounds-checked big-endian reader over one byte range.
class Reader
{
public:
    Reader(QByteArrayView data, quint32 offset, quint32 length)
    {
        if (quint64(offset) + length <= quint64(data.size()))
            m_data = data.sliced(offset, length);
    }
    explicit Reader(QByteArrayView data) : m_data(data) {}

    bool valid() const { return !m_data.isEmpty(); }
    qsizetype size() const { return m_data.size(); }
    bool has(qsizetype offset, qsizetype n) const { return offset >= 0 && n >= 0 && offset + n <= m_data.size(); }

    quint8 u8(qsizetype o) const { return has(o, 1) ? quint8(m_data[o]) : 0; }
    quint16 u16(qsizetype o) const { return has(o, 2) ? qFromBigEndian<quint16>(m_data.data() + o) : 0; }
    qint16 s16(qsizetype o) const { return qint16(u16(o)); }
    quint32 u32(qsizetype o) const { return has(o, 4) ? qFromBigEndian<quint32>(m_data.data() + o) : 0; }
    double fixed(qsizetype o) const { return double(qint32(u32(o))) / 65536.0; }
    QByteArrayView bytes(qsizetype o, qsizetype n) const { return has(o, n) ? m_data.sliced(o, n) : QByteArrayView(); }

private:
    QByteArrayView m_data;
};

QString tagText(quint32 t)
{
    const char c[4] = {char(t >> 24), char(t >> 16), char(t >> 8), char(t)};
    return QString::fromLatin1(c, 4).trimmed();
}

// Table directory of an sfnt starting at `offset` in `data`.
bool readDirectory(QByteArrayView data, quint32 offset, QVector<Table> *tables)
{
    const Reader r(data);
    if (!r.has(offset, 12))
        return false;
    const int count = r.u16(offset + 4);
    if (count == 0 || !r.has(offset + 12, qsizetype(count) * 16))
        return false;
    tables->reserve(count);
    for (int i = 0; i < count; ++i) {
        const qsizetype e = offset + 12 + qsizetype(i) * 16;
        Table t{r.u32(e), r.u32(e + 8), r.u32(e + 12)};
        // A table outside the file is dropped; the others still count.
        if (quint64(t.offset) + t.length <= quint64(data.size()))
            tables->append(t);
    }
    return !tables->isEmpty();
}

// ---- name ----------------------------------------------------------------

QString decodeName(const Reader &r, qsizetype offset, int length, int platform)
{
    const QByteArrayView b = r.bytes(offset, length);
    if (b.isEmpty())
        return {};
    if (platform == 0 || platform == 3) {
        QString s(b.size() / 2, Qt::Uninitialized);
        for (qsizetype i = 0; i < s.size(); ++i)
            s[i] = QChar(qFromBigEndian<quint16>(b.data() + i * 2));
        return s;
    }
    return QString::fromLatin1(b);   // Mac Roman: ASCII part matches
}

void readNames(const Reader &r, Face &face)
{
    if (!r.valid())
        return;
    const int count = r.u16(2);
    const qsizetype strings = r.u16(4);
    // Best record per name id: Windows English, any Windows, Unicode, Mac.
    struct Pick { int score = -1; QString text; };
    Pick picks[18];
    for (int i = 0; i < count; ++i) {
        const qsizetype rec = 6 + qsizetype(i) * 12;
        if (!r.has(rec, 12))
            break;
        const int platform = r.u16(rec), encoding = r.u16(rec + 2), language = r.u16(rec + 4);
        const int id = r.u16(rec + 6);
        if (id >= 18)
            continue;
        int score = -1;
        if (platform == 3 && (encoding == 1 || encoding == 10 || encoding == 0))
            score = language == 0x409 ? 4 : 3;
        else if (platform == 0)
            score = 2;
        else if (platform == 1 && encoding == 0)
            score = language == 0 ? 1 : 0;
        if (score <= picks[id].score)
            continue;
        const QString text = decodeName(r, strings + r.u16(rec + 10), r.u16(rec + 8), platform).trimmed();
        if (!text.isEmpty())
            picks[id] = {score, text};
    }
    face.copyright = picks[0].text;
    face.family = !picks[16].text.isEmpty() ? picks[16].text : picks[1].text;
    face.subfamily = !picks[17].text.isEmpty() ? picks[17].text : picks[2].text;
    face.fullName = picks[4].text;
    face.version = picks[5].text;
    face.postScriptName = picks[6].text;
    face.designer = picks[9].text;
    face.license = picks[13].text;
}

// ---- cmap ----------------------------------------------------------------

using CmapVector = std::vector<std::pair<char32_t, quint32>>;

void cmapFormat4(const Reader &r, qsizetype base, CmapVector &out)
{
    const int segCount = r.u16(base + 6) / 2;
    const qsizetype ends = base + 14, starts = ends + qsizetype(segCount) * 2 + 2;
    const qsizetype deltas = starts + qsizetype(segCount) * 2, ranges = deltas + qsizetype(segCount) * 2;
    if (!r.has(ranges, qsizetype(segCount) * 2))
        return;
    for (int s = 0; s < segCount; ++s) {
        const quint32 end = r.u16(ends + s * 2), start = r.u16(starts + s * 2);
        const quint16 delta = r.u16(deltas + s * 2);
        const quint16 rangeOffset = r.u16(ranges + s * 2);
        if (start > end)
            continue;
        // Overlapping segments could repeat the BMP thousands of times.
        if (qsizetype(out.size()) + qsizetype(end - start) + 1 > kMaxCmapEntries)
            return;
        for (quint32 c = start; c <= end && c != 0xFFFF; ++c) {
            quint32 glyph = 0;
            if (rangeOffset == 0) {
                glyph = (c + delta) & 0xFFFF;
            } else {
                const qsizetype at = ranges + s * 2 + rangeOffset + qsizetype(c - start) * 2;
                if (!r.has(at, 2))
                    break;
                glyph = r.u16(at);
                if (glyph != 0)
                    glyph = (glyph + delta) & 0xFFFF;
            }
            if (glyph != 0)
                out.emplace_back(char32_t(c), glyph);
        }
    }
}

void cmapFormat12(const Reader &r, qsizetype base, CmapVector &out)
{
    const quint32 groups = r.u32(base + 12);
    if (!r.has(base + 16, qsizetype(groups) * 12))
        return;
    for (quint32 g = 0; g < groups; ++g) {
        const qsizetype e = base + 16 + qsizetype(g) * 12;
        const quint32 first = r.u32(e), last = std::min<quint32>(r.u32(e + 4), 0x10FFFF);
        const quint32 glyph = r.u32(e + 8);
        if (first > last)
            continue;
        if (qsizetype(out.size()) + qsizetype(last - first) + 1 > kMaxCmapEntries)
            return;
        for (quint32 c = first; c <= last; ++c) {
            if (glyph + (c - first) != 0)
                out.emplace_back(char32_t(c), glyph + (c - first));
        }
    }
}

void cmapFormat0or6(const Reader &r, qsizetype base, int format, CmapVector &out)
{
    if (format == 0) {
        for (int c = 0; c < 256; ++c) {
            const quint32 glyph = r.u8(base + 6 + c);
            if (glyph != 0 && r.has(base + 6 + c, 1))
                out.emplace_back(char32_t(c), glyph);
        }
        return;
    }
    const quint32 first = r.u16(base + 6);
    const int count = r.u16(base + 8);
    for (int i = 0; i < count && r.has(base + 10 + i * 2, 2); ++i) {
        const quint32 glyph = r.u16(base + 10 + i * 2);
        if (glyph != 0)
            out.emplace_back(char32_t(first + quint32(i)), glyph);
    }
}

void readCmap(const Reader &r, Face &face)
{
    if (!r.valid())
        return;
    const int count = r.u16(2);
    // Best Unicode subtable: full repertoire first, then BMP, then symbol.
    int bestScore = -1;
    qsizetype best = -1;
    for (int i = 0; i < count; ++i) {
        const qsizetype rec = 4 + qsizetype(i) * 8;
        if (!r.has(rec, 8))
            break;
        const int platform = r.u16(rec), encoding = r.u16(rec + 2);
        const qsizetype offset = r.u32(rec + 4);
        if (!r.has(offset, 6))
            continue;
        const int format = r.u16(offset);
        if (format != 0 && format != 4 && format != 6 && format != 12)
            continue;
        int score = -1;
        if (format == 12 && ((platform == 3 && encoding == 10) || platform == 0))
            score = 5;
        else if (platform == 3 && encoding == 1)
            score = 4;
        else if (platform == 0)
            score = 3;
        else if (platform == 3 && encoding == 0)
            score = 2;      // symbol fonts: their codes sit in the private use area
        if (score > bestScore) {
            bestScore = score;
            best = offset;
        }
    }
    if (best < 0)
        return;
    CmapVector out;
    switch (r.u16(best)) {
    case 0: cmapFormat0or6(r, best, 0, out); break;
    case 4: cmapFormat4(r, best, out); break;
    case 6: cmapFormat0or6(r, best, 6, out); break;
    case 12: cmapFormat12(r, best, out); break;
    default: break;
    }
    if (!std::is_sorted(out.begin(), out.end()))
        std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end(),
                          [](const auto &a, const auto &b) { return a.first == b.first; }),
              out.end());
    face.cmap = std::move(out);
}

// ---- face ----------------------------------------------------------------

void readFace(QByteArrayView data, Face &face)
{
    auto reader = [&](const char *t) {
        const Table *table = face.table(t);
        return table ? Reader(data, table->offset, table->length) : Reader(QByteArrayView());
    };
    readNames(reader("name"), face);
    face.numGlyphs = reader("maxp").u16(4);
    const Reader head = reader("head");
    if (head.u32(12) == 0x5F0F3CF5)
        face.unitsPerEm = head.u16(18);
    face.weightClass = reader("OS/2").u16(4);

    if (face.table("glyf"))
        face.outlines = QStringLiteral("TrueType");
    else if (face.table("CFF2"))
        face.outlines = QStringLiteral("CFF2");
    else if (face.table("CFF "))
        face.outlines = QStringLiteral("CFF");
    else if (face.table("CBDT") || face.table("sbix") || face.table("EBDT"))
        face.outlines = QStringLiteral("Bitmap");
    face.colour = face.table("COLR") || face.table("CBDT") || face.table("sbix") || face.table("SVG ");
    face.hasShaping = face.table("GSUB") != nullptr;

    const Reader fvar = reader("fvar");
    if (fvar.valid()) {
        const qsizetype axesOffset = fvar.u16(4);
        const int count = fvar.u16(8), size = fvar.u16(10);
        for (int i = 0; i < count && size >= 20; ++i) {
            const qsizetype a = axesOffset + qsizetype(i) * size;
            if (!fvar.has(a, 20))
                break;
            face.axes.append({tagText(fvar.u32(a)), fvar.fixed(a + 4), fvar.fixed(a + 8), fvar.fixed(a + 12)});
        }
    }
    readCmap(reader("cmap"), face);
}

// ---- sfnt building -------------------------------------------------------

// A standalone sfnt from `tables` (each a tag plus its bytes), directory
// sorted by tag, tables 4-byte aligned.
QByteArray buildSfnt(quint32 version, std::vector<std::pair<quint32, QByteArrayView>> tables)
{
    std::sort(tables.begin(), tables.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    const auto count = quint16(tables.size());
    quint16 entrySelector = 0;
    while ((2u << entrySelector) <= count)
        ++entrySelector;
    const quint16 searchRange = quint16(16u << entrySelector);
    qsizetype total = 12 + qsizetype(count) * 16;
    for (const auto &t : tables)
        total += (t.second.size() + 3) & ~qsizetype(3);
    QByteArray out(total, '\0');
    char *p = out.data();
    qToBigEndian(version, p);
    qToBigEndian(count, p + 4);
    qToBigEndian(searchRange, p + 6);
    qToBigEndian(entrySelector, p + 8);
    qToBigEndian(quint16(count * 16 - searchRange), p + 10);
    quint32 offset = 12 + quint32(count) * 16;
    for (qsizetype i = 0; i < qsizetype(tables.size()); ++i) {
        char *e = p + 12 + i * 16;
        const QByteArrayView body = tables[size_t(i)].second;
        quint32 sum = 0;
        for (qsizetype j = 0; j < body.size(); j += 4) {
            quint32 word = 0;
            for (qsizetype k = 0; k < 4; ++k)
                word = (word << 8) | (j + k < body.size() ? quint8(body[j + k]) : 0);
            sum += word;
        }
        qToBigEndian(tables[size_t(i)].first, e);
        qToBigEndian(sum, e + 4);
        qToBigEndian(offset, e + 8);
        qToBigEndian(quint32(body.size()), e + 12);
        std::memcpy(p + offset, body.data(), size_t(body.size()));
        offset += quint32((body.size() + 3) & ~qsizetype(3));
    }
    return out;
}

Document parseWoff(const QByteArray &bytes)
{
    Document doc;
    doc.container = Container::Woff;
    const Reader r(bytes);
    const quint32 flavor = r.u32(4);
    const int count = r.u16(12);
    if (count == 0 || !r.has(44, qsizetype(count) * 20)) {
        doc.error = QStringLiteral("The WOFF table directory is damaged.");
        return doc;
    }
    std::vector<QByteArray> storage;
    storage.reserve(size_t(count));
    std::vector<std::pair<quint32, QByteArrayView>> tables;
    qint64 total = 0;
    for (int i = 0; i < count; ++i) {
        const qsizetype e = 44 + qsizetype(i) * 20;
        const quint32 t = r.u32(e), offset = r.u32(e + 4), compLength = r.u32(e + 8), origLength = r.u32(e + 12);
        total += origLength;
        if (total > kMaxFileBytes || !r.has(offset, compLength) || compLength > origLength) {
            doc.error = QStringLiteral("The WOFF table directory is damaged.");
            return doc;
        }
        if (compLength == origLength) {
            tables.emplace_back(t, r.bytes(offset, compLength));
            continue;
        }
        // qUncompress wants the expected size in front of the zlib stream.
        QByteArray z(4, '\0');
        qToBigEndian(origLength, z.data());
        z.append(r.bytes(offset, compLength));
        QByteArray plain = qUncompress(z);
        if (plain.size() != qsizetype(origLength)) {
            doc.error = QStringLiteral("A WOFF table does not decompress.");
            return doc;
        }
        storage.push_back(std::move(plain));
        tables.emplace_back(t, QByteArrayView(storage.back()));
    }
    doc.data = buildSfnt(flavor, std::move(tables));
    Face face;
    if (readDirectory(doc.data, 0, &face.tables)) {
        readFace(doc.data, face);
        doc.faces.append(std::move(face));
    } else {
        doc.error = QStringLiteral("The font has no tables.");
    }
    return doc;
}

} // namespace

const Table *Face::table(const char t[4]) const
{
    const quint32 want = tag(t);
    for (const Table &table : tables) {
        if (table.tag == want)
            return &table;
    }
    return nullptr;
}

bool looksLikeFont(QByteArrayView head)
{
    if (head.size() < 12)
        return false;
    const Reader r(head);
    const quint32 magic = r.u32(0);
    if (magic == tag("wOFF"))
        return r.u32(4) == kTrueType || r.u32(4) == tag("OTTO") || r.u32(4) == tag("true");
    if (magic == tag("ttcf"))
        return (r.u32(4) == 0x00010000 || r.u32(4) == 0x00020000) && r.u32(8) > 0 && r.u32(8) <= quint32(kMaxFaces);
    if (magic != kTrueType && magic != tag("OTTO") && magic != tag("true"))
        return false;
    // The table count and the first tag must be sane: 00 01 00 00 alone is weak.
    const int count = r.u16(4);
    if (count == 0 || count > 128 || head.size() < 16)
        return false;
    for (int i = 0; i < 4; ++i) {
        const quint8 c = r.u8(12 + i);
        if (c < 0x20 || c > 0x7E)
            return false;
    }
    return true;
}

Document parse(const QByteArray &bytes)
{
    Document doc;
    if (bytes.size() > kMaxFileBytes) {
        doc.error = QStringLiteral("Fonts over 256 MB are not previewed.");
        return doc;
    }
    const Reader r(bytes);
    const quint32 magic = r.u32(0);
    if (magic == tag("wOF2")) {
        doc.container = Container::Woff2;
        doc.error = QStringLiteral("WOFF 2 fonts are not supported.");
        return doc;
    }
    if (magic == tag("wOFF"))
        return parseWoff(bytes);

    doc.data = bytes;
    if (magic == tag("ttcf")) {
        doc.container = Container::Collection;
        const quint32 count = r.u32(8);
        if (count == 0 || count > quint32(kMaxFaces) || !r.has(12, qsizetype(count) * 4)) {
            doc.error = QStringLiteral("The font collection header is damaged.");
            return doc;
        }
        for (quint32 i = 0; i < count; ++i) {
            Face face;
            if (!readDirectory(bytes, r.u32(12 + qsizetype(i) * 4), &face.tables))
                continue;
            readFace(bytes, face);
            doc.faces.append(std::move(face));
        }
    } else if (magic == kTrueType || magic == tag("OTTO") || magic == tag("true")) {
        doc.container = Container::Sfnt;
        Face face;
        if (readDirectory(bytes, 0, &face.tables)) {
            readFace(bytes, face);
            doc.faces.append(std::move(face));
        }
    } else {
        doc.error = QStringLiteral("This is not a TrueType, OpenType or WOFF font.");
        return doc;
    }
    if (doc.faces.isEmpty() && doc.error.isEmpty())
        doc.error = QStringLiteral("The font has no readable tables.");
    return doc;
}

QByteArray faceData(const Document &doc, int faceIndex)
{
    if (faceIndex < 0 || faceIndex >= doc.faces.size())
        return {};
    if (doc.container != Container::Collection)
        return doc.data;
    return buildFace(doc.data, doc.faces[faceIndex].tables);
}

QByteArray buildFace(const QByteArray &collection, const QVector<Table> &tables)
{
    const QByteArrayView data(collection);
    std::vector<std::pair<quint32, QByteArrayView>> parts;
    parts.reserve(size_t(tables.size()));
    bool cff = false;
    for (const Table &t : tables) {
        if (quint64(t.offset) + t.length > quint64(data.size()))
            continue;
        parts.emplace_back(t.tag, data.sliced(t.offset, t.length));
        cff = cff || t.tag == tag("CFF ") || t.tag == tag("CFF2");
    }
    return buildSfnt(cff ? tag("OTTO") : kTrueType, std::move(parts));
}

} // namespace FontFile
