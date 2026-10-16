// Pure parts of the media preview (add-media-preview): the waveform envelope,
// audio header parsing, the media sniffers and the time / bitrate labels.
// Qt Multimedia is not involved; every input is synthesised in memory.

#include "AudioFileInfo.h"
#include "MediaFormat.h"
#include "MediaSniffers.h"
#include "WaveformPeaks.h"

#include <QBuffer>
#include <QtEndian>
#include <QtTest>

#include <cmath>
#include <cstring>
#include <vector>

namespace {

// ---- little byte writers -------------------------------------------------

void putLe16(QByteArray &b, quint16 v) { char c[2]; qToLittleEndian(v, c); b.append(c, 2); }
void putLe32(QByteArray &b, quint32 v) { char c[4]; qToLittleEndian(v, c); b.append(c, 4); }
void putBe16(QByteArray &b, quint16 v) { char c[2]; qToBigEndian(v, c); b.append(c, 2); }
void putBe32(QByteArray &b, quint32 v) { char c[4]; qToBigEndian(v, c); b.append(c, 4); }

QByteArray chunk(const char *id, const QByteArray &body)
{
    QByteArray b(id, 4);
    putLe32(b, quint32(body.size()));
    b += body;
    if (body.size() & 1)
        b.append('\0');
    return b;
}

QByteArray fmtChunk(quint16 tag, quint16 channels, quint32 rate, quint16 bits)
{
    QByteArray f;
    putLe16(f, tag);
    putLe16(f, channels);
    putLe32(f, rate);
    putLe32(f, rate * channels * (bits / 8));
    putLe16(f, quint16(channels * (bits / 8)));
    putLe16(f, bits);
    return chunk("fmt ", f);
}

QByteArray smplChunk(quint32 start, quint32 end)
{
    QByteArray s;
    for (int i = 0; i < 7; ++i)
        putLe32(s, 0);      // manufacturer .. SMPTE offset
    putLe32(s, 1);          // loop count
    putLe32(s, 0);          // sampler data
    putLe32(s, 0);          // cue id
    putLe32(s, 0);          // type: forward
    putLe32(s, start);
    putLe32(s, end);
    putLe32(s, 0);          // fraction
    putLe32(s, 0);          // play count
    return chunk("smpl", s);
}

QByteArray wav(const QList<QByteArray> &chunks)
{
    QByteArray body("WAVE");
    for (const QByteArray &c : chunks)
        body += c;
    QByteArray out("RIFF");
    putLe32(out, quint32(body.size()));
    return out + body;
}

AudioFileInfo::Info parse(const QByteArray &bytes)
{
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    return AudioFileInfo::parse(buffer);
}

// One Ogg page holding exactly one packet (< 255 * 255 bytes).
QByteArray oggPage(const QByteArray &packet, quint64 granule, quint32 sequence, quint8 headerType)
{
    QByteArray p("OggS");
    p.append('\0');
    p.append(char(headerType));
    char g[8];
    qToLittleEndian(granule, g);
    p.append(g, 8);
    putLe32(p, 0x1234);     // serial
    putLe32(p, sequence);
    putLe32(p, 0);          // CRC (not checked by the parser)
    QByteArray lacing;
    qsizetype left = packet.size();
    while (left >= 255) {
        lacing.append(char(255));
        left -= 255;
    }
    lacing.append(char(left));
    p.append(char(lacing.size()));
    p += lacing;
    p += packet;
    return p;
}

QByteArray vorbisComments(const QStringList &comments, bool withFramingAndPrefix)
{
    QByteArray c;
    if (withFramingAndPrefix)
        c += QByteArray("\x03vorbis", 7);
    const QByteArray vendor("test vendor");
    putLe32(c, quint32(vendor.size()));
    c += vendor;
    putLe32(c, quint32(comments.size()));
    for (const QString &s : comments) {
        const QByteArray u = s.toUtf8();
        putLe32(c, quint32(u.size()));
        c += u;
    }
    if (withFramingAndPrefix)
        c.append(char(1));
    return c;
}

QByteArray vorbisFile(quint32 rate, quint8 channels, const QStringList &comments, quint64 lastGranule)
{
    QByteArray ident("\x01vorbis", 7);
    putLe32(ident, 0);                 // version
    ident.append(char(channels));
    putLe32(ident, rate);
    putLe32(ident, 0);
    putLe32(ident, 128000);            // nominal bitrate
    putLe32(ident, 0);
    ident.append(char(0xB8));          // block sizes
    ident.append(char(1));             // framing
    QByteArray out = oggPage(ident, 0, 0, 0x02);
    out += oggPage(vorbisComments(comments, true), 0, 1, 0);
    out += oggPage(QByteArray(4000, 'a'), lastGranule / 2, 2, 0);
    out += oggPage(QByteArray(100, 'b'), lastGranule, 3, 0x04);
    return out;
}

} // namespace

