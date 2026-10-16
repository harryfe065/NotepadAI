#ifndef WAVEFORMPEAKS_H
#define WAVEFORMPEAKS_H

#include <QtGlobal>

#include <vector>

// Min / max envelope of decoded audio, 100 buckets per second, built
// incrementally while a decoder streams PCM in. Independent of Qt Multimedia:
// the caller names the sample format.
//
// add() is O(samples): runs of whole buckets reduced with min / max; memory is two
// bytes per bucket (a one-hour file is 720 KB). Past kMaxBuckets the envelope
// stops growing and truncated() says so.
class WaveformPeaks
{
public:
    // Interleaved little-endian samples; S24 is packed (3 bytes per sample).
    enum class Format { U8, S16, S24, S32, F32, F64 };

    struct Peak {
        qint8 min = 0;     // -127..127 of full scale
        qint8 max = 0;
    };

    static constexpr int kBucketsPerSecond = 100;
    static constexpr qint64 kMaxBuckets = 1'000'000;   // 2.8 hours

    void reset(int sampleRate, int channels);
    // `frames` interleaved frames of `channels` samples each.
    void add(const void *data, Format format, qint64 frames);
    // Flushes the partial bucket at the end of the stream.
    void finish();

    qsizetype size() const { return qsizetype(m_peaks.size()); }
    const std::vector<Peak> &peaks() const { return m_peaks; }
    bool truncated() const { return m_truncated; }
    qint64 frames() const { return m_frames; }
    int sampleRate() const { return m_rate; }
    int channels() const { return m_channels; }
    double seconds() const { return m_rate > 0 ? double(m_frames) / m_rate : 0.0; }

    static int bytesPerSample(Format format);

    // Envelope over buckets [first, last) as -1..1; false when the range is empty.
    bool range(qsizetype first, qsizetype last, float *lo, float *hi) const;

private:
    // `sample(i)` returns interleaved sample i as -1..1.
    template <typename Sample>
    void accumulate(qint64 frames, Sample sample);
    void flushBucket();

    std::vector<Peak> m_peaks;
    int m_rate = 0;
    int m_channels = 1;
    int m_framesPerBucket = 441;
    int m_inBucket = 0;
    float m_lo = 1.0f, m_hi = -1.0f;
    qint64 m_frames = 0;
    bool m_truncated = false;
};

#endif // WAVEFORMPEAKS_H
