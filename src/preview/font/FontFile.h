#ifndef FONTFILE_H
#define FONTFILE_H

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QVector>

#include <utility>
#include <vector>

// OpenType / TrueType reader for the font preview: containers (sfnt, TTC /
// OTC collections, WOFF 1) and the tables the viewer shows (name, maxp, head,
// OS/2, fvar, cmap). Pure: no Qt font engine, no font database.
//
// Every offset and length is checked against the buffer; a broken table
// leaves its fields empty instead of failing the font. A collection face is
// turned into a standalone sfnt only when asked (faceData), holding just that
// face's tables, so a 100 MB collection is never copied once per face.
namespace FontFile {

enum class Container { Unknown, Sfnt, Collection, Woff, Woff2 };

struct Table {
    quint32 tag = 0;
    quint32 offset = 0;     // into Document::data
    quint32 length = 0;
};

struct Axis {
    QString tag;
    double min = 0, def = 0, max = 0;
};

struct Face {
    QVector<Table> tables;
    QString family;         // typographic family (name 16) when present, else name 1
    QString subfamily;
    QString fullName;
    QString postScriptName;
    QString version;
    QString copyright;
    QString designer;
    QString license;
    int numGlyphs = 0;
    int unitsPerEm = 0;
    int weightClass = 0;
    QString outlines;       // "TrueType", "CFF", "CFF2", "Bitmap", or empty
    bool colour = false;    // COLR, CBDT, sbix or SVG glyphs
    bool hasShaping = false; // GSUB present: the sample shows unshaped text
    QVector<Axis> axes;
    // Unicode code point -> glyph id, sorted by code point, no duplicates.
    std::vector<std::pair<char32_t, quint32>> cmap;

    const Table *table(const char tag[4]) const;
};

struct Document {
    Container container = Container::Unknown;
    QByteArray data;        // the file, or the sfnt rebuilt from a WOFF
    QVector<Face> faces;
    QString error;          // set when faces is empty

    bool isValid() const { return !faces.isEmpty(); }
};

constexpr qint64 kMaxFileBytes = 256LL * 1024 * 1024;
constexpr int kMaxFaces = 256;

Document parse(const QByteArray &bytes);

// A standalone sfnt for one face, for QRawFont. Shares Document::data when
// the file is already a single sfnt.
QByteArray faceData(const Document &doc, int faceIndex);

// Head sniffer: sfnt (0x00010000, OTTO, true), ttcf and wOFF. WOFF 2 is not
// claimed; it is reported only when opened by extension.
bool looksLikeFont(QByteArrayView head);

constexpr quint32 tag(const char t[4])
{
    return quint32(quint8(t[0])) << 24 | quint32(quint8(t[1])) << 16 | quint32(quint8(t[2])) << 8 | quint8(t[3]);
}

} // namespace FontFile

#endif // FONTFILE_H
