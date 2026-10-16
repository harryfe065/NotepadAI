#include "AudioFileInfo.h"

#include <QIODevice>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace AudioFileInfo {

namespace {

constexpr qint64 kOggWindow = 64 * 1024;
constexpr qint64 kMaxTextChunk = 64 * 1024;     // LIST/INFO, comment blocks
constexpr int kMaxChunks = 4096;                // RIFF / IFF / FLAC block walk

QByteArray readAt(QIODevice &dev, qint64 offset, qint64 length)
{
    if (offset < 0 || length <= 0 || !dev.seek(offset))
        return {};
    return dev.read(length);
}

quint16 le16(const char *p) { return qFromLittleEndian<quint16>(p); }
quint32 le32(const char *p) { return qFromLittleEndian<quint32>(p); }
quint64 le64(const char *p) { return qFromLittleEndian<quint64>(p); }
quint16 be16(const char *p) { return qFromBigEndian<quint16>(p); }
quint32 be32(const char *p) { return qFromBigEndian<quint32>(p); }

bool tagIs(const char *p, const char *tag) { return std::memcmp(p, tag, 4) == 0; }

void finishDuration(Info &info)
{
    if (info.frames >= 0 && info.sampleRate > 0)
        info.durationMs = info.frames * 1000 / info.sampleRate;
}

void applyLoop(Info &info, qint64 start, qint64 end)
{
    if (start < 0 || end <= start)
        return;
    if (info.frames > 0) {
        if (start >= info.frames)
            return;
        end = std::min(end, info.frames);
    }
    info.hasLoop = true;
    info.loopStart = start;
    info.loopEnd = end;
}

// Vorbis comment list (vendor, count, "KEY=value" entries), without the
// "\x03vorbis" prefix. Reads TITLE and the loop tags RPG Maker and many
// engines write: LOOPSTART with LOOPLENGTH or LOOPEND, in samples.
void parseVorbisComments(QByteArrayView c, Info &info)
{
    qsizetype pos = 0;
    auto u32 = [&](quint32 *out) {
        if (pos + 4 > c.size())
            return false;
        *out = le32(c.data() + pos);
        pos += 4;
        return true;
    };
    quint32 vendor = 0, count = 0;
    if (!u32(&vendor) || vendor > quint32(c.size() - pos))
        return;
    pos += vendor;
    if (!u32(&count))
        return;
    qint64 loopStart = -1, loopLength = -1, loopEnd = -1;
    for (quint32 i = 0; i < count; ++i) {
        quint32 len = 0;
        if (!u32(&len) || len > quint32(c.size() - pos))
            break;
        const QByteArrayView entry = c.sliced(pos, len);
        pos += len;
        const qsizetype eq = entry.indexOf('=');
        if (eq <= 0)
            continue;
        const QByteArray key = entry.first(eq).toByteArray().toUpper();
        const QByteArrayView value = entry.sliced(eq + 1);
        bool ok = false;
        if (key == "TITLE" && info.title.isEmpty()) {
            info.title = QString::fromUtf8(value);
        } else if (key == "LOOPSTART" || key == "LOOP_START") {
            const qint64 v = value.toLongLong(&ok);
            if (ok) loopStart = v;
        } else if (key == "LOOPLENGTH" || key == "LOOP_LENGTH") {
            const qint64 v = value.toLongLong(&ok);
            if (ok) loopLength = v;
        } else if (key == "LOOPEND" || key == "LOOP_END") {
            const qint64 v = value.toLongLong(&ok);
            if (ok) loopEnd = v;
        }
    }
    if (loopStart >= 0) {
        if (loopLength > 0)
            applyLoop(info, loopStart, loopStart + loopLength);
        else if (loopEnd > loopStart)
            applyLoop(info, loopStart, loopEnd);
        else if (loopEnd < 0 && info.frames > loopStart)
            applyLoop(info, loopStart, info.frames);
    }
}

// ---- WAV -------------------------------------------------------------------

QString wavCodecName(quint16 tag)
{
    switch (tag) {
    case 0x0001: return QStringLiteral("PCM");
    case 0x0002: return QStringLiteral("MS ADPCM");
    case 0x0003: return QStringLiteral("IEEE float");
    case 0x0006: return QStringLiteral("A-law");
    case 0x0007: return QStringLiteral("µ-law");
    case 0x0011: return QStringLiteral("IMA ADPCM");
    case 0x0050: return QStringLiteral("MPEG");
    case 0x0055: return QStringLiteral("MPEG Layer III");
    case 0x00FF: return QStringLiteral("AAC");
    case 0x2000: return QStringLiteral("AC-3");
    default: return QStringLiteral("format 0x%1").arg(tag, 4, 16, QLatin1Char('0'));
    }
}

Info parseWav(QIODevice &dev, qint64 size, bool rf64)
{
    Info info;
    info.container = QStringLiteral("WAV");
    bool haveFmt = false;
    quint16 tag = 0;
    quint16 blockAlign = 0;
    quint32 byteRate = 0;
    qint64 factFrames = -1;
    qint64 rf64Data = -1;
    qint64 smplStart = -1, smplEnd = -1;

    qint64 off = 12;
    for (int n = 0; n < kMaxChunks && off + 8 <= size; ++n) {
        const QByteArray header = readAt(dev, off, 8);
        if (header.size() < 8)
            break;
        const char *id = header.constData();
        qint64 len = le32(header.constData() + 4);
        const qint64 body = off + 8;
        if (tagIs(id, "data") && rf64 && len == 0xFFFFFFFF && rf64Data >= 0)
            len = rf64Data;

        if (tagIs(id, "fmt ")) {
            if (len < 16 || body + len > size)
                return {};
            const QByteArray f = readAt(dev, body, std::min<qint64>(len, 64));
            if (f.size() < 16)
                return {};
            tag = le16(f.constData());
            info.channels = le16(f.constData() + 2);
            info.sampleRate = int(le32(f.constData() + 4));
            byteRate = le32(f.constData() + 8);
            blockAlign = le16(f.constData() + 12);
            info.bitsPerSample = le16(f.constData() + 14);
            // WAVE_FORMAT_EXTENSIBLE: the real tag opens the sub-format GUID.
            if (tag == 0xFFFE && f.size() >= 26)
                tag = le16(f.constData() + 24);
            haveFmt = true;
        } else if (tagIs(id, "ds64") && len >= 24) {
            const QByteArray d = readAt(dev, body, 24);
            if (d.size() == 24)
                rf64Data = qint64(le64(d.constData() + 8));
        } else if (tagIs(id, "data")) {
            info.dataOffset = body;
            info.dataBytes = std::max<qint64>(0, std::min(len, size - body));
        } else if (tagIs(id, "fact") && len >= 4) {
            const QByteArray f = readAt(dev, body, 4);
            if (f.size() == 4)
                factFrames = le32(f.constData());
        } else if (tagIs(id, "smpl") && len >= 36 + 24) {
            const QByteArray s = readAt(dev, body, 36 + 24);
            if (s.size() == 60 && le32(s.constData() + 28) > 0) {
                // First loop: cue id, type, start, end (inclusive), fraction, count.
                smplStart = le32(s.constData() + 36 + 8);
                smplEnd = qint64(le32(s.constData() + 36 + 12)) + 1;
            }
        } else if (tagIs(id, "LIST") && len >= 4) {
            const QByteArray list = readAt(dev, body, std::min(len, kMaxTextChunk));
            if (list.startsWith("INFO")) {
                qsizetype p = 4;
                while (p + 8 <= list.size()) {
                    const qsizetype sub = le32(list.constData() + p + 4);
                    if (sub > list.size() - p - 8)
                        break;
                    if (tagIs(list.constData() + p, "INAM")) {
                        QByteArrayView v(list.constData() + p + 8, sub);
                        const qsizetype nul = v.indexOf('\0');
                        info.title = QString::fromUtf8(nul >= 0 ? v.first(nul) : v).trimmed();
                    }
                    p += 8 + sub + (sub & 1);
                }
            }
        }
        off = body + len + (len & 1);
    }

    if (!haveFmt)
        return {};
    info.valid = true;
    info.codec = wavCodecName(tag);
    info.bitrate = qint64(byteRate) * 8;

    const int bytes = info.bitsPerSample / 8;
    if (tag == 0x0001) {
        info.pcmKnown = true;
        switch (info.bitsPerSample) {
        case 8: info.pcm = WaveformPeaks::Format::U8; break;
        case 16: info.pcm = WaveformPeaks::Format::S16; break;
        case 24: info.pcm = WaveformPeaks::Format::S24; break;
        case 32: info.pcm = WaveformPeaks::Format::S32; break;
        default: info.pcmKnown = false; break;
        }
    } else if (tag == 0x0003) {
        info.pcmKnown = info.bitsPerSample == 32 || info.bitsPerSample == 64;
        info.pcm = info.bitsPerSample == 64 ? WaveformPeaks::Format::F64 : WaveformPeaks::Format::F32;
    }
    // Packed samples only: a block align with padding is left to the decoder.
    if (info.pcmKnown && (info.channels <= 0 || blockAlign != info.channels * bytes))
        info.pcmKnown = false;
    if (!info.pcmKnown && tag != 0x0001 && tag != 0x0003)
        info.bitsPerSample = 0;     // ADPCM's 4 "bits" are not a depth

    if (info.dataOffset >= 0) {
        if (blockAlign > 0 && (tag == 0x0001 || tag == 0x0003))
            info.frames = info.dataBytes / blockAlign;
        else if (factFrames >= 0)
            info.frames = factFrames;
        else if (byteRate > 0 && info.sampleRate > 0)
            info.frames = info.dataBytes * info.sampleRate / byteRate;
    }
    finishDuration(info);
    if (smplStart >= 0)
        applyLoop(info, smplStart, smplEnd);
    return info;
}

// ---- AIFF / AIFC -----------------------------------------------------------

// 80-bit IEEE 754 extended (big endian), as AIFF stores the sample rate.
double extended80(const char *p)
{
    const quint16 se = be16(p);
    const quint64 mantissa = qFromBigEndian<quint64>(p + 2);
    const int exponent = int(se & 0x7FFF);
    if (exponent == 0 && mantissa == 0)
        return 0.0;
    const double value = std::ldexp(double(mantissa), exponent - 16383 - 63);
    return (se & 0x8000) ? -value : value;
}

Info parseAiff(QIODevice &dev, qint64 size, bool aifc)
{
    Info info;
    info.container = aifc ? QStringLiteral("AIFC") : QStringLiteral("AIFF");
    info.codec = QStringLiteral("PCM");
    bool haveComm = false;
    bool littleEndianPcm = false;
    qint64 off = 12;
    for (int n = 0; n < kMaxChunks && off + 8 <= size; ++n) {
        const QByteArray header = readAt(dev, off, 8);
        if (header.size() < 8)
            break;
        const qint64 len = be32(header.constData() + 4);
        const qint64 body = off + 8;
        if (tagIs(header.constData(), "COMM") && len >= 18) {
            const QByteArray c = readAt(dev, body, std::min<qint64>(len, 64));
            if (c.size() < 18)
                return {};
            info.channels = be16(c.constData());
            info.frames = be32(c.constData() + 2);
            info.bitsPerSample = be16(c.constData() + 6);
            const double rate = extended80(c.constData() + 8);
            info.sampleRate = rate > 0 && rate < 1e7 ? int(std::lround(rate)) : 0;
            if (aifc && c.size() >= 22) {
                const QByteArray type = c.mid(18, 4);
                littleEndianPcm = type == "sowt";
                if (type == "fl32" || type == "FL32")
                    info.codec = QStringLiteral("IEEE float");
                else if (type != "NONE" && type != "sowt" && type != "twos")
                    info.codec = QString::fromLatin1(type).trimmed();
            }
            haveComm = true;
        } else if (tagIs(header.constData(), "SSND") && len >= 8) {
            const QByteArray s = readAt(dev, body, 8);
            if (s.size() == 8) {
                info.dataOffset = body + 8 + be32(s.constData());
                info.dataBytes = std::max<qint64>(0, std::min(len - 8, size - info.dataOffset));
            }
        }
        off = body + len + (len & 1);
    }
    if (!haveComm)
        return {};
    info.valid = true;
    if (littleEndianPcm) {
        info.pcmKnown = true;
        switch (info.bitsPerSample) {
        case 8: info.pcm = WaveformPeaks::Format::U8; info.pcmKnown = false; break;  // signed 8-bit
        case 16: info.pcm = WaveformPeaks::Format::S16; break;
        case 24: info.pcm = WaveformPeaks::Format::S24; break;
        case 32: info.pcm = WaveformPeaks::Format::S32; break;
        default: info.pcmKnown = false; break;
        }
    }
    finishDuration(info);
    if (info.durationMs > 0 && info.sampleRate > 0 && info.channels > 0 && info.bitsPerSample > 0)
        info.bitrate = qint64(info.sampleRate) * info.channels * info.bitsPerSample;
    return info;
}

// ---- FLAC ------------------------------------------------------------------

Info parseFlac(QIODevice &dev, qint64 size, qint64 start)
{
    Info info;
    info.container = QStringLiteral("FLAC");
    info.codec = QStringLiteral("FLAC");
    bool haveStreamInfo = false;
    QByteArray comments;
    qint64 off = start + 4;
    for (int n = 0; n < kMaxChunks && off + 4 <= size; ++n) {
        const QByteArray h = readAt(dev, off, 4);
        if (h.size() < 4)
            break;
        const bool last = (quint8(h[0]) & 0x80) != 0;
        const int type = quint8(h[0]) & 0x7F;
        const qint64 len = (qint64(quint8(h[1])) << 16) | (qint64(quint8(h[2])) << 8) | quint8(h[3]);
        if (type == 0 && len >= 34) {
            const QByteArray si = readAt(dev, off + 4, 34);
            if (si.size() == 34) {
                const quint64 packed = qFromBigEndian<quint64>(si.constData() + 10);
                info.sampleRate = int(packed >> 44);
                info.channels = int((packed >> 41) & 0x7) + 1;
                info.bitsPerSample = int((packed >> 36) & 0x1F) + 1;
                const qint64 total = qint64(packed & 0xFFFFFFFFFull);
                info.frames = total > 0 ? total : -1;
                haveStreamInfo = info.sampleRate > 0;
            }
        } else if (type == 4) {
            comments = readAt(dev, off + 4, std::min(len, kMaxTextChunk));
        }
        if (last)
            break;
        off += 4 + len;
    }
    if (!haveStreamInfo)
        return {};
    info.valid = true;
    finishDuration(info);
    if (!comments.isEmpty())
        parseVorbisComments(comments, info);
    return info;
}

// ---- Ogg -------------------------------------------------------------------

// The first `want` packets of the stream that starts the file, from a window
// at its head. A packet cut off by the window is returned as far as it goes.
QList<QByteArray> oggPackets(QByteArrayView w, int want, quint32 *serialOut)
{
    QList<QByteArray> packets;
    QByteArray current;
    qsizetype p = 0;
    bool haveSerial = false;
    quint32 serial = 0;
    while (p + 27 <= w.size() && packets.size() < want) {
        if (std::memcmp(w.data() + p, "OggS", 4) != 0 || w[p + 4] != 0)
            break;
        const quint32 pageSerial = le32(w.data() + p + 14);
        const int segments = quint8(w[p + 26]);
        const qsizetype lacing = p + 27;
        if (lacing + segments > w.size())
            break;
        qsizetype bodySize = 0;
        for (int i = 0; i < segments; ++i)
            bodySize += quint8(w[lacing + i]);
        qsizetype data = lacing + segments;
        if (!haveSerial) {
            serial = pageSerial;
            haveSerial = true;
        }
        if (pageSerial == serial) {
            for (int i = 0; i < segments && packets.size() < want; ++i) {
                const int len = quint8(w[lacing + i]);
                const qsizetype avail = std::max<qsizetype>(0, std::min<qsizetype>(len, w.size() - data));
                current.append(w.data() + data, avail);
                data += len;
                if (len < 255) {
                    packets.append(current);
                    current.clear();
                }
            }
        }
        p = lacing + segments + bodySize;
    }
    if (!current.isEmpty() && packets.size() < want)
        packets.append(current);
    if (serialOut)
        *serialOut = serial;
    return packets;
}

// Granule position of the last page of `serial` in the file's tail.
qint64 oggLastGranule(QIODevice &dev, qint64 size, quint32 serial)
{
    const qint64 start = std::max<qint64>(0, size - kOggWindow);
    const QByteArray tail = readAt(dev, start, size - start);
    for (qsizetype p = tail.lastIndexOf("OggS"); p >= 0; p = p > 0 ? tail.lastIndexOf("OggS", p - 1) : -1) {
        if (p + 27 > tail.size() || tail[p + 4] != 0)
            continue;
        if (le32(tail.constData() + p + 14) != serial)
            continue;
        const qint64 granule = qint64(le64(tail.constData() + p + 6));
        if (granule >= 0)
            return granule;
    }
    return -1;
}

Info parseOgg(QIODevice &dev, qint64 size)
{
    const QByteArray head = readAt(dev, 0, std::min(size, kOggWindow));
    quint32 serial = 0;
    const QList<QByteArray> packets = oggPackets(head, 2, &serial);
    if (packets.isEmpty())
        return {};
    const QByteArray &id = packets[0];
    Info info;
    info.container = QStringLiteral("Ogg");
    qint64 preSkip = 0;
    QByteArrayView comments;
    if (id.size() >= 30 && id.startsWith("\x01vorbis")) {
        info.codec = QStringLiteral("Vorbis");
        info.channels = quint8(id[11]);
        info.sampleRate = int(le32(id.constData() + 12));
        const qint32 nominal = qint32(le32(id.constData() + 20));
        info.bitrate = nominal > 0 ? nominal : 0;
        if (packets.size() > 1 && packets[1].startsWith("\x03vorbis"))
            comments = QByteArrayView(packets[1]).sliced(7);
    } else if (id.size() >= 19 && id.startsWith("OpusHead")) {
        info.codec = QStringLiteral("Opus");
        info.channels = quint8(id[9]);
        preSkip = le16(id.constData() + 10);
        info.sampleRate = 48000;    // Opus always decodes at 48 kHz
        if (packets.size() > 1 && packets[1].startsWith("OpusTags"))
            comments = QByteArrayView(packets[1]).sliced(8);
    } else if (id.size() >= 51 && id.startsWith("\x7F" "FLAC")) {
        info.codec = QStringLiteral("FLAC");
        const quint64 packed = qFromBigEndian<quint64>(id.constData() + 13 + 4 + 10);
        info.sampleRate = int(packed >> 44);
        info.channels = int((packed >> 41) & 0x7) + 1;
        info.bitsPerSample = int((packed >> 36) & 0x1F) + 1;
        if (packets.size() > 1 && packets[1].size() > 4)
            comments = QByteArrayView(packets[1]).sliced(4);
    } else if (id.startsWith("Speex   ") && id.size() >= 52) {
        info.codec = QStringLiteral("Speex");
        info.sampleRate = int(le32(id.constData() + 36));
        info.channels = int(le32(id.constData() + 48));
    } else {
        return {};
    }
    if (info.sampleRate <= 0 || info.channels <= 0 || info.channels > 255)
        return {};
    info.valid = true;
    const qint64 granule = oggLastGranule(dev, size, serial);
    if (granule > preSkip)
        info.frames = granule - preSkip;
    finishDuration(info);
    if (!comments.isEmpty())
        parseVorbisComments(comments, info);
    if (info.bitrate == 0 && info.durationMs > 0)
        info.bitrate = size * 8 * 1000 / info.durationMs;
    return info;
}

// ---- MPEG audio --------------------------------------------------------------

struct MpegFrame {
    int version = 0;        // 1, 2, or 25 (2.5)
    int layer = 0;
    int bitrate = 0;        // kbps
    int sampleRate = 0;
    int channels = 0;
    int length = 0;         // bytes
    int samples = 0;
};

bool mpegFrame(const unsigned char *h, MpegFrame *f)
{
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0)
        return false;
    const int v = (h[1] >> 3) & 3, l = (h[1] >> 1) & 3, br = h[2] >> 4, sr = (h[2] >> 2) & 3;
    if (v == 1 || l == 0 || br == 0 || br == 15 || sr == 3)
        return false;
    static const int kV1[3][15] = {
        {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},   // layer I
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},      // layer II
        {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}};      // layer III
    static const int kV2[2][15] = {
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},      // layer I
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}};          // layers II, III
    static const int kRates[3] = {44100, 48000, 32000};
    f->version = v == 3 ? 1 : v == 2 ? 2 : 25;
    f->layer = 4 - l;
    f->bitrate = f->version == 1 ? kV1[f->layer - 1][br] : kV2[f->layer == 1 ? 0 : 1][br];
    f->sampleRate = kRates[sr] / (f->version == 1 ? 1 : f->version == 2 ? 2 : 4);
    f->channels = (h[3] >> 6) == 3 ? 1 : 2;
    const int pad = (h[2] >> 1) & 1;
    if (f->layer == 1) {
        f->samples = 384;
        f->length = (12 * f->bitrate * 1000 / f->sampleRate + pad) * 4;
    } else {
        f->samples = (f->layer == 3 && f->version != 1) ? 576 : 1152;
        f->length = f->samples / 8 * f->bitrate * 1000 / f->sampleRate + pad;
    }
    return f->length > 4;
}

