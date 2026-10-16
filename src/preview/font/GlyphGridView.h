#ifndef GLYPHGRIDVIEW_H
#define GLYPHGRIDVIEW_H

#include <QAbstractScrollArea>
#include <QRawFont>

#include <vector>

// Virtualised glyph grid of the font preview (add-media-preview D6). Holds
// only (code point, glyph id) pairs; a paint draws the visible rows, each as
// one QGlyphRun, plus a code point label per cell. Scrolling a 30 000-glyph
// font costs the same as scrolling ten glyphs.
//
// Ctrl+wheel resizes the cells. Hover reports the cell; double-click copies
// its character.
class GlyphGridView : public QAbstractScrollArea
{
    Q_OBJECT

public:
    struct Cell {
        char32_t codePoint = 0;     // 0: an unmapped glyph ("All glyphs")
        quint32 glyph = 0;
    };

    explicit GlyphGridView(QWidget *parent = nullptr);

    void setRawFont(const QRawFont &font);
    void setCells(std::vector<Cell> cells);
    const std::vector<Cell> &cells() const { return m_cells; }
    void setCellSize(int size);
    int cellSize() const { return m_cellSize; }
    int scrollRow() const;
    void setScrollRow(int row);

    static constexpr int kMinCell = 32;
    static constexpr int kMaxCell = 160;

signals:
    void hovered(int index);        // -1: none
    void cellSizeChanged(int size);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    int columns() const;
    int indexAt(const QPoint &viewportPos) const;
    QRect cellRect(int index) const;
    void updateScrollBars();
    void updateGlyphFont();

    QRawFont m_font;            // as set (any pixel size)
    QRawFont m_glyphFont;       // sized for the cells
    std::vector<Cell> m_cells;
    int m_cellSize = 56;
    int m_hover = -1;
    int m_selected = -1;
};

#endif // GLYPHGRIDVIEW_H
