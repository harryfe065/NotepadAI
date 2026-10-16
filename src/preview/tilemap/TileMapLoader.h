#ifndef TILEMAPLOADER_H
#define TILEMAPLOADER_H

#include "TileMapDocument.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <atomic>

// Loads Tiled (.tmx .tmj .tsx .tsj, and .json by content) and LDtk (.ldtk,
// .ldtkl) files into a TileMap::Document. Runs on a worker thread; the cancel
// flag is polled per layer and every 64K tiles, and tileset images are decoded
// through ImageDecoder (so DDS / TGA / PSD sheets work like PNG ones).
namespace TileMapLoader {

constexpr qint64 kMaxFileBytes = qint64(512) << 20;
constexpr qint64 kProgressBytes = qint64(32) << 20;
constexpr qint64 kMaxInstances = 25'000'000;   // 500 MB of Instance records

struct Result {
    TileMap::DocumentPtr doc;
    QString error;             // set when doc is null
    bool cancelled = false;
};

Result load(const QString &path, const std::atomic_bool &cancel);

// The same from bytes; `suffixLower` ("tmx", "ldtk", ...) picks the parser and
// `baseDir` resolves relative tileset and image paths.
Result loadFromBytes(const QByteArray &data, const QString &suffixLower, const QString &baseDir,
                     const std::atomic_bool &cancel);

// Content sniffers over a file head (the first 4 KB): Tiled JSON or XML maps and
// tilesets, and LDtk projects.
bool looksLikeTiledJson(QByteArrayView head);
bool looksLikeTiledXml(QByteArrayView head);   // .tmx / .tsx (also tells them from Translation Memory .tmx)
bool looksLikeLdtk(QByteArrayView head);
// Any of the three; for the explicit Preview actions on generic extensions.
bool looksLikeTileMap(QByteArrayView head);

} // namespace TileMapLoader

#endif // TILEMAPLOADER_H
