/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Notepad Next is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Notepad Next.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "TerminalWidget.h"

#include "TerminalBoxDrawing.h"
#include "TerminalScrollBar.h"
#include "TerminalTaskLauncher.h"
#include "TerminalCellSource.h"
#include "TerminalLinkDetector.h"
#include "TerminalIcons.h"
#include "TerminalSearchBar.h"
#include "ProfileScope.h"

#include "remote/ExecutionContext.h"

#include "iptyprocess.h"
#include "ptyqt.h"

#include <vterm.h>
#include <vterm_keycodes.h>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QMimeData>
#include <QtConcurrent/QtConcurrentRun>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QIODevice>
#include <QInputMethod>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QScreen>
#include <QToolTip>
#include <QUrl>
#include <QWheelEvent>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace {

// A resize is applied once the size has been stable this long, so a splitter
// drag reflows and signals the shell once, not per mouse move.
constexpr int kResizeSettleMs = 24;
// A missing file seen on hover is re-checked after this long (it may be built).
constexpr qint64 kMissingPathRecheckMs = 3000;
// Search slices on the UI thread stay under this budget per event-loop pass.
constexpr auto kSearchSliceBudget = std::chrono::milliseconds(4);
constexpr int kCursorBlinkMs = 500;
// Wheel: three lines per notch, as on every platform default.
constexpr int kWheelLines = 3;
// Search: re-run after typing settles; follow new output in small batches.
constexpr int kSearchDebounceMs = 100;
constexpr int kSearchFollowMs = 150;
// More new lines than this since the last search: redo it off-thread.
constexpr qint64 kSearchSyncLines = 5000;
constexpr std::size_t kMaxOsc8Spans = 2048;
constexpr int kMaxLogicalLineRows = 16;
constexpr int kMaxPathCache = 512;

class WidgetCellSource final : public TerminalCellSource
{
public:
    WidgetCellSource(const ScrollbackBuffer &scrollback,
                     VTermScreen *screen,
                     VTermState *state,
                     int liveRows,
                     int cols)
        : m_scrollback(scrollback)
        , m_screen(screen)
        , m_state(state)
        , m_liveRows(liveRows)
        , m_cols(cols)
    {
    }

    bool rowWraps(int row) const override
    {
        const int historySize = m_scrollback.size();
        if (row < historySize) {
            return m_scrollback.lineWraps(row);
        }
        // A live row wraps when libvterm marks the next one a continuation.
        const int next = row - historySize + 1;
        if (!m_state || next >= m_liveRows) return false;
        const VTermLineInfo *info = vterm_state_get_lineinfo(m_state, next);
        return info && info->continuation;
    }

    int rowCount() const override { return m_scrollback.size() + m_liveRows; }
    int cols() const override { return m_cols; }

    VTermScreenCell cellAt(int row, int col) const override
    {
        VTermScreenCell cell;
        std::memset(&cell, 0, sizeof(cell));
        const int historySize = m_scrollback.size();
        if (row < historySize) {
            // Per-cell read into the same row is amortised by caching the last row.
            if (m_cachedRow != row || static_cast<int>(m_cachedLine.size()) < m_cols) {
                if (static_cast<int>(m_cachedLine.size()) != m_cols) {
                    m_cachedLine.resize(static_cast<std::size_t>(m_cols));
                }
                if (!m_scrollback.lineAt(row, m_cols, m_cachedLine.data())) {
                    m_cachedRow = -1;
                    return cell;
                }
                m_cachedRow = row;
            }
            if (col >= 0 && col < m_cols) {
                return m_cachedLine[static_cast<std::size_t>(col)];
            }
            return cell;
        }
        if (m_screen) {
            VTermPos pos = { row - historySize, col };
            if (vterm_screen_get_cell(m_screen, pos, &cell)) {
                return cell;
            }
        }
        return cell;
    }

private:
    const ScrollbackBuffer &m_scrollback;
    VTermScreen *m_screen;
    VTermState *m_state;
    int m_liveRows;
    int m_cols;
    mutable int m_cachedRow = -1;
    mutable std::vector<VTermScreenCell> m_cachedLine;
};

qreal snapTo(qreal v, qreal dpr)
{
    return std::round(v * dpr) / dpr;
}

QRgb blendRgb(QRgb base, QRgb over, int alphaPercent)
{
    auto mix = [alphaPercent](int b, int o) { return (b * (100 - alphaPercent) + o * alphaPercent) / 100; };
    return qRgb(mix(qRed(base), qRed(over)), mix(qGreen(base), qGreen(over)), mix(qBlue(base), qBlue(over)));
}

} // namespace

TerminalWidget::TerminalWidget(QWidget *parent)
    : QAbstractScrollArea(parent)
    , m_scheme(TerminalColorScheme::darkScheme())
    , m_palette(TerminalPalette::fromScheme(m_scheme))
{
    m_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_glyphs.setFont(m_font);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    // Partial repaints rely on Qt not clearing the update region first.
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent, true);
    viewport()->setAttribute(Qt::WA_NoSystemBackground, true);
    viewport()->setCursor(Qt::IBeamCursor);
    // The overlay scrollbar lives in the right padding; a classic one would
    // change the grid width every time history appears.
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFrameShape(QFrame::NoFrame);
    m_scrollBar = new TerminalScrollBar(viewport());
    m_scrollBar->hide();
    connect(m_scrollBar, &TerminalScrollBar::offsetRequested, this, &TerminalWidget::setScrollOffset);

    m_blinkTimer = new QTimer(this);
    m_blinkTimer->setInterval(kCursorBlinkMs);
    connect(m_blinkTimer, &QTimer::timeout, this, &TerminalWidget::toggleCursorBlink);

    // Hover tracking drives Ctrl+hover link detection.
    viewport()->setMouseTracking(true);
    setAcceptDrops(true);
    viewport()->setAcceptDrops(true);

    m_searchDebounce = new QTimer(this);
    m_searchDebounce->setSingleShot(true);
    m_searchDebounce->setInterval(kSearchDebounceMs);
    connect(m_searchDebounce, &QTimer::timeout, this, &TerminalWidget::runSearch);
    m_searchFollow = new QTimer(this);
    m_searchFollow->setSingleShot(true);
    m_searchFollow->setInterval(kSearchFollowMs);
    connect(m_searchFollow, &QTimer::timeout, this, &TerminalWidget::searchNewOutput);
    m_searchSlice = new QTimer(this);
    m_searchSlice->setSingleShot(true);
    m_searchSlice->setInterval(0);
    connect(m_searchSlice, &QTimer::timeout, this, &TerminalWidget::searchSlice);
    m_clock.start();

    m_pathWatcher = new QFutureWatcher<QString>(this);
    connect(m_pathWatcher, &QFutureWatcherBase::finished, this, [this]() {
        const QString resolved = m_pathWatcher->result();
        const QString key = m_pathWatcher->property("nn_key").toString();
        if (m_pathCache.size() >= kMaxPathCache) {
            m_pathCache.clear();
        }
        m_pathCache.insert(key, PathProbe{resolved, m_clock.elapsed()});
        // Only apply if the pointer still rests on the same candidate.
        if (key != m_hoverPendingKey) return;
        m_hoverPendingKey.clear();
        if (resolved.isEmpty()) {
            clearHoverLink();
            return;
        }
        HoverLink link = m_hoverPendingLink;
        link.target = resolved;
        link.valid = true;
        setHoverLink(link);
    });

    m_batchTimer = new QTimer(this);
    m_batchTimer->setSingleShot(true);
    m_batchTimer->setInterval(0);
    connect(m_batchTimer, &QTimer::timeout, this, &TerminalWidget::processPendingInput);

    m_frameTimer = new QTimer(this);
    m_frameTimer->setSingleShot(true);
    m_frameTimer->setTimerType(Qt::PreciseTimer);
    connect(m_frameTimer, &QTimer::timeout, this, &TerminalWidget::flushFrame);

    m_resizeTimer = new QTimer(this);
    m_resizeTimer->setSingleShot(true);
    m_resizeTimer->setInterval(kResizeSettleMs);
    connect(m_resizeTimer, &QTimer::timeout, this, &TerminalWidget::applyPendingResize);

    recomputeMetrics();
}

TerminalWidget::~TerminalWidget()
{
    if (m_pty) {
        m_pty->kill();
        delete m_pty;
        m_pty = nullptr;
    }
    if (m_vt) {
        vterm_free(m_vt);
        m_vt = nullptr;
    }
}

// --- libvterm callbacks -----------------------------------------------------

int TerminalWidget::vtermDamage(VTermRect rect, void *user)
{
    static_cast<TerminalWidget *>(user)->markLiveRowsDirty(rect.start_row, rect.end_row);
    return 1;
}

int TerminalWidget::vtermMoveRect(VTermRect dest, VTermRect src, void *user)
{
    Q_UNUSED(src);
    static_cast<TerminalWidget *>(user)->markLiveRowsDirty(dest.start_row, dest.end_row);
    return 1;
}

int TerminalWidget::vtermMoveCursor(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
    auto *w = static_cast<TerminalWidget *>(user);
    w->markLiveRowsDirty(oldpos.row, oldpos.row + 1);
    w->m_cursor = QPoint(pos.col, pos.row);
    w->m_cursorVisible = visible != 0;
    w->m_cursorMoved = true;
    w->markLiveRowsDirty(pos.row, pos.row + 1);
    return 1;
}

int TerminalWidget::vtermSetTermProp(VTermProp prop, VTermValue *val, void *user)
{
    auto *w = static_cast<TerminalWidget *>(user);
    if (prop == VTERM_PROP_TITLE && val && val->string.str) {
        const QString s = QString::fromUtf8(val->string.str, static_cast<int>(val->string.len));
        if (val->string.initial && val->string.final) {
            w->m_title = s;
            emit w->titleChanged(w->m_title);
        } else if (val->string.initial) {
            w->m_title = s;
        } else {
            w->m_title.append(s);
            if (val->string.final) {
                emit w->titleChanged(w->m_title);
            }
        }
        return 1;
    }
    if (prop == VTERM_PROP_CURSORVISIBLE && val) {
        w->m_cursorVisible = val->boolean != 0;
        w->markLiveRowsDirty(w->m_cursor.y(), w->m_cursor.y() + 1);
        w->updateBlinkTimer();
        return 1;
    }
    if (prop == VTERM_PROP_ALTSCREEN && val) {
        w->m_altScreen = val->boolean != 0;
        // The screen buffer swapped: hyperlinks recorded on live rows no
        // longer describe what is shown there.
        const qint64 liveStart = w->m_linesDropped + w->m_scrollback.size();
        for (Osc8Span &span : w->m_osc8Spans) {
            if (span.endLine >= liveStart) span.dead = true;
        }
        // Full-screen apps own the whole grid; history is not shown under them.
        w->m_scrollOffset = 0;
        w->m_scrollbarDirty = true;
        w->markAllDirty();
        return 1;
    }
    if (prop == VTERM_PROP_MOUSE && val) {
        w->m_mouseMode = val->number;
        return 1;
    }
    if (prop == VTERM_PROP_CURSORSHAPE && val) {
        w->m_cursorShape = val->number;
        w->markLiveRowsDirty(w->m_cursor.y(), w->m_cursor.y() + 1);
        return 1;
    }
    if (prop == VTERM_PROP_CURSORBLINK && val) {
        w->m_programBlink = val->boolean != 0;
        w->updateBlinkTimer();
        return 1;
    }
    if (prop == VTERM_PROP_FOCUSREPORT && val) {
        w->m_focusReporting = val->boolean != 0;
        return 1;
    }
    return 0;
}

int TerminalWidget::vtermBell(void *user)
{
    auto *w = static_cast<TerminalWidget *>(user);
    if (w->m_bellSound) {
        QApplication::beep();
    }
    emit w->bellRung();
    return 1;
}

int TerminalWidget::vtermResize(int rows, int cols, void *user)
{
    auto *w = static_cast<TerminalWidget *>(user);
    w->m_rows = rows;
    w->m_cols = cols;
    w->m_dirtyRows.assign(static_cast<std::size_t>(rows), 0);
    w->m_scrollbarDirty = true;
    w->markAllDirty();
    return 1;
}

void TerminalWidget::vtermOutput(const char *s, size_t len, void *user)
{
    auto *w = static_cast<TerminalWidget *>(user);
    if (w->m_pty) {
        w->m_pty->write(QByteArray(s, static_cast<int>(len)));
    }
}

int TerminalWidget::vtermOsc(int command, VTermStringFragment frag, void *user)
{
    auto *w = static_cast<TerminalWidget *>(user);
    if (command != 7 && command != 8 && command != kTerminalTaskExitOsc) {
        return 0;
    }
    if (frag.initial) {
        w->m_oscBuffer.clear();
    }
    // Bound a hostile or broken stream; real payloads are a path or a URL.
    if (w->m_oscBuffer.size() + static_cast<qsizetype>(frag.len) <= 8192) {
        w->m_oscBuffer.append(frag.str, static_cast<qsizetype>(frag.len));
    }
    if (frag.final) {
        w->handleOsc(command, w->m_oscBuffer);
        w->m_oscBuffer.clear();
    }
    return 1;
}

