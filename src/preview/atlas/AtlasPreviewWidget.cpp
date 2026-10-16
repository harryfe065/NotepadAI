#include "AtlasPreviewWidget.h"

#include "ImageCanvas.h"
#include "ImageDecoder.h"

#include <QComboBox>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPainter>
#include <QScrollArea>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStringListModel>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kMaxWarningsShown = 3;
constexpr int kMaxListItems = 200000;   // the list stops here; the overlay still shows every rect
constexpr int kMinOutlinePx = 3;        // smaller rects are not outlined

QString zoomText(double zoom)
{
    const double pct = zoom * 100.0;
    if (pct >= 10.0 || std::abs(pct - std::round(pct)) < 0.05)
        return QStringLiteral("%1%").arg(qRound(pct));
    return QStringLiteral("%1%").arg(pct, 0, 'f', 1);
}

QString rectText(const QRectF &r)
{
    return QStringLiteral("%1 × %2 at %3, %4").arg(int(r.width())).arg(int(r.height())).arg(int(r.x())).arg(int(r.y()));
}

constexpr double kMaxIndexCells = double(1 << 20);   // 4 MB of starts per page
constexpr double kLargeCells = 4.0;                  // items wider / taller than this many cells are "large"

// Cell of a coordinate already divided by the cell size, clamped to [0, n - 1]
// in floating point first, so nothing overflows an int.
int clampCell(double v, int n)
{
    if (v >= double(n - 1))
        return n - 1;
    return v > 0 ? int(v) : 0;
}

} // namespace

void AtlasPreviewWidget::PageIndex::build(const std::vector<Item> &all, const std::vector<int> &mine)
{
    *this = PageIndex();
    if (mine.empty())
        return;
    double minX = 0, minY = 0, maxX = 0, maxY = 0, maxR = 0, maxB = 0;
    for (size_t k = 0; k < mine.size(); ++k) {
        const QRectF r = all[size_t(mine[k])].rect.normalized();
        if (k == 0) {
            minX = maxX = r.left();
            minY = maxY = r.top();
            maxR = r.right();
            maxB = r.bottom();
            continue;
        }
        minX = std::min(minX, r.left());
        minY = std::min(minY, r.top());
        maxX = std::max(maxX, r.left());
        maxY = std::max(maxY, r.top());
        maxR = std::max(maxR, r.right());
        maxB = std::max(maxB, r.bottom());
    }
    originX = minX;
    originY = minY;

    // About one item per cell on average (at least 16 px), grown until the
    // table is bounded. Rects come from ints, so every span is finite.
    const double area = std::max(1.0, (maxR - minX) * (maxB - minY));
    cell = std::max(16.0, std::sqrt(area / double(mine.size())));
    double c = 1, r = 1;
    for (;;) {
        c = std::floor((maxX - minX) / cell) + 1.0;
        r = std::floor((maxY - minY) / cell) + 1.0;
        if (c * r <= kMaxIndexCells)
            break;
        cell *= 2.0;
    }
    cols = int(c);
    rows = int(r);

    const double largeSide = kLargeCells * cell;
    auto cellOf = [this](const QRectF &rect) {
        const int cx = clampCell((rect.left() - originX) / cell, cols);
        const int cy = clampCell((rect.top() - originY) / cell, rows);
        return size_t(cy) * size_t(cols) + size_t(cx);
    };
    const size_t cellCount = size_t(cols) * size_t(rows);
    start.assign(cellCount + 1, 0);
    size_t smallCount = 0;
    for (const int idx : mine) {
        const QRectF rect = all[size_t(idx)].rect.normalized();
        if (rect.width() > largeSide || rect.height() > largeSide) {
            large.push_back(idx);
            continue;
        }
        maxW = std::max(maxW, rect.width());
        maxH = std::max(maxH, rect.height());
        ++start[cellOf(rect) + 1];
        ++smallCount;
    }
    for (size_t i = 0; i < cellCount; ++i)
        start[i + 1] += start[i];
    items.resize(smallCount);
    std::vector<uint32_t> fill(start.begin(), start.end() - 1);
    for (const int idx : mine) {
        const QRectF rect = all[size_t(idx)].rect.normalized();
        if (rect.width() > largeSide || rect.height() > largeSide)
            continue;
        items[fill[cellOf(rect)]++] = idx;
    }
}

