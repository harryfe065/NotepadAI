#ifndef IMAGEPREVIEWWIDGET_H
#define IMAGEPREVIEWWIDGET_H

#include "AssetPreviewWidget.h"
#include "ImageDecoder.h"

#include <QFutureWatcher>

#include <atomic>
#include <memory>

class ImageCanvas;
class QComboBox;
class QLabel;
class QSlider;
class QToolButton;

// Image and texture viewer tab: ImageCanvas plus its toolbar (zoom, channel,
// checkerboard, pixel grid, tiling, mip/face/layer, animation, HDR exposure).
// Controls that do not apply to the open document stay hidden.
class ImagePreviewWidget : public AssetPreviewWidget
{
    Q_OBJECT

public:
    ImagePreviewWidget(const QString &typeId, const QString &displayName, QWidget *parent = nullptr);
    ~ImagePreviewWidget() override;

protected:
    void startLoad(bool reload) override;
    void themeChanged(bool isDark) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void onDecoded();
    void documentReady(const ImageDocumentPtr &doc, bool reload);
    void syncSurfaceControls();
    void updateZoomLabel(double zoom, bool fit);
    void updateInfo();
    void cancelDecode();

    ImageCanvas *m_canvas = nullptr;
    QFutureWatcher<ImageDecoder::Result> m_watcher;
    std::shared_ptr<std::atomic_bool> m_cancel;
    bool m_pendingReload = false;

    QToolButton *m_zoomOut = nullptr;
    QLabel *m_zoomLabel = nullptr;
    QToolButton *m_zoomIn = nullptr;
    QToolButton *m_fit = nullptr;
    QToolButton *m_actual = nullptr;
    QComboBox *m_channel = nullptr;
    QToolButton *m_checker = nullptr;
    QToolButton *m_grid = nullptr;
    QToolButton *m_tile = nullptr;
    QComboBox *m_mip = nullptr;
    QComboBox *m_face = nullptr;
    QComboBox *m_layer = nullptr;
    QToolButton *m_play = nullptr;
    QToolButton *m_prevFrame = nullptr;
    QToolButton *m_nextFrame = nullptr;
    QLabel *m_frameLabel = nullptr;
    QLabel *m_exposureLabel = nullptr;
    QSlider *m_exposure = nullptr;

    QString m_hoverText;
};

#endif // IMAGEPREVIEWWIDGET_H
