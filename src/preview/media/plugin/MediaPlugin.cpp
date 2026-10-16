#include "MediaPlugin.h"

#include "AudioView.h"
#include "VideoView.h"
#ifdef NOTEPADAI_MEDIA_HAS_PDF
#include "PdfView.h"
#endif

MediaPlugin::View *MediaPluginFactory::create(MediaPlugin::Kind kind, MediaPlugin::Host *host, QWidget *parent)
{
    switch (kind) {
    case MediaPlugin::Kind::Audio:
        return new AudioView(host, parent);
    case MediaPlugin::Kind::Video:
        return new VideoView(host, parent);
    case MediaPlugin::Kind::Pdf:
#ifdef NOTEPADAI_MEDIA_HAS_PDF
        return new PdfView(host, parent);
#else
        return nullptr;
#endif
    }
    return nullptr;
}
