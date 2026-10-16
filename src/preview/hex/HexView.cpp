#include "HexView.h"

#include "HexFormat.h"
#include "HexSource.h"
#include "PaletteMix.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPaintEvent>
#include <QScrollBar>

#include <climits>
#include <cmath>

namespace {

constexpr int kMargin = 8;
constexpr int kClassCount = HexFormat::kByteClasses;

} // namespace

HexView::HexView(QWidget *parent)
    : QAbstractScrollArea(parent)
{
    QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    setFont(fixed);
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    viewport()->setCursor(Qt::IBeamCursor);
    updateLayout();
}

void HexView::setSource(HexSource *source)
{
    m_source = source;
    sourceChanged();
}

void HexView::sourceChanged()
{
    const qint64 size = m_source ? m_source->size() : 0;
    const qint64 last = std::max<qint64>(0, size - 1);
    m_cursor = std::clamp<qint64>(m_cursor, 0, last);
    m_anchor = std::clamp<qint64>(m_anchor, 0, last);
    updateLayout();
    updateScrollBars();
    viewport()->update();
    emit cursorChanged();
}

void HexView::setBytesPerRow(int bytes)
{
    if (bytes == m_bytesPerRow || bytes <= 0 || bytes > kMaxBytesPerRow)
        return;
    m_bytesPerRow = bytes;
    updateLayout();
    updateScrollBars();
    ensureVisible(m_cursor);
    viewport()->update();
}

qint64 HexView::selectionLength() const
{
    if (!m_source || m_source->size() == 0)
        return 0;
    return std::abs(m_cursor - m_anchor) + 1;
}

void HexView::updateLayout()
{
    const QFontMetricsF fm(font());
    Layout &l = m_layout;
    l.charWidth = fm.horizontalAdvance(QLatin1Char('0'));
    l.lineHeight = int(std::ceil(fm.height())) + 2;
    l.ascent = int(std::ceil(fm.ascent())) + 1;
    l.digits = HexFormat::offsetDigits(m_source ? m_source->size() : 0);
    l.offsetX = kMargin;
    l.hexX = l.offsetX + (l.digits + 2) * l.charWidth;
    l.asciiX = l.hexX + (HexFormat::hexColumnWidth(m_bytesPerRow) + 2) * l.charWidth;
    l.endX = l.asciiX + m_bytesPerRow * l.charWidth + kMargin;
    m_hexHeader = HexFormat::hexHeader(m_bytesPerRow);
    m_asciiHeader = HexFormat::asciiHeader(m_bytesPerRow);
}

qint64 HexView::rowCount() const
{
    const qint64 size = m_source ? m_source->size() : 0;
    return std::max<qint64>(1, (size + m_bytesPerRow - 1) / m_bytesPerRow);
}

qint64 HexView::rowsPerStep() const
{
    return std::max<qint64>(1, (rowCount() + INT_MAX - 1) / INT_MAX);
}

qint64 HexView::topRow() const
{
    return qint64(verticalScrollBar()->value()) * rowsPerStep();
}

int HexView::visibleRows() const
{
    // The first line is the column header.
    return std::max(1, (viewport()->height() - m_layout.lineHeight) / m_layout.lineHeight);
}

void HexView::updateScrollBars()
{
    const qint64 step = rowsPerStep();
    const qint64 maxTop = std::max<qint64>(0, rowCount() - visibleRows());
    QScrollBar *v = verticalScrollBar();
    v->setRange(0, int((maxTop + step - 1) / step));
    v->setPageStep(std::max(1, int(visibleRows() / step)));
    v->setSingleStep(1);
    QScrollBar *h = horizontalScrollBar();
    h->setRange(0, std::max(0, int(std::ceil(m_layout.endX)) - viewport()->width()));
    h->setPageStep(viewport()->width());
}

void HexView::ensureVisible(qint64 offset)
{
    const qint64 row = offset / m_bytesPerRow;
    const qint64 top = topRow();
    const int rows = visibleRows();
    qint64 newTop = top;
    if (row < top)
        newTop = row;
    else if (row >= top + rows)
        newTop = row - rows + 1;
    if (newTop == top)
        return;
    // Past INT_MAX rows a step spans several rows: round up when scrolling
    // down, so the row lands inside the view rather than just below it.
    const qint64 step = rowsPerStep();
    verticalScrollBar()->setValue(int(newTop > top ? (newTop + step - 1) / step : newTop / step));
}

