#ifndef MEDIAPREVIEWWIDGET_H
#define MEDIAPREVIEWWIDGET_H

#include "AssetPreviewWidget.h"
#include "MediaPluginInterface.h"

#include <QFutureWatcher>

#include <memory>

// Audio, video and PDF tab (add-media-preview). The asset chrome lives here;
// the viewer itself comes from the NotepadAIMedia plugin, which is loaded on
// a worker when the first such tab is shown. Until it is ready the tab shows
// "Loading…" in the banner; a missing or broken plugin shows an error with
// "Open as text".
class MediaPreviewWidget : public AssetPreviewWidget
{
    Q_OBJECT

public:
    MediaPreviewWidget(const QString &typeId, const QString &displayName, MediaPlugin::Kind kind,
                       QWidget *parent = nullptr);
    ~MediaPreviewWidget() override;

protected:
    void startLoad(bool reload) override;
    void themeChanged(bool isDark) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    class Host;

    void onPluginReady();

    MediaPlugin::Kind m_kind;
    std::unique_ptr<Host> m_host;
    MediaPlugin::View *m_view = nullptr;     // owned by the layout (its widget)
    QFutureWatcher<QString> m_pluginWatcher;
    bool m_waitingForPlugin = false;
    bool m_pendingReload = false;
};

#endif // MEDIAPREVIEWWIDGET_H
