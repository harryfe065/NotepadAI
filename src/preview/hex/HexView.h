#ifndef HEXVIEW_H
#define HEXVIEW_H

#include "HexFormat.h"

#include <QAbstractScrollArea>

#include <algorithm>

class HexSource;

// The hex view's grid (add-hex-preview D3): offset, hex and ASCII columns,
// virtualised by row. A paint reads the visible rows from the HexSource page
// cache and draws each pane as at most four text runs per row, one per byte
// class, so its cost depends on the viewport only.
//
// Scrolling is by rows; past INT_MAX rows a scroll step spans several rows.
// The cursor and anchor are byte offsets; the selection is the inclusive range
// between them. Keys that the application binds elsewhere (Ctrl+C, Ctrl+A,
// Ctrl+F, Ctrl+G, F3) are claimed while the view has focus.
class HexView : public QAbstractScrollArea
{
    Q_OBJECT

public:
    explicit HexView(QWidget *parent = nullptr);

    void setSource(HexSource *source);
    // The source's bytes or size changed: clamp the cursor, keep the scroll.
    void sourceChanged();
    void setBytesPerRow(int bytes);
    int bytesPerRow() const { return m_bytesPerRow; }

    qint64 cursorOffset() const { return m_cursor; }
    qint64 selectionStart() const { return std::min(m_anchor, m_cursor); }
    qint64 selectionLength() const;
    void setCursorOffset(qint64 offset, bool extend);
    void select(qint64 start, qint64 length);
    void selectAll();
    void ensureVisible(qint64 offset);

    static constexpr qint64 kMaxCopyBytes = 16LL * 1024 * 1024;
    static constexpr int kMaxBytesPerRow = 32;

    enum class CopyFormat : std::uint8_t { Hex, Text, CArray };
    void copy(CopyFormat format);
    void copyOffset();

signals:
    void cursorChanged();
    void findRequested();
    void findNextRequested(bool backward);
    void gotoRequested();

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void changeEvent(QEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    struct Layout {
        double charWidth = 8;
        int lineHeight = 16;
        int ascent = 12;
        int digits = 8;
        double offsetX = 0, hexX = 0, asciiX = 0, endX = 0;
    };

    qint64 rowCount() const;
    qint64 rowsPerStep() const;
    qint64 topRow() const;
    int visibleRows() const;
    void updateLayout();
    void updateScrollBars();
    // Byte offset under a viewport point (and whether it is in the ASCII pane); -1 when outside.
    qint64 offsetAt(const QPoint &pos, bool *ascii) const;

    HexSource *m_source = nullptr;
    int m_bytesPerRow = 16;
    qint64 m_cursor = 0;
    qint64 m_anchor = 0;
    bool m_asciiPane = false;
    bool m_dragging = false;
    Layout m_layout;
    QString m_hexHeader, m_asciiHeader;     // rebuilt with the layout
    HexFormat::ClassRuns m_runs;            // per-row scratch, reused across rows and paints
};

#endif // HEXVIEW_H
