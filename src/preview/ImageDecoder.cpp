#include "ImageDecoder.h"

#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QLocale>
#include <QPainter>
#include <QSvgRenderer>

#include <algorithm>
#include <cmath>

namespace ImageDecoder {

namespace {

constexpr qint64 kMaxSvgBytes = qint64(64) << 20;

QString describeFormat(QImage::Format f)
{
    switch (f) {
    case QImage::Format_Mono:
    case QImage::Format_MonoLSB: return QStringLiteral("1-bit");
    case QImage::Format_Indexed8: return QStringLiteral("Indexed");
    case QImage::Format_Grayscale8: return QStringLiteral("8-bit gray");
    case QImage::Format_Grayscale16: return QStringLiteral("16-bit gray");
    case QImage::Format_Alpha8: return QStringLiteral("8-bit alpha");
    case QImage::Format_RGB32:
    case QImage::Format_RGB888:
    case QImage::Format_BGR888:
    case QImage::Format_RGBX8888: return QStringLiteral("8-bit RGB");
    case QImage::Format_ARGB32:
    case QImage::Format_ARGB32_Premultiplied:
    case QImage::Format_RGBA8888:
    case QImage::Format_RGBA8888_Premultiplied: return QStringLiteral("8-bit RGBA");
    case QImage::Format_RGB16:
    case QImage::Format_RGB555:
    case QImage::Format_RGB444: return QStringLiteral("16-bit RGB");
    case QImage::Format_RGBX64: return QStringLiteral("16-bit RGB");
    case QImage::Format_RGBA64:
    case QImage::Format_RGBA64_Premultiplied: return QStringLiteral("16-bit RGBA");
    case QImage::Format_RGBX16FPx4:
    case QImage::Format_RGBA16FPx4:
    case QImage::Format_RGBA16FPx4_Premultiplied: return QStringLiteral("16-bit float");
    case QImage::Format_RGBX32FPx4:
    case QImage::Format_RGBA32FPx4:
    case QImage::Format_RGBA32FPx4_Premultiplied: return QStringLiteral("32-bit float");
    default: return QString();
    }
}

Result fail(const QString &error)
{
    Result r;
    r.error = error;
    return r;
}

Result tooLarge(qint64 bytes)
{
    Result r;
    r.tooLarge = true;
    r.error = QObject::tr("This image needs %1 of memory to preview; the limit is %2.")
                  .arg(formatBytes(bytes), formatBytes(kMaxDecodedBytes));
    return r;
}

bool isSvgSuffix(const QString &suffix)
{
    return suffix == QLatin1String("svg") || suffix == QLatin1String("svgz");
}

Result decodeSvg(const QString &path, const std::atomic_bool &cancel)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(file.errorString());
    if (file.size() > kMaxSvgBytes)
        return tooLarge(file.size());
    const QByteArray data = file.readAll();

    QSvgRenderer renderer(data);
    if (!renderer.isValid())
        return fail(QObject::tr("The SVG could not be parsed."));

    QSizeF intrinsic = renderer.defaultSize();
    if (intrinsic.isEmpty())
        intrinsic = renderer.viewBoxF().size();
    if (intrinsic.isEmpty())
        intrinsic = QSizeF(512, 512);

    // Base raster: sharp up to 4x the intrinsic size, at least 1024 px and at
    // most 4096 px on the longest side. Deeper zoom re-rasterises the visible
    // region on demand (ImageCanvas).
    const double longest = std::max(intrinsic.width(), intrinsic.height());
    const double target = std::clamp(longest * 4.0, 1024.0, 4096.0);
    const double scale = target / longest;
    const QSize rasterSize(std::max(1, int(std::ceil(intrinsic.width() * scale))),
                           std::max(1, int(std::ceil(intrinsic.height() * scale))));
    if (cancel.load(std::memory_order_relaxed)) {
        Result r;
        r.cancelled = true;
        return r;
    }