void TerminalWidget::handleOsc(int command, const QByteArray &payload)
{
    if (command == kTerminalTaskExitOsc) {
        bool ok = false;
        const int code = payload.trimmed().toInt(&ok);
        emit taskExited(ok ? code : 1);
        return;
    }
    if (command == 8) {
        // OSC 8 ; params ; URI starts a hyperlink, an empty URI ends it. Spans
        // are kept in absolute line ids so they scroll with their text.
        const qsizetype semi = payload.indexOf(';');
        const QString uri = semi < 0 ? QString() : QString::fromUtf8(payload.mid(semi + 1)).trimmed();
        VTermPos pos{0, 0};
        if (m_vt) {
            vterm_state_get_cursorpos(vterm_obtain_state(m_vt), &pos);
        }
        const qint64 line = m_linesDropped + m_scrollback.size() + pos.row;
        if (m_osc8Open && (line > m_osc8StartLine || (line == m_osc8StartLine && pos.col > m_osc8StartCol))) {
            m_osc8Spans.push_back({m_osc8StartLine, m_osc8StartCol, line, pos.col, m_osc8Uri,
                                   textBetween(m_osc8StartLine, m_osc8StartCol, line, pos.col)});
            if (m_osc8Spans.size() > kMaxOsc8Spans) {
                m_osc8Spans.pop_front();
            }
            markAllDirty();
        }
        // Schemes that would launch arbitrary OS handlers are not links.
        const QString scheme = QUrl(uri).scheme().toLower();
        m_osc8Open = scheme == QLatin1String("http") || scheme == QLatin1String("https")
            || scheme == QLatin1String("mailto") || scheme == QLatin1String("file");
        if (m_osc8Open) {
            m_osc8StartLine = line;
            m_osc8StartCol = pos.col;
            m_osc8Uri = uri;
        }
        return;
    }
    if (command == 7) {
        // file://host/path, percent-encoded. The host is ignored (it names
        // the machine the shell runs on, which is ours or the SSH host).
        const QUrl url(QString::fromUtf8(payload));
        QString path = url.isLocalFile() ? url.toLocalFile() : url.path();
#ifdef Q_OS_WIN
        // file:///C:/x arrives as /C:/x through path().
        if (path.size() > 2 && path.at(0) == QLatin1Char('/') && path.at(2) == QLatin1Char(':')) {
            path.remove(0, 1);
        }
#endif
        if (!path.isEmpty() && path != m_reportedCwd) {
            m_reportedCwd = path;
            emit cwdChanged(path);
        }
    }
}

int TerminalWidget::vtermSbPushLine(int cols, const VTermScreenCell *cells, void *user)
{
    auto *w = static_cast<TerminalWidget *>(user);
    // libvterm only pushes rows of the primary buffer, so alt-screen apps
    // (vim, htop, less) never reach here; a resize while one runs still saves
    // the primary rows it pushes out.

    // libvterm shifts its line info before pushing, so row 0 now describes the
    // line after the pushed one: its continuation bit means the pushed line
    // soft-wrapped. A wrapped line always reaches the last column, which
    // filters the multi-line scroll case where row 0 is further down. During a
    // resize the line info is being rebuilt, so it is not consulted.
    bool wraps = false;
    if (w->m_vt && cols > 0 && !w->m_inResize) {
        const VTermLineInfo *info = vterm_state_get_lineinfo(vterm_obtain_state(w->m_vt), 0);
        const bool lastFilled = cells[cols - 1].chars[0] != 0
            || (cols >= 2 && cells[cols - 2].width == 2);
        wraps = info && info->continuation && lastFilled;
    }

    const int evicted = w->m_scrollback.pushLine(cols, cells, wraps);
    w->noteLinesDropped(evicted);

    w->shiftSelectionForEviction(evicted);

    // Sticky bottom: only follow new content if the user was already there;
    // otherwise keep the viewed lines in place.
    if (w->m_scrollOffset > 0) {
        w->m_scrollOffset = std::min(w->m_scrollOffset + 1, w->m_scrollback.size());
        if (evicted > 0) {
            w->markAllDirty();
        }
    }
    w->m_scrollbarDirty = true;
    return 1;
}

int TerminalWidget::vtermSbPopLine(int cols, VTermScreenCell *cells, void *user)
{
    auto *w = static_cast<TerminalWidget *>(user);
    if (!w->m_scrollback.popLine(cols, cells)) {
        return 0;
    }
    w->m_scrollOffset = std::min(w->m_scrollOffset, w->m_scrollback.size());
    // The popped line is live again: searched history ends before it.
    const qint64 historyEnd = w->m_linesDropped + w->m_scrollback.size();
    w->m_searchedHistoryEnd = std::min(w->m_searchedHistoryEnd, historyEnd);
    w->m_searchScanNext = std::min(w->m_searchScanNext, historyEnd);
    w->m_scrollbarDirty = true;
    w->markAllDirty();
    return 1;
}

int TerminalWidget::vtermSbClear(void *user)
{
    // CSI 3 J: the application asked for the scrollback to be erased.
    auto *w = static_cast<TerminalWidget *>(user);
    w->noteLinesDropped(w->m_scrollback.size());
    w->m_scrollback.clear();
    w->m_scrollOffset = 0;
    w->m_hasSelection = false;
    w->m_scrollbarDirty = true;
    w->markAllDirty();
    return 1;
}

// --- damage and frame scheduling ---------------------------------------------

void TerminalWidget::markLiveRowsDirty(int startRow, int endRow)
{
    const int shift = m_altScreen ? 0 : m_scrollOffset;
    const int rows = static_cast<int>(m_dirtyRows.size());
    const int from = std::max(0, startRow + shift);
    const int to = std::min(rows, endRow + shift);
    for (int r = from; r < to; ++r) {
        m_dirtyRows[static_cast<std::size_t>(r)] = 1;
    }
}

void TerminalWidget::markViewportRowDirty(int viewportRow)
{
    if (viewportRow >= 0 && viewportRow < static_cast<int>(m_dirtyRows.size())) {
        m_dirtyRows[static_cast<std::size_t>(viewportRow)] = 1;
    }
}

void TerminalWidget::markAllDirty()
{
    m_allDirty = true;
}

// Repaints at most once per display frame. Output that arrives long after the
// last paint (typing echo) is shown immediately; a flood is coalesced.
void TerminalWidget::scheduleFrame()
{
    if (m_frameTimer->isActive()) return;
    const qint64 since = m_sincePaint.isValid() ? m_sincePaint.elapsed() : m_frameIntervalMs;
    if (since >= m_frameIntervalMs) {
        flushFrame();
    } else {
        m_frameTimer->start(static_cast<int>(m_frameIntervalMs - since));
    }
}

void TerminalWidget::flushFrame()
{
    if (m_scrollbarDirty) {
        syncScrollbar();
    }

    QRegion region;
    if (m_allDirty) {
        region = viewport()->rect();
    } else {
        const int width = viewport()->width();
        const int rows = static_cast<int>(m_dirtyRows.size());
        for (int r = 0; r < rows; ++r) {
            if (!m_dirtyRows[static_cast<std::size_t>(r)]) continue;
            int end = r + 1;
            while (end < rows && m_dirtyRows[static_cast<std::size_t>(end)]) ++end;
            const int top = static_cast<int>(std::floor(rowTop(r)));
            const int bottom = static_cast<int>(std::ceil(rowTop(end)));
            region += QRect(0, top, width, bottom - top);
            r = end;
        }
    }
    m_allDirty = false;
    std::fill(m_dirtyRows.begin(), m_dirtyRows.end(), 0);

    if (!region.isEmpty()) {
        viewport()->update(region);
    }
    if (m_cursorMoved) {
        m_cursorMoved = false;
        if (m_hasFocus) {
            QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle);
        }
    }
}

void TerminalWidget::syncScrollbar()
{
    m_scrollbarDirty = false;
    const int history = m_scrollback.size();
    m_scrollOffset = std::min(m_scrollOffset, history);
    m_scrollBar->setState(history, m_rows, m_scrollOffset);
    // Full-screen apps own the grid; there is nothing to scroll back to.
    const bool show = history > 0 && !m_altScreen;
    if (show != m_scrollBar->isVisible()) {
        m_scrollBar->setVisible(show);
    }
    syncSearchMarks();
}

void TerminalWidget::setScrollOffset(int offset)
{
    offset = std::clamp(offset, 0, m_altScreen ? 0 : m_scrollback.size());
    if (offset == m_scrollOffset) return;
    m_scrollOffset = offset;
    m_scrollbarDirty = true;
    markAllDirty();
    updateBlinkTimer();
    scheduleFrame();
}

void TerminalWidget::scrollToBottom()
{
    setScrollOffset(0);
}

// --- cursor style -------------------------------------------------------------

// The user's style becomes libvterm's own cursor state, so a program's
// DECSCUSR overrides it and DECRC restores to it instead of to a hard block.
void TerminalWidget::applyCursorStyleToState()
{
    if (!m_vt) return;
    VTermState *state = vterm_obtain_state(m_vt);
    VTermValue shape;
    switch (m_cursorShapeSetting) {
    case TerminalCursorShape::Bar:       shape.number = VTERM_PROP_CURSORSHAPE_BAR_LEFT; break;
    case TerminalCursorShape::Underline: shape.number = VTERM_PROP_CURSORSHAPE_UNDERLINE; break;
    default:                             shape.number = VTERM_PROP_CURSORSHAPE_BLOCK; break;
    }
    vterm_state_set_termprop(state, VTERM_PROP_CURSORSHAPE, &shape);
    // libvterm resets to a blinking cursor; a steady one is the quiet default.
    VTermValue blink;
    blink.boolean = 0;
    vterm_state_set_termprop(state, VTERM_PROP_CURSORBLINK, &blink);
}

bool TerminalWidget::cursorBlinkEnabled() const
{
    switch (m_cursorBlinkSetting) {
    case TerminalCursorBlink::On:  return true;
    case TerminalCursorBlink::Off: return false;
    default:                       return m_programBlink;
    }
}

// The blink timer only runs while it can matter: focused, cursor shown, at the
// live bottom. Idle terminals cost nothing.
void TerminalWidget::updateBlinkTimer()
{
    const bool atBottom = m_altScreen || m_scrollOffset == 0;
    const bool run = cursorBlinkEnabled() && m_hasFocus && m_cursorVisible && atBottom && !m_exited;
    if (run && !m_blinkTimer->isActive()) {
        m_blinkTimer->start();
    } else if (!run && m_blinkTimer->isActive()) {
        m_blinkTimer->stop();
    }
    if (!run && !m_blinkPhaseOn) {
        m_blinkPhaseOn = true;
        markLiveRowsDirty(m_cursor.y(), m_cursor.y() + 1);
        scheduleFrame();
    }
}

// Typing keeps the cursor solid; blinking resumes a full period later.
void TerminalWidget::restartBlink()
{
    if (!m_blinkPhaseOn) {
        m_blinkPhaseOn = true;
        markLiveRowsDirty(m_cursor.y(), m_cursor.y() + 1);
    }
    if (m_blinkTimer->isActive()) {
        m_blinkTimer->start();
    }
}

void TerminalWidget::toggleCursorBlink()
{
    m_blinkPhaseOn = !m_blinkPhaseOn;
    // Only the cursor cell changes; repaint just that rect.
    viewport()->update(cursorRect().toAlignedRect());
}

// --- lifecycle ----------------------------------------------------------------

bool TerminalWidget::start(const QString &shell, const QString &cwd, const QStringList &env,
                           remote::ExecutionContext *ctx)
{
    if (m_pty) {
        return false;
    }

    recomputeMetrics();
    if (viewport()->width() > 0 && viewport()->height() > 0) {
        const QSize grid = gridSizeFor(viewport()->size());
        m_cols = grid.width();
        m_rows = grid.height();
    }
    m_dirtyRows.assign(static_cast<std::size_t>(m_rows), 0);

    m_vt = vterm_new(m_rows, m_cols);
    vterm_set_utf8(m_vt, 1);

    m_screen = vterm_obtain_screen(m_vt);
    static const VTermScreenCallbacks callbacks = {
        &TerminalWidget::vtermDamage,
        &TerminalWidget::vtermMoveRect,
        &TerminalWidget::vtermMoveCursor,
        &TerminalWidget::vtermSetTermProp,
        &TerminalWidget::vtermBell,
        &TerminalWidget::vtermResize,
        &TerminalWidget::vtermSbPushLine,
        &TerminalWidget::vtermSbPopLine,
        &TerminalWidget::vtermSbClear,
    };
    vterm_screen_set_callbacks(m_screen, &callbacks, this);
    // OSC 7 (cwd), 8 (hyperlinks) and the task exit report are not handled by
    // libvterm itself; it hands them to these fallbacks.
    static const VTermStateFallbacks fallbacks = {
        nullptr, nullptr, &TerminalWidget::vtermOsc, nullptr, nullptr, nullptr, nullptr,
    };
    vterm_screen_set_unrecognised_fallbacks(m_screen, &fallbacks, this);
    vterm_screen_enable_altscreen(m_screen, 1);
    vterm_screen_enable_reflow(m_screen, true);
    // Merge damage down to one scroll + rects per flush; we flush once per batch.
    vterm_screen_set_damage_merge(m_screen, VTERM_DAMAGE_SCROLL);
    vterm_screen_reset(m_screen, 1);
    applyCursorStyleToState();

    vterm_output_set_callback(m_vt, &TerminalWidget::vtermOutput, this);

    // PTY backend comes from the execution context: the local context returns
    // the IDENTICAL PtyQt object used before this change (so the read/write hot
    // path is byte-for-byte unchanged — only this one virtual call is added),
    // and a remote context returns an SSH-backed PTY. A null ctx falls back to
    // a local PtyQt directly so existing callers behave exactly as before.
    if (ctx) {
        m_pty = ctx->createPty(this);
    } else {
        m_pty = PtyQt::createPtyProcess(IPtyProcess::AutoPty);
    }
    if (!m_pty) {
        m_errorMessage = tr("Terminal: PTY backend unavailable on this platform.");
        emit spawnFailed(m_errorMessage);
        viewport()->update();
        return false;
    }

    if (!m_pty->isAvailable()) {
#ifdef Q_OS_WIN
        m_errorMessage = tr("Terminal: ConPTY requires Windows 10 build 17763 (1809) or later.");
#else
        m_errorMessage = tr("Terminal: PTY is unavailable on this platform.");
#endif
        emit spawnFailed(m_errorMessage);
        viewport()->update();
        return false;
    }

    if (!(ctx && ctx->isRemote())) {
        const int limitGb = std::clamp(m_childMemoryLimitGb, 0, 256);
        if (limitGb > 0) {
            m_pty->setChildCommitLimit(static_cast<quint64>(limitGb) << 30);
        }
        connect(m_pty, &IPtyProcess::memoryLimitExceeded, this, [this, limitGb]() {
            const QString text = limitGb > 0
                ? TerminalWidget::tr("Stopped: a program exceeded the %1 GB terminal memory limit.").arg(limitGb)
                : TerminalWidget::tr("Stopped: a program exceeded the terminal memory limit.");
            injectOutput(QStringLiteral("\r\n\x1b[31m%1\x1b[0m\r\n").arg(text).toUtf8());
        });
    }

    if (!m_launchArguments.isEmpty()) {
        m_pty->setArguments(m_launchArguments);
    }
    if (!m_pty->startProcess(shell, cwd, env, static_cast<qint16>(m_cols), static_cast<qint16>(m_rows))) {
        m_errorMessage = tr("Terminal: failed to launch shell '%1'.\n%2").arg(shell, m_pty->lastError());
        emit spawnFailed(m_errorMessage);
        viewport()->update();
        return false;
    }

    if (QIODevice *n = m_pty->notifier()) {
        connect(n, &QIODevice::readyRead, this, &TerminalWidget::onPtyReadyRead);
    }
    connect(m_pty, &IPtyProcess::finished, this, &TerminalWidget::onPtyFinished);

    markAllDirty();
    scheduleFrame();
    return true;
}