class TestMediaFormats : public QObject
{
    Q_OBJECT

private slots:
    // ---- WaveformPeaks ----

    void peaksBucketPerTenMilliseconds()
    {
        WaveformPeaks peaks;
        peaks.reset(1000, 1);           // 10 frames per bucket
        std::vector<qint16> s(25, 0);
        s[3] = 16384;                   // +0.5 in bucket 0
        s[12] = -32768;                 // -1.0 in bucket 1
        peaks.add(s.data(), WaveformPeaks::Format::S16, qint64(s.size()));
        peaks.finish();
        QCOMPARE(peaks.size(), 3);
        QCOMPARE(peaks.peaks()[0].max, qint8(64));
        QCOMPARE(peaks.peaks()[0].min, qint8(0));
        QCOMPARE(peaks.peaks()[1].min, qint8(-127));
        QCOMPARE(peaks.frames(), qint64(25));
        QCOMPARE(peaks.seconds(), 0.025);
    }

    void peaksDoNotDriftAtFractionalRates()
    {
        // 22 050 Hz is 220.5 frames a bucket: 100 s must be exactly 10 000 buckets.
        WaveformPeaks peaks;
        peaks.reset(22050, 1);
        const std::vector<qint16> second(22050, 0);
        for (int s = 0; s < 100; ++s)
            peaks.add(second.data(), WaveformPeaks::Format::S16, qint64(second.size()));
        peaks.finish();
        QCOMPARE(peaks.size(), qsizetype(10000));
        QCOMPARE(peaks.frames(), qint64(2205000));
    }

    void peaksFoldChannels()
    {
        WaveformPeaks peaks;
        peaks.reset(100, 2);            // 1 frame per bucket
        const float s[] = {0.25f, -0.5f, 1.0f, 0.0f};
        peaks.add(s, WaveformPeaks::Format::F32, 2);
        QCOMPARE(peaks.size(), 2);
        QCOMPARE(peaks.peaks()[0].min, qint8(-64));
        QCOMPARE(peaks.peaks()[0].max, qint8(32));
        QCOMPARE(peaks.peaks()[1].max, qint8(127));
    }

    void peaksEveryFormatMapsFullScale()
    {
        struct Case {
            WaveformPeaks::Format format;
            QByteArray bytes;
        };
        // Each case holds one negative full-scale sample then one positive one.
        QByteArray s24;
        s24.append("\x00\x00\x80", 3);      // -8388608
        s24.append("\xFF\xFF\x7F", 3);      // +8388607
        QByteArray u8("\x00\xFF", 2);
        QByteArray s32;
        putLe32(s32, 0x80000000u);
        putLe32(s32, 0x7FFFFFFFu);
        QByteArray f64;
        const double d[] = {-1.0, 1.0};
        f64.append(reinterpret_cast<const char *>(d), sizeof d);
        const Case cases[] = {{WaveformPeaks::Format::S24, s24}, {WaveformPeaks::Format::U8, u8},
                              {WaveformPeaks::Format::S32, s32}, {WaveformPeaks::Format::F64, f64}};
        for (const Case &c : cases) {
            WaveformPeaks peaks;
            peaks.reset(200, 1);        // 2 frames per bucket
            peaks.add(c.bytes.constData(), c.format, 2);
            QCOMPARE(peaks.size(), 1);
            QCOMPARE(peaks.peaks()[0].min, qint8(-127));
            QVERIFY(peaks.peaks()[0].max >= 126);
        }
    }

    void peaksIgnoreNonFinite()
    {
        WaveformPeaks peaks;
        peaks.reset(100, 1);
        const float s[] = {std::nanf(""), INFINITY};
        peaks.add(s, WaveformPeaks::Format::F32, 2);
        QCOMPARE(peaks.peaks()[0].max, qint8(0));
        QCOMPARE(peaks.peaks()[1].max, qint8(0));
    }

    void peaksStopAtTheCap()
    {
        WaveformPeaks peaks;
        peaks.reset(100, 1);
        std::vector<quint8> s(size_t(WaveformPeaks::kMaxBuckets + 10), 200);
        peaks.add(s.data(), WaveformPeaks::Format::U8, qint64(s.size()));
        QCOMPARE(peaks.size(), qsizetype(WaveformPeaks::kMaxBuckets));
        QVERIFY(peaks.truncated());
    }

