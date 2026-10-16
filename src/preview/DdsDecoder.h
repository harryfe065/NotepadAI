#ifndef DDSDECODER_H
#define DDSDECODER_H

#include "ImageDecoder.h"

#include <QByteArrayView>

#include <atomic>

// DirectDraw Surface reader: legacy and DX10 headers; BC1-BC7 (UNORM, SNORM,
// sRGB, BC6H signed/unsigned), 8/16/32-bit, float and packed uncompressed
// formats, legacy bit-mask formats; mip chains, cubemaps (partial too),
// texture arrays and volume slices (mip 0 only). Input is the whole file
// (memory-mapped by the caller); nothing is copied before decoding.
namespace DdsDecoder {

bool looksLikeDds(QByteArrayView head);

// Decoded size from the header, -1 when this is not a DDS we can read.
qint64 estimate(QByteArrayView data);

ImageDecoder::Result decode(QByteArrayView data, const std::atomic_bool &cancel);

} // namespace DdsDecoder

#endif // DDSDECODER_H
