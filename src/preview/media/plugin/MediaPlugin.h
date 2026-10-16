#ifndef MEDIAPLUGIN_H
#define MEDIAPLUGIN_H

#include "MediaPluginInterface.h"

#include <QObject>

// Entry point of the NotepadAIMedia plugin (add-media-preview D1).
class MediaPluginFactory : public QObject, public MediaPlugin::Factory
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID NotepadAIMediaFactory_iid)
    Q_INTERFACES(MediaPlugin::Factory)

public:
    MediaPlugin::View *create(MediaPlugin::Kind kind, MediaPlugin::Host *host, QWidget *parent) override;
};

#endif // MEDIAPLUGIN_H