    void peaksRangeMergesBuckets()
    {
        WaveformPeaks peaks;
        peaks.reset(100, 1);
        const float s[] = {0.5f, -0.25f, 0.1f};
        peaks.add(s, WaveformPeaks::Format::F32, 3);
        float lo = 0, hi = 0;
        QVERIFY(peaks.range(0, 2, &lo, &hi));
        QCOMPARE(qRound(hi * 127), 64);
        QCOMPARE(qRound(lo * 127), -32);
        QVERIFY(!peaks.range(5, 9, &lo, &hi));
    }

    // ---- AudioFileInfo: WAV ----

    void wavPcm24WithLoop()
    {
        QByteArray data(48000 * 2 * 3, '\0');     // one second, stereo, 24-bit
        const QByteArray bytes = wav({fmtChunk(1, 2, 48000, 24), chunk("data", data), smplChunk(1000, 47999)});
        const AudioFileInfo::Info info = parse(bytes);
        QVERIFY(info.valid);
        QCOMPARE(info.container, QStringLiteral("WAV"));
        QCOMPARE(info.codec, QStringLiteral("PCM"));
        QCOMPARE(info.sampleRate, 48000);
        QCOMPARE(info.channels, 2);
        QCOMPARE(info.bitsPerSample, 24);
        QCOMPARE(info.durationMs, qint64(1000));
        QVERIFY(info.pcm == WaveformPeaks::Format::S24);
        QCOMPARE(info.dataOffset, qint64(12 + 8 + 16 + 8));
        QCOMPARE(info.dataBytes, qint64(data.size()));
        QVERIFY(info.hasLoop);
        QCOMPARE(info.loopStart, qint64(1000));
        QCOMPARE(info.loopEnd, qint64(48000));       // smpl end is inclusive
    }

    void wavFloatAndInfoTitle()
    {
        QByteArray list("INFO");
        list += chunk("INAM", QByteArray("Boss theme\0", 11));
        const QByteArray bytes = wav({fmtChunk(3, 1, 44100, 32), chunk("LIST", list),
                                      chunk("data", QByteArray(44100 * 4 / 2, '\0'))});
        const AudioFileInfo::Info info = parse(bytes);
        QVERIFY(info.valid);
        QCOMPARE(info.codec, QStringLiteral("IEEE float"));
        QVERIFY(info.pcm == WaveformPeaks::Format::F32);
        QCOMPARE(info.durationMs, qint64(500));
        QCOMPARE(info.title, QStringLiteral("Boss theme"));
        QVERIFY(!info.hasLoop);
    }

    void wavExtensibleUsesSubFormat()
    {
        QByteArray f;
        putLe16(f, 0xFFFE);
        putLe16(f, 2);
        putLe32(f, 44100);
        putLe32(f, 44100 * 4);
        putLe16(f, 4);
        putLe16(f, 16);
        putLe16(f, 22);          // cbSize
        putLe16(f, 16);          // valid bits
        putLe32(f, 3);           // channel mask
        putLe16(f, 1);           // sub-format GUID starts with the format tag (PCM)
        f.append(QByteArray::fromHex("000000001000800000aa00389b71"));
        const AudioFileInfo::Info info =
            parse(wav({chunk("fmt ", f), chunk("data", QByteArray(44100 * 4, '\0'))}));
        QVERIFY(info.valid);
        QCOMPARE(info.codec, QStringLiteral("PCM"));
        QVERIFY(info.pcm == WaveformPeaks::Format::S16);
        QCOMPARE(info.durationMs, qint64(1000));
    }

    void wavCompressedHasNoFastPath()
    {
        const AudioFileInfo::Info info =
            parse(wav({fmtChunk(2, 1, 22050, 4), chunk("data", QByteArray(100, '\0'))}));
        QVERIFY(info.valid);
        QCOMPARE(info.codec, QStringLiteral("MS ADPCM"));
        QVERIFY(!info.pcmFastPath());
    }

    void wavTruncatedDataIsClampedToFile()
    {
        QByteArray bytes = wav({fmtChunk(1, 1, 8000, 16), chunk("data", QByteArray(16000, '\0'))});
        bytes.chop(8000);                 // half the samples are missing
        const AudioFileInfo::Info info = parse(bytes);
        QVERIFY(info.valid);
        QCOMPARE(info.dataBytes, qint64(8000));
        QCOMPARE(info.durationMs, qint64(500));
    }

    // ---- AudioFileInfo: AIFF ----