void AtlasPreviewWidget::PageIndex::query(const QRectF &r, bool skipSmall, std::vector<int> &out) const
{
    out.insert(out.end(), large.begin(), large.end());
    if (skipSmall || items.empty())
        return;
    // A small item is stored under its top-left cell and reaches right / down
    // by at most maxW / maxH, so widen the query to the left / up.
    const double x0 = r.left() - maxW, y0 = r.top() - maxH, x1 = r.right(), y1 = r.bottom();
    if (!(x1 >= originX && y1 >= originY && x0 <= originX + cols * cell && y0 <= originY + rows * cell))
        return;
    const int cx0 = clampCell(std::floor((x0 - originX) / cell), cols);
    const int cx1 = clampCell(std::floor((x1 - originX) / cell), cols);
    const int cy0 = clampCell(std::floor((y0 - originY) / cell), rows);
    const int cy1 = clampCell(std::floor((y1 - originY) / cell), rows);
    for (int cy = cy0; cy <= cy1; ++cy) {
        const size_t row = size_t(cy) * size_t(cols);
        // Cells of one row are contiguous: one range per row.
        out.insert(out.end(), items.begin() + start[row + size_t(cx0)], items.begin() + start[row + size_t(cx1) + 1]);
    }
}

AtlasPreviewWidget::AtlasPreviewWidget(const QString &typeId, const QString &displayName, QWidget *parent)
    : AssetPreviewWidget(typeId, displayName, parent)
    , m_cancel(std::make_shared<std::atomic_bool>(false))
{
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);
    m_canvas = new ImageCanvas(splitter);
    splitter->addWidget(m_canvas);

    auto *side = new QWidget(splitter);
    auto *sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(6, 6, 6, 6);
    sideLayout->setSpacing(6);
    m_filter = new QLineEdit(side);
    m_filter->setClearButtonEnabled(true);
    m_filter->setPlaceholderText(tr("Filter by name…"));
    sideLayout->addWidget(m_filter);
    m_list = new QListView(side);
    m_list->setUniformItemSizes(true);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_model = new QStringListModel(this);
    m_proxy = new QSortFilterProxyModel(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_list->setModel(m_proxy);
    sideLayout->addWidget(m_list, 1);

    m_samplePanel = new QWidget(side);
    auto *sampleLayout = new QVBoxLayout(m_samplePanel);
    sampleLayout->setContentsMargins(0, 0, 0, 0);
    sampleLayout->setSpacing(4);
    m_sampleText = new QLineEdit(m_samplePanel);
    m_sampleText->setText(QStringLiteral("The quick brown fox jumps over the lazy dog 0123456789"));
    m_sampleText->setPlaceholderText(tr("Sample text"));
    sampleLayout->addWidget(m_sampleText);
    auto *scroll = new QScrollArea(m_samplePanel);
    scroll->setFixedHeight(150);
    scroll->setWidgetResizable(false);
    m_samplePreview = new QLabel;
    m_samplePreview->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    scroll->setWidget(m_samplePreview);
    sampleLayout->addWidget(scroll);
    sideLayout->addWidget(m_samplePanel);
    m_samplePanel->hide();

    splitter->addWidget(side);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    splitter->setSizes({700, 260});
    setContentWidget(splitter);

    m_zoomOut = addToolButton(QStringLiteral("−"), tr("Zoom out (-)"));
    m_zoomLabel = new QLabel(QStringLiteral("100%"));
    m_zoomLabel->setMinimumWidth(m_zoomLabel->fontMetrics().horizontalAdvance(QStringLiteral("10000%")));
    m_zoomLabel->setAlignment(Qt::AlignCenter);
    addToolWidget(m_zoomLabel);
    m_zoomIn = addToolButton(QStringLiteral("+"), tr("Zoom in (+)"));
    m_fit = addToolButton(tr("Fit"), tr("Fit to the tab (0)"), true);
    m_actual = addToolButton(QStringLiteral("1:1"), tr("Actual size (1)"));
    addToolGap();
    m_frames = addToolButton(tr("Outlines"), tr("Outline every frame or glyph"), true);
    m_frames->setChecked(true);
    m_checker = addToolButton(tr("Checker"), tr("Checkerboard under transparent pixels"), true);
    m_checker->setChecked(true);
    m_pageBox = new QComboBox;
    m_pageBox->setToolTip(tr("Page"));
    addToolWidget(m_pageBox);
    m_pageBox->hide();

    connect(m_zoomOut, &QToolButton::clicked, m_canvas, &ImageCanvas::zoomOut);
    connect(m_zoomIn, &QToolButton::clicked, m_canvas, &ImageCanvas::zoomIn);
    connect(m_fit, &QToolButton::clicked, m_canvas, &ImageCanvas::zoomToFit);
    connect(m_actual, &QToolButton::clicked, m_canvas, &ImageCanvas::zoomToActual);
    connect(m_canvas, &ImageCanvas::zoomChanged, this, &AtlasPreviewWidget::updateZoomLabel);
    connect(m_checker, &QToolButton::toggled, m_canvas, &ImageCanvas::setCheckerboard);
    connect(m_frames, &QToolButton::toggled, this, [this](bool on) {
        m_showFrames = on;
        m_canvas->updateOverlay();
    });
    connect(m_pageBox, QOverload<int>::of(&QComboBox::activated), this, [this](int page) { showPage(page, false); });

    connect(m_filter, &QLineEdit::textChanged, m_proxy, &QSortFilterProxyModel::setFilterFixedString);
    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &current, const QModelIndex &) {
                if (current.isValid())
                    select(m_proxy->mapToSource(current).row(), true);
            });
    connect(m_canvas, &ImageCanvas::imagePointHovered, this, &AtlasPreviewWidget::hoverAt);
    connect(m_canvas, &ImageCanvas::imageClicked, this, [this](const QPointF &p) {
        hoverAt(p);
        select(m_hover, false);
        if (m_selected >= 0) {
            const QModelIndex idx = m_proxy->mapFromSource(m_model->index(m_selected));
            if (idx.isValid()) {
                QSignalBlocker block(m_list->selectionModel());
                m_list->setCurrentIndex(idx);
                m_list->scrollTo(idx);
            }
        }
    });
    connect(m_sampleText, &QLineEdit::textChanged, this, &AtlasPreviewWidget::updateSample);
    connect(&m_watcher, &QFutureWatcher<AtlasLoader::Result>::finished, this, &AtlasPreviewWidget::onLoaded);

    m_canvas->setOverlay([this](QPainter &p, const ImageViewMath &m) { paintOverlay(p, m); });
}