Info parseMpeg(QIODevice &dev, qint64 size, qint64 start)
{
    const QByteArray w = readAt(dev, start, std::min<qint64>(size - start, kOggWindow));
    const auto *b = reinterpret_cast<const unsigned char *>(w.constData());
    // Two consecutive valid headers, so a stray 0xFFE in a tag is not a frame.
    for (qsizetype p = 0; p + 4 <= w.size(); ++p) {
        MpegFrame f, next;
        if (!mpegFrame(b + p, &f))
            continue;
        const qsizetype q = p + f.length;
        if (q + 4 <= w.size() && !(mpegFrame(b + q, &next) && next.sampleRate == f.sampleRate))
            continue;
        Info info;
        info.valid = true;
        info.container = QStringLiteral("MP3");
        static const char *kVersions[] = {"", "MPEG-1", "MPEG-2"};
        const QString version = f.version == 25 ? QStringLiteral("MPEG-2.5") : QString::fromLatin1(kVersions[f.version]);
        static const char *kLayers[] = {"", "I", "II", "III"};
        info.codec = QStringLiteral("%1 Layer %2").arg(version, QLatin1String(kLayers[f.layer]));
        if (f.layer != 3)
            info.container = QStringLiteral("MPEG audio");
        info.sampleRate = f.sampleRate;
        info.channels = f.channels;
        info.bitrate = qint64(f.bitrate) * 1000;
        // Xing / Info header (VBR frame count) after the side information.
        const int side = f.version == 1 ? (f.channels == 1 ? 17 : 32) : (f.channels == 1 ? 9 : 17);
        const qsizetype x = p + 4 + side;
        if (f.layer == 3 && x + 12 <= w.size()
            && (std::memcmp(b + x, "Xing", 4) == 0 || std::memcmp(b + x, "Info", 4) == 0)) {
            const quint32 flags = be32(w.constData() + x + 4);
            if (flags & 1)
                info.frames = qint64(be32(w.constData() + x + 8)) * f.samples;
        }
        if (info.frames < 0 && f.bitrate > 0) {
            const qint64 audioBytes = size - start - p;
            info.durationMs = audioBytes * 8 / f.bitrate;   // bytes * 8 / (kbps * 1000) * 1000
        }
        finishDuration(info);
        if (info.frames > 0 && info.durationMs > 0)
            info.bitrate = (size - start - p) * 8 * 1000 / info.durationMs;
        return info;
    }
    return {};
}

} // namespace

