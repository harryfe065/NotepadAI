#ifndef RASTERDECODERS_H
#define RASTERDECODERS_H

#include "ImageDecoder.h"

#include <QByteArrayView>

#include <atomic>

// Single-image formats Qt does not read on every install: TGA and the PSD
// composite (stb_image, 8-bit straight ARGB32), Radiance HDR (stb_image) and
// OpenEXR (tinyexr; scanline and tiled, level 0 of the first part), both
// decoded to linear RGBA16F. Input is the whole file, memory-mapped.
namespace RasterDecoders {

enum class Kind : quint8 { Tga, Psd, Radiance, Exr };

bool looksLikePsd(QByteArrayView head);
bool looksLikeRadiance(QByteArrayView head);
bool looksLikeExr(QByteArrayView head);

// Decoded size from the header, -1 when the header cannot be read.
qint64 estimate(Kind kind, QByteArrayView data);

ImageDecoder::Result decode(Kind kind, QByteArrayView data, const std::atomic_bool &cancel);

} // namespace RasterDecoders

#endif // RASTERDECODERS_H