void TerminalWidget::setColorScheme(const TerminalColorScheme &scheme)
{
    m_scheme = scheme;
    m_palette = TerminalPalette::fromScheme(scheme);
    m_scrollBar->setThumbColor(m_palette.defaultFg);
    markAllDirty();
    scheduleFrame();
}

void TerminalWidget::setAppearance(const TerminalAppearance &appearance)
{
    setColorScheme(appearance.scheme);
    m_contrast.setMinimum(appearance.minimumContrast);
    m_copyOnSelect = appearance.copyOnSelect;
    m_bellSound = appearance.bellSound;
    setScrollbackLines(appearance.scrollbackLines);

    const bool styleChanged = m_cursorShapeSetting != appearance.cursorShape;
    m_cursorShapeSetting = appearance.cursorShape;
    m_cursorBlinkSetting = appearance.cursorBlink;
    if (styleChanged) {
        applyCursorStyleToState();
    }
    updateBlinkTimer();

    const qreal lineHeight = std::clamp(appearance.lineHeight, 1.0, 2.0);
    if (appearance.font != m_font || !qFuzzyCompare(lineHeight, m_lineHeight)) {
        m_font = appearance.font;
        m_lineHeight = lineHeight;
        m_glyphs.setFont(m_font);
        recomputeMetrics();
        requestResizeToViewport();
    }
    markAllDirty();
    scheduleFrame();
}

void TerminalWidget::clearScrollback()
{
    noteLinesDropped(m_scrollback.size());
    m_scrollback.clear();
    m_scrollOffset = 0;
    m_hasSelection = false;
    m_scrollbarDirty = true;
    markAllDirty();
    scheduleFrame();
}


void TerminalWidget::setScrollbackLines(int lines)
{
    lines = std::clamp(lines, 0, 100000);
    if (lines == m_scrollback.maxLines()) return;
    const int evicted = m_scrollback.setMaxLines(lines);
    noteLinesDropped(evicted);
    shiftSelectionForEviction(evicted);
    m_scrollOffset = std::min(m_scrollOffset, m_scrollback.size());
    m_scrollbarDirty = true;
    markAllDirty();
    scheduleFrame();
}

void TerminalWidget::setTerminalFont(const QFont &font)
{
    m_font = font;
    m_glyphs.setFont(m_font);
    recomputeMetrics();
    requestResizeToViewport();
    markAllDirty();
    scheduleFrame();
}

void TerminalWidget::setChildMemoryLimitGb(int gb)
{
    m_childMemoryLimitGb = gb;
}

void TerminalWidget::setLineHeight(qreal factor)
{
    factor = std::clamp(factor, 1.0, 2.0);
    if (qFuzzyCompare(factor, m_lineHeight)) return;
    m_lineHeight = factor;
    recomputeMetrics();
    requestResizeToViewport();
    markAllDirty();
    scheduleFrame();
}

bool TerminalWidget::isProcessRunning() const
{
    if (m_exited) return false;
    if (!m_pty) return false;
    return m_pty->pid() > 0;
}

qint64 TerminalWidget::ptyPid() const
{
    return m_pty ? m_pty->pid() : 0;
}

void TerminalWidget::killProcess()
{
    if (m_pty) {
        m_pty->kill();
    }
}

void TerminalWidget::onPtyReadyRead()
{
    if (!m_pty || !m_vt) return;
    const QByteArray data = m_pty->readAll();
    if (data.isEmpty()) return;
    if (!m_firstOutputEmitted) {
        m_firstOutputEmitted = true;
        emit firstOutputReceived();
    }
    m_pendingInput.append(data);
    if (!m_batchTimer->isActive()) {
        m_batchTimer->start();
    }
}

void TerminalWidget::processPendingInput()
{
    if (!m_vt) return;
    const qsizetype total = m_pendingInput.size();
    if (m_pendingOffset >= total) return;

    PROFILE_SCOPE("TerminalWidget::parse");
    static constexpr qsizetype kChunkSize = 16 * 1024;
    static constexpr auto kMaxTime = std::chrono::milliseconds(4);

    const auto start = std::chrono::steady_clock::now();
    while (m_pendingOffset < total) {
        const qsizetype chunk = std::min(kChunkSize, total - m_pendingOffset);
        vterm_input_write(m_vt, m_pendingInput.constData() + m_pendingOffset, static_cast<size_t>(chunk));
        m_pendingOffset += chunk;
        if (std::chrono::steady_clock::now() - start > kMaxTime) {
            break;
        }
    }
    vterm_screen_flush_damage(m_screen);

    // Consume by offset; the buffer is only compacted when fully drained or
    // when the consumed prefix dominates, so a flood never memmoves per batch.
    if (m_pendingOffset >= m_pendingInput.size()) {
        m_pendingInput.clear();
        m_pendingOffset = 0;
    } else if (m_pendingOffset > (1 << 20) && m_pendingOffset * 2 > m_pendingInput.size()) {
        m_pendingInput.remove(0, m_pendingOffset);
        m_pendingOffset = 0;
    }

    scheduleFrame();
    if (m_searchBar && m_searchBar->isVisible() && m_searchRe.isValid() && !m_searchFollow->isActive()) {
        m_searchFollow->start();
    }

    if (m_pendingOffset < m_pendingInput.size()) {
        m_batchTimer->start();
    }
}

void TerminalWidget::onPtyFinished(int exitCode)
{
    m_exited = true;
    m_exitCode = exitCode;
    while (m_pendingOffset < m_pendingInput.size()) {
        processPendingInput();
    }
    updateBlinkTimer();
    emit processExited(exitCode);
    markAllDirty();
    scheduleFrame();
}

void TerminalWidget::writeToPty(const QByteArray &data)
{
    if (m_pty) {
        m_pty->write(data);
    }
}

void TerminalWidget::injectOutput(const QByteArray &data)
{
    if (!m_vt || data.isEmpty()) return;
    m_pendingInput.append(data);
    if (!m_batchTimer->isActive()) {
        m_batchTimer->start();
    }
}

bool TerminalWidget::isMouseReportingActive(Qt::KeyboardModifiers mods) const
{
    if (m_mouseMode == VTERM_PROP_MOUSE_NONE) return false;
    if (mods & Qt::ShiftModifier) return false;
    return true;
}

VTermModifier TerminalWidget::qtModsToVterm(Qt::KeyboardModifiers mods) const
{
    VTermModifier vmod = VTERM_MOD_NONE;
    if (mods & Qt::ShiftModifier)   vmod = static_cast<VTermModifier>(vmod | VTERM_MOD_SHIFT);
    if (mods & Qt::ControlModifier) vmod = static_cast<VTermModifier>(vmod | VTERM_MOD_CTRL);
    if (mods & Qt::AltModifier)     vmod = static_cast<VTermModifier>(vmod | VTERM_MOD_ALT);
    return vmod;
}

// --- geometry -------------------------------------------------------------------

void TerminalWidget::recomputeMetrics()
{
    m_dpr = std::max<qreal>(1.0, viewport()->devicePixelRatioF());
    const QFontMetricsF fm(m_font);
    const qreal advance = fm.horizontalAdvance(QLatin1Char('M'));
    const qreal fontHeight = fm.ascent() + fm.descent();
    // Whole device pixels, rounded up: at fractional scales the font's
    // advance (11.42 px at 150%) is narrower than its glyphs' ink (11.64 px),
    // so rounding down would make neighbouring glyphs overlap and every glyph
    // a slow untiled draw. The 1e-3 keeps exact advances (8.000 at 100%).
    m_cellW = std::max<qreal>(1.0, std::ceil(advance * m_dpr - 1e-3)) / m_dpr;
    m_cellH = std::max<qreal>(1.0, std::round(fontHeight * m_lineHeight * m_dpr)) / m_dpr;
    // Extra line height is split evenly above and below the glyphs.
    m_baseline = snapTo(fm.ascent() + (m_cellH - fontHeight) / 2, m_dpr);
    m_lineWidth = std::max(1.0 / m_dpr, snapTo(fm.lineWidth(), m_dpr));
    m_underlineY = std::min(m_baseline + std::max(m_lineWidth, snapTo(fm.underlinePos(), m_dpr)),
                            m_cellH - m_lineWidth);
    m_strikeY = m_baseline - snapTo(fm.strikeOutPos(), m_dpr);

    if (const QScreen *s = screen()) {
        const qreal hz = s->refreshRate();
        if (hz > 1) {
            m_frameIntervalMs = std::clamp(static_cast<int>(1000.0 / hz), 4, 33);
        }
    }
}

QSize TerminalWidget::gridSizeFor(const QSize &viewportSize) const
{
    constexpr qreal kEpsilon = 1e-6;
    const int cols = static_cast<int>(std::floor((viewportSize.width() - 2 * m_padX) / m_cellW + kEpsilon));
    const int rows = static_cast<int>(std::floor((viewportSize.height() - 2 * m_padY) / m_cellH + kEpsilon));
    return QSize(std::max(20, cols), std::max(5, rows));
}

// Leftover pixels below the last whole row stay at the bottom, except on the
// alternate screen where full-screen apps anchor their status line to it.
qreal TerminalWidget::originY() const
{
    if (m_altScreen) {
        const qreal leftover = viewport()->height() - 2 * m_padY - m_rows * m_cellH;
        if (leftover > 0) {
            return m_padY + std::floor(leftover * m_dpr) / m_dpr;
        }
    }
    return m_padY;
}

QRectF TerminalWidget::cursorRect() const
{
    const int viewportRow = m_cursor.y() + (m_altScreen ? 0 : m_scrollOffset);
    int span = 1;
    if (m_screen) {
        VTermScreenCell cell;
        if (vterm_screen_get_cell(m_screen, VTermPos{m_cursor.y(), m_cursor.x()}, &cell) && cell.width == 2) {
            span = 2;
        }
    }
    return QRectF(columnX(m_cursor.x()), rowTop(viewportRow), span * m_cellW, m_cellH);
}

void TerminalWidget::requestResizeToViewport()
{
    const QSize grid = gridSizeFor(viewport()->size());
    if (!m_vt) {
        return;
    }
    if (grid.width() == m_cols && grid.height() == m_rows) return;
    // vtermResize updates m_rows/m_cols; reflow may push or pop scrollback.
    m_inResize = true;
    vterm_set_size(m_vt, grid.height(), grid.width());
    m_inResize = false;
    vterm_screen_flush_damage(m_screen);
    if (m_pty) {
        m_pty->resize(static_cast<qint16>(m_cols), static_cast<qint16>(m_rows));
    }
    m_scrollbarDirty = true;
    markAllDirty();
    scheduleFrame();
}

void TerminalWidget::applyPendingResize()
{
    requestResizeToViewport();
}

void TerminalWidget::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    m_scrollBar->setGeometry(viewport()->width() - TerminalScrollBar::kWidth, 0,
                             TerminalScrollBar::kWidth, viewport()->height());
    positionSearchBar();
    markAllDirty();
    // Splitter drags fire a resize per mouse move; only the size that holds
    // for kResizeSettleMs is applied (restarting the timer drops the rest).
    if (m_vt) {
        m_resizeTimer->start();
    }
}

QPoint TerminalWidget::pixelToCell(const QPoint &p) const
{
    const int col = static_cast<int>(std::floor((p.x() - m_padX) / m_cellW));
    const int row = static_cast<int>(std::floor((p.y() - originY()) / m_cellH));
    return QPoint(std::clamp(col, 0, m_cols - 1), std::clamp(row, 0, m_rows - 1));
}

