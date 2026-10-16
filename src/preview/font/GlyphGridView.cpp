#include "GlyphGridView.h"
#include "PaletteMix.h"

#include <QApplication>
#include <QClipboard>
#include <QGlyphRun>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QScrollBar>
#include <QWheelEvent>

#include <algorithm>
#include <climits>
#include <cmath>

namespace {

constexpr int kLabelHeight = 14;

} // namespace

GlyphGridView::GlyphGridView(QWidget *parent)
    : QAbstractScrollArea(parent)
{
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    viewport()->setMouseTracking(true);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    setFrameShape(QFrame::NoFrame);
    setToolTip(tr("Double-click a glyph to copy its character; Ctrl+wheel resizes the grid"));
}

void GlyphGridView::setRawFont(const QRawFont &font)
{
    m_font = font;
    updateGlyphFont();
    viewport()->update();
}

void GlyphGridView::updateGlyphFont()
{
    if (!m_font.isValid()) {
        m_glyphFont = QRawFont();
        return;
    }
    m_glyphFont = m_font;
    m_glyphFont.setPixelSize(std::max(8.0, (m_cellSize - kLabelHeight) * 0.62));
}

void GlyphGridView::setCells(std::vector<Cell> cells)
{
    m_cells = std::move(cells);
    m_hover = -1;
    m_selected = -1;
    updateScrollBars();
    viewport()->update();
}

void GlyphGridView::setCellSize(int size)
{
    size = std::clamp(size, kMinCell, kMaxCell);
    if (size == m_cellSize)
        return;
    // Keep the first visible glyph in place across the relayout.
    const int firstIndex = scrollRow() * columns();
    m_cellSize = size;
    updateGlyphFont();
    updateScrollBars();
    setScrollRow(firstIndex / columns());
    viewport()->update();
    emit cellSizeChanged(size);
}

int GlyphGridView::columns() const
{
    return std::max(1, viewport()->width() / m_cellSize);
}

int GlyphGridView::scrollRow() const
{
    return verticalScrollBar()->value() / m_cellSize;
}

void GlyphGridView::setScrollRow(int row)
{
    verticalScrollBar()->setValue(row * m_cellSize);
}

void GlyphGridView::updateScrollBars()
{
    const int cols = columns();
    const qint64 rows = (qint64(m_cells.size()) + cols - 1) / cols;
    const qint64 contentHeight = rows * m_cellSize;
    QScrollBar *bar = verticalScrollBar();
    bar->setRange(0, int(std::clamp<qint64>(contentHeight - viewport()->height(), 0, INT_MAX)));
    bar->setPageStep(viewport()->height());
    bar->setSingleStep(m_cellSize / 2);
}

QRect GlyphGridView::cellRect(int index) const
{
    const int cols = columns();
    const int left = (viewport()->width() - cols * m_cellSize) / 2;
    return QRect(left + (index % cols) * m_cellSize, (index / cols) * m_cellSize - verticalScrollBar()->value(),
                 m_cellSize, m_cellSize);
}

int GlyphGridView::indexAt(const QPoint &pos) const
{
    const int cols = columns();
    const int left = (viewport()->width() - cols * m_cellSize) / 2;
    const int x = pos.x() - left;
    if (x < 0 || x >= cols * m_cellSize)
        return -1;
    const qint64 row = (qint64(pos.y()) + verticalScrollBar()->value()) / m_cellSize;
    const qint64 index = row * cols + x / m_cellSize;
    return index >= 0 && index < qint64(m_cells.size()) ? int(index) : -1;
}

