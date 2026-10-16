#ifndef TILEMAPPREVIEWWIDGET_H
#define TILEMAPPREVIEWWIDGET_H

#include "AssetPreviewWidget.h"
#include "TileMapLoader.h"

#include <QFutureWatcher>

#include <atomic>
#include <memory>

class TileMapCanvas;
class QLabel;
class QMenu;
class QToolButton;

// Tile map tab (Tiled .tmx/.tmj/.tsx/.tsj, LDtk .ldtk/.ldtkl): the chunk-cached
// TileMapCanvas plus zoom, layer toggles, grid, objects and animation controls.
// The file and its tileset images load on a worker with a cancellable banner.
class TileMapPreviewWidget : public AssetPreviewWidget
{
    Q_OBJECT

public:
    TileMapPreviewWidget(const QString &typeId, const QString &displayName, QWidget *parent = nullptr);
    ~TileMapPreviewWidget() override;

protected:
    void startLoad(bool reload) override;
    void themeChanged(bool isDark) override;

private:
    void onLoaded();
    void rebuildLayerMenu();
    void updateInfo();
    void updateZoomLabel(double zoom, bool fit);
    void cancelLoad();

    TileMapCanvas *m_canvas = nullptr;
    QFutureWatcher<TileMapLoader::Result> m_watcher;
    std::shared_ptr<std::atomic_bool> m_cancel;
    bool m_pendingReload = false;

    QToolButton *m_zoomOut = nullptr;
    QLabel *m_zoomLabel = nullptr;
    QToolButton *m_zoomIn = nullptr;
    QToolButton *m_fit = nullptr;
    QToolButton *m_actual = nullptr;
    QToolButton *m_layers = nullptr;
    QMenu *m_layerMenu = nullptr;
    QToolButton *m_grid = nullptr;
    QToolButton *m_objects = nullptr;
    QToolButton *m_animate = nullptr;

    QString m_hoverText;
};

#endif // TILEMAPPREVIEWWIDGET_H