AtlasPreviewWidget::~AtlasPreviewWidget()
{
    m_cancel->store(true);
}

void AtlasPreviewWidget::cancelLoad()
{
    m_cancel->store(true);
    m_cancel = std::make_shared<std::atomic_bool>(false);
}

void AtlasPreviewWidget::startLoad(bool reload)
{
    cancelLoad();
    m_pendingReload = reload;
    const QString path = filePath();
    const QFileInfo info(path);
    if (!info.exists()) {
        showBanner(BannerKind::Error, tr("The file no longer exists."));
        return;
    }
    if (info.size() > AtlasLoader::kMaxFileBytes) {
        showBanner(BannerKind::Error, tr("Atlas files over %1 are not previewed.")
                                          .arg(ImageDecoder::formatBytes(AtlasLoader::kMaxFileBytes)));
        return;
    }
    const std::shared_ptr<std::atomic_bool> cancel = m_cancel;
    m_watcher.setFuture(QtConcurrent::run([path, cancel]() { return AtlasLoader::load(path, *cancel); }));
}

void AtlasPreviewWidget::onLoaded()
{
    AtlasLoader::Result result = m_watcher.result();
    if (result.cancelled)
        return;
    if (!result.doc) {
        if (m_doc && m_pendingReload)
            showBanner(BannerKind::Warning, tr("Could not reload the file: %1").arg(result.error));
        else
            showBanner(BannerKind::Error, tr("Could not open the file: %1").arg(result.error));
        return;
    }

    const bool keepView = m_pendingReload && m_doc && m_doc->pages.size() == result.doc->pages.size();
    m_doc = result.doc;
    m_images = result.images;

    // One list of drawable rectangles for atlases and fonts alike.
    m_items.clear();
    std::vector<std::vector<int>> pageItems(m_doc->pages.size());
    QStringList labels;
    const bool font = m_doc->kind == Atlas::Document::Kind::BitmapFont;
    if (font) {
        m_items.reserve(m_doc->glyphs.size());
        for (const Atlas::Glyph &g : m_doc->glyphs) {
            Item it;
            it.rect = QRectF(g.rect);
            it.page = g.page;
            const QString ch = g.id >= 0x20 && g.id != 0x7f ? QString::fromUcs4(reinterpret_cast<const char32_t *>(&g.id), 1)
                                                            : QString();
            it.label = QStringLiteral("U+%1  %2").arg(QString::number(g.id, 16).toUpper().rightJustified(4, QLatin1Char('0')), ch);
            m_items.push_back(std::move(it));
        }
    } else {
        m_items.reserve(m_doc->frames.size());
        for (const Atlas::Frame &f : m_doc->frames) {
            Item it;
            it.rect = QRectF(f.rect);
            it.page = f.page;
            it.label = f.name;
            m_items.push_back(std::move(it));
        }
    }
    for (size_t i = 0; i < m_items.size(); ++i) {
        const int page = m_items[i].page;
        if (page >= 0 && size_t(page) < pageItems.size())
            pageItems[size_t(page)].push_back(int(i));
        if (int(i) < kMaxListItems)
            labels << m_items[i].label;
    }
    m_pageIndex.assign(pageItems.size(), {});
    for (size_t pg = 0; pg < pageItems.size(); ++pg)
        m_pageIndex[pg].build(m_items, pageItems[pg]);
    m_model->setStringList(labels);
    m_filter->setPlaceholderText(font ? tr("Filter glyphs…") : tr("Filter by name…"));
    m_samplePanel->setVisible(font);

    m_pageBox->clear();
    for (size_t i = 0; i < m_doc->pages.size(); ++i)
        m_pageBox->addItem(tr("Page %1  %2").arg(i + 1).arg(QFileInfo(m_doc->pages[i].file).fileName()));
    m_pageBox->setVisible(m_doc->pages.size() > 1);

    if (!m_doc->warnings.isEmpty()) {
        QStringList shown = m_doc->warnings.mid(0, kMaxWarningsShown);
        if (m_doc->warnings.size() > kMaxWarningsShown)
            shown << tr("and %n more", nullptr, int(m_doc->warnings.size() - kMaxWarningsShown));
        showBanner(BannerKind::Warning, shown.join(QStringLiteral("\n")));
    } else {
        hideBanner();
    }

    m_selected = m_hover = -1;
    int first = 0;
    for (qsizetype i = 0; i < m_images.size(); ++i) {
        if (m_images[i]) {
            first = int(i);
            break;
        }
    }
    showPage(keepView ? std::min(m_page, int(m_doc->pages.size()) - 1) : first, keepView);
    updateSample();
    updateInfo();
}