    void aiffCommonChunk()
    {
        QByteArray comm;
        putBe16(comm, 2);                // channels
        putBe32(comm, 88200);            // frames
        putBe16(comm, 16);               // bits
        // 44100 as an 80-bit extended float.
        comm.append(QByteArray::fromHex("400EAC44000000000000"));
        QByteArray ssnd;
        putBe32(ssnd, 0);
        putBe32(ssnd, 0);
        ssnd += QByteArray(16, '\0');
        QByteArray body("AIFF");
        body += "COMM";
        putBe32(body, quint32(comm.size()));
        body += comm;
        body += "SSND";
        putBe32(body, quint32(ssnd.size()));
        body += ssnd;
        QByteArray bytes("FORM");
        putBe32(bytes, quint32(body.size()));
        bytes += body;

        const AudioFileInfo::Info info = parse(bytes);
        QVERIFY(info.valid);
        QCOMPARE(info.container, QStringLiteral("AIFF"));
        QCOMPARE(info.sampleRate, 44100);
        QCOMPARE(info.channels, 2);
        QCOMPARE(info.bitsPerSample, 16);
        QCOMPARE(info.durationMs, qint64(2000));
    }

    // ---- AudioFileInfo: FLAC ----

    void flacStreamInfoAndLoopTags()
    {
        QByteArray si(34, '\0');
        // Bytes 10..17: 20-bit rate, 3-bit channels-1, 5-bit bits-1, 36-bit total samples.
        const quint64 rate = 44100, channels = 2, bits = 16, total = 441000;
        const quint64 packed = (rate << 44) | ((channels - 1) << 41) | ((bits - 1) << 36) | total;
        qToBigEndian(packed, si.data() + 10);
        QByteArray bytes("fLaC");
        bytes.append(char(0x00));        // STREAMINFO, not last
        bytes.append(char(0)); bytes.append(char(0)); bytes.append(char(34));
        bytes += si;
        const QByteArray comments = vorbisComments({QStringLiteral("TITLE=Field"),
                                                    QStringLiteral("LOOPSTART=44100"),
                                                    QStringLiteral("LOOPEND=132300")}, false);
        bytes.append(char(0x84));        // VORBIS_COMMENT, last
        bytes.append(char((comments.size() >> 16) & 0xFF));
        bytes.append(char((comments.size() >> 8) & 0xFF));
        bytes.append(char(comments.size() & 0xFF));
        bytes += comments;
        bytes += QByteArray(64, '\xFF');

        const AudioFileInfo::Info info = parse(bytes);
        QVERIFY(info.valid);
        QCOMPARE(info.container, QStringLiteral("FLAC"));
        QCOMPARE(info.sampleRate, 44100);
        QCOMPARE(info.channels, 2);
        QCOMPARE(info.bitsPerSample, 16);
        QCOMPARE(info.durationMs, qint64(10000));
        QCOMPARE(info.title, QStringLiteral("Field"));
        QVERIFY(info.hasLoop);
        QCOMPARE(info.loopStart, qint64(44100));
        QCOMPARE(info.loopEnd, qint64(132300));
    }

    // ---- AudioFileInfo: Ogg ----

    void oggVorbisRpgMakerLoop()
    {
        const QByteArray bytes = vorbisFile(44100, 2, {QStringLiteral("LOOPSTART=44100"),
                                                       QStringLiteral("LOOPLENGTH=88200")}, 441000);
        const AudioFileInfo::Info info = parse(bytes);
        QVERIFY(info.valid);
        QCOMPARE(info.container, QStringLiteral("Ogg"));
        QCOMPARE(info.codec, QStringLiteral("Vorbis"));
        QCOMPARE(info.sampleRate, 44100);
        QCOMPARE(info.channels, 2);
        QCOMPARE(info.durationMs, qint64(10000));
        QCOMPARE(info.bitrate, qint64(128000));
        QVERIFY(info.hasLoop);
        QCOMPARE(info.loopStart, qint64(44100));
        QCOMPARE(info.loopEnd, qint64(132300));
    }

    void oggOpusUsesPreSkipAt48k()
    {
        QByteArray head("OpusHead");
        head.append(char(1));
        head.append(char(1));            // channels
        putLe16(head, 312);              // pre-skip
        putLe32(head, 44100);            // input rate (informational)
        putLe16(head, 0);
        head.append(char(0));
        QByteArray tags("OpusTags");
        tags += vorbisComments({QStringLiteral("title=Blip")}, false);
        QByteArray bytes = oggPage(head, 0, 0, 0x02);
        bytes += oggPage(tags, 0, 1, 0);
        bytes += oggPage(QByteArray(50, 'x'), 48000 + 312, 2, 0x04);
        const AudioFileInfo::Info info = parse(bytes);
        QVERIFY(info.valid);
        QCOMPARE(info.codec, QStringLiteral("Opus"));
        QCOMPARE(info.sampleRate, 48000);
        QCOMPARE(info.channels, 1);
        QCOMPARE(info.durationMs, qint64(1000));
        QCOMPARE(info.title, QStringLiteral("Blip"));
    }

