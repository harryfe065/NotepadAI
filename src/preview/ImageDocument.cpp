#include "ImageDocument.h"

#include <QFloat16>
#include <QtConcurrent>

#include <algorithm>
#include <array>
#include <cmath>

qint64 ImageDocument::decodedBytes() const
{
    qint64 total = 0;
    for (const QImage &s : surfaces)
        total += s.sizeInBytes();
    return total;
}

namespace ImageOps {

namespace {

constexpr int kParallelPixels = 1 << 20; // below this one thread is faster than the fan-out

// Runs fn(y0, y1) over [0, rows) in chunks on the global pool when the job is
// big enough; the calling thread takes part, so it is safe from pool threads.
template <typename Fn>
void forRows(int rows, qint64 pixels, Fn fn)
{
    if (pixels < kParallelPixels || rows < 64) {
        fn(0, rows);
        return;
    }
    const int chunks = std::min(rows / 16, QThreadPool::globalInstance()->maxThreadCount() * 4);
    std::vector<std::pair<int, int>> ranges;
    ranges.reserve(static_cast<size_t>(chunks));
    for (int c = 0; c < chunks; ++c) {
        const int y0 = static_cast<int>(qint64(rows) * c / chunks);
        const int y1 = static_cast<int>(qint64(rows) * (c + 1) / chunks);
        if (y1 > y0) ranges.emplace_back(y0, y1);
    }
    QtConcurrent::blockingMap(ranges, [&fn](const std::pair<int, int> &r) { fn(r.first, r.second); });
}

} // namespace

void parallelRows(int rows, qint64 pixels, const std::function<void(int, int)> &fn)
{
    forRows(rows, pixels, fn);
}

namespace {

// Linear [0,1] -> sRGB byte, 4096 steps (error < 0.5 LSB after rounding).
const std::array<quint8, 4096> &srgbLut()
{
    static const std::array<quint8, 4096> lut = []() {
        std::array<quint8, 4096> t{};
        for (int i = 0; i < 4096; ++i) {
            const double v = i / 4095.0;
            const double s = v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
            t[static_cast<size_t>(i)] = static_cast<quint8>(std::clamp(std::lround(s * 255.0), 0L, 255L));
        }
        return t;
    }();
    return lut;
}

inline quint8 encodeSrgb(float linear, const std::array<quint8, 4096> &lut)
{
    if (!(linear > 0.0f)) return 0; // also catches NaN
    if (linear >= 1.0f) return 255;
    return lut[static_cast<size_t>(linear * 4095.0f + 0.5f)]; // NOLINT(bugprone-incorrect-roundings): linear is in (0,1), +0.5 truncation is the cheap exact round
}

inline quint32 premul(quint32 r, quint32 g, quint32 b, quint32 a)
{
    // (x * a + 127) / 255 without a divide.
    auto mul = [a](quint32 c) { quint32 t = c * a + 128; return (t + (t >> 8)) >> 8; };
    return (a << 24) | (mul(r) << 16) | (mul(g) << 8) | mul(b);
}

inline quint32 gray(quint32 v) { return 0xff000000u | (v << 16) | (v << 8) | v; }

QImage halveArgb32(const QImage &src)
{
    const int sw = src.width(), sh = src.height();
    const int dw = std::max(1, sw / 2), dh = std::max(1, sh / 2);
    QImage dst(dw, dh, QImage::Format_ARGB32);
    if (dst.isNull()) return dst;
    const RowWriter rows(dst);
    forRows(dh, qint64(dw) * dh, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const int sy0 = std::min(2 * y, sh - 1), sy1 = std::min(2 * y + 1, sh - 1);
            const auto *r0 = reinterpret_cast<const quint32 *>(src.constScanLine(sy0));
            const auto *r1 = reinterpret_cast<const quint32 *>(src.constScanLine(sy1));
            auto *out = rows.row<quint32>(y);
            for (int x = 0; x < dw; ++x) {
                const int sx0 = std::min(2 * x, sw - 1), sx1 = std::min(2 * x + 1, sw - 1);
                const quint32 p[4] = {r0[sx0], r0[sx1], r1[sx0], r1[sx1]};
                quint32 a = 0, ra = 0, ga = 0, ba = 0, rs = 0, gs = 0, bs = 0;
                for (quint32 v : p) {
                    const quint32 pa = v >> 24, pr = (v >> 16) & 0xff, pg = (v >> 8) & 0xff, pb = v & 0xff;
                    a += pa; ra += pr * pa; ga += pg * pa; ba += pb * pa;
                    rs += pr; gs += pg; bs += pb;
                }
                quint32 r, g, b;
                if (a == 0) {
                    // Fully transparent: keep the plain average so channel
                    // views still show the colour stored under alpha 0.
                    r = (rs + 2) >> 2; g = (gs + 2) >> 2; b = (bs + 2) >> 2;
                } else if (a == 4 * 255) {
                    r = (rs + 2) >> 2; g = (gs + 2) >> 2; b = (bs + 2) >> 2;
                } else {
                    r = (ra + a / 2) / a; g = (ga + a / 2) / a; b = (ba + a / 2) / a;
                }
                out[x] = (((a + 2) >> 2) << 24) | (r << 16) | (g << 8) | b;
            }
        }
    });
    return dst;
}

