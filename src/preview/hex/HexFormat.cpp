#include "HexFormat.h"

#include <QCoreApplication>
#include <QStringList>
#include <QtEndian>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace HexFormat {

namespace {

constexpr char kDigits[] = "0123456789ABCDEF";

} // namespace

const QString &separator()
{
    static const QString s = QStringLiteral("  ·  ");
    return s;
}

int offsetDigits(qint64 size)
{
    int digits = 8;
    const quint64 last = size > 0 ? quint64(size - 1) : 0;
    while (digits < 16 && (last >> (digits * 4)) != 0)
        ++digits;
    return digits;
}

QString offsetText(qint64 offset, int digits)
{
    return QStringLiteral("%1").arg(quint64(offset), digits, 16, QLatin1Char('0')).toUpper();
}

int hexColumnWidth(int bytesPerRow)
{
    return bytesPerRow * 3 - 1 + std::max(0, (bytesPerRow - 1) / 8);
}

int hexColumnPos(int i)
{
    return i * 3 + i / 8;
}

void splitByClass(QByteArrayView row, int bytesPerRow, ClassRuns &runs)
{
    const int hexWidth = hexColumnWidth(bytesPerRow);
    for (int c = 0; c < kByteClasses; ++c) {
        runs.hex[c].fill(QLatin1Char(' '), hexWidth);
        runs.ascii[c].fill(QLatin1Char(' '), bytesPerRow);
        runs.used[c] = false;
    }
    const qsizetype n = std::min<qsizetype>(row.size(), bytesPerRow);
    for (qsizetype i = 0; i < n; ++i) {
        const auto b = quint8(row[i]);
        const ByteClass cls = classify(b);
        const int c = int(cls);
        runs.used[c] = true;
        QChar *hex = runs.hex[c].data();
        const int at = hexColumnPos(int(i));
        hex[at] = QLatin1Char(kDigits[b >> 4]);
        hex[at + 1] = QLatin1Char(kDigits[b & 0xF]);
        runs.ascii[c].data()[i] = cls == ByteClass::Printable ? QLatin1Char(char(b)) : QLatin1Char('.');
    }
}

QString hexHeader(int bytesPerRow)
{
    QString out(hexColumnWidth(bytesPerRow), QLatin1Char(' '));
    for (int i = 0; i < bytesPerRow; ++i) {
        out[hexColumnPos(i)] = QLatin1Char(kDigits[(i >> 4) & 0xF]);
        out[hexColumnPos(i) + 1] = QLatin1Char(kDigits[i & 0xF]);
    }
    return out;
}

QString asciiHeader(int bytesPerRow)
{
    QString out(bytesPerRow, QLatin1Char(' '));
    for (int i = 0; i < bytesPerRow; ++i)
        out[i] = QLatin1Char(kDigits[i & 0xF]);
    return out;
}

std::optional<qint64> parseOffset(const QString &input, qint64 cursor)
{
    QString text = input.trimmed();
    int sign = 0;
    if (text.startsWith(QLatin1Char('+')) || text.startsWith(QLatin1Char('-'))) {
        sign = text.startsWith(QLatin1Char('+')) ? 1 : -1;
        text = text.mid(1).trimmed();
    }
    if (text.isEmpty())
        return std::nullopt;
    bool ok = false;
    qint64 value = 0;
    if (text.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
        value = text.mid(2).toLongLong(&ok, 16);
    } else {
        value = text.toLongLong(&ok, 10);
        if (!ok)
            value = text.toLongLong(&ok, 16);
    }
    if (!ok || value < 0)
        return std::nullopt;
    if (sign == 0)
        return value;
    return std::max<qint64>(0, cursor + sign * value);
}