    void loopEndBeforeStartIsDropped()
    {
        const AudioFileInfo::Info info =
            parse(vorbisFile(44100, 1, {QStringLiteral("LOOPSTART=500"), QStringLiteral("LOOPEND=100")}, 44100));
        QVERIFY(info.valid);
        QVERIFY(!info.hasLoop);
    }

    void garbageIsNotAudio()
    {
        QVERIFY(!parse(QByteArray("hello world, not audio")).valid);
        QVERIFY(!parse(QByteArray()).valid);
        // RIFF that is not WAVE.
        QByteArray avi("RIFF");
        putLe32(avi, 4);
        avi += "AVI ";
        QVERIFY(!parse(avi).valid);
        // A fmt chunk claiming more bytes than the file has.
        QByteArray bad("RIFF\x40\0\0\0WAVEfmt \xFF\xFF\0\0", 20);
        QVERIFY(!parse(bad).valid);
    }

    // ---- sniffers ----

    void sniffersRecogniseContainers()
    {
        QVERIFY(MediaSniffers::isAudio(wav({fmtChunk(1, 1, 8000, 8)})));
        QVERIFY(MediaSniffers::isAudio(QByteArray("ID3\x04\0\0\0\0\0\0", 10)));
        QVERIFY(MediaSniffers::isAudio(QByteArray("fLaC\0\0\0\x22", 8)));
        QVERIFY(MediaSniffers::isAudio(vorbisFile(8000, 1, {}, 8000)));
        QVERIFY(MediaSniffers::isAudio(QByteArray("\0\0\0\x20" "ftypM4A \0\0\0\0", 16)));
        QVERIFY(!MediaSniffers::isVideo(QByteArray("\0\0\0\x20" "ftypM4A \0\0\0\0", 16)));
        QVERIFY(MediaSniffers::isVideo(QByteArray("\0\0\0\x20" "ftypisom\0\0\0\0", 16)));
        QVERIFY(MediaSniffers::isVideo(QByteArray("\x1A\x45\xDF\xA3\x01\0\0\0", 8)));
        QVERIFY(MediaSniffers::isVideo(QByteArray("RIFF\0\0\0\0AVI LIST", 16)));
        QVERIFY(MediaSniffers::isPdf(QByteArray("%PDF-1.7\n")));
        QVERIFY(!MediaSniffers::isPdf(QByteArray("% PDF notes")));
        QVERIFY(!MediaSniffers::isAudio(QByteArray("RIFF\0\0\0\0AVI ", 12)));
        QVERIFY(!MediaSniffers::isVideo(QByteArray("plain text file")));
        // An Ogg stream with a Theora first packet is video.
        QByteArray theora("\x80theora", 7);
        theora += QByteArray(40, '\0');
        QVERIFY(MediaSniffers::isVideo(oggPage(theora, 0, 0, 0x02)));
        QVERIFY(!MediaSniffers::isAudio(oggPage(theora, 0, 0, 0x02)));
    }

    // ---- labels ----

    void labels()
    {
        QCOMPARE(MediaFormat::timeText(0), QStringLiteral("0:00"));
        QCOMPARE(MediaFormat::timeText(187'000), QStringLiteral("3:07"));
        QCOMPARE(MediaFormat::timeText(3'723'000), QStringLiteral("1:02:03"));
        QCOMPARE(MediaFormat::timeText(-5), QStringLiteral("0:00"));
        QCOMPARE(MediaFormat::preciseTimeText(1'234), QStringLiteral("0:01.234"));
        QCOMPARE(MediaFormat::bitrateText(128'000), QStringLiteral("128 kbps"));
        QCOMPARE(MediaFormat::bitrateText(1'400'000), QStringLiteral("1.4 Mbps"));
        QCOMPARE(MediaFormat::bitrateText(0), QString());
        QCOMPARE(MediaFormat::channelsText(1), QStringLiteral("mono"));
        QCOMPARE(MediaFormat::channelsText(2), QStringLiteral("stereo"));
        QCOMPARE(MediaFormat::channelsText(6), QStringLiteral("6 channels"));
    }
};

QTEST_GUILESS_MAIN(TestMediaFormats)
#include "test_media_formats.moc"