    QImage raster(rasterSize, QImage::Format_ARGB32_Premultiplied);
    if (raster.isNull())
        return tooLarge(qint64(rasterSize.width()) * rasterSize.height() * 4);
    raster.fill(Qt::transparent);
    {
        QPainter p(&raster);
        p.setRenderHint(QPainter::Antialiasing);
        renderer.render(&p, QRectF(QPointF(0, 0), QSizeF(rasterSize)));
    }

    auto doc = std::make_shared<ImageDocument>();
    doc->format = QStringLiteral("SVG");
    doc->detail = QObject::tr("Vector");
    doc->fileSize = file.size();
    doc->width = std::max(1, int(std::round(intrinsic.width())));
    doc->height = std::max(1, int(std::round(intrinsic.height())));
    doc->hasAlpha = true;
    doc->vectorData = data;
    doc->rasterScale = scale;
    doc->surfaces.push_back(raster.convertToFormat(QImage::Format_ARGB32));

    Result r;
    r.pyramid = ImageOps::buildPyramid(doc->surfaces[0]);
    r.doc = std::move(doc);
    return r;
}

Result decodeQt(const QString &path, const std::atomic_bool &cancel)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    // The default 256 MB allocation limit would refuse large textures that are
    // within our own cap; enforce ours instead.
    reader.setAllocationLimit(int(kMaxDecodedBytes >> 20) + 64);
    if (!reader.canRead())
        return fail(reader.errorString());

    const QSize size = reader.size();
    const int count = reader.imageCount();
    const bool animated = reader.supportsAnimation() && count != 1;
    if (size.isValid()) {
        const qint64 frame = qint64(size.width()) * size.height() * 4;
        if (frame * std::max(1, animated ? count : 1) > kMaxDecodedBytes)
            return tooLarge(frame * std::max(1, count));
    }

    auto doc = std::make_shared<ImageDocument>();
    doc->format = QString::fromLatin1(reader.format()).toUpper();
    doc->detail = describeFormat(reader.imageFormat());
    doc->fileSize = QFileInfo(path).size();

    qint64 bytes = 0;
    QImage frame;
    while (reader.read(&frame)) {
        if (cancel.load(std::memory_order_relaxed)) {
            Result r;
            r.cancelled = true;
            return r;
        }
        if (doc->surfaces.empty()) {
            doc->width = frame.width();
            doc->height = frame.height();
            if (doc->detail.isEmpty())
                doc->detail = describeFormat(frame.format());
        }
        doc->hasAlpha = doc->hasAlpha || frame.hasAlphaChannel();
        QImage argb = frame.format() == QImage::Format_ARGB32 ? frame : frame.convertToFormat(QImage::Format_ARGB32);
        if (argb.isNull())
            return tooLarge(bytes + qint64(frame.width()) * frame.height() * 4);
        bytes += argb.sizeInBytes();
        if (bytes > kMaxDecodedBytes)
            return tooLarge(bytes);
        doc->surfaces.push_back(std::move(argb));
        doc->frameDelaysMs.append(reader.nextImageDelay());
        if (!animated)
            break;
    }
    if (doc->surfaces.empty())
        return fail(reader.errorString());

    doc->layerCount = static_cast<int>(doc->surfaces.size());
    doc->isAnimation = doc->layerCount > 1;
    if (!doc->isAnimation)
        doc->frameDelaysMs.clear();

    Result r;
    if (!doc->isAnimation)
        r.pyramid = ImageOps::buildPyramid(doc->surfaces[0]);
    r.doc = std::move(doc);
    return r;
}

} // namespace

QString formatBytes(qint64 bytes)
{
    return QLocale::system().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

qint64 estimateDecodedBytes(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (isSvgSuffix(suffix))
        return 16 << 20;
    QImageReader reader(path);
    const QSize size = reader.size();
    if (!size.isValid())
        return -1;
    const int count = reader.supportsAnimation() ? std::max(1, reader.imageCount()) : 1;
    return qint64(size.width()) * size.height() * 4 * count;
}

Result decode(const QString &path, const std::atomic_bool &cancel, bool vector)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (vector || isSvgSuffix(suffix))
        return decodeSvg(path, cancel);
    return decodeQt(path, cancel);
}

} // namespace ImageDecoder