QImage halveRgba16f(const QImage &src)
{
    const int sw = src.width(), sh = src.height();
    const int dw = std::max(1, sw / 2), dh = std::max(1, sh / 2);
    QImage dst(dw, dh, QImage::Format_RGBA16FPx4);
    if (dst.isNull()) return dst;
    const RowWriter rows(dst);
    forRows(dh, qint64(dw) * dh, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const int sy0 = std::min(2 * y, sh - 1), sy1 = std::min(2 * y + 1, sh - 1);
            const auto *r0 = reinterpret_cast<const qfloat16 *>(src.constScanLine(sy0));
            const auto *r1 = reinterpret_cast<const qfloat16 *>(src.constScanLine(sy1));
            auto *out = rows.row<qfloat16>(y);
            for (int x = 0; x < dw; ++x) {
                const int sx0 = std::min(2 * x, sw - 1), sx1 = std::min(2 * x + 1, sw - 1);
                const qfloat16 *p[4] = {r0 + 4 * sx0, r0 + 4 * sx1, r1 + 4 * sx0, r1 + 4 * sx1};
                float a = 0, ra = 0, ga = 0, ba = 0, rs = 0, gs = 0, bs = 0;
                for (const qfloat16 *v : p) {
                    const float pr = v[0], pg = v[1], pb = v[2], pa = v[3];
                    a += pa; ra += pr * pa; ga += pg * pa; ba += pb * pa;
                    rs += pr; gs += pg; bs += pb;
                }
                float r, g, b;
                if (a > 1e-6f && a < 3.9999f) {
                    r = ra / a; g = ga / a; b = ba / a;
                } else {
                    r = rs * 0.25f; g = gs * 0.25f; b = bs * 0.25f;
                }
                qfloat16 *o = out + 4 * x;
                o[0] = qfloat16(r); o[1] = qfloat16(g); o[2] = qfloat16(b); o[3] = qfloat16(a * 0.25f);
            }
        }
    });
    return dst;
}

} // namespace

QImage halve(const QImage &src)
{
    if (src.format() == QImage::Format_RGBA16FPx4)
        return halveRgba16f(src);
    if (src.format() == QImage::Format_ARGB32)
        return halveArgb32(src);
    return halveArgb32(src.convertToFormat(QImage::Format_ARGB32));
}

ImagePyramidPtr buildPyramid(const QImage &level0)
{
    auto pyramid = std::make_shared<ImagePyramid>();
    pyramid->levels.push_back(level0);
    while (true) {
        const QImage &last = pyramid->levels.back();
        if (last.isNull() || std::max(last.width(), last.height()) <= 32)
            break;
        QImage next = halve(last);
        if (next.isNull())
            break;
        pyramid->levels.push_back(std::move(next));
    }
    return pyramid;
}