int TerminalWidget::viewportToAbsoluteRow(int viewportRow) const
{
    // Alt screen hides history: viewport rows map straight to live rows.
    const int historySize = m_scrollback.size();
    const int total = historySize + m_rows;
    if (total <= 0) return 0;
    int absolute;
    if (m_altScreen) {
        absolute = historySize + viewportRow;
    } else {
        absolute = historySize - m_scrollOffset + viewportRow;
    }
    return std::clamp(absolute, 0, total - 1);
}

int TerminalWidget::absoluteToViewportRow(int absoluteRow) const
{
    if (m_altScreen) {
        return absoluteRow - m_scrollback.size();
    }
    return absoluteRow - (m_scrollback.size() - m_scrollOffset);
}

QPoint TerminalWidget::pixelToAbsoluteCell(const QPoint &p) const
{
    const QPoint vp = pixelToCell(p);
    return QPoint(vp.x(), viewportToAbsoluteRow(vp.y()));
}

// --- painting -------------------------------------------------------------------

bool TerminalWidget::fetchViewportRow(int viewportRow, VTermScreenCell *out)
{
    const int historySize = m_scrollback.size();
    const int absolute = m_altScreen ? historySize + viewportRow
                                     : historySize - m_scrollOffset + viewportRow;
    if (absolute < 0) return false;
    if (absolute < historySize) {
        return m_scrollback.lineAt(absolute, m_cols, out);
    }
    const int liveRow = absolute - historySize;
    if (liveRow >= m_rows) return false;
    VTermPos pos{liveRow, 0};
    for (pos.col = 0; pos.col < m_cols; ++pos.col) {
        if (!vterm_screen_get_cell(m_screen, pos, &out[pos.col])) {
            return false;
        }
    }
    return true;
}

void TerminalWidget::paintEvent(QPaintEvent *event)
{
    PROFILE_SCOPE("TerminalWidget::paint");
    m_sincePaint.restart();

    if (!qFuzzyCompare(std::max<qreal>(1.0, viewport()->devicePixelRatioF()), m_dpr)) {
        // Moved to a screen with another scale: re-snap cells to its pixels.
        recomputeMetrics();
        if (m_vt && !m_resizeTimer->isActive()) {
            m_resizeTimer->start();
        }
    }

    QPainter p(viewport());
    const QRect bounds = event->rect();

    if (!m_errorMessage.isEmpty() && (!m_pty || m_pty->pid() == 0)) {
        p.fillRect(bounds, QColor::fromRgb(m_palette.defaultBg));
        p.setPen(QColor::fromRgb(m_palette.defaultFg));
        p.setFont(m_font);
        p.drawText(viewport()->rect().adjusted(8, 8, -8, -8), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, m_errorMessage);
        return;
    }
    if (!m_screen) {
        p.fillRect(bounds, QColor::fromRgb(m_palette.defaultBg));
        return;
    }

    if (static_cast<int>(m_rowCells.size()) < m_cols) {
        m_rowCells.resize(static_cast<std::size_t>(m_cols));
    }

    // Rows are rendered into an opaque frame in device pixels (tiles are
    // copied straight into its memory), then the frame is blitted once.
    const QSize deviceSize(static_cast<int>(std::ceil(viewport()->width() * m_dpr)),
                           static_cast<int>(std::ceil(viewport()->height() * m_dpr)));
    if (m_frame.size() != deviceSize || m_frame.devicePixelRatio() != m_dpr) {
        m_frame = QImage(deviceSize, QImage::Format_RGB32);
        m_frame.setDevicePixelRatio(m_dpr);
    }
    m_glyphs.setTileGeometry(static_cast<int>(std::lround(m_cellW * m_dpr)),
                             static_cast<int>(std::lround(m_cellH * m_dpr)), m_baseline, m_dpr);
    m_glyphs.beginFrame();
    TerminalGlyphCache::Frame frame;
    frame.bits = reinterpret_cast<uint32_t *>(m_frame.bits());
    frame.stride = m_frame.bytesPerLine() / 4;
    frame.width = m_frame.width();
    frame.height = m_frame.height();
    {
        QPainter fp(&m_frame);
        // The same pixels without the scale: glyphs are drawn in device
        // pixels, which keeps Qt on its cached glyph path at every scale.
        QImage deviceView(m_frame.bits(), m_frame.width(), m_frame.height(), m_frame.bytesPerLine(),
                          QImage::Format_RGB32);
        QPainter dp(&deviceView);
        fp.fillRect(bounds, QColor::fromRgb(m_palette.defaultBg));

        const qreal top = originY();
        const int first = std::max(0, static_cast<int>(std::floor((bounds.top() - top) / m_cellH)));
        const int last = std::min(m_rows - 1, static_cast<int>(std::floor((bounds.bottom() - top) / m_cellH)));
        const QRegion &region = event->region();
        const int width = viewport()->width();
        const auto touchedSize = static_cast<std::size_t>(m_cols) + 2;
        m_touchedRow.assign(touchedSize, 0);
        m_touchedBelow.assign(touchedSize, 0);
        int previous = -2;
        for (int row = first; row <= last; ++row) {
            const QRect rowRect(0, static_cast<int>(std::floor(rowTop(row))), width,
                                static_cast<int>(std::ceil(m_cellH)) + 1);
            if (!region.intersects(rowRect)) continue;
            // What Qt drew into this row from the row above, if that one was
            // painted in this pass.
            if (row == previous + 1) {
                std::swap(m_touchedRow, m_touchedBelow);
            } else {
                std::fill(m_touchedRow.begin(), m_touchedRow.end(), uint8_t(0));
            }
            std::fill(m_touchedBelow.begin(), m_touchedBelow.end(), uint8_t(0));
            paintRow(fp, dp, frame, row);
            previous = row;
        }
    }
    // Same device scale on both sides: a clipped 1:1 copy.
    p.drawImage(QPointF(0, 0), m_frame);

    paintCursor(p);
    paintPreedit(p);
}

void TerminalWidget::paintRow(QPainter &p, QPainter &devicePainter, const TerminalGlyphCache::Frame &frame,
                              int viewportRow)
{
    if (!fetchViewportRow(viewportRow, m_rowCells.data())) return;

    int selFrom = 0;
    int selTo = 0;
    const int historySize = m_scrollback.size();
    const int absolute = m_altScreen ? historySize + viewportRow
                                     : historySize - m_scrollOffset + viewportRow;
    const qint64 lineId = lineIdForAbsoluteRow(absolute);
    if (m_hasSelection) {
        const QPoint a = m_selStart;
        const QPoint b = m_selEnd;
        const bool aFirst = a.y() < b.y() || (a.y() == b.y() && a.x() <= b.x());
        const QPoint lo = aFirst ? a : b;
        const QPoint hi = aFirst ? b : a;
        if (absolute >= lo.y() && absolute <= hi.y()) {
            selFrom = (absolute == lo.y()) ? lo.x() : 0;
            selTo = (absolute == hi.y()) ? hi.x() + 1 : m_cols;
        }
    }

    const int highlightCount = collectHighlights(lineId);
    layoutTerminalRow(m_rowCells.data(), m_cols, m_palette, selFrom, selTo, m_layout, &m_contrast,
                      m_rowHighlights.data(), highlightCount);

    const qreal y = rowTop(viewportRow);
    const int cellW = static_cast<int>(std::lround(m_cellW * m_dpr));
    const int cellH = static_cast<int>(std::lround(m_cellH * m_dpr));
    const int x0 = static_cast<int>(std::lround(m_padX * m_dpr));
    const int y0 = static_cast<int>(std::lround(y * m_dpr));
    // Per-cell background: tiles are pre-blended onto it.
    m_rowBg.assign(static_cast<std::size_t>(m_cols) + 1, m_palette.defaultBg);
    for (const auto &bg : m_layout.backgrounds) {
        std::fill_n(m_rowBg.begin() + bg.col, std::min(bg.span, m_cols - bg.col), bg.color);
        const int x = x0 + bg.col * cellW;
        const int right = std::min(frame.width, x + bg.span * cellW);
        const int bottom = std::min(frame.height, y0 + cellH);
        for (int yy = std::max(0, y0); yy < bottom; ++yy) {
            uint32_t *line = frame.bits + static_cast<qsizetype>(yy) * frame.stride;
            std::fill(line + std::max(0, x), line + std::max(0, right), 0xFF000000u | bg.color);
        }
    }
    for (const auto &special : m_layout.specials) {
        TerminalBoxDrawing::paint(p, QRectF(columnX(special.col), y, special.width * m_cellW, m_cellH),
                                  special.cp, QColor::fromRgb(special.fg), m_lineWidth, m_dpr);
    }
    const qreal baseline = y + m_baseline;
    TerminalGlyphCache::Touched touched{m_touchedRow.data(), m_touchedBelow.data(), m_cols};
    for (const auto &run : m_layout.runs) {
        m_glyphs.blitRun(devicePainter, frame, m_layout.glyphs.data() + run.firstGlyph, run.glyphCount,
                         run.font, run.fg, m_rowBg.data(), touched, x0, y0);
    }
    for (const auto &cluster : m_layout.clusters) {
        m_glyphs.drawCluster(p, cluster, m_padX, baseline, m_cellW);
        // Shaped clusters may reach into the row below; keep its tiles off them.
        touched.markBelow(cluster.col - 1, cluster.col + cluster.width + 1);
    }
    paintDecorations(p, y);

    // OSC 8 hyperlinks: a faint underline marks them without Ctrl.
    if (!m_osc8Spans.empty()) {
        QColor faint = QColor::fromRgb(m_palette.defaultFg);
        faint.setAlpha(90);
        int scanned = 0;
        for (auto it = m_osc8Spans.rbegin(); it != m_osc8Spans.rend() && scanned < 256; ++it, ++scanned) {
            if (it->endLine < lineId || it->startLine > lineId || !osc8SpanAlive(*it)) continue;
            const int from = it->startLine == lineId ? it->startCol : 0;
            const int to = it->endLine == lineId ? it->endCol : m_cols;
            if (to > from) {
                p.fillRect(QRectF(columnX(from), y + m_underlineY, (to - from) * m_cellW, m_lineWidth), faint);
            }
        }
    }
    // The current search hit is also outlined, so it does not rely on colour.
    if (m_rowActiveFrom >= 0 && m_rowActiveTo > m_rowActiveFrom) {
        p.save();
        p.setPen(QPen(QColor::fromRgb(m_palette.defaultFg), m_lineWidth));
        p.setBrush(Qt::NoBrush);
        const qreal h = m_lineWidth / 2;
        p.drawRect(QRectF(columnX(m_rowActiveFrom), y, (m_rowActiveTo - m_rowActiveFrom) * m_cellW, m_cellH)
                       .adjusted(h, h, -h, -h));
        p.restore();
    }
    // The Ctrl-hovered link.
    if (m_hover.valid && lineId >= m_hover.startLine && lineId <= m_hover.endLine) {
        const int from = m_hover.startLine == lineId ? m_hover.startCol : 0;
        const int to = m_hover.endLine == lineId ? m_hover.endCol : m_cols;
        if (to > from) {
            p.fillRect(QRectF(columnX(from), y + m_underlineY, (to - from) * m_cellW, m_lineWidth),
                       QColor::fromRgb(m_palette.defaultFg));
        }
    }
}

void TerminalWidget::paintDecorations(QPainter &p, qreal y)
{
    for (const auto &d : m_layout.decorations) {
        const qreal x = columnX(d.col);
        const qreal w = d.span * m_cellW;
        const QColor color = QColor::fromRgb(d.color);
        switch (d.kind) {
        case TerminalRowLayout::Underline:
            p.fillRect(QRectF(x, y + m_underlineY, w, m_lineWidth), color);
            break;
        case TerminalRowLayout::DoubleUnderline: {
            const qreal top = std::min(m_underlineY, m_cellH - 3 * m_lineWidth);
            p.fillRect(QRectF(x, y + top, w, m_lineWidth), color);
            p.fillRect(QRectF(x, y + top + 2 * m_lineWidth, w, m_lineWidth), color);
            break;
        }
        case TerminalRowLayout::CurlyUnderline: {
            const qreal amp = std::max(m_lineWidth, m_cellH * 0.05);
            const qreal mid = std::min(m_underlineY + amp, m_cellH - amp - m_lineWidth / 2);
            const qreal half = m_cellW / 2;
            QPainterPath path(QPointF(x, y + mid));
            bool up = true;
            for (qreal cx = x; cx < x + w - 0.01; cx += half) {
                path.quadTo(QPointF(cx + half / 2, y + mid + (up ? -amp : amp) * 2),
                            QPointF(cx + half, y + mid));
                up = !up;
            }
            p.save();
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setClipRect(QRectF(x, y, w, m_cellH), Qt::IntersectClip);
            p.strokePath(path, QPen(color, m_lineWidth));
            p.restore();
            break;
        }
        case TerminalRowLayout::Strike:
            p.fillRect(QRectF(x, y + m_strikeY - m_lineWidth / 2, w, m_lineWidth), color);
            break;
        default:
            break;
        }
    }
}