void HexView::setCursorOffset(qint64 offset, bool extend)
{
    const qint64 size = m_source ? m_source->size() : 0;
    offset = std::clamp<qint64>(offset, 0, std::max<qint64>(0, size - 1));
    m_cursor = offset;
    if (!extend)
        m_anchor = offset;
    ensureVisible(offset);
    viewport()->update();
    emit cursorChanged();
}

void HexView::select(qint64 start, qint64 length)
{
    const qint64 size = m_source ? m_source->size() : 0;
    if (size == 0)
        return;
    start = std::clamp<qint64>(start, 0, size - 1);
    m_anchor = start;
    m_cursor = std::clamp<qint64>(start + std::max<qint64>(1, length) - 1, 0, size - 1);
    ensureVisible(m_cursor);
    ensureVisible(m_anchor);
    viewport()->update();
    emit cursorChanged();
}

void HexView::selectAll()
{
    const qint64 size = m_source ? m_source->size() : 0;
    if (size == 0)
        return;
    m_anchor = 0;
    m_cursor = size - 1;
    viewport()->update();
    emit cursorChanged();
}

void HexView::copy(CopyFormat format)
{
    if (!m_source || m_source->size() == 0)
        return;
    const qint64 length = selectionLength();
    // A C array spends 12 bytes of text per byte; keep its clipboard text near 50 MB.
    const qint64 limit = format == CopyFormat::CArray ? kMaxCopyBytes / 4 : kMaxCopyBytes;
    if (length > limit) {
        QMessageBox::information(this, tr("Copy"),
                                 tr("The selection is larger than %1 MB; select less to copy it.")
                                     .arg(limit / (1024 * 1024)));
        return;
    }
    QByteArray bytes(length, Qt::Uninitialized);
    const qint64 got = m_source->read(selectionStart(), bytes.data(), length);
    bytes.truncate(std::max<qint64>(0, got));
    QString text;
    switch (format) {
    case CopyFormat::Hex: text = HexFormat::toHex(bytes); break;
    case CopyFormat::Text: text = HexFormat::toText(bytes); break;
    case CopyFormat::CArray: text = HexFormat::toCArray(bytes); break;
    }
    QApplication::clipboard()->setText(text);
}

void HexView::copyOffset()
{
    QApplication::clipboard()->setText(QStringLiteral("0x") + HexFormat::offsetText(m_cursor, m_layout.digits));
}

qint64 HexView::offsetAt(const QPoint &pos, bool *ascii) const
{
    const Layout &l = m_layout;
    const double x = pos.x() + horizontalScrollBar()->value();
    const int line = (pos.y() - l.lineHeight) / l.lineHeight;
    if (pos.y() < l.lineHeight || !m_source)
        return -1;
    const qint64 row = topRow() + line;
    int column = -1;
    if (x >= l.hexX - l.charWidth && x < l.asciiX - l.charWidth) {
        // Nearest byte in the hex pane: positions are 3 chars apart plus a group gap.
        const double c = (x - l.hexX) / l.charWidth;
        for (int i = m_bytesPerRow - 1; i >= 0; --i) {
            if (c >= HexFormat::hexColumnPos(i) - 0.5) {
                column = i;
                break;
            }
        }
        if (column < 0)
            column = 0;
        *ascii = false;
    } else if (x >= l.asciiX - l.charWidth) {
        column = std::clamp(int((x - l.asciiX) / l.charWidth), 0, m_bytesPerRow - 1);
        *ascii = true;
    } else {
        return -1;
    }
    const qint64 offset = row * m_bytesPerRow + column;
    return std::min(offset, std::max<qint64>(0, m_source->size() - 1));
}

bool HexView::event(QEvent *event)
{
    if (event->type() == QEvent::ShortcutOverride) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->matches(QKeySequence::Copy) || key->matches(QKeySequence::SelectAll)
            || key->matches(QKeySequence::Find) || key->matches(QKeySequence::FindNext)
            || key->matches(QKeySequence::FindPrevious)
            || (key->modifiers() == Qt::ControlModifier && key->key() == Qt::Key_G)) {
            event->accept();
            return true;
        }
    }
    return QAbstractScrollArea::event(event);
}

