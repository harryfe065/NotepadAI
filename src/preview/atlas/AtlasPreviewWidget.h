#ifndef ATLASPREVIEWWIDGET_H
#define ATLASPREVIEWWIDGET_H

#include "AssetPreviewWidget.h"
#include "AtlasLoader.h"

#include <QFutureWatcher>
#include <QRectF>
#include <QVector>

#include <atomic>
#include <cstdint>
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

    // One page's items bucketed by the cell of their top-left corner (CSR, no
    // per-cell allocation); items much larger than a cell sit in `large` and
    // are always visited, so the query widening stays a few cells. Paint and
    // hover then visit only the cells around the view / the pointer.
    struct PageIndex {
        double cell = 64;
        int cols = 0, rows = 0;
        double originX = 0, originY = 0;
        double maxW = 0, maxH = 0;       // largest small item
        std::vector<uint32_t> start;     // cols * rows + 1
        std::vector<int> items;          // indices into m_items
        std::vector<int> large;

        void build(const std::vector<Item> &all, const std::vector<int> &mine);
        // Appends the items that may intersect `r` (unordered); with
        // `skipSmall`, only the large ones.
        void query(const QRectF &r, bool skipSmall, std::vector<int> &out) const;
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
    std::vector<PageIndex> m_pageIndex;
    // Per-paint / per-hover buffers, kept so neither allocates.
    std::vector<int> m_queryScratch;
    QVector<QRectF> m_rectScratch;
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
