#include "WaveformPeaks.h"

#include <algorithm>
#include <cmath>
#include <cstring>

void WaveformPeaks::reset(int sampleRate, int channels)
{
    m_peaks.clear();
    m_taken = 0;
    m_rate = std::max(1, sampleRate);
    m_channels = std::max(1, channels);
    m_bucket = 0;
    m_bucketEnd = bucketStart(1);
    m_lo = 1.0f;
    m_hi = -1.0f;
    m_frames = 0;
    m_truncated = false;
}

void WaveformPeaks::flushBucket()
{
    if (size() >= kMaxBuckets) {
        m_truncated = true;
    } else {
        Peak p;
        if (m_hi >= m_lo) {
            p.min = qint8(std::lround(std::clamp(m_lo, -1.0f, 1.0f) * 127.0f));
            p.max = qint8(std::lround(std::clamp(m_hi, -1.0f, 1.0f) * 127.0f));
        }
        m_peaks.push_back(p);
    }
    ++m_bucket;
    m_bucketEnd = bucketStart(m_bucket + 1);
    m_lo = 1.0f;
    m_hi = -1.0f;
}

template <typename Sample>
void WaveformPeaks::accumulate(qint64 frames, Sample sample)
{
    // Runs of whole buckets: the inner loop is a plain min/max reduction with
    // no per-frame bookkeeping. All channels of a frame fold into one envelope.
    const qint64 channels = m_channels;
    qint64 index = 0;
    qint64 left = frames;
    while (left > 0 && !m_truncated) {
        const qint64 take = std::min<qint64>(left, m_bucketEnd - m_frames);
        const qint64 end = index + take * channels;
        float lo = m_lo, hi = m_hi;
        for (qint64 i = index; i < end; ++i) {
            const float s = sample(i);
            lo = std::min(lo, s);
            hi = std::max(hi, s);
        }
        m_lo = lo;
        m_hi = hi;
        index = end;
        left -= take;
        m_frames += take;
        if (m_frames >= m_bucketEnd)
            flushBucket();
    }
    // Frames past a truncated envelope still count toward the length.
    m_frames += left;
}

void WaveformPeaks::add(const void *data, Format format, qint64 frames)
{
    if (!data || frames <= 0 || m_truncated)
        return;
    // memcpy loads: callers hand in file blocks with no alignment guarantee.
    const auto *bytes = static_cast<const unsigned char *>(data);
    switch (format) {
    case Format::U8:
        accumulate(frames, [bytes](qint64 i) { return (float(bytes[i]) - 128.0f) / 128.0f; });
        break;
    case Format::S16:
        accumulate(frames, [bytes](qint64 i) {
            qint16 v;
            std::memcpy(&v, bytes + i * 2, 2);
            return float(v) / 32768.0f;
        });
        break;
    case Format::S24:
        accumulate(frames, [bytes](qint64 i) {
            const unsigned char *p = bytes + i * 3;
            // Sign-extend through the top byte of a 32-bit value.
            const qint32 v = qint32(quint32(p[0]) << 8 | quint32(p[1]) << 16 | quint32(p[2]) << 24) >> 8;
            return float(v) / 8388608.0f;
        });
        break;
    case Format::S32:
        accumulate(frames, [bytes](qint64 i) {
            qint32 v;
            std::memcpy(&v, bytes + i * 4, 4);
            return float(double(v) / 2147483648.0);
        });
        break;
    case Format::F32:
        accumulate(frames, [bytes](qint64 i) {
            float v;
            std::memcpy(&v, bytes + i * 4, 4);
            return std::isfinite(v) ? v : 0.0f;
        });
        break;
    case Format::F64:
        accumulate(frames, [bytes](qint64 i) {
            double v;
            std::memcpy(&v, bytes + i * 8, 8);
            return std::isfinite(v) ? float(v) : 0.0f;
        });
        break;
    }
}

int WaveformPeaks::bytesPerSample(Format format)
{
    switch (format) {
    case Format::U8: return 1;
    case Format::S16: return 2;
    case Format::S24: return 3;
    case Format::S32: return 4;
    case Format::F32: return 4;
    case Format::F64: return 8;
    }
    return 1;
}

void WaveformPeaks::finish()
{
    if (m_frames > bucketStart(m_bucket))
        flushBucket();
}

std::vector<WaveformPeaks::Peak> WaveformPeaks::takePeaks()
{
    std::vector<Peak> out;
    out.swap(m_peaks);
    m_taken += qsizetype(out.size());
    return out;
}

bool WaveformPeaks::fold(const Peak *peaks, qsizetype count, qsizetype first, qsizetype last, qint8 *lo,
                         qint8 *hi)
{
    first = std::max<qsizetype>(0, first);
    last = std::min(last, count);
    if (first >= last)
        return false;
    qint8 mn = 127, mx = -127;
    for (qsizetype i = first; i < last; ++i) {
        mn = std::min(mn, peaks[i].min);
        mx = std::max(mx, peaks[i].max);
    }
    *lo = mn;
    *hi = mx;
    return true;
}

bool WaveformPeaks::range(qsizetype first, qsizetype last, float *lo, float *hi) const
{
    qint8 mn = 0, mx = 0;
    if (!fold(m_peaks.data(), qsizetype(m_peaks.size()), first, last, &mn, &mx))
        return false;
    *lo = float(mn) / 127.0f;
    *hi = float(mx) / 127.0f;
    return true;
}