void TerminalWidget::paintCursor(QPainter &p)
{
    if (!m_screen || !m_cursorVisible || !m_preedit.isEmpty()) return;
    // The cursor lives at the live bottom; scrolled-back history has none.
    if (!m_altScreen && m_scrollOffset != 0) return;

    const QRectF r = cursorRect();
    const QColor cursorColor = QColor::fromRgb(m_palette.cursor);
    if (!m_hasFocus) {
        p.save();
        p.setPen(QPen(cursorColor, m_lineWidth));
        p.setBrush(Qt::NoBrush);
        const qreal h = m_lineWidth / 2;
        p.drawRect(r.adjusted(h, h, -h, -h));
        p.restore();
        return;
    }
    if (!m_blinkPhaseOn) return;

    // Bar and underline are two device-pixel-snapped logical pixels thick.
    const qreal thin = std::max(2.0 / m_dpr, snapTo(2.0, m_dpr));
    if (m_cursorShape == VTERM_PROP_CURSORSHAPE_BAR_LEFT) {
        p.fillRect(QRectF(r.x(), r.y(), thin, r.height()), cursorColor);
        return;
    }
    if (m_cursorShape == VTERM_PROP_CURSORSHAPE_UNDERLINE) {
        p.fillRect(QRectF(r.x(), r.bottom() - thin, r.width(), thin), cursorColor);
        return;
    }

    p.fillRect(r, cursorColor);

    // Redraw the glyph under the block in the cell's background colour so the
    // character stays readable.
    VTermScreenCell cell;
    if (!vterm_screen_get_cell(m_screen, VTermPos{m_cursor.y(), m_cursor.x()}, &cell)) return;
    const uint32_t ch = cell.chars[0];
    if (ch == 0 || ch == U' ' || ch == kTerminalWideSpacer || cell.attrs.conceal) return;
    const TerminalCellColors colors = resolveTerminalCellColors(cell, m_palette);
    const auto font = static_cast<uint8_t>((cell.attrs.bold ? TerminalFontBold : 0)
                                         | (cell.attrs.italic ? TerminalFontItalic : 0));
    if (TerminalBoxDrawing::isProcedural(ch)) {
        TerminalBoxDrawing::paint(p, r, ch, QColor::fromRgb(colors.bg), m_lineWidth, m_dpr);
    } else if (cell.chars[1] != 0) {
        TerminalRowLayout::Cluster cluster{m_cursor.x(), 1, colors.bg, font, {}};
        for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; ++i) {
            cluster.chars[i] = cell.chars[i];
        }
        m_glyphs.drawCluster(p, cluster, m_padX, r.y() + m_baseline, m_cellW);
    } else {
        const TerminalRowLayout::Glyph glyph{m_cursor.x(), ch};
        m_glyphs.drawRun(p, &glyph, 1, font, colors.bg, m_padX, r.y() + m_baseline, m_cellW);
    }
}

void TerminalWidget::paintPreedit(QPainter &p)
{
    if (m_preedit.isEmpty()) return;
    const QFontMetricsF fm(m_font);
    QRectF r = cursorRect();
    r.setWidth(std::max(m_cellW, fm.horizontalAdvance(m_preedit)));
    p.fillRect(r, QColor::fromRgb(m_palette.defaultBg));
    p.setPen(QColor::fromRgb(m_palette.defaultFg));
    p.setFont(m_font);
    p.drawText(QPointF(r.x(), r.y() + m_baseline), m_preedit);
    p.fillRect(QRectF(r.x(), r.y() + m_underlineY, r.width(), m_lineWidth), QColor::fromRgb(m_palette.defaultFg));
}

// --- input ------------------------------------------------------------------------

void TerminalWidget::focusInEvent(QFocusEvent *event)
{
    m_hasFocus = true;
    if (m_focusReporting && m_pty) {
        writeToPty(QByteArray("\x1b[I", 3));
    }
    markLiveRowsDirty(m_cursor.y(), m_cursor.y() + 1);
    m_blinkPhaseOn = true;
    updateBlinkTimer();
    scheduleFrame();
    QAbstractScrollArea::focusInEvent(event);
}


void TerminalWidget::focusOutEvent(QFocusEvent *event)
{
    m_hasFocus = false;
    if (m_focusReporting && m_pty) {
        writeToPty(QByteArray("\x1b[O", 3));
    }
    markLiveRowsDirty(m_cursor.y(), m_cursor.y() + 1);
    updateBlinkTimer();
    scheduleFrame();
    QAbstractScrollArea::focusOutEvent(event);
}


void TerminalWidget::wheelEvent(QWheelEvent *event)
{
    if (m_vt && isMouseReportingActive(event->modifiers())) {
        const QPoint cell = pixelToCell(event->position().toPoint());
        VTermModifier vmod = qtModsToVterm(event->modifiers());
        const int delta = event->angleDelta().y();
        const int button = (delta > 0) ? 4 : 5;
        vterm_mouse_move(m_vt, cell.y(), cell.x(), vmod);
        vterm_mouse_button(m_vt, button, true, vmod);
        vterm_mouse_button(m_vt, button, false, vmod);
        event->accept();
        return;
    }
    event->accept();

    // Ctrl+wheel zooms every terminal (the owner persists the size).
    if (event->modifiers() & Qt::ControlModifier) {
        // Accumulate: smooth wheels and touchpads send deltas below a notch.
        m_zoomAccum += event->angleDelta().y();
        const int notches = m_zoomAccum / 120;
        if (notches != 0) {
            m_zoomAccum -= notches * 120;
            emit zoomRequested(notches);
        }
        return;
    }

    // Accumulate in lines so high-resolution wheels and touchpads scroll
    // smoothly instead of rounding every small delta away.
    if (!event->pixelDelta().isNull()) {
        m_wheelAccum += event->pixelDelta().y() / m_cellH;
    } else {
        m_wheelAccum += event->angleDelta().y() / 120.0 * kWheelLines;
    }
    const int lines = static_cast<int>(m_wheelAccum);
    if (lines == 0) return;
    m_wheelAccum -= lines;

    if (m_altScreen) {
        // Alternate scroll: full-screen apps without mouse mode (less, man)
        // scroll with the arrow keys.
        if (m_vt) {
            const VTermKey key = lines > 0 ? VTERM_KEY_UP : VTERM_KEY_DOWN;
            for (int i = 0; i < std::abs(lines); ++i) {
                vterm_keyboard_key(m_vt, key, VTERM_MOD_NONE);
            }
        }
        return;
    }
    setScrollOffset(m_scrollOffset + lines);
}


void TerminalWidget::inputMethodEvent(QInputMethodEvent *event)
{
    if (!event->commitString().isEmpty()) {
        scrollToBottom();
        writeToPty(event->commitString().toUtf8());
    }
    const QString preedit = event->preeditString();
    if (preedit != m_preedit) {
        m_preedit = preedit;
        // The preedit can be wider than one cell; repaint the whole row.
        markLiveRowsDirty(m_cursor.y(), m_cursor.y() + 1);
        scheduleFrame();
    }
    event->accept();
}


QVariant TerminalWidget::inputMethodQuery(Qt::InputMethodQuery query) const
{
    switch (query) {
    case Qt::ImEnabled:
        return true;
    case Qt::ImCursorRectangle: {
        QRectF r = cursorRect();
        if (!m_preedit.isEmpty()) {
            r.setWidth(std::max(m_cellW, QFontMetricsF(m_font).horizontalAdvance(m_preedit)));
        }
        // Queries are answered in this widget's coordinates, not the viewport's.
        return r.translated(viewport()->pos()).toAlignedRect();
    }
    case Qt::ImFont:
        return m_font;
    case Qt::ImHints:
        return static_cast<int>(Qt::ImhNoAutoUppercase | Qt::ImhNoPredictiveText);
    case Qt::ImCursorPosition:
    case Qt::ImAnchorPosition:
        return 0;
    case Qt::ImSurroundingText:
    case Qt::ImCurrentSelection:
        return QString();
    default:
        return QAbstractScrollArea::inputMethodQuery(query);
    }
}


// --- keyboard -------------------------------------------------------------------

// The key that makes control characters: Ctrl, which Qt reports as Meta on macOS.
bool TerminalWidget::terminalCtrl(Qt::KeyboardModifiers mods) const
{
#ifdef Q_OS_MACOS
    return (mods & Qt::MetaModifier) != 0;
#else
    return (mods & Qt::ControlModifier) != 0;
#endif
}

// The app's command key: Ctrl on Windows/Linux, Cmd on macOS (Qt's Control).
bool TerminalWidget::appCommand(Qt::KeyboardModifiers mods) const
{
    return (mods & Qt::ControlModifier) != 0;
}

bool TerminalWidget::event(QEvent *event)
{
    if (event->type() == QEvent::Hide) {
        // A hidden terminal keeps no pixels; the first paint rebuilds them.
        m_frame = QImage();
        m_glyphs.releaseTiles();
    }
    if (event->type() == QEvent::ShortcutOverride && m_vt && m_pty) {
        QKeyEvent *ke = static_cast<QKeyEvent *>(event);
        const int key = ke->key();
        const auto mods = ke->modifiers() & ~Qt::KeypadModifier;

        if (key == Qt::Key_Control || key == Qt::Key_Shift ||
            key == Qt::Key_Alt || key == Qt::Key_Meta) {
            return QAbstractScrollArea::event(event);
        }

        if (key >= Qt::Key_F1 && key <= Qt::Key_F35) {
            return QAbstractScrollArea::event(event);
        }

        struct ShortcutEntry { Qt::KeyboardModifiers mods; int key; };
        static constexpr ShortcutEntry appShortcuts[] = {
            {Qt::ControlModifier, Qt::Key_S},
            {Qt::ControlModifier, Qt::Key_N},
            {Qt::ControlModifier, Qt::Key_O},
            {Qt::ControlModifier, Qt::Key_W},
            {Qt::ControlModifier, Qt::Key_Q},
            {Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_S},
            {Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_W},
            {Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_T},
            {Qt::ControlModifier, Qt::Key_Tab},
            {Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_Tab},
            {Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_Backtab},
        };

        for (const auto &s : appShortcuts) {
            if (mods == s.mods && key == s.key) {
                return QAbstractScrollArea::event(event);
            }
        }

        ke->accept();
        return true;
    }

    // Qt routes Tab/Backtab through focusNextPrevChild() in QWidget::event(),
    // so they never reach keyPressEvent unless we intercept here.
    if (event->type() == QEvent::KeyPress) {
        QKeyEvent *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab) {
            keyPressEvent(ke);
            if (ke->isAccepted()) {
                return true;
            }
        }
    }
    return QAbstractScrollArea::event(event);
}

// Terminal-level shortcuts. Returns true when the key was consumed.
bool TerminalWidget::handleTerminalShortcut(QKeyEvent *event)
{
    const auto mods = event->modifiers() & ~Qt::KeypadModifier;
    const int key = event->key();
    const bool cmd = appCommand(mods);
    const bool shift = (mods & Qt::ShiftModifier) != 0;
    const bool alt = (mods & Qt::AltModifier) != 0;
    if (!cmd || alt) {
        return false;
    }

    if (shift) {
        switch (key) {
        case Qt::Key_C: copySelection(); return true;
        case Qt::Key_V: pasteClipboard(); return true;
        case Qt::Key_F: openSearch(); return true;
        case Qt::Key_L: emit clearRequested(); return true;
        case Qt::Key_A:
            if (m_hasSelection) emit sendToAiRequested();
            return true;
        case Qt::Key_Plus: emit zoomRequested(1); return true;  // Ctrl+Shift+= on US layouts
        default: return false;
        }
    }

    switch (key) {
    case Qt::Key_C:
        // Windows Terminal style: copy when there is a selection, otherwise
        // Ctrl+C reaches the program as an interrupt.
        if (m_hasSelection) {
            copySelection();
            clearSelection();
            return true;
        }
#ifdef Q_OS_MACOS
        return true;   // Cmd+C with nothing selected does nothing
#else
        return false;
#endif
    case Qt::Key_V: pasteClipboard(); return true;
    case Qt::Key_Equal:
    case Qt::Key_Plus: emit zoomRequested(1); return true;
    case Qt::Key_Minus: emit zoomRequested(-1); return true;
    case Qt::Key_0: emit zoomRequested(0); return true;
    default:
        break;
    }
#ifdef Q_OS_MACOS
    // Other Cmd combinations are never terminal input.
    return true;
#else
    return false;
#endif
}

void TerminalWidget::keyPressEvent(QKeyEvent *event)
{
    if ((event->key() == Qt::Key_Control || event->key() == Qt::Key_Meta) && viewport()->underMouse()) {
        // Holding the link modifier over text reveals links without moving.
        updateHoverLink(m_hoverPixel, event->modifiers());
    }
    if (!m_vt || !m_pty) {
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }
    if (handleTerminalShortcut(event)) {
        return;
    }

    const int key = event->key();
    const auto mods = event->modifiers();
    if (key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt || key == Qt::Key_Meta) {
        return;
    }

    // Typing returns the view to the live screen, like every other terminal.
    scrollToBottom();
    restartBlink();
    if (m_hasSelection && !event->text().isEmpty()) {
        clearSelection();
    }

    const bool ctrl = terminalCtrl(mods);
    VTermModifier vmod = VTERM_MOD_NONE;
    if (mods & Qt::ShiftModifier) vmod = static_cast<VTermModifier>(vmod | VTERM_MOD_SHIFT);
    if (ctrl)                     vmod = static_cast<VTermModifier>(vmod | VTERM_MOD_CTRL);
    if (mods & Qt::AltModifier)   vmod = static_cast<VTermModifier>(vmod | VTERM_MOD_ALT);

    auto sendKey = [&](VTermKey k) {
        vterm_keyboard_key(m_vt, k, vmod);
    };

    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:    sendKey(VTERM_KEY_ENTER); return;
    case Qt::Key_Backspace: sendKey(VTERM_KEY_BACKSPACE); return;
    case Qt::Key_Tab:       sendKey(VTERM_KEY_TAB); return;
    case Qt::Key_Backtab:   sendKey(VTERM_KEY_TAB); return;
    case Qt::Key_Escape:    sendKey(VTERM_KEY_ESCAPE); return;
    case Qt::Key_Up:        sendKey(VTERM_KEY_UP); return;
    case Qt::Key_Down:      sendKey(VTERM_KEY_DOWN); return;
    case Qt::Key_Left:      sendKey(VTERM_KEY_LEFT); return;
    case Qt::Key_Right:     sendKey(VTERM_KEY_RIGHT); return;
    case Qt::Key_Insert:    sendKey(VTERM_KEY_INS); return;
    case Qt::Key_Delete:    sendKey(VTERM_KEY_DEL); return;
    case Qt::Key_Home:      sendKey(VTERM_KEY_HOME); return;
    case Qt::Key_End:       sendKey(VTERM_KEY_END); return;
    case Qt::Key_PageUp:    sendKey(VTERM_KEY_PAGEUP); return;
    case Qt::Key_PageDown:  sendKey(VTERM_KEY_PAGEDOWN); return;
    default:
        break;
    }

    if (key >= Qt::Key_F1 && key <= Qt::Key_F35) {
        sendKey(static_cast<VTermKey>(VTERM_KEY_FUNCTION_0 + 1 + (key - Qt::Key_F1)));
        return;
    }

    if (ctrl && key >= Qt::Key_A && key <= Qt::Key_Z) {
        vterm_keyboard_unichar(m_vt, static_cast<uint32_t>('a' + (key - Qt::Key_A)), vmod);
        return;
    }

    const QString text = event->text();
    if (!text.isEmpty()) {
        for (uint cp : text.toUcs4()) {
            vterm_keyboard_unichar(m_vt, static_cast<uint32_t>(cp), vmod);
        }
        return;
    }

    QAbstractScrollArea::keyPressEvent(event);
}

