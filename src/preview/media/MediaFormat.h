#ifndef MEDIAFORMAT_H
#define MEDIAFORMAT_H

#include <QLocale>
#include <QString>

namespace MediaFormat {

// "3:07" or "1:02:03"; negative values show as 0.
inline QString timeText(qint64 ms)
{
    const qint64 total = ms > 0 ? ms / 1000 : 0;
    const qint64 h = total / 3600, m = (total / 60) % 60, s = total % 60;
    if (h > 0)
        return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

// "0:01.234": timeText with milliseconds, for loop points and the hover time.
inline QString preciseTimeText(qint64 ms)
{
    const qint64 clamped = ms > 0 ? ms : 0;
    return QStringLiteral("%1.%2").arg(timeText(clamped)).arg(clamped % 1000, 3, 10, QLatin1Char('0'));
}

inline QString channelsText(int channels)
{
    if (channels == 1)
        return QStringLiteral("mono");
    if (channels == 2)
        return QStringLiteral("stereo");
    return QStringLiteral("%1 channels").arg(channels);
}

// "128 kbps" / "1.4 Mbps" from bits per second; empty when unknown.
inline QString bitrateText(qint64 bitsPerSecond)
{
    if (bitsPerSecond <= 0)
        return {};
    if (bitsPerSecond >= 1'000'000)
        return QStringLiteral("%1 Mbps").arg(double(bitsPerSecond) / 1e6, 0, 'f', 1);
    return QStringLiteral("%1 kbps").arg(qint64(bitsPerSecond / 1000));
}

// One playback volume (linear 0..1) for every audio and video tab of the
// session, as a media player would. Header-inline: one instance per module.
inline float &sessionVolume()
{
    static float volume = 0.8f;
    return volume;
}

// File sizes as every asset info line shows them (ImageDecoder::formatBytes).
inline QString bytesText(qint64 bytes)
{
    return QLocale::system().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

// Info-line separator shared by the asset viewers.
inline QString separator()
{
    return QStringLiteral("  ·  ");
}

} // namespace MediaFormat

#endif // MEDIAFORMAT_H
