#ifndef IMAGEDECODER_H
#define IMAGEDECODER_H

#include "ImageDocument.h"

#include <QString>

#include <atomic>

// Decodes an image file into an ImageDocument. Runs on a worker thread; the
// cancel flag is polled between frames / surfaces.
namespace ImageDecoder {

// Decoded-memory cap (pixels x bytes, all frames / surfaces).
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

// Decoded size from the header alone, -1 when the format cannot tell cheaply.
qint64 estimateDecodedBytes(const QString &path);

// `vector` forces the SVG path (a sniffed SVG without an .svg suffix).
Result decode(const QString &path, const std::atomic_bool &cancel, bool vector = false);

// Human label for a decoded size ("1.2 GB").
QString formatBytes(qint64 bytes);

// Result builders shared by the format decoders.
Result failed(const QString &error);
Result tooLarge(qint64 bytes);
Result cancelled();

} // namespace ImageDecoder

#endif // IMAGEDECODER_H