void AtlasPreviewWidget::showPage(int page, bool keepView)
{
    if (!m_doc || page < 0 || size_t(page) >= m_doc->pages.size())
        return;
    m_page = page;
    m_pageBox->setCurrentIndex(page);
    // A page without an image clears the canvas: the previous page's pixels
    // must not sit under this page's outlines.
    const bool hasImage = qsizetype(page) < m_images.size() && m_images[page];
    m_canvas->setDocument(hasImage ? m_images[page] : ImageDocumentPtr(), keepView && hasImage);
    m_canvas->updateOverlay();
    updateInfo();
}

void AtlasPreviewWidget::paintOverlay(QPainter &p, const ImageViewMath &m)
{
    if (!m_showFrames || size_t(m_page) >= m_pageIndex.size())
        return;
    const QRectF visible = m.visibleImageRect();
    const PageIndex &index = m_pageIndex[size_t(m_page)];
    // Small items below kMinOutlinePx on both sides are never outlined: when
    // even the largest is, skip them all (a huge font zoomed out).
    const bool skipSmall = index.maxW * m.zoom < kMinOutlinePx && index.maxH * m.zoom < kMinOutlinePx;
    m_queryScratch.clear();
    index.query(visible, skipSmall, m_queryScratch);
    m_rectScratch.clear();   // keeps its capacity
    for (const int idx : m_queryScratch) {
        const Item &it = m_items[size_t(idx)];
        if (idx == m_selected || idx == m_hover || !it.rect.intersects(visible))
            continue;
        const QRectF v(m.imageToView(it.rect.topLeft()), it.rect.size() * m.zoom);
        if (v.width() < kMinOutlinePx && v.height() < kMinOutlinePx)
            continue;
        m_rectScratch.append(v);
    }
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(m_outline, 0));
    p.drawRects(m_rectScratch.constData(), int(m_rectScratch.size()));

    auto emphasise = [&](int idx, int fillAlpha) {
        if (idx < 0 || size_t(idx) >= m_items.size() || m_items[size_t(idx)].page != m_page)
            return;
        const Item &it = m_items[size_t(idx)];
        const QRectF v(m.imageToView(it.rect.topLeft()), it.rect.size() * m.zoom);
        QColor fill = m_accent;
        fill.setAlpha(fillAlpha);
        p.setPen(QPen(m_accent, 2));
        p.setBrush(fill);
        p.drawRect(v);
    };
    emphasise(m_hover, 40);
    emphasise(m_selected, 70);
}

