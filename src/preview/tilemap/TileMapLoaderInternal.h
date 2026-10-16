#ifndef TILEMAPLOADERINTERNAL_H
#define TILEMAPLOADERINTERNAL_H

#include "TileMapLoader.h"

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>

#include <memory>

// Shared by TiledLoader.cpp, LdtkLoader.cpp and TileMapLoader.cpp.
namespace TileMapLoader::detail {

struct Context {
    QString baseDir;                       // directory relative paths resolve against
    const std::atomic_bool *cancel = nullptr;
    QStringList warnings;
    qint64 instances = 0;                  // running total against kMaxInstances
    QString error;                         // set by a loader that gives up

    bool cancelled() const { return cancel && cancel->load(std::memory_order_relaxed); }

    // Decodes (once per path) an image into ARGB32_Premultiplied; null + a
    // warning on failure.
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

// Fills a layer's animated list and grid index once its instances are final.
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
