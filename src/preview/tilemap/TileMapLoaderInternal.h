#ifndef TILEMAPLOADERINTERNAL_H
#define TILEMAPLOADERINTERNAL_H

#include "TileMapLoader.h"

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>

#include <memory>

// Shared by TiledLoader.cpp, LdtkLoader.cpp and TileMapLoader.cpp.
namespace TileMapLoader::detail {

// Bounds on numbers read from files, so arithmetic on them never overflows.
constexpr int kMaxTileSide = 65535;        // Instance::w / h are 16-bit
constexpr int kMaxGap = 1 << 20;           // tileset margin / spacing / padding
constexpr int kMaxTileId = 1 << 20;        // collection tile ids and placeholder tilesets
constexpr int kMaxXmlDepth = 64;           // TMX / TSX element nesting (real maps nest groups a few levels)
constexpr int kMaxGroupDepth = 64;         // nested group layers
constexpr float kMaxCoord = 1e9f;          // instances beyond (or NaN) are dropped

struct Context {
    QString baseDir;                       // directory relative paths resolve against
    const std::atomic_bool *cancel = nullptr;
    QStringList warnings;
    qint64 instances = 0;                  // running total against kMaxInstances
    QString error;                         // set by a loader that gives up

    bool cancelled() const { return cancel && cancel->load(std::memory_order_relaxed); }
    // Room left under kMaxInstances (negative once over).
    qint64 instanceBudget() const { return kMaxInstances - instances; }

    // Absolute path of `ref`, a path written in the file, against `dir`.
    // Network paths are refused: empty, with a warning.
    QString resolve(const QString &dir, const QString &ref);
    // Decodes (once per path) an image into ARGB32_Premultiplied; null + a
    // warning on failure, null silently for an empty (refused) path.
    QImage loadImage(const QString &absPath);
    // Reads a small side file (external tileset / level); empty on failure.
    QByteArray readFile(const QString &absPath);

private:
    QHash<QString, QImage> m_images;
};

using DocPtr = std::unique_ptr<TileMap::Document>;

DocPtr loadTiled(const QByteArray &data, const QString &suffixLower, Context &ctx);
DocPtr loadLdtk(const QByteArray &data, const QString &suffixLower, Context &ctx);

// Inflates zlib / gzip data to exactly `expected` bytes (false on any mismatch).
bool inflateExact(const QByteArray &src, bool gzip, size_t expected, QByteArray *out);

// Drops instances with unusable coordinates, then fills a layer's animated
// list and grid indexes once its instances are final.
void finalizeLayer(TileMap::Layer &layer);

// Bounds of everything drawn by `doc` (instances, objects, images, levels).
QRectF contentBounds(const TileMap::Document &doc);

// JSON accessors that accept the string forms XML attributes produce.
double jnum(const QJsonObject &o, const char *key, double def = 0);
int jint(const QJsonObject &o, const char *key, int def = 0);
QString jstr(const QJsonObject &o, const char *key);
bool jflag(const QJsonObject &o, const char *key, bool def);

} // namespace TileMapLoader::detail

#endif // TILEMAPLOADERINTERNAL_H
