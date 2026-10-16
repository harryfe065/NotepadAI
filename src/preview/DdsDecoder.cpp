#include "DdsDecoder.h"

#include "bcdec.h"

#include <QFloat16>
#include <QObject>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace DdsDecoder {

namespace {

// --- header constants ---------------------------------------------------------

constexpr quint32 kMagic = 0x20534444; // "DDS "
constexpr int kHeaderSize = 124;
constexpr int kDx10Size = 20;

constexpr quint32 DDSD_MIPMAPCOUNT = 0x20000;
constexpr quint32 DDPF_ALPHAPIXELS = 0x1;
constexpr quint32 DDPF_ALPHA = 0x2;
constexpr quint32 DDPF_FOURCC = 0x4;
constexpr quint32 DDPF_RGB = 0x40;
constexpr quint32 DDPF_LUMINANCE = 0x20000;
constexpr quint32 DDPF_BUMPDUDV = 0x80000;
constexpr quint32 DDSCAPS2_CUBEMAP = 0x200;
constexpr quint32 DDSCAPS2_VOLUME = 0x200000;
constexpr quint32 kCubeFaceBits[6] = {0x400, 0x800, 0x1000, 0x2000, 0x4000, 0x8000};
constexpr quint32 DX10_MISC_TEXTURECUBE = 0x4;
constexpr quint32 DX10_DIMENSION_TEXTURE3D = 4;

constexpr quint32 fourCC(char a, char b, char c, char d)
{
    return quint32(quint8(a)) | (quint32(quint8(b)) << 8) | (quint32(quint8(c)) << 16) | (quint32(quint8(d)) << 24);
}

// --- pixel formats --------------------------------------------------------------

enum class Px : quint8 {
    Invalid,
    BC1, BC2, BC3, BC4, BC4S, BC5, BC5S, BC6H, BC6HS, BC7,
    RGBA8, BGRA8, BGRX8, RGB10A2, RGBA16, RG16, R16, R8, RG8, A8,
    B5G6R5, B5G5R5A1, B4G4R4A4,
    RGBA16F, RG16F, R16F, RGBA32F, RGB32F, RG32F, R32F, R11G11B10F, RGB9E5,
    Masked, // legacy uncompressed described by bit masks
};

struct Format {
    Px px = Px::Invalid;
    bool srgb = false;
    QString name;
    // Masked only
    int bitCount = 0;
    quint32 masks[4] = {0, 0, 0, 0}; // r g b a
    bool luminance = false;
};

bool isBlock(Px p) { return p >= Px::BC1 && p <= Px::BC7; }
bool isHdr(Px p) { return p == Px::BC6H || p == Px::BC6HS || (p >= Px::RGBA16F && p <= Px::RGB9E5); }

int blockBytes(Px p)
{
    switch (p) {
    case Px::BC1: case Px::BC4: case Px::BC4S: return 8;
    default: return 16;
    }
}

int bitsPerPixel(const Format &f)
{
    switch (f.px) {
    case Px::RGBA8: case Px::BGRA8: case Px::BGRX8: case Px::RGB10A2: case Px::RG16:
    case Px::RG16F: case Px::R32F: case Px::R11G11B10F: case Px::RGB9E5: return 32;
    case Px::RGBA16: case Px::RGBA16F: case Px::RG32F: return 64;
    case Px::RGBA32F: return 128;
    case Px::RGB32F: return 96;
    case Px::R16: case Px::RG8: case Px::B5G6R5: case Px::B5G5R5A1: case Px::B4G4R4A4: case Px::R16F: return 16;
    case Px::R8: case Px::A8: return 8;
    case Px::Masked: return f.bitCount;
    default: return 0;
    }
}

Format fromDxgi(quint32 dxgi)
{
    Format f;
    auto set = [&f](Px p, const char *name, bool srgb = false) {
        f.px = p;
        f.name = QString::fromLatin1(name);
        f.srgb = srgb;
    };
    switch (dxgi) {
    case 1: case 2: set(Px::RGBA32F, "RGBA32F"); break;
    case 5: case 6: set(Px::RGB32F, "RGB32F"); break;
    case 9: case 10: set(Px::RGBA16F, "RGBA16F"); break;
    case 11: set(Px::RGBA16, "RGBA16"); break;
    case 15: case 16: set(Px::RG32F, "RG32F"); break;
    case 23: case 24: set(Px::RGB10A2, "RGB10A2"); break;
    case 26: set(Px::R11G11B10F, "R11G11B10F"); break;
    case 27: case 28: set(Px::RGBA8, "RGBA8"); break;
    case 29: set(Px::RGBA8, "RGBA8 sRGB", true); break;
    case 33: case 34: set(Px::RG16F, "RG16F"); break;
    case 35: set(Px::RG16, "RG16"); break;
    case 39: case 41: set(Px::R32F, "R32F"); break;
    case 48: case 49: set(Px::RG8, "RG8"); break;
    case 53: case 54: set(Px::R16F, "R16F"); break;
    case 56: set(Px::R16, "R16"); break;
    case 60: case 61: set(Px::R8, "R8"); break;
    case 65: set(Px::A8, "A8"); break;
    case 67: set(Px::RGB9E5, "RGB9E5"); break;
    case 70: case 71: set(Px::BC1, "BC1"); break;
    case 72: set(Px::BC1, "BC1 sRGB", true); break;
    case 73: case 74: set(Px::BC2, "BC2"); break;
    case 75: set(Px::BC2, "BC2 sRGB", true); break;
    case 76: case 77: set(Px::BC3, "BC3"); break;
    case 78: set(Px::BC3, "BC3 sRGB", true); break;
    case 79: case 80: set(Px::BC4, "BC4"); break;
    case 81: set(Px::BC4S, "BC4 SNORM"); break;
    case 82: case 83: set(Px::BC5, "BC5"); break;
    case 84: set(Px::BC5S, "BC5 SNORM"); break;
    case 85: set(Px::B5G6R5, "B5G6R5"); break;
    case 86: set(Px::B5G5R5A1, "B5G5R5A1"); break;
    case 87: case 90: set(Px::BGRA8, "BGRA8"); break;
    case 88: case 92: set(Px::BGRX8, "BGRX8"); break;
    case 91: set(Px::BGRA8, "BGRA8 sRGB", true); break;
    case 93: set(Px::BGRX8, "BGRX8 sRGB", true); break;
    case 94: case 95: set(Px::BC6H, "BC6H UF16"); break;
    case 96: set(Px::BC6HS, "BC6H SF16"); break;
    case 97: case 98: set(Px::BC7, "BC7"); break;
    case 99: set(Px::BC7, "BC7 sRGB", true); break;
    case 115: set(Px::B4G4R4A4, "B4G4R4A4"); break;
    default: break;
    }
    return f;
}

Format fromLegacy(quint32 pfFlags, quint32 cc, quint32 bitCount, const quint32 masks[4])
{
    Format f;
    auto set = [&f](Px p, const char *name) {
        f.px = p;
        f.name = QString::fromLatin1(name);
    };
    if (pfFlags & DDPF_FOURCC) {
        switch (cc) {
        case fourCC('D', 'X', 'T', '1'): set(Px::BC1, "BC1 (DXT1)"); break;
        case fourCC('D', 'X', 'T', '2'): set(Px::BC2, "BC2 (DXT2)"); break;
        case fourCC('D', 'X', 'T', '3'): set(Px::BC2, "BC2 (DXT3)"); break;
        case fourCC('D', 'X', 'T', '4'): set(Px::BC3, "BC3 (DXT4)"); break;
        case fourCC('D', 'X', 'T', '5'): set(Px::BC3, "BC3 (DXT5)"); break;
        case fourCC('A', 'T', 'I', '1'): case fourCC('B', 'C', '4', 'U'): set(Px::BC4, "BC4 (ATI1)"); break;
        case fourCC('B', 'C', '4', 'S'): set(Px::BC4S, "BC4 SNORM"); break;
        case fourCC('A', 'T', 'I', '2'): case fourCC('B', 'C', '5', 'U'): set(Px::BC5, "BC5 (ATI2)"); break;
        case fourCC('B', 'C', '5', 'S'): set(Px::BC5S, "BC5 SNORM"); break;
        case 36: set(Px::RGBA16, "RGBA16"); break;
        case 111: set(Px::R16F, "R16F"); break;
        case 112: set(Px::RG16F, "RG16F"); break;
        case 113: set(Px::RGBA16F, "RGBA16F"); break;
        case 114: set(Px::R32F, "R32F"); break;
        case 115: set(Px::RG32F, "RG32F"); break;
        case 116: set(Px::RGBA32F, "RGBA32F"); break;
        default: break;
        }
        return f;
    }
    if (bitCount != 8 && bitCount != 16 && bitCount != 24 && bitCount != 32)
        return f;
    if (!(pfFlags & (DDPF_RGB | DDPF_LUMINANCE | DDPF_ALPHA | DDPF_BUMPDUDV)))
        return f;
    f.px = Px::Masked;
    f.bitCount = int(bitCount);
    f.luminance = (pfFlags & DDPF_LUMINANCE) != 0;
    for (int i = 0; i < 4; ++i)
        f.masks[i] = masks[i];
    if (!(pfFlags & (DDPF_ALPHAPIXELS | DDPF_ALPHA)))
        f.masks[3] = 0;
    if ((pfFlags & DDPF_ALPHA) && !(pfFlags & (DDPF_RGB | DDPF_LUMINANCE)))
        f.masks[0] = f.masks[1] = f.masks[2] = 0;
    f.name = f.luminance ? QStringLiteral("L%1").arg(bitCount)
           : (f.masks[0] | f.masks[1] | f.masks[2]) == 0 ? QStringLiteral("A%1").arg(bitCount)
           : QStringLiteral("%1-bit RGB%2").arg(bitCount).arg(f.masks[3] ? QStringLiteral("A") : QString());
    return f;
}

// --- layout -----------------------------------------------------------------------

struct Layout {
    Format format;
    int width = 0, height = 0;
    int mips = 1, faces = 1, layers = 1;
    bool volume = false;
    QStringList faceNames;
    qsizetype dataOffset = 0;
};

qint64 surfaceBytes(const Format &f, int w, int h)
{
    if (isBlock(f.px))
        return qint64(std::max(1, (w + 3) / 4)) * std::max(1, (h + 3) / 4) * blockBytes(f.px);
    return qint64((qint64(w) * bitsPerPixel(f) + 7) / 8) * h;
}

int mipDim(int d, int mip) { return std::max(1, d >> mip); }

quint32 rd32(const uchar *p) { return qFromLittleEndian<quint32>(p); }

bool parse(QByteArrayView data, Layout &out, QString *error)
{
    const auto *b = reinterpret_cast<const uchar *>(data.data());
    if (data.size() < 4 + kHeaderSize || rd32(b) != kMagic || rd32(b + 4) != kHeaderSize) {
        if (error) *error = QObject::tr("Not a DDS file.");
        return false;
    }
    const uchar *h = b + 4;
    const quint32 flags = rd32(h + 4);
    out.height = int(rd32(h + 8));
    out.width = int(rd32(h + 12));
    const quint32 depth = rd32(h + 20);
    const quint32 mipCount = rd32(h + 24);
    const quint32 pfFlags = rd32(h + 76);
    const quint32 cc = rd32(h + 80);
    const quint32 bitCount = rd32(h + 84);
    const quint32 masks[4] = {rd32(h + 88), rd32(h + 92), rd32(h + 96), rd32(h + 100)};
    const quint32 caps2 = rd32(h + 108);
    out.dataOffset = 4 + kHeaderSize;

    if (out.width <= 0 || out.height <= 0 || out.width > 65536 || out.height > 65536) {
        if (error) *error = QObject::tr("The DDS header has an invalid size.");
        return false;
    }

    if ((pfFlags & DDPF_FOURCC) && cc == fourCC('D', 'X', '1', '0')) {
        if (data.size() < out.dataOffset + kDx10Size) {
            if (error) *error = QObject::tr("The DDS file is truncated.");
            return false;
        }
        const uchar *x = b + out.dataOffset;
        out.format = fromDxgi(rd32(x));
        const quint32 dimension = rd32(x + 4);
        const quint32 misc = rd32(x + 8);
        const quint32 arraySize = std::max<quint32>(1, rd32(x + 12));
        out.dataOffset += kDx10Size;
        if (out.format.px == Px::Invalid) {
            if (error) *error = QObject::tr("DXGI format %1 is not supported.").arg(rd32(x));
            return false;
        }
        out.volume = dimension == DX10_DIMENSION_TEXTURE3D && depth > 1;
        out.faces = (misc & DX10_MISC_TEXTURECUBE) ? 6 : 1;
        out.layers = out.volume ? int(std::min<quint32>(depth, 65536)) : int(std::min<quint32>(arraySize, 65536));
    } else {
        out.format = fromLegacy(pfFlags, cc, bitCount, masks);
        if (out.format.px == Px::Invalid) {
            if (error) *error = QObject::tr("This DDS pixel format is not supported.");
            return false;
        }
        out.volume = (caps2 & DDSCAPS2_VOLUME) && depth > 1;
        if (out.volume)
            out.layers = int(std::min<quint32>(depth, 65536));
        if (caps2 & DDSCAPS2_CUBEMAP) {
            static const char *const names[6] = {"+X", "−X", "+Y", "−Y", "+Z", "−Z"};
            out.faces = 0;
            for (int i = 0; i < 6; ++i) {
                if (caps2 & kCubeFaceBits[i]) {
                    ++out.faces;
                    out.faceNames << QString::fromUtf8(names[i]);
                }
            }
            if (out.faces == 0) {
                out.faces = 6;
                out.faceNames.clear();
            }
        }
    }
    if (out.faces == 6 && out.faceNames.isEmpty())
        out.faceNames = {QStringLiteral("+X"), QStringLiteral("−X"), QStringLiteral("+Y"),
                         QStringLiteral("−Y"), QStringLiteral("+Z"), QStringLiteral("−Z")};
    if (out.faces == 1)
        out.faceNames.clear();

    const int maxMips = 1 + int(std::floor(std::log2(std::max(out.width, out.height))));
    // Some writers set the count but forget DDSD_MIPMAPCOUNT; trust the count.
    out.mips = ((flags & DDSD_MIPMAPCOUNT) || mipCount > 1)
                   ? std::clamp(int(std::min<quint32>(mipCount, 64)), 1, maxMips) : 1;

    if (out.volume) {
        // Volume slices of mip 0 only: lower mips have fewer slices, which
        // the (layer, face, mip) model cannot express.
        out.mips = 1;
        out.faces = 1;
        out.faceNames.clear();
        return true;
    }

    // Short files: drop trailing mips that are missing; fail when even the
    // first mip of every face/layer is not there.
    qint64 chain = 0;
    for (int m = 0; m < out.mips; ++m)
        chain += surfaceBytes(out.format, mipDim(out.width, m), mipDim(out.height, m));
    const qint64 available = data.size() - out.dataOffset;
    if (chain * out.faces * out.layers > available) {
        if (out.faces * out.layers == 1) {
            qint64 acc = 0;
            int fit = 0;
            for (int m = 0; m < out.mips; ++m) {
                acc += surfaceBytes(out.format, mipDim(out.width, m), mipDim(out.height, m));
                if (acc > available) break;
                fit = m + 1;
            }
            out.mips = fit;
        } else {
            out.mips = 0;
        }
        if (out.mips == 0) {
            if (error) *error = QObject::tr("The DDS file is truncated.");
            return false;
        }
    }
    return true;
}

qint64 decodedBytes(const Layout &l)
{
    const int bpp = isHdr(l.format.px) ? 8 : 4;
    qint64 chain = 0;
    for (int m = 0; m < l.mips; ++m)
        chain += qint64(mipDim(l.width, m)) * mipDim(l.height, m) * bpp;
    return chain * l.faces * l.layers;
}

// --- pixel conversion ---------------------------------------------------------------

inline quint32 argb(quint32 r, quint32 g, quint32 b, quint32 a) { return (a << 24) | (r << 16) | (g << 8) | b; }

inline quint8 snormToUnorm(qint8 s)
{
    const int v = std::max<int>(s, -127) + 127; // [-127,127] -> [0,254]
    return quint8((v * 255 + 127) / 254);
}

// Expands an n-bit field (mask) to 8 bits.
struct MaskField {
    int shift = 0, bits = 0;
    explicit MaskField(quint32 mask)
    {
        if (!mask) return;
        while (!(mask & 1u)) { mask >>= 1; ++shift; }
        while (mask & 1u) { mask >>= 1; ++bits; }
    }
    quint32 get(quint32 v) const
    {
        if (!bits) return 0;
        const quint32 x = (v >> shift) & ((bits >= 32) ? 0xffffffffu : ((1u << bits) - 1));
        if (bits >= 8) return x >> (bits - 8);
        const quint32 max = (1u << bits) - 1;
        return (x * 255 + max / 2) / max;
    }
};

float unpackSmallFloat(quint32 v, int mantBits)
{
    const quint32 e = v >> mantBits;
    const quint32 m = v & ((1u << mantBits) - 1);
    if (e == 0)
        return m ? std::ldexp(float(m), -14 - mantBits) : 0.0f;
    if (e == 31)
        return m ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
    return std::ldexp(1.0f + float(m) / float(1u << mantBits), int(e) - 15);
}

// One row of an uncompressed LDR format into straight ARGB32.
void convertLdrRow(const Format &f, const uchar *src, quint32 *dst, int w)
{
    switch (f.px) {
    case Px::RGBA8:
        for (int x = 0; x < w; ++x, src += 4) dst[x] = argb(src[0], src[1], src[2], src[3]);
        break;
    case Px::BGRA8:
        std::memcpy(dst, src, size_t(w) * 4); // memory order B G R A == ARGB32 on little endian
        break;
    case Px::BGRX8:
        for (int x = 0; x < w; ++x) dst[x] = qFromLittleEndian<quint32>(src + 4 * x) | 0xff000000u;
        break;
    case Px::RGB10A2:
        for (int x = 0; x < w; ++x) {
            const quint32 v = qFromLittleEndian<quint32>(src + 4 * x);
            dst[x] = argb((v & 0x3ff) >> 2, ((v >> 10) & 0x3ff) >> 2, ((v >> 20) & 0x3ff) >> 2, ((v >> 30) * 255) / 3);
        }
        break;
    case Px::RGBA16:
        for (int x = 0; x < w; ++x, src += 8) dst[x] = argb(src[1], src[3], src[5], src[7]);
        break;
    case Px::RG16:
        for (int x = 0; x < w; ++x, src += 4) dst[x] = argb(src[1], src[3], 0, 255);
        break;
    case Px::R16:
        for (int x = 0; x < w; ++x, src += 2) dst[x] = argb(src[1], src[1], src[1], 255);
        break;
    case Px::R8:
        for (int x = 0; x < w; ++x) dst[x] = argb(src[x], src[x], src[x], 255);
        break;
    case Px::RG8:
        for (int x = 0; x < w; ++x, src += 2) dst[x] = argb(src[0], src[1], 0, 255);
        break;
    case Px::A8:
        for (int x = 0; x < w; ++x) dst[x] = argb(0, 0, 0, src[x]);
        break;
    case Px::B5G6R5:
        for (int x = 0; x < w; ++x) {
            const quint32 v = qFromLittleEndian<quint16>(src + 2 * x);
            const quint32 r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
            dst[x] = argb((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255);
        }
        break;
    case Px::B5G5R5A1:
        for (int x = 0; x < w; ++x) {
            const quint32 v = qFromLittleEndian<quint16>(src + 2 * x);
            const quint32 r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
            dst[x] = argb((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), (v >> 15) ? 255 : 0);
        }
        break;
    case Px::B4G4R4A4:
        for (int x = 0; x < w; ++x) {
            const quint32 v = qFromLittleEndian<quint16>(src + 2 * x);
            dst[x] = argb(((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, ((v >> 12) & 15) * 17);
        }
        break;
    case Px::Masked: {
        const MaskField r(f.masks[0]), g(f.masks[1]), b(f.masks[2]), a(f.masks[3]);
        const int bytes = f.bitCount / 8;
        for (int x = 0; x < w; ++x, src += bytes) {
            quint32 v = 0;
            for (int i = 0; i < bytes; ++i) v |= quint32(src[i]) << (8 * i);
            const quint32 av = f.masks[3] ? a.get(v) : 255;
            if (f.luminance) {
                const quint32 l = r.get(v);
                dst[x] = argb(l, l, l, av);
            } else {
                dst[x] = argb(r.get(v), g.get(v), b.get(v), av);
            }
        }
        break;
    }
    default:
        break;
    }
}

// One row of an uncompressed HDR format into RGBA16F.
void convertHdrRow(const Format &f, const uchar *src, qfloat16 *dst, int w, float *scratch)
{
    const qfloat16 one(1.0f), zero(0.0f);
    switch (f.px) {
    case Px::RGBA16F:
        std::memcpy(dst, src, size_t(w) * 8);
        return;
    case Px::RG16F:
        for (int x = 0; x < w; ++x) {
            std::memcpy(dst + 4 * x, src + 4 * x, 4);
            dst[4 * x + 2] = zero;
            dst[4 * x + 3] = one;
        }
        return;
    case Px::R16F:
        for (int x = 0; x < w; ++x) {
            qfloat16 r;
            std::memcpy(&r, src + 2 * x, 2);
            dst[4 * x] = dst[4 * x + 1] = dst[4 * x + 2] = r;
            dst[4 * x + 3] = one;
        }
        return;
    case Px::RGBA32F:
        // Bulk float -> half (F16C when available).
        std::memcpy(scratch, src, size_t(w) * 16);
        qFloatToFloat16(dst, scratch, qsizetype(w) * 4);
        return;
    default:
        break;
    }
    // Remaining formats expand to float RGBA first, then convert in bulk.
    for (int x = 0; x < w; ++x) {
        float *o = scratch + 4 * x;
        switch (f.px) {
        case Px::RGB32F:
            std::memcpy(o, src + 12 * x, 12);
            o[3] = 1.0f;
            break;
        case Px::RG32F:
            std::memcpy(o, src + 8 * x, 8);
            o[2] = 0.0f;
            o[3] = 1.0f;
            break;
        case Px::R32F:
            std::memcpy(o, src + 4 * x, 4);
            o[1] = o[2] = o[0];
            o[3] = 1.0f;
            break;
        case Px::R11G11B10F: {
            const quint32 v = qFromLittleEndian<quint32>(src + 4 * x);
            o[0] = unpackSmallFloat(v & 0x7ff, 6);
            o[1] = unpackSmallFloat((v >> 11) & 0x7ff, 6);
            o[2] = unpackSmallFloat((v >> 22) & 0x3ff, 5);
            o[3] = 1.0f;
            break;
        }
        case Px::RGB9E5: {
            const quint32 v = qFromLittleEndian<quint32>(src + 4 * x);
            const float scale = std::ldexp(1.0f, int(v >> 27) - 15 - 9);
            o[0] = float(v & 0x1ff) * scale;
            o[1] = float((v >> 9) & 0x1ff) * scale;
            o[2] = float((v >> 18) & 0x1ff) * scale;
            o[3] = 1.0f;
            break;
        }
        default:
            o[0] = o[1] = o[2] = 0.0f;
            o[3] = 1.0f;
            break;
        }
    }
    qFloatToFloat16(dst, scratch, qsizetype(w) * 4);
}

// Decodes one 4x4 LDR block into 16 straight ARGB32 pixels.
void decodeLdrBlock(Px px, const uchar *block, quint32 out[16])
{
    switch (px) {
    case Px::BC1: case Px::BC2: case Px::BC3: case Px::BC7: {
        uchar rgba[64];
        if (px == Px::BC1) bcdec_bc1(block, rgba, 16);
        else if (px == Px::BC2) bcdec_bc2(block, rgba, 16);
        else if (px == Px::BC3) bcdec_bc3(block, rgba, 16);
        else bcdec_bc7(block, rgba, 16);
        for (int i = 0; i < 16; ++i)
            out[i] = argb(rgba[4 * i], rgba[4 * i + 1], rgba[4 * i + 2], rgba[4 * i + 3]);
        break;
    }
    case Px::BC4: case Px::BC4S: {
        uchar r[16];
        bcdec_bc4(block, r, 4, px == Px::BC4S);
        for (int i = 0; i < 16; ++i) {
            const quint32 v = px == Px::BC4S ? snormToUnorm(qint8(r[i])) : r[i];
            out[i] = argb(v, v, v, 255);
        }
        break;
    }
    case Px::BC5: case Px::BC5S: {
        uchar rg[32];
        bcdec_bc5(block, rg, 8, px == Px::BC5S);
        for (int i = 0; i < 16; ++i) {
            quint32 r = rg[2 * i], g = rg[2 * i + 1];
            if (px == Px::BC5S) {
                r = snormToUnorm(qint8(r));
                g = snormToUnorm(qint8(g));
            }
            out[i] = argb(r, g, 0, 255);
        }
        break;
    }
    default:
        std::fill(out, out + 16, 0u);
        break;
    }
}

QImage decodeSurface(const Format &f, const uchar *src, int w, int h)
{
    const bool hdr = isHdr(f.px);
    QImage img(w, h, hdr ? QImage::Format_RGBA16FPx4 : QImage::Format_ARGB32);
    if (img.isNull())
        return img;
    const qint64 pixels = qint64(w) * h;

    if (isBlock(f.px)) {
        const int bw = std::max(1, (w + 3) / 4), bh = std::max(1, (h + 3) / 4);
        const int bb = blockBytes(f.px);
        ImageOps::parallelRows(bh, pixels, [&](int by0, int by1) {
            for (int by = by0; by < by1; ++by) {
                const uchar *row = src + qint64(by) * bw * bb;
                const int y0 = by * 4, rows = std::min(4, h - y0);
                for (int bx = 0; bx < bw; ++bx) {
                    const int x0 = bx * 4, cols = std::min(4, w - x0);
                    if (hdr) {
                        quint16 rgb[48];
                        bcdec_bc6h_half(row + qint64(bx) * bb, rgb, 12, f.px == Px::BC6HS);
                        for (int y = 0; y < rows; ++y) {
                            auto *d = reinterpret_cast<quint16 *>(img.scanLine(y0 + y)) + 4 * x0;
                            for (int x = 0; x < cols; ++x) {
                                const quint16 *s = rgb + 12 * y + 3 * x;
                                d[4 * x] = s[0];
                                d[4 * x + 1] = s[1];
                                d[4 * x + 2] = s[2];
                                d[4 * x + 3] = 0x3c00; // 1.0
                            }
                        }
                    } else {
                        quint32 px[16];
                        decodeLdrBlock(f.px, row + qint64(bx) * bb, px);
                        for (int y = 0; y < rows; ++y)
                            std::memcpy(reinterpret_cast<quint32 *>(img.scanLine(y0 + y)) + x0, px + 4 * y,
                                        size_t(cols) * 4);
                    }
                }
            }
        });
        return img;
    }

    const qint64 pitch = (qint64(w) * bitsPerPixel(f) + 7) / 8;
    ImageOps::parallelRows(h, pixels, [&](int y0, int y1) {
        std::vector<float> scratch;
        if (hdr) scratch.resize(size_t(w) * 4);
        for (int y = y0; y < y1; ++y) {
            const uchar *s = src + pitch * y;
            if (hdr)
                convertHdrRow(f, s, reinterpret_cast<qfloat16 *>(img.scanLine(y)), w, scratch.data());
            else
                convertLdrRow(f, s, reinterpret_cast<quint32 *>(img.scanLine(y)), w);
        }
    });
    return img;
}

bool hasAlpha(const Format &f)
{
    switch (f.px) {
    case Px::BC1: case Px::BC2: case Px::BC3: case Px::BC7: case Px::RGBA8: case Px::BGRA8:
    case Px::RGB10A2: case Px::RGBA16: case Px::A8: case Px::B5G5R5A1: case Px::B4G4R4A4:
    case Px::RGBA16F: case Px::RGBA32F:
        return true;
    case Px::Masked:
        return f.masks[3] != 0;
    default:
        return false;
    }
}

} // namespace

bool looksLikeDds(QByteArrayView head)
{
    return head.size() >= 8 && head.startsWith("DDS ")
           && rd32(reinterpret_cast<const uchar *>(head.data()) + 4) == kHeaderSize;
}

qint64 estimate(QByteArrayView data)
{
    Layout l;
    if (!parse(data, l, nullptr))
        return -1;
    return decodedBytes(l);
}

ImageDecoder::Result decode(QByteArrayView data, const std::atomic_bool &cancel)
{
    Layout l;
    QString error;
    if (!parse(data, l, &error))
        return ImageDecoder::failed(error);
    const qint64 needed = decodedBytes(l);
    if (needed > ImageDecoder::kMaxDecodedBytes)
        return ImageDecoder::tooLarge(needed);

    auto doc = std::make_shared<ImageDocument>();
    doc->format = QStringLiteral("DDS");
    doc->detail = l.format.name;
    doc->fileSize = data.size();
    doc->width = l.width;
    doc->height = l.height;
    doc->mipCount = l.mips;
    doc->faceCount = l.faces;
    doc->layerCount = l.layers;
    doc->faceNames = l.faceNames;
    doc->hasAlpha = hasAlpha(l.format);
    doc->isHdr = isHdr(l.format.px);
    if (l.volume) {
        doc->detail += QObject::tr(" · volume");
        for (int z = 0; z < l.layers; ++z)
            doc->layerNames << QObject::tr("Slice %1").arg(z);
    }
    doc->surfaces.reserve(size_t(l.mips) * l.faces * l.layers);

    const auto *base = reinterpret_cast<const uchar *>(data.data());
    qsizetype offset = l.dataOffset;
    for (int layer = 0; layer < l.layers; ++layer) {
        for (int face = 0; face < l.faces; ++face) {
            for (int mip = 0; mip < l.mips; ++mip) {
                if (cancel.load(std::memory_order_relaxed))
                    return ImageDecoder::cancelled();
                const int w = mipDim(l.width, mip), h = mipDim(l.height, mip);
                const qint64 bytes = surfaceBytes(l.format, w, h);
                if (offset + bytes > data.size())
                    return ImageDecoder::failed(QObject::tr("The DDS file is truncated."));
                QImage s = decodeSurface(l.format, base + offset, w, h);
                if (s.isNull())
                    return ImageDecoder::tooLarge(needed);
                doc->surfaces.push_back(std::move(s));
                offset += bytes;
            }
        }
    }

    ImageDecoder::Result r;
    if (doc->surfaces.size() == 1)
        r.pyramid = ImageOps::buildPyramid(doc->surfaces[0]);
    r.doc = std::move(doc);
    return r;
}

} // namespace DdsDecoder
