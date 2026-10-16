#ifndef IMAGEDECODER_H
#define IMAGEDECODER_H

#include "ImageDocument.h"

#include <QString>

#include <atomic>
#include <functional>
#include <limits>

// Decodes an image file into an ImageDocument. Runs on a worker thread; the
// cancel flag is polled between frames / surfaces.
namespace ImageDecoder {

// Decoded-memory cap (peak bytes while decoding, all frames / surfaces).
constexpr qint64 kMaxDecodedBytes = qint64(1) << 30;
// Above this the viewer shows a cancellable "Decoding…" banner.
constexpr qint64 kProgressBytes = qint64(256) << 20;

struct Result {
    ImageDocumentPtr doc;
    ImagePyramidPtr pyramid;   // of surface 0 (still images)
    QString error;             // set when doc is null
    bool tooLarge = false;
    bool cancelled = false;
};

// Called on the decoding thread with the expected decoded size as soon as the
// header gives it (and it is within the cap), before the heavy work.
using SizeCallback = std::function<void(qint64 bytes)>;

// `vector` forces the SVG path (a sniffed SVG without an .svg suffix). The
// size estimate and the "too large" decision happen here, on the caller's
// (worker) thread; a file over the cap comes back as a tooLarge() result.
Result decode(const QString &path, const std::atomic_bool &cancel, bool vector = false,
              const SizeCallback &onSize = {});

// Size arithmetic on non-negative counts read from a file: saturates at the
// qint64 maximum instead of overflowing, so the result just fails every cap.
constexpr qint64 mulSat(qint64 a, qint64 b)
{
    return (a != 0 && b > std::numeric_limits<qint64>::max() / a) ? std::numeric_limits<qint64>::max() : a * b;
}
constexpr qint64 addSat(qint64 a, qint64 b)
{
    return b > std::numeric_limits<qint64>::max() - a ? std::numeric_limits<qint64>::max() : a + b;
}

// Human label for a decoded size ("1.2 GB").
QString formatBytes(qint64 bytes);

// Result builders shared by the format decoders.
Result failed(const QString &error);
Result tooLarge(qint64 bytes);
Result cancelled();

} // namespace ImageDecoder

#endif // IMAGEDECODER_H
