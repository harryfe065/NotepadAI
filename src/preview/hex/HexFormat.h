#ifndef HEXFORMAT_H
#define HEXFORMAT_H

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <optional>

// Text side of the hex view (add-hex-preview): row columns, offsets, search
// patterns, copy formats and the data inspector. Pure functions.
namespace HexFormat {

enum class ByteClass : quint8 { Null, Printable, Control, High };

inline ByteClass classify(quint8 b)
{
    if (b == 0)
        return ByteClass::Null;
    if (b >= 0x20 && b < 0x7F)
        return ByteClass::Printable;
    return b < 0x80 ? ByteClass::Control : ByteClass::High;
}

// Hex digits of the offset column: 8, more once offsets pass 0xFFFFFFFF.
int offsetDigits(qint64 size);
QString offsetText(qint64 offset, int digits);

// Characters of the hex column for `bytesPerRow` bytes: "XX" per byte, one
// space between bytes and one more between groups of 8.
int hexColumnWidth(int bytesPerRow);
// Character position of byte `i` in the hex column.
int hexColumnPos(int i);
// One row split by byte class for drawing: per class, the hex and ASCII
// columns with that class's bytes in place and every other byte as spaces, so
// each class is one text run per pane. Reuses the strings' storage.
constexpr int kByteClasses = 4;
struct ClassRuns {
    QString hex[kByteClasses];
    QString ascii[kByteClasses];
    bool used[kByteClasses] = {};
};
void splitByClass(QByteArrayView row, int bytesPerRow, ClassRuns &runs);

// The column header: byte numbers over the hex and ASCII panes.
QString hexHeader(int bytesPerRow);
QString asciiHeader(int bytesPerRow);

// Info-line separator, shared by the hex tab.
const QString &separator();

// "0x1F", "1f" (hex by its letters), "4096", "+16", "-0x10" (relative to
// `cursor`, clamped at 0). nullopt when it is not a number.
std::optional<qint64> parseOffset(const QString &text, qint64 cursor);

// Hex patterns: digit pairs, ignoring spaces, commas, "0x" and "\x". Text
// patterns are UTF-8. False (and *error) for an empty or malformed pattern.
bool parsePattern(const QString &text, bool hex, QByteArray *out, QString *error);

QString toHex(QByteArrayView bytes);
// Printable ASCII, tab and newlines as they are; anything else as '.'.
QString toText(QByteArrayView bytes);
// "0x4D, 0x5A, ..." with a line break every 16 bytes.
QString toCArray(QByteArrayView bytes);

// Little-endian values of the bytes at the cursor ("u8 77  ·  i8 77  ·  u16 ..."),
// only the widths `bytes` holds (up to 8 bytes are used).
QString inspector(QByteArrayView bytes);

} // namespace HexFormat

#endif // HEXFORMAT_H
