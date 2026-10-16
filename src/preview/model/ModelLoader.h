#ifndef MODELLOADER_H
#define MODELLOADER_H

#include "ModelDocument.h"

#include <QByteArrayView>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QString>

#include <atomic>
#include <vector>

// Loads a model file into a ModelDocument on a worker thread. Each format has
// its own loader (GltfLoader, FbxLoader, ObjLoader, SimpleMeshLoaders); this
// is the dispatch plus what they share: the mapped file, the caps, texture
// lookup and parallel texture decoding.
namespace ModelLoader {

constexpr qint64 kMaxFileBytes = qint64(2) << 30;         // 2 GB source files
constexpr qint64 kMaxGeometryBytes = qint64(3) << 29;     // 1.5 GB vertices + indices
constexpr qint64 kMaxTextureBytes = qint64(1) << 30;      // 1 GB decoded texels, then halved
constexpr int kMaxTextureSize = 8192;                     // longest side kept
constexpr qint64 kProgressBytes = qint64(64) << 20;       // cancellable banner above this

enum class Format : quint8 { Unknown, Gltf, Fbx, Obj, Stl, Ply, Vox };

struct Result {
    ModelDocumentPtr doc;
    QString error;
    bool tooLarge = false;
    bool cancelled = false;
};

Result failed(const QString &error);
Result tooLarge(qint64 bytes);
Result cancelled();
// A converter's outcome: cancelled, else too large (tooLargeBytes > 0), else
// the error, else the document.
Result finish(ModelDocumentPtr doc, const QString &error, quint64 tooLargeBytes, const std::atomic_bool &cancel);

// Suffix first, then magic (files the resolver sniffed without an extension).
Format formatFor(const QString &suffixLower, QByteArrayView head);

// Magic sniffers for the resolver.
bool looksLikeGlb(QByteArrayView head);
bool looksLikeFbx(QByteArrayView head);
bool looksLikePly(QByteArrayView head);
bool looksLikeVox(QByteArrayView head);

Result load(const QString &path, const std::atomic_bool &cancel);

// The whole file as one read-only view: mapped when possible, else read.
struct MappedFile {
    QFile file;
    QByteArray copy;
    QByteArrayView view;
    QString error;
    explicit MappedFile(const QString &path);
};

// Texture images the loaders reference. Sources are de-duplicated by key
// (file path or embedded blob), each decoded once even when it is used both
// as colour (sRGB) and data (linear) texture.
class TextureSet
{
public:
    explicit TextureSet(const QString &modelPath);

    // Index into doc.textures for a file the model references (raw path as
    // written in the file; looked up next to the model when it does not exist
    // as is). -1 for an empty path.
    int fromFile(ModelDocument &doc, const QString &rawPath, bool srgb, ModelWrap wrapS = ModelWrap::Repeat,
                 ModelWrap wrapT = ModelWrap::Repeat, bool nearest = false);
    // Embedded image bytes; `key` identifies the blob (it must stay valid
    // until decode()). `hint` is a mime type or suffix ("image/png", "tga").
    int fromBytes(ModelDocument &doc, const QString &key, QByteArrayView bytes, const QString &hint, bool srgb,
                  ModelWrap wrapS = ModelWrap::Repeat, ModelWrap wrapT = ModelWrap::Repeat, bool nearest = false,
                  const QString &name = QString());
    // An image the format cannot provide (KTX2 / Basis): recorded as missing.
    int unsupported(ModelDocument &doc, const QString &name, bool srgb);

    // Decodes every source in parallel into doc.textures, applies the size
    // caps and adds warnings for missing or undecodable images.
    void decode(ModelDocument &doc, const std::atomic_bool &cancel);

    QDir modelDir() const { return m_dir; }

private:
    struct Source {
        QString path;           // resolved file, or empty
        QByteArrayView bytes;   // embedded data
        QString hint;
        QString name;
        bool missing = false;
        bool unsupported = false;
        QImage image;
        QString error;
    };
    int addTexture(ModelDocument &doc, int source, bool srgb, ModelWrap wrapS, ModelWrap wrapT, bool nearest);

    QDir m_dir;
    std::vector<Source> m_sources;
    std::vector<int> m_textureSource; // per doc texture
    QHash<QString, int> m_byKey;
};

// Finds a texture the file names: as written (absolute or relative to the
// model), else by file name in the model's folder and its usual texture
// sub-folders. Empty when not found.
QString resolveTexturePath(const QDir &modelDir, const QString &raw);

// Image bytes (PNG/JPEG/... via Qt, DDS/TGA/PSD/HDR/EXR via the texture
// decoders) to a straight-alpha RGBA8888 image capped at kMaxTextureSize.
QImage decodeImage(QByteArrayView bytes, const QString &hint, const std::atomic_bool &cancel, QString *error);

} // namespace ModelLoader

#endif // MODELLOADER_H