void TerminalWidget::keyReleaseEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Control || event->key() == Qt::Key_Meta || !appCommand(event->modifiers())) {
        clearHoverLink();
    }
    QAbstractScrollArea::keyReleaseEvent(event);
}

// --- mouse ----------------------------------------------------------------------

void TerminalWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        setFocus(Qt::MouseFocusReason);
    }

    // Ctrl/Cmd+click opens links, even while a program has the mouse.
    if (event->button() == Qt::LeftButton && appCommand(event->modifiers())) {
        if (!m_hover.valid) {
            m_hoverProbePixel = QPoint(-1000, -1000);
            updateHoverLink(event->pos(), event->modifiers());
        }
        if (openHoverLink()) {
            event->accept();
            return;
        }
    }

    if (m_vt && isMouseReportingActive(event->modifiers())) {
        const QPoint cell = pixelToCell(event->pos());
        int button = 0;
        if (event->button() == Qt::LeftButton)   button = 1;
        else if (event->button() == Qt::MiddleButton) button = 2;
        else if (event->button() == Qt::RightButton)  button = 3;
        if (button > 0) {
            VTermModifier vmod = qtModsToVterm(event->modifiers());
            vterm_mouse_move(m_vt, cell.y(), cell.x(), vmod);
            vterm_mouse_button(m_vt, button, true, vmod);
        }
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton) {
        // A third click soon after a double click selects the whole line.
        if (m_lastDoubleClick.isValid()
            && m_lastDoubleClick.elapsed() <= QApplication::doubleClickInterval()
            && (event->pos() - m_lastDoubleClickPixel).manhattanLength() <= 4) {
            m_lastDoubleClick.invalidate();
            selectLine(pixelToAbsoluteCell(event->pos()).y());
            event->accept();
            return;
        }
        clearSelection();
        m_selStart = pixelToAbsoluteCell(event->pos());
        m_selEnd = m_selStart;
        m_pressPixel = event->pos();
        m_pressArmed = true;
        m_selecting = false;
    } else if (event->button() == Qt::MiddleButton) {
        const QClipboard *cb = QApplication::clipboard();
        const QString sel = cb->text(QClipboard::Selection);
        if (!sel.isEmpty()) {
            pasteText(sel);
        }
    }
    QAbstractScrollArea::mousePressEvent(event);
}

void TerminalWidget::mouseMoveEvent(QMouseEvent *event)
{
    m_hoverPixel = event->pos();
    if (event->buttons() == Qt::NoButton && appCommand(event->modifiers())) {
        updateHoverLink(event->pos(), event->modifiers());
    } else if (m_hover.valid) {
        clearHoverLink();
    }

    if (m_vt && isMouseReportingActive(event->modifiers())) {
        const QPoint cell = pixelToCell(event->pos());
        bool shouldReport = false;
        if (m_mouseMode == VTERM_PROP_MOUSE_MOVE) {
            shouldReport = true;
        } else if (m_mouseMode == VTERM_PROP_MOUSE_DRAG && (event->buttons() != Qt::NoButton)) {
            shouldReport = true;
        }
        if (shouldReport) {
            VTermModifier vmod = qtModsToVterm(event->modifiers());
            vterm_mouse_move(m_vt, cell.y(), cell.x(), vmod);
        }
        event->accept();
        return;
    }

    // A drag only starts selecting once it moves 2px, so a click never
    // selects one stray cell.
    if (m_pressArmed && (event->buttons() & Qt::LeftButton)
        && (event->pos() - m_pressPixel).manhattanLength() >= 2) {
        m_pressArmed = false;
        m_selecting = true;
    }
    if (m_selecting) {
        const QPoint end = pixelToAbsoluteCell(event->pos());
        if (end != m_selEnd || !m_hasSelection) {
            m_selEnd = end;
            m_hasSelection = true;
            markAllDirty();
            scheduleFrame();
        }
    }
    QAbstractScrollArea::mouseMoveEvent(event);
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_vt && isMouseReportingActive(event->modifiers())) {
        int button = 0;
        if (event->button() == Qt::LeftButton)   button = 1;
        else if (event->button() == Qt::MiddleButton) button = 2;
        else if (event->button() == Qt::RightButton)  button = 3;
        if (button > 0) {
            const QPoint cell = pixelToCell(event->pos());
            VTermModifier vmod = qtModsToVterm(event->modifiers());
            vterm_mouse_move(m_vt, cell.y(), cell.x(), vmod);
            vterm_mouse_button(m_vt, button, false, vmod);
        }
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton) {
        m_pressArmed = false;
        if (m_selecting) {
            m_selecting = false;
            m_selEnd = pixelToAbsoluteCell(event->pos());
            m_hasSelection = true;
            markAllDirty();
            scheduleFrame();
            QClipboard *cb = QApplication::clipboard();
            const QString text = selectionText();
            if (cb->supportsSelection()) {
                cb->setText(text, QClipboard::Selection);
            }
            if (m_copyOnSelect) {
                cb->setText(text);
            }
        }
    }
    QAbstractScrollArea::mouseReleaseEvent(event);
}

void TerminalWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && !isMouseReportingActive(event->modifiers())) {
        setFocus(Qt::MouseFocusReason);
        m_pressArmed = false;
        selectWord(pixelToAbsoluteCell(event->pos()));
        m_lastDoubleClick.start();
        m_lastDoubleClickPixel = event->pos();
        event->accept();
        return;
    }
    QAbstractScrollArea::mouseDoubleClickEvent(event);
}

void TerminalWidget::leaveEvent(QEvent *event)
{
    clearHoverLink();
    QAbstractScrollArea::leaveEvent(event);
}

// --- selection --------------------------------------------------------------------

QString TerminalWidget::selectionText() const
{
    if (!m_screen || !m_hasSelection) return QString();
    WidgetCellSource src(m_scrollback, m_screen, vterm_obtain_state(m_vt), m_rows, m_cols);
    return extractSelectionText(src, m_selStart, m_selEnd);
}

QString TerminalWidget::selectedText() const
{
    return selectionText();
}

void TerminalWidget::copySelection()
{
    if (!m_hasSelection) return;
    QApplication::clipboard()->setText(selectionText());
}

void TerminalWidget::clearSelection()
{
    m_selecting = false;
    if (!m_hasSelection) return;
    m_hasSelection = false;
    markAllDirty();
    scheduleFrame();
}

void TerminalWidget::selectAll()
{
    if (!m_screen) return;
    m_selStart = QPoint(0, 0);
    m_selEnd = QPoint(m_cols - 1, m_scrollback.size() + m_rows - 1);
    m_hasSelection = true;
    m_selecting = false;
    markAllDirty();
    scheduleFrame();
}

void TerminalWidget::selectWord(const QPoint &absCell)
{
    if (!m_screen) return;
    WidgetCellSource src(m_scrollback, m_screen, vterm_obtain_state(m_vt), m_rows, m_cols);
    const int row = absCell.y();
    const int cols = src.cols();
    if (row < 0 || row >= src.rowCount()) return;

    static const auto isWordChar = [](uint32_t ch) {
        if (ch == 0 || ch == ' ' || ch == '\t') return false;
        static constexpr char32_t delims[] = U"!@#$%^&*(){}[]|;:'\",.<>?`~=+";
        for (char32_t d : delims) {
            if (ch == d) return false;
        }
        return true;
    };

    int startCol = absCell.x();
    int endCol = absCell.x();

    while (startCol > 0) {
        VTermScreenCell cell = src.cellAt(row, startCol - 1);
        if (!isWordChar(cell.chars[0])) break;
        --startCol;
    }
    while (endCol < cols - 1) {
        VTermScreenCell cell = src.cellAt(row, endCol + 1);
        if (!isWordChar(cell.chars[0])) break;
        ++endCol;
    }

    m_selStart = QPoint(startCol, row);
    m_selEnd = QPoint(endCol, row);
    m_hasSelection = (startCol != endCol || src.cellAt(row, startCol).chars[0] != 0);
    m_selecting = false;
    markAllDirty();
    scheduleFrame();

    if (m_hasSelection) {
        QClipboard *cb = QApplication::clipboard();
        const QString text = selectionText();
        if (cb->supportsSelection()) {
            cb->setText(text, QClipboard::Selection);
        }
        if (m_copyOnSelect) {
            cb->setText(text);
        }
    }
}

// Triple click: the whole logical line, following soft wraps both ways.
void TerminalWidget::selectLine(int absoluteRow)
{
    if (!m_screen) return;
    WidgetCellSource src(m_scrollback, m_screen, vterm_obtain_state(m_vt), m_rows, m_cols);
    const int total = src.rowCount();
    if (absoluteRow < 0 || absoluteRow >= total) return;
    int first = absoluteRow;
    while (first > 0 && src.rowWraps(first - 1)) --first;
    int last = absoluteRow;
    while (last + 1 < total && src.rowWraps(last)) ++last;
    m_selStart = QPoint(0, first);
    m_selEnd = QPoint(m_cols - 1, last);
    m_hasSelection = true;
    m_selecting = false;
    markAllDirty();
    scheduleFrame();
    QClipboard *cb = QApplication::clipboard();
    const QString text = selectionText();
    if (cb->supportsSelection()) {
        cb->setText(text, QClipboard::Selection);
    }
    if (m_copyOnSelect) {
        cb->setText(text);
    }
}

void TerminalWidget::pasteClipboard()
{
    pasteText(QApplication::clipboard()->text());
}

// Bracketed when the program asked for it (DEC 2004), so shells do not run
// pasted lines one by one.
void TerminalWidget::pasteText(const QString &text)
{
    // Control characters are dropped so pasted text can never end bracketed
    // paste early or smuggle escape sequences to the program.
    const QString clean = sanitizeTerminalPaste(text);
    if (clean.isEmpty() || !m_vt) return;
    scrollToBottom();
    vterm_keyboard_start_paste(m_vt);
    for (uint cp : clean.toUcs4()) {
        vterm_keyboard_unichar(m_vt, static_cast<uint32_t>(cp), VTERM_MOD_NONE);
    }
    vterm_keyboard_end_paste(m_vt);
}

void TerminalWidget::shiftSelectionForEviction(int evicted)
{
    // Eviction shifts absolute history rows down; keep the selection on its
    // content and trim the part that fell off the top.
    if (evicted <= 0 || !m_hasSelection) return;
    m_selStart.ry() -= evicted;
    m_selEnd.ry() -= evicted;
    if (m_selStart.y() < 0 && m_selEnd.y() < 0) {
        m_hasSelection = false;
    } else {
        if (m_selStart.y() < 0) m_selStart = QPoint(0, 0);
        if (m_selEnd.y() < 0) m_selEnd = QPoint(0, 0);
    }
}

void TerminalWidget::noteLinesDropped(int count)
{
    if (count <= 0) return;
    m_linesDropped += count;
    while (!m_osc8Spans.empty() && m_osc8Spans.front().endLine < m_linesDropped) {
        m_osc8Spans.pop_front();
    }
    if (!m_matches.empty() && m_matches.front().line < m_linesDropped) {
        const auto keep = std::lower_bound(m_matches.begin(), m_matches.end(), m_linesDropped,
                                           [](const TerminalSearchMatch &m, qint64 line) { return m.line < line; });
        const int removed = static_cast<int>(keep - m_matches.begin());
        m_matches.erase(m_matches.begin(), keep);
        // An evicted current match leaves none current.
        m_activeMatch = m_activeMatch < removed ? -1 : m_activeMatch - removed;
    }
    if (m_hover.valid && m_hover.startLine < m_linesDropped) {
        clearHoverLink();
    }
}

