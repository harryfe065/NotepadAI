#ifndef ATLASLOADER_H
#define ATLASLOADER_H

#include "AtlasDocument.h"
#include "ImageDocument.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QVector>

#include <atomic>

// Reads atlas / bitmap-font descriptions and decodes their page images. Runs on
// a worker thread; the cancel flag is polled between pages.
namespace AtlasLoader {

constexpr qint64 kMaxFileBytes = qint64(256) << 20;

struct Result {
    Atlas::DocumentPtr doc;
    QVector<ImageDocumentPtr> images;   // per page; null where the image is missing or unreadable
    QString error;                      // set when doc is null
    bool cancelled = false;
};

// Parses a description. `suffixLower` ("atlas", "json", "xml", "plist", "fnt",
// "tpsheet", "txt") is a hint only: the content decides. Page paths resolve
// against `baseDir`. Returns null with `error` set when nothing parses.
std::shared_ptr<Atlas::Document> parse(const QByteArray &data, const QString &suffixLower, const QString &baseDir,
                                       QString *error);

Result load(const QString &path, const std::atomic_bool &cancel);

// Head sniffers (the first 4 KB) for the explicit Preview actions.
bool looksLikeBmFont(QByteArrayView head);
bool looksLikeAtlasContent(QByteArrayView head);   // JSON / XML / plist / libGDX text atlases

} // namespace AtlasLoader

#endif // ATLASLOADER_H