void AtlasPreviewWidget::hoverAt(const QPointF &p)
{
    int hit = -1;
    if (p.x() > -1e8 && size_t(m_page) < m_pageIndex.size()) {
        m_queryScratch.clear();
        m_pageIndex[size_t(m_page)].query(QRectF(p, QSizeF(0, 0)), false, m_queryScratch);
        double bestArea = 0;
        for (const int idx : m_queryScratch) {
            const QRectF &r = m_items[size_t(idx)].rect;
            if (!r.contains(p))
                continue;
            // Nested rectangles: the smallest one is the one pointed at (the
            // first listed on a tie; the query is unordered).
            const double area = r.width() * r.height();
            if (hit < 0 || area < bestArea || (area == bestArea && idx < hit)) {
                hit = idx;
                bestArea = area;
            }
        }
    }
    if (hit == m_hover)
        return;
    m_hover = hit;
    m_canvas->updateOverlay();
    updateInfo();
}

void AtlasPreviewWidget::select(int item, bool reveal)
{
    m_selected = item;
    if (reveal && item >= 0 && size_t(item) < m_items.size()) {
        const Item &it = m_items[size_t(item)];
        if (it.page != m_page)
            showPage(it.page, false);
        m_canvas->revealImageRect(it.rect);
    }
    m_canvas->updateOverlay();
    updateInfo();
}