// Absolute candidates for a path written in the output: as written when
// absolute (with ~ expanded), else against the shell's reported cwd and then
// the base directories (spawn directory, workspace root).
QStringList TerminalWidget::pathCandidates(const QString &written) const
{
    QString path = written;
    if (path == QLatin1String("~") || path.startsWith(QLatin1String("~/")) || path.startsWith(QLatin1String("~\\"))) {
        path = QDir::home().filePath(path.mid(2));
    }
    if (QDir::isAbsolutePath(path)) {
        return {QDir::cleanPath(path)};
    }
    QStringList out;
    if (!m_reportedCwd.isEmpty()) {
        out << QDir(m_reportedCwd).filePath(path);
    }
    for (const QString &base : m_linkBaseDirs) {
        const QString candidate = QDir(base).filePath(path);
        if (!out.contains(candidate)) out << candidate;
    }
    return out;
}

// Web and mail links go to the OS; file URLs are opened like paths (never
// executed) and only on this machine; anything else is not followed.
void TerminalWidget::openUrlTarget(const QString &target)
{
    const QUrl url(target);
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https") || scheme == QLatin1String("mailto")) {
        QDesktopServices::openUrl(url);
        return;
    }
    if (scheme != QLatin1String("file") || !m_localPathLinks) return;
    const QString path = url.toLocalFile();
    // A UNC target would reach out to another machine.
    if (path.isEmpty() || path.startsWith(QLatin1String("//")) || path.startsWith(QLatin1String("\\\\"))) return;
    const QFileInfo fi(path);
    if (fi.isDir()) {
        emit revealPathRequested(fi.absoluteFilePath());
    } else if (fi.exists()) {
        emit openFileRequested(fi.absoluteFilePath(), 0, 0);
    }
}

// The text between two absolute positions, or a null string when part of it
// is no longer in history.
QString TerminalWidget::textBetween(qint64 startLine, int startCol, qint64 endLine, int endCol) const
{
    if (!m_screen) return QString();
    WidgetCellSource src(m_scrollback, m_screen, vterm_obtain_state(m_vt), m_rows, m_cols);
    QString out;
    for (qint64 line = startLine; line <= endLine; ++line) {
        const qint64 row = line - m_linesDropped;
        if (row < 0 || row >= src.rowCount()) return QString();
        const int from = line == startLine ? startCol : 0;
        const int to = line == endLine ? endCol : m_cols;
        for (int col = from; col < to && col < m_cols; ++col) {
            const VTermScreenCell cell = src.cellAt(static_cast<int>(row), col);
            if (cell.chars[0] == kTerminalWideSpacer) continue;
            if (cell.chars[0] == 0) {
                out.append(QLatin1Char(' '));
                continue;
            }
            for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; ++i) {
                const char32_t cp = cell.chars[i];
                out.append(QString::fromUcs4(&cp, 1));
            }
        }
    }
    return out;
}

// An OSC 8 span is only a link while its text is still what it covered;
// output that overwrites it (clear, a redraw) kills it for good.
bool TerminalWidget::osc8SpanAlive(Osc8Span &span) const
{
    if (span.dead) return false;
    if (textBetween(span.startLine, span.startCol, span.endLine, span.endCol) != span.text) {
        span.dead = true;
    }
    return !span.dead;
}

// --- context menu ----------------------------------------------------------------

void TerminalWidget::contextMenuEvent(QContextMenuEvent *event)
{
    // Shift+right-click always reaches the menu, even when a program owns the mouse.
    if (isMouseReportingActive(QGuiApplication::keyboardModifiers())) {
        event->ignore();
        return;
    }

    QMenu menu(this);

    QAction *copyAct = menu.addAction(tr("Copy"));
    copyAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_C));
    copyAct->setEnabled(m_hasSelection);
    connect(copyAct, &QAction::triggered, this, [this]() { copySelection(); });

    QAction *pasteAct = menu.addAction(tr("Paste"));
    pasteAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_V));
    connect(pasteAct, &QAction::triggered, this, [this]() { pasteClipboard(); });

    QAction *allAct = menu.addAction(tr("Select All"));
    connect(allAct, &QAction::triggered, this, [this]() { selectAll(); });

    menu.addSeparator();
    QAction *findAct = menu.addAction(tr("Find..."));
    findAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));
    connect(findAct, &QAction::triggered, this, [this]() { openSearch(); });
    QAction *clearAct = menu.addAction(tr("Clear"));
    clearAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_L));
    connect(clearAct, &QAction::triggered, this, [this]() { emit clearRequested(); });

    // Owner section: Send to AI.
    emit contextMenuAboutToShow(&menu);

    // Link under the pointer.
    const QPoint local = viewport()->mapFromGlobal(event->globalPos());
    const QPoint cell = pixelToCell(local);
    const int absRow = viewportToAbsoluteRow(cell.y());
    QString text;
    std::vector<qint64> lineOf;
    std::vector<int> colOf;
    if (logicalLineAt(absRow, &text, &lineOf, &colOf)) {
        const qint64 lineId = lineIdForAbsoluteRow(absRow);
        int index = -1;
        for (int i = 0; i < static_cast<int>(colOf.size()); ++i) {
            if (lineOf[static_cast<std::size_t>(i)] == lineId && colOf[static_cast<std::size_t>(i)] <= cell.x()) {
                index = i;
            }
        }
        const TerminalLink link = index >= 0 ? findTerminalLinkAt(text, index) : TerminalLink{};
        if (link.kind == TerminalLink::Url) {
            menu.addSeparator();
            const QString url = link.target;
            connect(menu.addAction(tr("Open Link")), &QAction::triggered, this,
                    [this, url]() { openUrlTarget(url); });
            connect(menu.addAction(tr("Copy Link")), &QAction::triggered, this,
                    [url]() { QApplication::clipboard()->setText(url); });
        } else if (link.kind == TerminalLink::Path && m_localPathLinks) {
            menu.addSeparator();
            const QStringList candidates = pathCandidates(link.target);
            const int line = link.line;
            const int column = link.column;
            connect(menu.addAction(tr("Open File")), &QAction::triggered, this, [this, candidates, line, column]() {
                for (const QString &c : candidates) {
                    const QFileInfo fi(c);
                    if (fi.isDir()) {
                        emit revealPathRequested(fi.absoluteFilePath());
                        return;
                    }
                    if (fi.exists()) {
                        emit openFileRequested(fi.absoluteFilePath(), line, column);
                        return;
                    }
                }
            });
            const QString path = link.target;
            connect(menu.addAction(tr("Copy Path")), &QAction::triggered, this,
                    [path]() { QApplication::clipboard()->setText(path); });
        }
    }

    // Owner section: Rename / New Terminal / Close.
    emit contextMenuTailAboutToShow(&menu);
    menu.exec(event->globalPos());
}

// --- links ----------------------------------------------------------------------

// The logical line around `absoluteRow` (soft-wrapped rows joined), with the
// line id and column of every UTF-16 unit.
bool TerminalWidget::logicalLineAt(int absoluteRow, QString *text, std::vector<qint64> *lineOf,
                                   std::vector<int> *colOf) const
{
    if (!m_screen) return false;
    WidgetCellSource src(m_scrollback, m_screen, vterm_obtain_state(m_vt), m_rows, m_cols);
    const int total = src.rowCount();
    if (absoluteRow < 0 || absoluteRow >= total) return false;
    int first = absoluteRow;
    while (first > 0 && absoluteRow - first < kMaxLogicalLineRows && src.rowWraps(first - 1)) --first;
    int last = absoluteRow;
    while (last + 1 < total && last - absoluteRow < kMaxLogicalLineRows && src.rowWraps(last)) ++last;

    text->clear();
    lineOf->clear();
    colOf->clear();
    for (int row = first; row <= last; ++row) {
        const qint64 id = lineIdForAbsoluteRow(row);
        for (int col = 0; col < m_cols; ++col) {
            const VTermScreenCell cell = src.cellAt(row, col);
            const uint32_t ch = cell.chars[0];
            if (ch == kTerminalWideSpacer) continue;
            if (ch == 0) {
                text->append(QLatin1Char(' '));
                lineOf->push_back(id);
                colOf->push_back(col);
                continue;
            }
            for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; ++i) {
                const char32_t cp = cell.chars[i];
                const QString s = QString::fromUcs4(&cp, 1);
                for (const QChar c : s) {
                    text->append(c);
                    lineOf->push_back(id);
                    colOf->push_back(col);
                }
            }
        }
    }
    return true;
}

void TerminalWidget::updateHoverLink(const QPoint &pixel, Qt::KeyboardModifiers mods)
{
    if (!appCommand(mods) || !m_screen || !viewport()->rect().contains(pixel)) {
        clearHoverLink();
        return;
    }
    // Throttled like Zed: a probe per 5px of travel or 100ms.
    if (m_hoverThrottle.isValid() && m_hoverThrottle.elapsed() < 100
        && (pixel - m_hoverProbePixel).manhattanLength() < 5) {
        return;
    }
    m_hoverThrottle.start();
    m_hoverProbePixel = pixel;

    const QPoint cell = pixelToCell(pixel);
    const int absRow = viewportToAbsoluteRow(cell.y());
    const qint64 lineId = lineIdForAbsoluteRow(absRow);

    // OSC 8 hyperlinks first: the program said exactly what they point at.
    for (auto it = m_osc8Spans.rbegin(); it != m_osc8Spans.rend(); ++it) {
        const bool after = lineId > it->startLine || (lineId == it->startLine && cell.x() >= it->startCol);
        const bool before = lineId < it->endLine || (lineId == it->endLine && cell.x() < it->endCol);
        if (after && before && osc8SpanAlive(*it)) {
            HoverLink link;
            link.valid = true;
            link.url = true;
            link.startLine = it->startLine;
            link.startCol = it->startCol;
            link.endLine = it->endLine;
            link.endCol = it->endCol;
            link.target = it->uri;
            setHoverLink(link);
            return;
        }
        if (it->endLine < lineId) break;
    }

    QString text;
    std::vector<qint64> lineOf;
    std::vector<int> colOf;
    if (!logicalLineAt(absRow, &text, &lineOf, &colOf)) {
        clearHoverLink();
        return;
    }
    int index = -1;
    for (int i = 0; i < static_cast<int>(colOf.size()); ++i) {
        if (lineOf[static_cast<std::size_t>(i)] == lineId && colOf[static_cast<std::size_t>(i)] <= cell.x()) {
            index = i;
        }
    }
    const TerminalLink found = index >= 0 ? findTerminalLinkAt(text, index) : TerminalLink{};
    if (found.kind == TerminalLink::None) {
        clearHoverLink();
        return;
    }

    const auto first = static_cast<std::size_t>(found.start);
    const auto last = static_cast<std::size_t>(found.start + found.length - 1);
    HoverLink link;
    link.startLine = lineOf[first];
    link.startCol = colOf[first];
    link.endLine = lineOf[last];
    link.endCol = colOf[last] + 1;
    link.line = found.line;
    link.column = found.column;

    if (found.kind == TerminalLink::Url) {
        link.valid = true;
        link.url = true;
        link.target = found.target;
        setHoverLink(link);
        return;
    }
    if (!m_localPathLinks) {
        clearHoverLink();
        return;
    }

    // Paths: the file must exist, checked off the UI thread and cached.
    const QStringList candidates = pathCandidates(found.target);
    if (candidates.isEmpty()) {
        clearHoverLink();
        return;
    }
    const QString key = candidates.join(QLatin1Char('\n'));
    const auto cached = m_pathCache.constFind(key);
    // A missing file is re-probed after a while: it may have been created.
    if (cached != m_pathCache.constEnd()
        && (!cached->resolved.isEmpty() || m_clock.elapsed() - cached->checkedMs < kMissingPathRecheckMs)) {
        if (cached->resolved.isEmpty()) {
            clearHoverLink();
        } else {
            link.valid = true;
            link.target = cached->resolved;
            setHoverLink(link);
        }
        return;
    }
    if (key == m_hoverPendingKey) return;
    m_hoverPendingKey = key;
    m_hoverPendingLink = link;
    m_pathWatcher->setProperty("nn_key", key);
    m_pathWatcher->setFuture(QtConcurrent::run([candidates]() {
        for (const QString &c : candidates) {
            const QFileInfo fi(c);
            if (fi.exists()) return fi.absoluteFilePath();
        }
        return QString();
    }));
}

void TerminalWidget::setHoverLink(const HoverLink &link)
{
    const bool same = m_hover.valid && link.valid && m_hover.startLine == link.startLine
        && m_hover.startCol == link.startCol && m_hover.endLine == link.endLine
        && m_hover.endCol == link.endCol && m_hover.target == link.target;
    if (same) return;
    if (m_hover.valid) {
        markLineIdsDirty(m_hover.startLine, m_hover.endLine);
    }
    m_hover = link;
    markLineIdsDirty(link.startLine, link.endLine);
    scheduleFrame();
    viewport()->setCursor(Qt::PointingHandCursor);

    QString tip = link.target;
    if (!link.url && link.line > 0) {
        tip += QLatin1Char(':') + QString::number(link.line);
        if (link.column > 0) tip += QLatin1Char(':') + QString::number(link.column);
    }
#ifdef Q_OS_MACOS
    tip += QLatin1Char('\n') + tr("Cmd+Click to open");
#else
    tip += QLatin1Char('\n') + tr("Ctrl+Click to open");
#endif
    QToolTip::showText(viewport()->mapToGlobal(m_hoverPixel), tip, viewport());
}

void TerminalWidget::clearHoverLink()
{
    m_hoverPendingKey.clear();
    if (!m_hover.valid) return;
    markLineIdsDirty(m_hover.startLine, m_hover.endLine);
    m_hover = HoverLink{};
    scheduleFrame();
    viewport()->setCursor(Qt::IBeamCursor);
    QToolTip::hideText();
}