void HexView::paintEvent(QPaintEvent *event)
{
    QPainter p(viewport());
    const QPalette &pal = palette();
    const QColor base = pal.color(QPalette::Base);
    const QColor text = pal.color(QPalette::Text);
    const QColor muted = pal.color(QPalette::PlaceholderText);
    // Byte classes from palette roles, so both themes stay readable.
    const QColor classColors[kClassCount] = {
        paletteMix(muted, base, 0.35),                                 // NUL
        text,                                                   // printable
        paletteMix(text, pal.color(QPalette::Link), 0.7),              // control
        paletteMix(text, pal.color(QPalette::Highlight), 0.55)};       // high
    p.fillRect(event->rect(), base);
    p.setFont(font());

    const Layout &l = m_layout;
    const double dx = -horizontalScrollBar()->value();
    const int rows = visibleRows() + 1;
    const qint64 top = topRow();
    const qint64 size = m_source ? m_source->size() : 0;
    const qint64 selFirst = selectionStart();
    const qint64 selLast = selFirst + selectionLength() - 1;
    QColor selection = pal.color(QPalette::Highlight);
    selection.setAlpha(hasFocus() ? 90 : 50);

    // Column header.
    p.fillRect(QRectF(0, 0, viewport()->width(), l.lineHeight), pal.color(QPalette::AlternateBase));
    p.setPen(muted);
    p.drawText(QPointF(dx + l.hexX, l.ascent), m_hexHeader);
    p.drawText(QPointF(dx + l.asciiX, l.ascent), m_asciiHeader);

    char rowBytes[kMaxBytesPerRow];
    for (int r = 0; r < rows; ++r) {
        const qint64 row = top + r;
        const qint64 offset = row * m_bytesPerRow;
        if (offset >= size && !(size == 0 && row == 0))
            break;
        const int y = l.lineHeight * (r + 1);
        if (y > event->rect().bottom())
            break;
        if (y + l.lineHeight < event->rect().top())
            continue;
        const qint64 got = m_source ? m_source->read(offset, rowBytes, m_bytesPerRow) : 0;
        const int n = int(std::max<qint64>(0, got));
        const int baseline = y + l.ascent;

        // Selection background in both panes.
        if (n > 0 && selLast >= offset && selFirst < offset + n) {
            const int i0 = int(std::max(selFirst, offset) - offset);
            const int i1 = int(std::min(selLast, offset + n - 1) - offset);
            p.fillRect(QRectF(dx + l.hexX + (HexFormat::hexColumnPos(i0) - 0.5) * l.charWidth, y,
                              (HexFormat::hexColumnPos(i1) - HexFormat::hexColumnPos(i0) + 3) * l.charWidth,
                              l.lineHeight),
                       selection);
            p.fillRect(QRectF(dx + l.asciiX + i0 * l.charWidth, y, (i1 - i0 + 1) * l.charWidth, l.lineHeight),
                       selection);
        }
        // Cursor box: solid in the active pane, faint in the other.
        if (n > 0 && m_cursor >= offset && m_cursor < offset + n) {
            const int i = int(m_cursor - offset);
            const QRectF hexBox(dx + l.hexX + (HexFormat::hexColumnPos(i) - 0.25) * l.charWidth, y,
                                2.5 * l.charWidth, l.lineHeight - 1);
            const QRectF asciiBox(dx + l.asciiX + i * l.charWidth, y, l.charWidth, l.lineHeight - 1);
            QColor strong = pal.color(QPalette::Highlight);
            QColor faint = strong;
            faint.setAlpha(110);
            p.setPen(QPen(m_asciiPane ? faint : strong, 1));
            p.drawRect(hexBox);
            p.setPen(QPen(m_asciiPane ? strong : faint, 1));
            p.drawRect(asciiBox);
        }

        p.setPen(muted);
        p.drawText(QPointF(dx + l.offsetX, baseline), HexFormat::offsetText(offset, l.digits));

        // One text run per byte class and pane; other classes are spaces.
        HexFormat::splitByClass(QByteArrayView(rowBytes, n), m_bytesPerRow, m_runs);
        for (int c = 0; c < kClassCount; ++c) {
            if (!m_runs.used[c])
                continue;
            p.setPen(classColors[c]);
            p.drawText(QPointF(dx + l.hexX, baseline), m_runs.hex[c]);
            p.drawText(QPointF(dx + l.asciiX, baseline), m_runs.ascii[c]);
        }
    }
}

void HexView::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    updateScrollBars();
}

void HexView::scrollContentsBy(int dx, int dy)
{
    Q_UNUSED(dx);
    Q_UNUSED(dy);
    viewport()->update();
}