void GlyphGridView::paintEvent(QPaintEvent *event)
{
    QPainter p(viewport());
    const QPalette &pal = palette();
    const QColor base = pal.color(QPalette::Base);
    const QColor text = pal.color(QPalette::Text);
    const QColor muted = pal.color(QPalette::PlaceholderText);
    const QColor grid = paletteMix(text, base, 0.88);
    p.fillRect(event->rect(), base);
    if (m_cells.empty() || !m_glyphFont.isValid())
        return;

    const int cols = columns();
    const int offset = verticalScrollBar()->value();
    const int firstRow = std::max(0, (offset + event->rect().top()) / m_cellSize);
    const int lastRow = (offset + event->rect().bottom()) / m_cellSize;
    const int left = (viewport()->width() - cols * m_cellSize) / 2;
    const double ascent = m_glyphFont.ascent();
    const double glyphHeight = ascent + m_glyphFont.descent();
    const double glyphTop = (m_cellSize - kLabelHeight - glyphHeight) / 2.0;

    QFont labelFont = font();
    labelFont.setPixelSize(9);
    p.setFont(labelFont);

    std::vector<quint32> glyphs;
    std::vector<QPointF> advances;
    QList<quint32> runGlyphs;
    QList<QPointF> runPositions;
    for (int row = firstRow; row <= lastRow; ++row) {
        const qint64 first = qint64(row) * cols;
        if (first >= qint64(m_cells.size()))
            break;
        const int count = int(std::min<qint64>(cols, qint64(m_cells.size()) - first));
        const int y = row * m_cellSize - offset;

        glyphs.resize(size_t(count));
        advances.resize(size_t(count));
        for (int i = 0; i < count; ++i)
            glyphs[size_t(i)] = m_cells[size_t(first + i)].glyph;
        m_glyphFont.advancesForGlyphIndexes(glyphs.data(), advances.data(), count);

        runGlyphs.clear();
        runPositions.clear();
        for (int i = 0; i < count; ++i) {
            const int index = int(first + i);
            const QRect cell(left + i * m_cellSize, y, m_cellSize, m_cellSize);
            if (index == m_selected || index == m_hover) {
                QColor fill = pal.color(QPalette::Highlight);
                fill.setAlpha(index == m_selected ? 70 : 35);
                p.fillRect(cell, fill);
            }
            runGlyphs.append(glyphs[size_t(i)]);
            runPositions.append(QPointF(cell.left() + (m_cellSize - advances[size_t(i)].x()) / 2.0,
                                        cell.top() + glyphTop + ascent));
            const Cell &c = m_cells[size_t(index)];
            p.setPen(muted);
            const QString label = c.codePoint ? QString::number(quint32(c.codePoint), 16).toUpper()
                                              : QStringLiteral("#%1").arg(c.glyph);
            p.drawText(QRect(cell.left(), cell.bottom() - kLabelHeight, m_cellSize, kLabelHeight),
                       Qt::AlignHCenter | Qt::AlignVCenter, label);
        }
        QGlyphRun run;
        run.setRawFont(m_glyphFont);
        run.setGlyphIndexes(runGlyphs);
        run.setPositions(runPositions);
        p.setPen(text);
        p.drawGlyphRun(QPointF(0, 0), run);

        p.setPen(grid);
        p.drawLine(left, y + m_cellSize - 1, left + count * m_cellSize, y + m_cellSize - 1);
        for (int i = 0; i <= count; ++i)
            p.drawLine(left + i * m_cellSize, y, left + i * m_cellSize, y + m_cellSize - 1);
    }
}

void GlyphGridView::resizeEvent(QResizeEvent *event)
{
    const int firstIndex = scrollRow() * columns();
    QAbstractScrollArea::resizeEvent(event);
    updateScrollBars();
    setScrollRow(firstIndex / columns());
}

void GlyphGridView::mouseMoveEvent(QMouseEvent *event)
{
    const int index = indexAt(event->position().toPoint());
    if (index == m_hover)
        return;
    const int old = m_hover;
    m_hover = index;
    if (old >= 0)
        viewport()->update(cellRect(old));
    if (index >= 0)
        viewport()->update(cellRect(index));
    emit hovered(index);
}

void GlyphGridView::mousePressEvent(QMouseEvent *event)
{
    const int index = indexAt(event->position().toPoint());
    if (index == m_selected)
        return;
    const int old = m_selected;
    m_selected = index;
    if (old >= 0)
        viewport()->update(cellRect(old));
    if (index >= 0)
        viewport()->update(cellRect(index));
}

void GlyphGridView::mouseDoubleClickEvent(QMouseEvent *event)
{
    const int index = indexAt(event->position().toPoint());
    if (index < 0 || m_cells[size_t(index)].codePoint == 0)
        return;
    const char32_t cp = m_cells[size_t(index)].codePoint;
    QApplication::clipboard()->setText(QString::fromUcs4(&cp, 1));
}

void GlyphGridView::leaveEvent(QEvent *event)
{
    QAbstractScrollArea::leaveEvent(event);
    if (m_hover >= 0) {
        viewport()->update(cellRect(m_hover));
        m_hover = -1;
        emit hovered(-1);
    }
}

void GlyphGridView::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        const int steps = event->angleDelta().y() / 120;
        if (steps != 0)
            setCellSize(m_cellSize + steps * 8);
        event->accept();
        return;
    }
    QAbstractScrollArea::wheelEvent(event);
}

void GlyphGridView::scrollContentsBy(int dx, int dy)
{
    Q_UNUSED(dx);
    Q_UNUSED(dy);
    // The cell under a still mouse changed; it is picked up on the next move.
    if (m_hover >= 0) {
        m_hover = -1;
        emit hovered(-1);
    }
    viewport()->update();
}