bool parsePattern(const QString &text, bool hex, QByteArray *out, QString *error)
{
    out->clear();
    if (!hex) {
        *out = text.toUtf8();
        if (out->isEmpty()) {
            *error = QCoreApplication::translate("HexFormat", "Enter text to find.");
            return false;
        }
        return true;
    }
    QString digits;
    digits.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text[i];
        if (c.isSpace() || c == QLatin1Char(','))
            continue;
        // "0x" and "\x" prefixes.
        if ((c == QLatin1Char('0') || c == QLatin1Char('\\')) && i + 1 < text.size()
            && (text[i + 1] == QLatin1Char('x') || text[i + 1] == QLatin1Char('X'))
            && (c == QLatin1Char('\\') || digits.size() % 2 == 0)) {
            ++i;
            continue;
        }
        if (c.unicode() > 0x7F || !std::isxdigit(static_cast<unsigned char>(c.toLatin1()))) {
            *error = QCoreApplication::translate("HexFormat", "\"%1\" is not a hex digit.").arg(c);
            return false;
        }
        digits.append(c);
    }
    if (digits.isEmpty()) {
        *error = QCoreApplication::translate("HexFormat", "Enter hex bytes to find, e.g. 50 4B 03 04.");
        return false;
    }
    if (digits.size() % 2 != 0) {
        *error = QCoreApplication::translate("HexFormat", "Hex bytes need two digits each.");
        return false;
    }
    *out = QByteArray::fromHex(digits.toLatin1());
    return true;
}

QString toHex(QByteArrayView bytes)
{
    QString out(bytes.isEmpty() ? 0 : bytes.size() * 3 - 1, QLatin1Char(' '));
    QChar *p = out.data();
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        const auto b = quint8(bytes[i]);
        p[i * 3] = QLatin1Char(kDigits[b >> 4]);
        p[i * 3 + 1] = QLatin1Char(kDigits[b & 0xF]);
    }
    return out;
}

QString toText(QByteArrayView bytes)
{
    QString out(bytes.size(), QLatin1Char('.'));
    QChar *p = out.data();
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        const auto b = quint8(bytes[i]);
        if (classify(b) == ByteClass::Printable || b == '\t' || b == '\n' || b == '\r')
            p[i] = QLatin1Char(char(b));
    }
    return out;
}

QString toCArray(QByteArrayView bytes)
{
    QString out;
    out.reserve(bytes.size() * 6);
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        if (i > 0)
            out += (i % 16 == 0) ? QStringLiteral(",\n") : QStringLiteral(", ");
        const auto b = quint8(bytes[i]);
        out += QLatin1String("0x");
        out += QLatin1Char(kDigits[b >> 4]);
        out += QLatin1Char(kDigits[b & 0xF]);
    }
    return out;
}

QString inspector(QByteArrayView bytes)
{
    const qsizetype n = bytes.size();
    if (n == 0)
        return {};
    const char *p = bytes.data();
    QStringList parts;
    parts << QStringLiteral("u8 %1").arg(int(quint8(p[0]))) << QStringLiteral("i8 %1").arg(int(qint8(p[0])));
    if (n >= 2)
        parts << QStringLiteral("u16 %1").arg(int(qFromLittleEndian<quint16>(p)));
    if (n >= 4) {
        parts << QStringLiteral("u32 %1").arg(qFromLittleEndian<quint32>(p));
        parts << QStringLiteral("i32 %1").arg(qFromLittleEndian<qint32>(p));
        float f = 0;
        const quint32 bits = qFromLittleEndian<quint32>(p);
        std::memcpy(&f, &bits, 4);
        parts << QStringLiteral("f32 %1").arg(double(f), 0, 'g', 7);
    }
    if (n >= 8) {
        parts << QStringLiteral("u64 %1").arg(qFromLittleEndian<quint64>(p));
        double d = 0;
        const quint64 bits = qFromLittleEndian<quint64>(p);
        std::memcpy(&d, &bits, 8);
        parts << QStringLiteral("f64 %1").arg(d, 0, 'g', 15);
    }
    return parts.join(separator());
}

} // namespace HexFormat
