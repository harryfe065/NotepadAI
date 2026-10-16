#include "MediaPluginLoader.h"

#include "MediaPluginInterface.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QPluginLoader>
#include <QtConcurrent>

#include <optional>

namespace MediaPluginLoader {

namespace {

#if defined(Q_OS_WIN)
constexpr auto kFileName = "NotepadAIMedia.dll";
#else
constexpr auto kFileName = "NotepadAIMedia.so";
#endif

struct State {
    std::optional<QString> path;
    QFuture<QString> prefetch;
    bool prefetchStarted = false;
    QPluginLoader *loader = nullptr;
    MediaPlugin::Factory *factory = nullptr;
    QString error;
};

State &state()
{
    static State s;
    return s;
}

} // namespace

QString path()
{
    State &s = state();
    if (!s.path) {
        // Next to the executable (Windows, build trees), the AppImage's
        // usr/lib, the macOS bundle's PlugIns.
        const QString appDir = QCoreApplication::applicationDirPath();
        const QString candidates[] = {appDir, appDir + QStringLiteral("/../lib"),
                                      appDir + QStringLiteral("/../PlugIns")};
        s.path = QString();
        for (const QString &dir : candidates) {
            const QString file = QDir::cleanPath(dir + QLatin1Char('/') + QLatin1String(kFileName));
            if (QFileInfo::exists(file)) {
                s.path = file;
                break;
            }
        }
    }
    return *s.path;
}

QFuture<QString> prefetch()
{
    State &s = state();
    if (!s.prefetchStarted) {
        s.prefetchStarted = true;
        const QString file = path();
        // QLibrary::load maps the plugin, Qt Multimedia and Qt Pdf and runs
        // their static initialisers off the GUI thread. The library stays
        // loaded after the QLibrary object goes away.
        s.prefetch = QtConcurrent::run([file]() -> QString {
            if (file.isEmpty())
                return QCoreApplication::translate("MediaPluginLoader", "The media plugin is not installed.");
            QLibrary library(file);
            if (!library.load())
                return library.errorString();
            return QString();
        });
    }
    return s.prefetch;
}

MediaPlugin::Factory *factory(QString *error)
{
    State &s = state();
    if (s.factory || !s.error.isEmpty()) {
        if (error) *error = s.error;
        return s.factory;
    }
    const QFuture<QString> pending = prefetch();
    // Callers wait for the prefetch with a watcher; this only blocks if one
    // did not, and then only until the worker finishes.
    const QString loadError = pending.result();
    if (!loadError.isEmpty()) {
        s.error = loadError;
    } else {
        s.loader = new QPluginLoader(path());
        QObject *instance = s.loader->instance();
        s.factory = qobject_cast<MediaPlugin::Factory *>(instance);
        if (!s.factory)
            s.error = instance ? QCoreApplication::translate("MediaPluginLoader", "The media plugin is incompatible.")
                               : s.loader->errorString();
    }
    if (error) *error = s.error;
    return s.factory;
}

} // namespace MediaPluginLoader