bool TerminalWidget::openHoverLink()
{
    if (!m_hover.valid) return false;
    const HoverLink link = m_hover;
    clearHoverLink();
    if (link.url) {
        openUrlTarget(link.target);
    } else if (QFileInfo(link.target).isDir()) {
        emit revealPathRequested(link.target);
    } else {
        emit openFileRequested(link.target, link.line, link.column);
    }
    return true;
}

void TerminalWidget::markLineIdsDirty(qint64 from, qint64 to)
{
    const int historySize = m_scrollback.size();
    for (int v = 0; v < m_rows; ++v) {
        const int absolute = m_altScreen ? historySize + v : historySize - m_scrollOffset + v;
        const qint64 id = lineIdForAbsoluteRow(absolute);
        if (id >= from && id <= to) {
            markViewportRowDirty(v);
        }
    }
}

// --- search ---------------------------------------------------------------------

void TerminalWidget::ensureSearchBar()
{
    if (m_searchBar) return;
    m_searchBar = new TerminalSearchBar(viewport());
    m_searchBar->hide();
    connect(m_searchBar, &TerminalSearchBar::optionsChanged, m_searchDebounce, qOverload<>(&QTimer::start));
    connect(m_searchBar, &TerminalSearchBar::nextRequested, this, [this]() { stepSearch(1); });
    connect(m_searchBar, &TerminalSearchBar::previousRequested, this, [this]() { stepSearch(-1); });
    connect(m_searchBar, &TerminalSearchBar::closeRequested, this, &TerminalWidget::closeSearch);
}

void TerminalWidget::positionSearchBar()
{
    if (!m_searchBar) return;
    m_searchBar->adjustSize();
    const int x = std::max(0, viewport()->width() - m_searchBar->width() - TerminalScrollBar::kWidth - 8);
    m_searchBar->move(x, 6);
}

void TerminalWidget::openSearch()
{
    ensureSearchBar();
    if (m_hasSelection) {
        const QString selected = selectionText();
        if (!selected.isEmpty() && !selected.contains(QLatin1Char('\n')) && selected.size() <= 200) {
            m_searchBar->setQuery(selected);
        }
    }
    positionSearchBar();
    m_searchBar->show();
    m_searchBar->raise();
    m_searchBar->focusQuery();
    m_searchDebounce->stop();
    runSearch();
}

void TerminalWidget::closeSearch()
{
    m_searchDebounce->stop();
    m_searchFollow->stop();
    m_searchSlice->stop();
    m_searchScanning = false;
    if (m_searchBar) {
        m_searchBar->hide();
    }
    m_searchRe = QRegularExpression();
    m_matches.clear();
    m_activeMatch = -1;
    syncSearchMarks();
    markAllDirty();
    scheduleFrame();
    setFocus(Qt::OtherFocusReason);
}

// Full search: history on a worker (over a snapshot of the compact buffer),
// then the tail and the live screen on the UI thread when it returns.
void TerminalWidget::runSearch()
{
    if (!m_searchBar) return;
    m_searchOptions = m_searchBar->options();
    m_searchFollow->stop();
    m_searchSlice->stop();
    m_searchScanning = false;
    m_matches.clear();
    m_activeMatch = -1;
    if (m_searchOptions.query.isEmpty()) {
        m_searchRe = QRegularExpression();
        m_searchBar->setInvalid(false);
        m_searchBar->setCount(-1, 0);
        syncSearchMarks();
        markAllDirty();
        scheduleFrame();
        return;
    }
    QRegularExpression re = terminalSearchRegex(m_searchOptions);
    if (!re.isValid()) {
        m_searchRe = QRegularExpression();
        m_searchBar->setInvalid(true);
        syncSearchMarks();
        markAllDirty();
        scheduleFrame();
        return;
    }
    m_searchBar->setInvalid(false);
    m_searchRe = re;
    // Blank rows cannot hold a literal match without spaces; regexes might.
    m_searchSkipBlank = !m_searchOptions.regex && !m_searchOptions.query.contains(QLatin1Char(' '));
    m_searchScanNext = m_linesDropped;
    m_searchedHistoryEnd = m_linesDropped;
    m_searchScanning = true;
    m_searchRevealPending = true;
    searchSlice();
}

// History is searched in slices of at most kSearchSliceBudget per event-loop
// pass, oldest to newest, straight from the compact buffer: no copy of the
// scrollback, no locking, and the UI stays responsive on 100k lines.
void TerminalWidget::searchSlice()
{
    if (!m_searchScanning || !m_searchRe.isValid()) return;
    const auto start = std::chrono::steady_clock::now();
    const qint64 historyEnd = m_linesDropped + m_scrollback.size();
    m_searchScanNext = std::max(m_searchScanNext, m_linesDropped);
    if (static_cast<int>(m_rowCells.size()) < m_cols) {
        m_rowCells.resize(static_cast<std::size_t>(m_cols));
    }
    QString text;
    std::vector<int> colOf;
    int scanned = 0;
    while (m_searchScanNext < historyEnd) {
        if (m_scrollback.lineAt(absoluteRowForLineId(m_searchScanNext), m_cols, m_rowCells.data())) {
            searchTerminalRow(m_searchRe, m_searchScanNext, m_rowCells.data(), m_cols, m_matches, text, colOf,
                              m_searchSkipBlank);
        }
        ++m_searchScanNext;
        if ((++scanned & 63) == 0 && std::chrono::steady_clock::now() - start > kSearchSliceBudget) {
            break;
        }
    }
    m_searchedHistoryEnd = m_searchScanNext;
    if (m_searchScanNext < historyEnd) {
        if (m_searchBar) m_searchBar->setCount(m_activeMatch, static_cast<int>(m_matches.size()));
        m_searchSlice->start();
        return;
    }

    m_searchScanning = false;
    searchNewOutput();
    // Start from the most recent hit, like a terminal search from the prompt.
    if (m_searchRevealPending) {
        m_searchRevealPending = false;
        if (!m_matches.empty()) {
            m_activeMatch = static_cast<int>(m_matches.size()) - 1;
            revealMatch(m_matches.back());
        }
    }
    if (m_searchBar) m_searchBar->setCount(m_activeMatch, static_cast<int>(m_matches.size()));
    syncSearchMarks();
    markAllDirty();
    scheduleFrame();
}



// Incremental pass: history pushed since the last search plus the live rows.
// Old matches on rows that were live (they may have changed) are replaced.
// Incremental pass: history pushed since the last search plus the live rows.
// Old matches on rows that were live (they may have changed) are replaced.
void TerminalWidget::searchNewOutput()
{
    // While history is still being sliced, the slice reaches the new lines.
    if (!m_searchRe.isValid() || m_searchScanning) return;
    const qint64 historyEnd = m_linesDropped + m_scrollback.size();
    // History can shrink (a taller window pops lines back): clamp.
    const qint64 from = std::clamp(m_searchedHistoryEnd, m_linesDropped, historyEnd);

    TerminalSearchMatch active{};
    const bool hadActive = m_activeMatch >= 0 && m_activeMatch < static_cast<int>(m_matches.size());
    if (hadActive) active = m_matches[static_cast<std::size_t>(m_activeMatch)];

    m_matches.erase(std::remove_if(m_matches.begin(), m_matches.end(),
                                   [this, from](const TerminalSearchMatch &m) {
                                       return m.line >= from || m.line < m_linesDropped;
                                   }),
                    m_matches.end());

    if (historyEnd - from > kSearchSyncLines) {
        // Too much to do in one pass: continue in slices.
        m_activeMatch = -1;
        m_searchScanNext = from;
        m_searchScanning = true;
        m_searchSlice->start();
        return;
    }

    if (static_cast<int>(m_rowCells.size()) < m_cols) {
        m_rowCells.resize(static_cast<std::size_t>(m_cols));
    }
    QString text;
    std::vector<int> colOf;
    for (qint64 id = from; id < historyEnd; ++id) {
        if (m_scrollback.lineAt(absoluteRowForLineId(id), m_cols, m_rowCells.data())) {
            searchTerminalRow(m_searchRe, id, m_rowCells.data(), m_cols, m_matches, text, colOf, m_searchSkipBlank);
        }
    }
    m_searchedHistoryEnd = historyEnd;
    VTermPos pos{0, 0};
    for (pos.row = 0; pos.row < m_rows; ++pos.row) {
        bool ok = true;
        for (pos.col = 0; pos.col < m_cols && ok; ++pos.col) {
            ok = vterm_screen_get_cell(m_screen, pos, &m_rowCells[static_cast<std::size_t>(pos.col)]) != 0;
        }
        if (ok) {
            searchTerminalRow(m_searchRe, historyEnd + pos.row, m_rowCells.data(), m_cols, m_matches, text, colOf,
                              m_searchSkipBlank);
        }
    }

    m_activeMatch = -1;
    if (hadActive) {
        for (std::size_t i = 0; i < m_matches.size(); ++i) {
            if (m_matches[i].line == active.line && m_matches[i].col == active.col) {
                m_activeMatch = static_cast<int>(i);
                break;
            }
        }
    }
    if (m_activeMatch < 0 && !m_matches.empty() && hadActive) {
        m_activeMatch = static_cast<int>(m_matches.size()) - 1;
    }
    if (m_searchBar) m_searchBar->setCount(m_activeMatch, static_cast<int>(m_matches.size()));
    syncSearchMarks();
    markAllDirty();
    scheduleFrame();
}

void TerminalWidget::stepSearch(int delta)
{
    if (m_matches.empty()) return;
    const int n = static_cast<int>(m_matches.size());
    m_activeMatch = m_activeMatch < 0 ? (delta > 0 ? 0 : n - 1) : ((m_activeMatch + delta) % n + n) % n;
    revealMatch(m_matches[static_cast<std::size_t>(m_activeMatch)]);
    if (m_searchBar) m_searchBar->setCount(m_activeMatch, n);
    syncSearchMarks();
    markAllDirty();
    scheduleFrame();
}

void TerminalWidget::revealMatch(const TerminalSearchMatch &match)
{
    if (m_altScreen) return;
    const int absRow = absoluteRowForLineId(match.line);
    const int historySize = m_scrollback.size();
    const int top = historySize - m_scrollOffset;
    if (absRow >= top && absRow < top + m_rows) return;
    // Centre the hit; live rows are reached at offset 0.
    setScrollOffset(std::clamp(historySize - absRow + m_rows / 2, 0, historySize));
}

void TerminalWidget::syncSearchMarks()
{
    if (m_matches.empty()) {
        m_scrollBar->setMarks({}, -1, 0, 0);
        return;
    }
    std::vector<int> rows;
    rows.reserve(m_matches.size());
    int lastRow = -1;
    int active = -1;
    for (std::size_t i = 0; i < m_matches.size(); ++i) {
        const int row = absoluteRowForLineId(m_matches[i].line);
        if (static_cast<int>(i) == m_activeMatch) {
            active = static_cast<int>(rows.size());
            rows.push_back(row);
            lastRow = row;
            continue;
        }
        if (row != lastRow) {
            rows.push_back(row);
            lastRow = row;
        }
    }
    // The scheme's yellows, so marks match the terminal's palette.
    const QRgb mark = blendRgb(m_palette.defaultBg, m_palette.indexed[3], 80);
    const QRgb activeMark = m_palette.indexed[11];
    m_scrollBar->setMarks(std::move(rows), active, mark, activeMark);
}

int TerminalWidget::collectHighlights(qint64 lineId)
{
    m_rowHighlights.clear();
    m_rowActiveFrom = -1;
    m_rowActiveTo = -1;
    if (m_matches.empty()) return 0;
    auto it = std::lower_bound(m_matches.begin(), m_matches.end(), lineId,
                               [](const TerminalSearchMatch &m, qint64 line) { return m.line < line; });
    // Hits use the scheme's yellow; the current one is stronger and outlined.
    const QRgb normal = blendRgb(m_palette.defaultBg, m_palette.indexed[3], 35);
    const QRgb strong = blendRgb(m_palette.defaultBg, m_palette.indexed[11], 60);
    for (; it != m_matches.end() && it->line == lineId; ++it) {
        const bool isActive = (it - m_matches.begin()) == m_activeMatch;
        m_rowHighlights.push_back({it->col, it->col + it->width, isActive ? strong : normal});
        if (isActive) {
            m_rowActiveFrom = it->col;
            m_rowActiveTo = it->col + it->width;
        }
    }
    return static_cast<int>(m_rowHighlights.size());
}

// --- drag and drop ----------------------------------------------------------------

void TerminalWidget::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void TerminalWidget::dragMoveEvent(QDragMoveEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

// Dropped files are typed as shell-quoted paths, space separated.
void TerminalWidget::dropEvent(QDropEvent *event)
{
    QStringList paths;
    const auto urls = event->mimeData()->urls();
    for (const QUrl &url : urls) {
        if (url.isLocalFile()) {
            paths << url.toLocalFile();
        }
    }
    if (paths.isEmpty()) return;
    event->acceptProposedAction();
    pasteDroppedPaths(paths);
}

void TerminalWidget::pasteDroppedPaths(const QStringList &paths)
{
    // Local paths mean nothing to a remote shell.
    if (!m_localPathLinks) return;
    QStringList words;
    for (QString path : paths) {
        if (path.isEmpty()) continue;
        if (m_shellKind == TerminalShellKind::PowerShell || m_shellKind == TerminalShellKind::Cmd) {
            path = QDir::toNativeSeparators(path);
        }
        words << quotePathForShell(path, m_shellKind);
    }
    if (words.isEmpty()) return;
    setFocus(Qt::MouseFocusReason);
    pasteText(words.join(QLatin1Char(' ')) + QLatin1Char(' '));
}
