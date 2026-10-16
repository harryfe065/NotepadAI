#ifndef AUDIOFILEINFO_H
#define AUDIOFILEINFO_H

#include "WaveformPeaks.h"

#include <QString>

class QIODevice;

// What an audio file says about itself, read from its own headers: Qt
// Multimedia reports neither bit depth nor loop points, and its sample rate
// is the decoder's output rate, not the file's.
//
// parse() reads only what it needs (chunk and block headers, the first and
// last 64 KB of an Ogg stream), seeking past sample data, so a 2 GB WAV costs
// a handful of small reads. Every length is bounds-checked against the
// device; a truncated file yields what could be read.
namespace AudioFileInfo {

struct Info {
    bool valid = false;
    QString container;      // "WAV", "AIFF", "FLAC", "Ogg", "MP3", "MPEG-4", ...
    QString codec;          // "PCM", "IEEE float", "Vorbis", "Opus", "MPEG-1 Layer III", ...
    QString title;
    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;  // 0 when the codec has no fixed depth
    qint64 frames = -1;     // per channel; -1 when unknown
    qint64 durationMs = -1;
    qint64 bitrate = 0;     // bits per second, 0 when unknown

    // Loop in frames, end exclusive (WAV smpl; LOOPSTART / LOOPLENGTH / LOOPEND comments).
    bool hasLoop = false;
    qint64 loopStart = 0;
    qint64 loopEnd = 0;

    // Little-endian interleaved PCM stored as-is (WAV, AIFC 'sowt'): the
    // waveform reads it straight from the file instead of decoding.
    bool pcmKnown = false;
    WaveformPeaks::Format pcm = WaveformPeaks::Format::S16;
    qint64 dataOffset = -1;
    qint64 dataBytes = 0;

    bool pcmFastPath() const { return valid && pcmKnown && dataOffset >= 0 && dataBytes > 0 && channels > 0; }
};

Info parse(QIODevice &device);

} // namespace AudioFileInfo

#endif // AUDIOFILEINFO_H
