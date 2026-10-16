#include "ModelLoader.h"

#include "DdsDecoder.h"
#include "FbxLoader.h"
#include "GltfLoader.h"
#include "ImageDecoder.h"
#include "ObjLoader.h"
#include "RasterDecoders.h"
#include "SimpleMeshLoaders.h"

#include <QFileInfo>
#include <QObject>
#include <QtConcurrent>

#include <algorithm>
#include <cstring>

namespace ModelLoader {

Result failed(const QString &error)
{
    Result r;
    r.error = error;
    return r;
}

Result tooLarge(qint64 bytes)
{
    Result r;
    r.tooLarge = true;
    r.error = QObject::tr("This model needs %1 of memory to preview; the limit is %2.")
                  .arg(ImageDecoder::formatBytes(bytes), ImageDecoder::formatBytes(kMaxGeometryBytes));
    return r;
}

Result cancelled()
{
    Result r;
    r.cancelled = true;
    return r;
}

namespace {

bool startsWith(QByteArrayView head, const char *magic, qsizetype len)
{
    return head.size() >= len && std::memcmp(head.data(), magic, size_t(len)) == 0;
}

} // namespace

bool looksLikeGlb(QByteArrayView head)
{
    // "glTF" + version 2 (little-endian).
    return startsWith(head, "glTF", 4) && head.size() >= 8 && static_cast<unsigned char>(head[4]) == 2
           && head[5] == 0 && head[6] == 0 && head[7] == 0;
}

bool looksLikeFbx(QByteArrayView head)
{
    if (startsWith(head, "Kaydara FBX Binary", 18))
        return true;
    // ASCII FBX starts with a "; FBX 7.x.x project file" comment.
    qsizetype i = 0;
    if (head.size() >= 3 && static_cast<unsigned char>(head[0]) == 0xEF && static_cast<unsigned char>(head[1]) == 0xBB
        && static_cast<unsigned char>(head[2]) == 0xBF)
        i = 3;
    return head.sliced(i).startsWith("; FBX ");
}

bool looksLikePly(QByteArrayView head)
{
    return startsWith(head, "ply\n", 4) || startsWith(head, "ply\r\n", 5);
}

bool looksLikeVox(QByteArrayView head)
{
    return startsWith(head, "VOX ", 4) && head.size() >= 12 && std::memcmp(head.data() + 8, "MAIN", 4) == 0;
}

Format formatFor(const QString &suffixLower, QByteArrayView head)
{
    if (suffixLower == QLatin1String("gltf") || suffixLower == QLatin1String("glb")) return Format::Gltf;
    if (suffixLower == QLatin1String("fbx")) return Format::Fbx;
    if (suffixLower == QLatin1String("obj")) return Format::Obj;
    if (suffixLower == QLatin1String("stl")) return Format::Stl;
    if (suffixLower == QLatin1String("ply")) return Format::Ply;
    if (suffixLower == QLatin1String("vox")) return Format::Vox;
    if (looksLikeGlb(head)) return Format::Gltf;
    if (looksLikeFbx(head)) return Format::Fbx;
    if (looksLikePly(head)) return Format::Ply;
    if (looksLikeVox(head)) return Format::Vox;
    return Format::Unknown;
}

MappedFile::MappedFile(const QString &path) : file(path)
{
    if (!file.open(QIODevice::ReadOnly)) {
        error = file.errorString();
        return;
    }
    const qint64 size = file.size();
    if (size <= 0)
        return;
    if (uchar *p = file.map(0, size)) {
        view = QByteArrayView(reinterpret_cast<const char *>(p), size);
        return;
    }
    copy = file.readAll();
    view = QByteArrayView(copy);
}

Result load(const QString &path, const std::atomic_bool &cancel)
{
    const QFileInfo info(path);
    if (info.size() > kMaxFileBytes)
        return failed(QObject::tr("Models over %1 are not previewed.").arg(ImageDecoder::formatBytes(kMaxFileBytes)));

    QByteArray head;
    {
        QFile probe(path);
        if (!probe.open(QIODevice::ReadOnly))
            return failed(probe.errorString());
        head = probe.read(256);
    }
    if (head.isEmpty())
        return failed(QObject::tr("The file is empty."));
    const Format format = formatFor(info.suffix().toLower(), head);
    if (format == Format::Unknown)
        return failed(QObject::tr("Unknown model format."));

    Result r;
    try {
        if (format == Format::Obj) {
            // Wavefront OBJ is text; the same extension names compiled object
            // files (COFF), which would otherwise parse as an empty model.
            if (head.contains('\0'))
                return failed(QObject::tr("This .obj file is a compiled object file, not a Wavefront OBJ model."));
            // fast_obj streams the file itself (and its .mtl) in chunks.
            r = ObjLoader::load(path, cancel);
        } else {
            // The mapping lives until the loader returns: embedded buffers and
            // images are read straight from it.
            MappedFile file(path);
            if (!file.error.isEmpty())
                return failed(file.error);
            switch (format) {
            case Format::Gltf: r = GltfLoader::load(path, file.view, cancel); break;
            case Format::Fbx: r = FbxLoader::load(path, file.view, cancel); break;
            case Format::Stl: r = SimpleMeshLoaders::loadStl(file.view, cancel); break;
            case Format::Ply: r = SimpleMeshLoaders::loadPly(file.view, cancel); break;
            case Format::Vox: r = SimpleMeshLoaders::loadVox(file.view, cancel); break;
            case Format::Obj:
            case Format::Unknown: break;
            }
        }
    } catch (const std::bad_alloc &) {
        // Header counts are checked against the caps before allocating; this
        // is the backstop for a machine that is simply out of memory.
        return failed(QObject::tr("Not enough memory to load this model."));
    }
    if (cancel.load(std::memory_order_relaxed))
        return cancelled();
    if (!r.doc)
        return r;
    if (r.doc->geometryBytes() > kMaxGeometryBytes)
        return tooLarge(r.doc->geometryBytes());
    r.doc->fileSize = info.size();
    ModelScene::finalize(*r.doc);
    return r;
}

// ---------------------------------------------------------------------------
// Textures

QString resolveTexturePath(const QDir &modelDir, const QString &raw)
{
    QString p = raw.trimmed();
    if (p.isEmpty())
        return QString();
    if (p.startsWith(QLatin1String("file:///"), Qt::CaseInsensitive))
        p = p.mid(8);
    else if (p.startsWith(QLatin1String("file://"), Qt::CaseInsensitive))
        p = p.mid(7);
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    // Never reach out of the machine: no UNC shares, no URLs.
    if (p.startsWith(QLatin1String("//")) || p.contains(QLatin1String("://")))
        return QString();

    auto existingFile = [](const QString &candidate) {
        const QFileInfo fi(candidate);
        return fi.isFile() ? fi.absoluteFilePath() : QString();
    };
    if (QDir::isAbsolutePath(p)) {
        const QString hit = existingFile(p);
        if (!hit.isEmpty())
            return hit;
    } else {
        const QString hit = existingFile(modelDir.filePath(p));
        if (!hit.isEmpty())
            return hit;
    }
    // Exporters often write the artist's absolute path; look for the file name
    // next to the model.
    const QString name = p.section(QLatin1Char('/'), -1);
    if (name.isEmpty())
        return QString();
    static const char *const kDirs[] = {".", "textures", "Textures", "texture", "Texture", "tex", "images",
                                        "../textures", "../Textures", "../texture", "../images", ".."};
    for (const char *dir : kDirs) {
        const QString hit = existingFile(modelDir.filePath(QLatin1String(dir) + QLatin1Char('/') + name));
        if (!hit.isEmpty())
            return hit;
    }
    return QString();
}

namespace {

enum class ImageKind : quint8 { Qt, Dds, Tga, Psd, Radiance, Exr, Ktx, Basis };

ImageKind imageKind(QByteArrayView bytes, const QString &hintLower)
{
    if (DdsDecoder::looksLikeDds(bytes)) return ImageKind::Dds;
    if (bytes.startsWith("\xABKTX ")) return ImageKind::Ktx;
    if (RasterDecoders::looksLikeExr(bytes)) return ImageKind::Exr;
    if (RasterDecoders::looksLikePsd(bytes)) return ImageKind::Psd;
    if (RasterDecoders::looksLikeRadiance(bytes)) return ImageKind::Radiance;
    if (hintLower.endsWith(QLatin1String("tga")) || hintLower.endsWith(QLatin1String("targa")))
        return ImageKind::Tga;
    if (hintLower.endsWith(QLatin1String("ktx2")) || hintLower.endsWith(QLatin1String("ktx"))) return ImageKind::Ktx;
    if (hintLower.endsWith(QLatin1String("basis"))) return ImageKind::Basis;
    return ImageKind::Qt;
}

QImage capped(QImage image)
{
    if (image.isNull())
        return image;
    if (image.width() > kMaxTextureSize || image.height() > kMaxTextureSize)
        image = image.scaled(kMaxTextureSize, kMaxTextureSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (image.format() != QImage::Format_RGBA8888)
        image = image.convertToFormat(QImage::Format_RGBA8888);
    return image;
}

} // namespace

QImage decodeImage(QByteArrayView bytes, const QString &hint, const std::atomic_bool &cancel, QString *error)
{
    if (bytes.isEmpty()) {
        if (error) *error = QObject::tr("empty image");
        return QImage();
    }
    const QString hintLower = hint.toLower();
    ImageDecoder::Result r;
    switch (imageKind(bytes, hintLower)) {
    case ImageKind::Ktx:
    case ImageKind::Basis:
        if (error) *error = QObject::tr("KTX / Basis textures are not supported");
        return QImage();
    case ImageKind::Dds: r = DdsDecoder::decode(bytes, cancel); break;
    case ImageKind::Tga: r = RasterDecoders::decode(RasterDecoders::Kind::Tga, bytes, cancel); break;
    case ImageKind::Psd: r = RasterDecoders::decode(RasterDecoders::Kind::Psd, bytes, cancel); break;
    case ImageKind::Radiance: r = RasterDecoders::decode(RasterDecoders::Kind::Radiance, bytes, cancel); break;
    case ImageKind::Exr: r = RasterDecoders::decode(RasterDecoders::Kind::Exr, bytes, cancel); break;
    case ImageKind::Qt: {
        QImage image = QImage::fromData(bytes);
        if (image.isNull() && error)
            *error = QObject::tr("unreadable image");
        return capped(std::move(image));
    }
    }
    if (!r.doc || r.doc->surfaces.empty()) {
        if (error) *error = r.error.isEmpty() ? QObject::tr("unreadable image") : r.error;
        return QImage();
    }
    return capped(r.doc->surfaces.front());
}

TextureSet::TextureSet(const QString &modelPath) : m_dir(QFileInfo(modelPath).absoluteDir()) {}

int TextureSet::addTexture(ModelDocument &doc, int source, bool srgb, ModelWrap wrapS, ModelWrap wrapT, bool nearest)
{
    // Same source, same use: share the texture entry.
    for (size_t i = 0; i < m_textureSource.size(); ++i) {
        const ModelTexture &t = doc.textures[i];
        if (m_textureSource[i] == source && t.srgb == srgb && t.wrapS == wrapS && t.wrapT == wrapT
            && t.nearest == nearest)
            return int(i);
    }
    ModelTexture t;
    t.name = m_sources[size_t(source)].name;
    t.srgb = srgb;
    t.wrapS = wrapS;
    t.wrapT = wrapT;
    t.nearest = nearest;
    doc.textures.push_back(std::move(t));
    m_textureSource.push_back(source);
    return int(doc.textures.size()) - 1;
}

int TextureSet::fromFile(ModelDocument &doc, const QString &rawPath, bool srgb, ModelWrap wrapS, ModelWrap wrapT,
                         bool nearest)
{
    if (rawPath.trimmed().isEmpty())
        return -1;
    const QString resolved = resolveTexturePath(m_dir, rawPath);
    const QString key = resolved.isEmpty() ? QStringLiteral("missing:") + rawPath : QStringLiteral("file:") + resolved;
    int source = m_byKey.value(key, -1);
    if (source < 0) {
        Source s;
        s.path = resolved;
        s.name = QString(rawPath).replace(QLatin1Char('\\'), QLatin1Char('/')).section(QLatin1Char('/'), -1);
        s.missing = resolved.isEmpty();
        m_sources.push_back(std::move(s));
        source = int(m_sources.size()) - 1;
        m_byKey.insert(key, source);
    }
    return addTexture(doc, source, srgb, wrapS, wrapT, nearest);
}

int TextureSet::fromBytes(ModelDocument &doc, const QString &key, QByteArrayView bytes, const QString &hint, bool srgb,
                          ModelWrap wrapS, ModelWrap wrapT, bool nearest, const QString &name)
{
    const QString k = QStringLiteral("blob:") + key;
    int source = m_byKey.value(k, -1);
    if (source < 0) {
        Source s;
        s.bytes = bytes;
        s.hint = hint;
        s.name = name.isEmpty() ? QObject::tr("embedded image") : name;
        m_sources.push_back(std::move(s));
        source = int(m_sources.size()) - 1;
        m_byKey.insert(k, source);
    }
    return addTexture(doc, source, srgb, wrapS, wrapT, nearest);
}

int TextureSet::unsupported(ModelDocument &doc, const QString &name, bool srgb)
{
    const QString k = QStringLiteral("unsupported:") + name;
    int source = m_byKey.value(k, -1);
    if (source < 0) {
        Source s;
        s.name = name;
        s.unsupported = true;
        m_sources.push_back(std::move(s));
        source = int(m_sources.size()) - 1;
        m_byKey.insert(k, source);
    }
    return addTexture(doc, source, srgb, ModelWrap::Repeat, ModelWrap::Repeat, false);
}

void TextureSet::decode(ModelDocument &doc, const std::atomic_bool &cancel)
{
    QtConcurrent::blockingMap(m_sources, [&cancel](Source &s) {
        if (cancel.load(std::memory_order_relaxed) || s.missing || s.unsupported)
            return;
        if (!s.path.isEmpty()) {
            MappedFile file(s.path);
            if (!file.error.isEmpty()) {
                s.error = file.error;
                return;
            }
            s.image = decodeImage(file.view, QFileInfo(s.path).suffix(), cancel, &s.error);
        } else {
            s.image = decodeImage(s.bytes, s.hint, cancel, &s.error);
        }
    });
    if (cancel.load(std::memory_order_relaxed))
        return;

    // Over the budget: halve the largest images until it fits.
    qint64 total = 0;
    for (const Source &s : m_sources)
        total += s.image.sizeInBytes();
    while (total > kMaxTextureBytes) {
        auto largest = std::max_element(m_sources.begin(), m_sources.end(), [](const Source &a, const Source &b) {
            return a.image.sizeInBytes() < b.image.sizeInBytes();
        });
        if (largest == m_sources.end() || largest->image.width() <= 64)
            break;
        total -= largest->image.sizeInBytes();
        largest->image = largest->image.scaled(largest->image.width() / 2, std::max(1, largest->image.height() / 2),
                                               Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        total += largest->image.sizeInBytes();
    }

    QStringList missing;
    QStringList broken;
    QStringList unsupportedNames;
    for (const Source &s : m_sources) {
        if (s.missing) missing << s.name;
        else if (s.unsupported) unsupportedNames << s.name;
        else if (s.image.isNull()) broken << QStringLiteral("%1 (%2)").arg(s.name, s.error);
    }
    for (size_t i = 0; i < doc.textures.size() && i < m_textureSource.size(); ++i)
        doc.textures[i].image = m_sources[size_t(m_textureSource[i])].image; // implicitly shared

    auto listed = [](const QStringList &names) {
        QStringList shown = names.mid(0, 3);
        QString text = shown.join(QStringLiteral(", "));
        if (names.size() > 3)
            text += QStringLiteral(", …");
        return text;
    };
    if (!missing.isEmpty())
        doc.warnings << QObject::tr("%n texture(s) not found: %1", nullptr, int(missing.size())).arg(listed(missing));
    if (!unsupportedNames.isEmpty())
        doc.warnings << QObject::tr("%n texture(s) in KTX2 / Basis, not shown", nullptr, int(unsupportedNames.size()));
    if (!broken.isEmpty())
        doc.warnings << QObject::tr("%n texture(s) could not be decoded: %1", nullptr, int(broken.size())).arg(listed(broken));
}

} // namespace ModelLoader