Info parse(QIODevice &dev)
{
    const qint64 size = dev.size();
    const QByteArray head = readAt(dev, 0, std::min<qint64>(size, 16));
    if (head.size() < 4)
        return {};
    const char *h = head.constData();

    if (head.size() >= 12 && (tagIs(h, "RIFF") || tagIs(h, "RF64")) && tagIs(h + 8, "WAVE"))
        return parseWav(dev, size, tagIs(h, "RF64"));
    if (head.size() >= 12 && tagIs(h, "FORM") && (tagIs(h + 8, "AIFF") || tagIs(h + 8, "AIFC")))
        return parseAiff(dev, size, tagIs(h + 8, "AIFC"));
    if (tagIs(h, "fLaC"))
        return parseFlac(dev, size, 0);
    if (tagIs(h, "OggS"))
        return parseOgg(dev, size);

    qint64 audioStart = 0;
    if (head.size() >= 10 && std::memcmp(h, "ID3", 3) == 0) {
        // ID3v2: 10-byte header, syncsafe size, optional 10-byte footer.
        const auto *b = reinterpret_cast<const unsigned char *>(h);
        const qint64 tagSize = (qint64(b[6] & 0x7F) << 21) | (qint64(b[7] & 0x7F) << 14)
                               | (qint64(b[8] & 0x7F) << 7) | qint64(b[9] & 0x7F);
        audioStart = 10 + tagSize + ((b[5] & 0x10) ? 10 : 0);
        if (audioStart + 4 <= size) {
            const QByteArray after = readAt(dev, audioStart, 4);
            if (after.size() == 4 && tagIs(after.constData(), "fLaC"))
                return parseFlac(dev, size, audioStart);
        }
    }
    if (head.size() >= 12 && tagIs(h + 4, "ftyp")) {
        Info info;
        info.valid = true;
        info.container = QStringLiteral("MPEG-4");
        return info;
    }
    if (audioStart > 0 || (quint8(h[0]) == 0xFF && (quint8(h[1]) & 0xE0) == 0xE0))
        return parseMpeg(dev, size, audioStart);
    return {};
}

} // namespace AudioFileInfo