QString AtlasPreviewWidget::describe(int item) const
{
    if (!m_doc || item < 0 || size_t(item) >= m_items.size())
        return {};
    if (m_doc->kind == Atlas::Document::Kind::BitmapFont) {
        const Atlas::Glyph &g = m_doc->glyphs[size_t(item)];
        return tr("%1  ·  %2  ·  offset %3, %4  ·  advance %5")
            .arg(m_items[size_t(item)].label.simplified(), rectText(m_items[size_t(item)].rect))
            .arg(g.xoffset).arg(g.yoffset).arg(g.xadvance);
    }
    const Atlas::Frame &f = m_doc->frames[size_t(item)];
    QString text = QStringLiteral("%1  ·  %2").arg(f.name, rectText(m_items[size_t(item)].rect));
    if (f.rotated)
        text += tr("  ·  rotated");
    // A rotated frame is stored turned: its untrimmed size compares with the
    // rect's sides swapped.
    const QSize shown = f.rotated ? f.rect.size().transposed() : f.rect.size();
    if (f.sourceSize.isValid() && f.sourceSize != shown)
        text += tr("  ·  trimmed from %1 × %2").arg(f.sourceSize.width()).arg(f.sourceSize.height());
    if (f.hasPivot)
        text += tr("  ·  pivot %1, %2").arg(f.pivot.x()).arg(f.pivot.y());
    if (!f.tag.isEmpty())
        text += tr("  ·  tag %1").arg(f.tag);
    return text;
}

void AtlasPreviewWidget::updateZoomLabel(double zoom, bool fit)
{
    m_zoomLabel->setText(zoomText(zoom));
    m_fit->setChecked(fit);
}

void AtlasPreviewWidget::updateInfo()
{
    if (!m_doc) {
        setInfoText(QString());
        return;
    }
    QStringList parts;
    parts << m_doc->format;
    if (m_doc->kind == Atlas::Document::Kind::BitmapFont) {
        if (!m_doc->face.isEmpty())
            parts << m_doc->face;
        if (m_doc->fontSize > 0)
            parts << tr("%1 px").arg(m_doc->fontSize);
        parts << tr("%n glyph(s)", nullptr, int(m_doc->glyphs.size()));
        if (!m_doc->kernings.empty())
            parts << tr("%n kerning pair(s)", nullptr, int(m_doc->kernings.size()));
    } else {
        parts << tr("%n frame(s)", nullptr, int(m_doc->frames.size()));
    }
    if (m_doc->pages.size() > 1)
        parts << tr("%n page(s)", nullptr, int(m_doc->pages.size()));
    QString text = parts.join(QStringLiteral("  ·  "));
    const int shown = m_hover >= 0 ? m_hover : m_selected;
    if (shown >= 0)
        text += QStringLiteral("        ") + describe(shown);
    setInfoText(text);
}

void AtlasPreviewWidget::updateSample()
{
    if (!m_doc || m_doc->kind != Atlas::Document::Kind::BitmapFont) {
        m_samplePreview->clear();
        return;
    }
    std::vector<QImage> pages;
    pages.reserve(size_t(m_images.size()));
    for (const ImageDocumentPtr &doc : m_images)
        pages.push_back(doc && !doc->surfaces.empty() ? doc->surfaces.front() : QImage());
    const QImage text = Atlas::renderBitmapFont(*m_doc, pages, m_sampleText->text(), 420);
    if (text.isNull()) {
        m_samplePreview->clear();
        m_samplePreview->resize(1, 1);
        return;
    }
    // Fonts are usually white or alpha-only: show them on a neutral grey.
    QImage shown(text.size() + QSize(16, 16), QImage::Format_RGB32);
    shown.fill(QColor(54, 54, 58));
    QPainter p(&shown);
    p.drawImage(QPoint(8, 8), text);
    p.end();
    m_samplePreview->setPixmap(QPixmap::fromImage(shown));
    m_samplePreview->resize(shown.size());
}

void AtlasPreviewWidget::themeChanged(bool isDark)
{
    const QColor base = palette().color(QPalette::Base);
    const QColor shade = isDark ? base.lighter(150) : base.darker(118);
    m_canvas->setColors(base, base, shade);
    m_accent = palette().color(QPalette::Highlight);
    m_outline = isDark ? QColor(255, 255, 255, 110) : QColor(0, 0, 0, 120);
    m_canvas->updateOverlay();
}
