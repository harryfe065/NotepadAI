#ifndef MEDIAPLUGINLOADER_H
#define MEDIAPLUGINLOADER_H

#include <QFuture>
#include <QString>

namespace MediaPlugin {
class Factory;
}

// Finds and loads the NotepadAIMedia plugin (add-media-preview D1). Nothing
// here runs at startup except path(): one exists() per candidate directory.
//
// prefetch() maps the plugin and its Qt modules on a pool thread the first
// time a media tab is shown; factory() is then cheap on the GUI thread.
// The plugin is never unloaded. GUI thread only.
namespace MediaPluginLoader {

// The plugin file, or empty when it is not installed. Cached after the first call.
QString path();

// Starts loading on a worker; idempotent. The future yields an error message
// (empty on success).
QFuture<QString> prefetch();

// The plugin's factory once prefetch() finished; nullptr (and *error) on failure.
MediaPlugin::Factory *factory(QString *error);

} // namespace MediaPluginLoader

#endif // MEDIAPLUGINLOADER_H
