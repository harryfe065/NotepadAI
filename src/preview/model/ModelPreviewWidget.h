#ifndef MODELPREVIEWWIDGET_H
#define MODELPREVIEWWIDGET_H

#include "AssetPreviewWidget.h"
#include "ModelLoader.h"

#include <QFutureWatcher>

#include <atomic>
#include <memory>

class ModelViewWindow;
class QComboBox;
class QLabel;
class QSlider;
class QToolButton;

// 3D model tab (glTF/GLB, FBX, OBJ, STL, PLY, VOX): a native GL view
// (ModelViewWindow in a window container) plus its toolbar (shading, wire,
// grid, skeleton, view presets, projection, Z-up, animation playback).
// The file loads on a worker; large files get a cancellable banner.
class ModelPreviewWidget : public AssetPreviewWidget
{
    Q_OBJECT

public:
    ModelPreviewWidget(const QString &typeId, const QString &displayName, QWidget *parent = nullptr);
    ~ModelPreviewWidget() override;

protected:
    void startLoad(bool reload) override;
    void themeChanged(bool isDark) override;

private:
    void onLoaded();
    void syncControls();
    void updateTimeLabel(float seconds);
    void updateInfo();
    void cancelLoad();

    ModelViewWindow *m_view = nullptr;
    QWidget *m_container = nullptr;
    QFutureWatcher<ModelLoader::Result> m_watcher;
    std::shared_ptr<std::atomic_bool> m_cancel;
    bool m_pendingReload = false;
    bool m_glFailed = false;

    QComboBox *m_shading = nullptr;
    QToolButton *m_wire = nullptr;
    QToolButton *m_grid = nullptr;
    QToolButton *m_skeleton = nullptr;
    QToolButton *m_frame = nullptr;
    QToolButton *m_viewMenu = nullptr;
    QToolButton *m_ortho = nullptr;
    QToolButton *m_zUp = nullptr;
    QComboBox *m_animation = nullptr;
    QToolButton *m_play = nullptr;
    QSlider *m_timeline = nullptr;
    QLabel *m_timeLabel = nullptr;
};

#endif // MODELPREVIEWWIDGET_H