QImage renderCrop(const QImage &level, const QRect &rectIn, const ViewParams &params)
{
    const QRect rect = rectIn.intersected(level.rect());
    if (rect.isEmpty())
        return {};
    const int w = rect.width(), h = rect.height();
    const qint64 pixels = qint64(w) * h;

    if (level.format() == QImage::Format_ARGB32) {
        // A view onto the rect's scanlines, no copy.
        const QImage view(level.constBits() + qsizetype(rect.y()) * level.bytesPerLine() + qsizetype(rect.x()) * 4,
                          w, h, level.bytesPerLine(), QImage::Format_ARGB32);
        if (params.channel == Channel::Rgba)
            return view.convertToFormat(QImage::Format_ARGB32_Premultiplied);

        QImage out(w, h, QImage::Format_ARGB32_Premultiplied);
        if (out.isNull()) return out;
        const Channel ch = params.channel;
        const RowWriter rows(out);
        forRows(h, pixels, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                const auto *in = reinterpret_cast<const quint32 *>(view.constScanLine(y));
                auto *o = rows.row<quint32>(y);
                switch (ch) {
                case Channel::Rgb:
                    for (int x = 0; x < w; ++x) o[x] = in[x] | 0xff000000u;
                    break;
                case Channel::R:
                    for (int x = 0; x < w; ++x) o[x] = gray((in[x] >> 16) & 0xff);
                    break;
                case Channel::G:
                    for (int x = 0; x < w; ++x) o[x] = gray((in[x] >> 8) & 0xff);
                    break;
                case Channel::B:
                    for (int x = 0; x < w; ++x) o[x] = gray(in[x] & 0xff);
                    break;
                case Channel::A:
                    for (int x = 0; x < w; ++x) o[x] = gray(in[x] >> 24);
                    break;
                case Channel::Rgba:
                    break;
                }
            }
        });
        return out;
    }

    if (level.format() == QImage::Format_RGBA16FPx4) {
        QImage out(w, h, QImage::Format_ARGB32_Premultiplied);
        if (out.isNull()) return out;
        const auto &lut = srgbLut();
        const float scale = std::exp2(params.exposure);
        const Channel ch = params.channel;
        const RowWriter rows(out);
        forRows(h, pixels, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                const auto *in = reinterpret_cast<const qfloat16 *>(level.constScanLine(rect.y() + y)) + 4 * rect.x();
                auto *o = rows.row<quint32>(y);
                for (int x = 0; x < w; ++x) {
                    const qfloat16 *p = in + 4 * x;
                    const quint32 r = encodeSrgb(float(p[0]) * scale, lut);
                    const quint32 g = encodeSrgb(float(p[1]) * scale, lut);
                    const quint32 b = encodeSrgb(float(p[2]) * scale, lut);
                    const float af = float(p[3]);
                    const quint32 a = af >= 1.0f ? 255u : (af > 0.0f ? quint32(af * 255.0f + 0.5f) : 0u); // NOLINT(bugprone-incorrect-roundings): af is in (0,1), +0.5 truncation is the cheap exact round
                    switch (ch) {
                    case Channel::Rgba: o[x] = premul(r, g, b, a); break;
                    case Channel::Rgb:  o[x] = 0xff000000u | (r << 16) | (g << 8) | b; break;
                    case Channel::R:    o[x] = gray(r); break;
                    case Channel::G:    o[x] = gray(g); break;
                    case Channel::B:    o[x] = gray(b); break;
                    case Channel::A:    o[x] = gray(a); break;
                    }
                }
            }
        });
        return out;
    }

    // Other formats never reach here (decoders normalise), but stay correct.
    return renderCrop(level.convertToFormat(QImage::Format_ARGB32), rectIn, params);
}

QString describePixel(const QImage &surface, int x, int y)
{
    if (x < 0 || y < 0 || x >= surface.width() || y >= surface.height())
        return {};
    if (surface.format() == QImage::Format_RGBA16FPx4) {
        const auto *p = reinterpret_cast<const qfloat16 *>(surface.constScanLine(y)) + 4 * x;
        return QStringLiteral("R %1  G %2  B %3  A %4")
            .arg(double(float(p[0])), 0, 'f', 3)
            .arg(double(float(p[1])), 0, 'f', 3)
            .arg(double(float(p[2])), 0, 'f', 3)
            .arg(double(float(p[3])), 0, 'f', 3);
    }
    const quint32 v = reinterpret_cast<const quint32 *>(surface.constScanLine(y))[x];
    return QStringLiteral("R %1  G %2  B %3  A %4  #%5")
        .arg((v >> 16) & 0xff).arg((v >> 8) & 0xff).arg(v & 0xff).arg(v >> 24)
        .arg(v & 0xffffffu, 6, 16, QLatin1Char('0'));
}

} // namespace ImageOps