void HexView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    bool ascii = false;
    const qint64 offset = offsetAt(event->position().toPoint(), &ascii);
    if (offset < 0)
        return;
    m_asciiPane = ascii;
    m_dragging = true;
    setCursorOffset(offset, event->modifiers().testFlag(Qt::ShiftModifier));
}

void HexView::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging || !(event->buttons() & Qt::LeftButton)) {
        m_dragging = false;
        return;
    }
    QPoint pos = event->position().toPoint();
    // Dragging past the top or bottom scrolls one row per move.
    if (pos.y() < m_layout.lineHeight) {
        verticalScrollBar()->triggerAction(QAbstractSlider::SliderSingleStepSub);
        pos.setY(m_layout.lineHeight);
    } else if (pos.y() >= viewport()->height()) {
        verticalScrollBar()->triggerAction(QAbstractSlider::SliderSingleStepAdd);
        pos.setY(viewport()->height() - 1);
    }
    bool ascii = false;
    const qint64 offset = offsetAt(pos, &ascii);
    if (offset >= 0)
        setCursorOffset(offset, true);
}

void HexView::keyPressEvent(QKeyEvent *event)
{
    if (event->matches(QKeySequence::Copy)) {
        copy(m_asciiPane ? CopyFormat::Text : CopyFormat::Hex);
        return;
    }
    if (event->matches(QKeySequence::SelectAll)) {
        selectAll();
        return;
    }
    if (event->matches(QKeySequence::Find)) {
        emit findRequested();
        return;
    }
    if (event->matches(QKeySequence::FindNext) || event->matches(QKeySequence::FindPrevious)) {
        emit findNextRequested(event->matches(QKeySequence::FindPrevious));
        return;
    }
    if (event->modifiers() == Qt::ControlModifier && event->key() == Qt::Key_G) {
        emit gotoRequested();
        return;
    }
    const bool extend = event->modifiers().testFlag(Qt::ShiftModifier);
    const bool ctrl = event->modifiers().testFlag(Qt::ControlModifier);
    const qint64 page = qint64(visibleRows()) * m_bytesPerRow;
    const qint64 rowStart = m_cursor - m_cursor % m_bytesPerRow;
    switch (event->key()) {
    case Qt::Key_Left: setCursorOffset(m_cursor - 1, extend); break;
    case Qt::Key_Right: setCursorOffset(m_cursor + 1, extend); break;
    case Qt::Key_Up: setCursorOffset(m_cursor - m_bytesPerRow, extend); break;
    case Qt::Key_Down: setCursorOffset(m_cursor + m_bytesPerRow, extend); break;
    case Qt::Key_PageUp: setCursorOffset(m_cursor - page, extend); break;
    case Qt::Key_PageDown: setCursorOffset(m_cursor + page, extend); break;
    case Qt::Key_Home: setCursorOffset(ctrl ? 0 : rowStart, extend); break;
    case Qt::Key_End:
        setCursorOffset(ctrl ? (m_source ? m_source->size() - 1 : 0) : rowStart + m_bytesPerRow - 1, extend);
        break;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        m_asciiPane = !m_asciiPane;
        viewport()->update();
        break;
    default:
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }
    event->accept();
}

void HexView::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    const bool canCopy = m_source && m_source->size() > 0 && selectionLength() <= kMaxCopyBytes;
    menu.addAction(tr("Copy as hex"), this, [this]() { copy(CopyFormat::Hex); })->setEnabled(canCopy);
    menu.addAction(tr("Copy as text"), this, [this]() { copy(CopyFormat::Text); })->setEnabled(canCopy);
    menu.addAction(tr("Copy as C array"), this, [this]() { copy(CopyFormat::CArray); })
        ->setEnabled(canCopy && selectionLength() <= kMaxCopyBytes / 4);
    menu.addAction(tr("Copy offset"), this, &HexView::copyOffset);
    menu.addSeparator();
    menu.addAction(tr("Select all"), this, &HexView::selectAll);
    menu.addAction(tr("Go to offset…"), this, &HexView::gotoRequested);
    menu.exec(event->globalPos());
}

void HexView::focusInEvent(QFocusEvent *event)
{
    QAbstractScrollArea::focusInEvent(event);
    viewport()->update();
}

void HexView::focusOutEvent(QFocusEvent *event)
{
    QAbstractScrollArea::focusOutEvent(event);
    viewport()->update();
}

void HexView::changeEvent(QEvent *event)
{
    QAbstractScrollArea::changeEvent(event);
    if (event->type() == QEvent::FontChange) {
        updateLayout();
        updateScrollBars();
    }
}
