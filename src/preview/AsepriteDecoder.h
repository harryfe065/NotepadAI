#ifndef ASEPRITEDECODER_H
#define ASEPRITEDECODER_H

#include "ImageDecoder.h"

#include <QByteArrayView>

#include <atomic>

// Aseprite (.ase/.aseprite) reader: RGBA, grayscale and indexed sprites,
// every frame flattened from its visible layers (raw, compressed, linked and
// tilemap cels, z-index order, layer/cel opacity, the separable blend modes)
// into an animation with per-frame durations and tag names.
namespace AsepriteDecoder {

bool looksLikeAseprite(QByteArrayView head);

// Decoded size (all frames) from the header, -1 when this is not a sprite.
qint64 estimate(QByteArrayView data);

ImageDecoder::Result decode(QByteArrayView data, const std::atomic_bool &cancel);

} // namespace AsepriteDecoder

#endif // ASEPRITEDECODER_H
