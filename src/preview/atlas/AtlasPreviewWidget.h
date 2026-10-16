#ifndef ATLASPREVIEWWIDGET_H
#define ATLASPREVIEWWIDGET_H

#include "AssetPreviewWidget.h"
#include "AtlasLoader.h"

#include <QFutureWatcher>
#include <QRectF>

#include <atomic>
#include <memory>
#include <vector>

class ImageCanvas;
class QComboBox;
class QLabel;
class QLineEdit;
class QListView;
class QSortFilterProxyModel;
class QStringListModel;
class QToolButton;
class QPainter;
struct ImageViewMath;

// Atlas / bitmap font tab: the page image on an ImageCanvas with every frame
// (or glyph) outlined, a filterable list of them, and for bitmap fonts a
// sample-text renderer using the font's advances and kerning.
class AtlasPreviewWidget : public AssetPreviewWidget
{
    Q_OBJECT

public:
    AtlasPreviewWidget(const QString &typeId, const QString &displayName, QWidget *parent = nullptr);
    ~AtlasPreviewWidget() override;

protected:
    void startLoad(bool reload) override;
    void themeChanged(bool isDark) override;

private:
    struct Item {
        QRectF rect;
        int page = 0;
        QString label;
    };

    void onLoaded();
    void showPage(int page, bool keepView);
    void paintOverlay(QPainter &p, const ImageViewMath &math);
    void select(int item, bool reveal);
    void hoverAt(const QPointF &imagePos);
    QString describe(int item) const;
    void updateInfo();
    void updateZoomLabel(double zoom, bool fit);
    void updateSample();
    void cancelLoad();

    ImageCanvas *m_canvas = nullptr;
    QFutureWatcher<AtlasLoader::Result> m_watcher;
    std::shared_ptr<std::atomic_bool> m_cancel;
    bool m_pendingReload = false;

    Atlas::DocumentPtr m_doc;
    QVector<ImageDocumentPtr> m_images;
    std::vector<Item> m_items;
    std::vector<std::vector<int>> m_pageItems;
    int m_page = 0;
    int m_selected = -1;
    int m_hover = -1;
    bool m_showFrames = true;
    QColor m_outline, m_accent;

    QToolButton *m_zoomOut = nullptr;
    QLabel *m_zoomLabel = nullptr;
    QToolButton *m_zoomIn = nullptr;
    QToolButton *m_fit = nullptr;
    QToolButton *m_actual = nullptr;
    QToolButton *m_frames = nullptr;
    QToolButton *m_checker = nullptr;
    QComboBox *m_pageBox = nullptr;

    QLineEdit *m_filter = nullptr;
    QListView *m_list = nullptr;
    QStringListModel *m_model = nullptr;
    QSortFilterProxyModel *m_proxy = nullptr;
    QWidget *m_samplePanel = nullptr;
    QLineEdit *m_sampleText = nullptr;
    QLabel *m_samplePreview = nullptr;

    QString m_status;
};

#endif // ATLASPREVIEWWIDGET_H
