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
#include "TerminalCellSource.h"
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
#include <QScrollBar>
#include <QWheelEvent>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace {

constexpr uint32_t kWideSpacer = 0xFFFFFFFFu;
// Resizes are applied at most once per this interval while a splitter drags.
constexpr int kResizeThrottleMs = 16;

class WidgetCellSource final : public TerminalCellSource
{
public:
    WidgetCellSource(const ScrollbackBuffer &scrollback,
                     VTermScreen *screen,
                     int liveRows,
                     int cols)
        : m_scrollback(scrollback)
        , m_screen(screen)
        , m_liveRows(liveRows)
        , m_cols(cols)
    {
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
    int m_liveRows;
    int m_cols;
    mutable int m_cachedRow = -1;
    mutable std::vector<VTermScreenCell> m_cachedLine;
};

qreal snapTo(qreal v, qreal dpr)
{
    return std::round(v * dpr) / dpr;
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
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    if (QScrollBar *sb = verticalScrollBar()) {
        sb->setSingleStep(1);
        sb->setPageStep(m_rows);
        sb->setRange(0, 0);
    }

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
    m_resizeTimer->setInterval(kResizeThrottleMs);
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
        return 1;
    }
    if (prop == VTERM_PROP_ALTSCREEN && val) {
        w->m_altScreen = val->boolean != 0;
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
    if (prop == VTERM_PROP_FOCUSREPORT && val) {
        w->m_focusReporting = val->boolean != 0;
        return 1;
    }
    return 0;
}

int TerminalWidget::vtermBell(void *user)
{
    Q_UNUSED(user);
    QApplication::beep();
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

    // Eviction shifts absolute history-row indices down; keep selection anchored
    // to content and trim the part that fell off the top.
    if (evicted > 0 && w->m_hasSelection) {
        w->m_selStart.ry() -= evicted;
        w->m_selEnd.ry()   -= evicted;
        if (w->m_selStart.y() < 0 && w->m_selEnd.y() < 0) {
            w->m_hasSelection = false;
        } else {
            if (w->m_selStart.y() < 0) w->m_selStart = QPoint(0, 0);
            if (w->m_selEnd.y()   < 0) w->m_selEnd = QPoint(0, 0);
        }
    }

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
    w->m_scrollbarDirty = true;
    w->markAllDirty();
    return 1;
}

int TerminalWidget::vtermSbClear(void *user)
{
    // CSI 3 J: the application asked for the scrollback to be erased.
    auto *w = static_cast<TerminalWidget *>(user);
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
    QScrollBar *sb = verticalScrollBar();
    if (!sb) return;
    const int max = m_scrollback.size();
    m_scrollOffset = std::min(m_scrollOffset, max);
    m_inScrollbarSync = true;
    sb->setRange(0, max);
    sb->setPageStep(m_rows);
    sb->setValue(max - m_scrollOffset);
    m_inScrollbarSync = false;
}

void TerminalWidget::scrollContentsBy(int dx, int dy)
{
    Q_UNUSED(dx);
    Q_UNUSED(dy);
    if (m_inScrollbarSync) return;
    if (QScrollBar *sb = verticalScrollBar()) {
        m_scrollOffset = sb->maximum() - sb->value();
    }
    markAllDirty();
    scheduleFrame();
}

void TerminalWidget::scrollToBottom()
{
    if (m_scrollOffset == 0) return;
    m_scrollOffset = 0;
    m_scrollbarDirty = true;
    markAllDirty();
    scheduleFrame();
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
    vterm_screen_enable_altscreen(m_screen, 1);
    vterm_screen_enable_reflow(m_screen, true);
    // Merge damage down to one scroll + rects per flush; we flush once per batch.
    vterm_screen_set_damage_merge(m_screen, VTERM_DAMAGE_SCROLL);
    vterm_screen_reset(m_screen, 1);

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
    m_cellW = std::max<qreal>(1.0, std::round(advance * m_dpr)) / m_dpr;
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
    markAllDirty();
    // Splitter drags fire a resize per mouse move; apply the latest size at
    // most once per kResizeThrottleMs so the shell is not flooded with SIGWINCH.
    if (m_vt && !m_resizeTimer->isActive()) {
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
    p.fillRect(bounds, QColor::fromRgb(m_palette.defaultBg));

    if (!m_errorMessage.isEmpty() && (!m_pty || m_pty->pid() == 0)) {
        p.setPen(QColor::fromRgb(m_palette.defaultFg));
        p.setFont(m_font);
        p.drawText(viewport()->rect().adjusted(8, 8, -8, -8), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, m_errorMessage);
        return;
    }
    if (!m_screen) {
        return;
    }

    if (static_cast<int>(m_rowCells.size()) < m_cols) {
        m_rowCells.resize(static_cast<std::size_t>(m_cols));
    }

    const qreal top = originY();
    const int first = std::max(0, static_cast<int>(std::floor((bounds.top() - top) / m_cellH)));
    const int last = std::min(m_rows - 1, static_cast<int>(std::floor((bounds.bottom() - top) / m_cellH)));
    const QRegion &region = event->region();
    const int width = viewport()->width();
    for (int row = first; row <= last; ++row) {
        const QRect rowRect(0, static_cast<int>(std::floor(rowTop(row))), width,
                            static_cast<int>(std::ceil(m_cellH)) + 1);
        if (region.intersects(rowRect)) {
            paintRow(p, row);
        }
    }

    paintCursor(p);
    paintPreedit(p);
}

void TerminalWidget::paintRow(QPainter &p, int viewportRow)
{
    if (!fetchViewportRow(viewportRow, m_rowCells.data())) return;

    int selFrom = 0;
    int selTo = 0;
    if (m_hasSelection) {
        const int historySize = m_scrollback.size();
        const int absolute = m_altScreen ? historySize + viewportRow
                                         : historySize - m_scrollOffset + viewportRow;
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

    layoutTerminalRow(m_rowCells.data(), m_cols, m_palette, selFrom, selTo, m_layout);

    const qreal y = rowTop(viewportRow);
    for (const auto &bg : m_layout.backgrounds) {
        p.fillRect(QRectF(columnX(bg.col), y, bg.span * m_cellW, m_cellH), QColor::fromRgb(bg.color));
    }
    for (const auto &special : m_layout.specials) {
        TerminalBoxDrawing::paint(p, QRectF(columnX(special.col), y, special.width * m_cellW, m_cellH),
                                  special.cp, QColor::fromRgb(special.fg), m_lineWidth, m_dpr);
    }
    const qreal baseline = y + m_baseline;
    for (const auto &run : m_layout.runs) {
        m_glyphs.drawRun(p, m_layout.glyphs.data() + run.firstGlyph, run.glyphCount,
                         run.font, run.fg, m_padX, baseline, m_cellW);
    }
    for (const auto &cluster : m_layout.clusters) {
        m_glyphs.drawCluster(p, cluster, m_padX, baseline, m_cellW);
    }
    paintDecorations(p, y);
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

    p.fillRect(r, cursorColor);

    // Redraw the glyph under the block in the cell's background colour so the
    // character stays readable.
    VTermScreenCell cell;
    if (!vterm_screen_get_cell(m_screen, VTermPos{m_cursor.y(), m_cursor.x()}, &cell)) return;
    const uint32_t ch = cell.chars[0];
    if (ch == 0 || ch == U' ' || ch == kWideSpacer || cell.attrs.conceal) return;
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

bool TerminalWidget::event(QEvent *event)
{
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

void TerminalWidget::keyPressEvent(QKeyEvent *event)
{
    if (!m_vt || !m_pty) {
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }

    const int key = event->key();
    const auto mods = event->modifiers();

    if ((mods & Qt::ControlModifier) && (mods & Qt::ShiftModifier)) {
        if (key == Qt::Key_C) {
            copySelection();
            return;
        }
        if (key == Qt::Key_V) {
            pasteClipboard();
            return;
        }
    }

    // Typing returns the view to the live screen, like every other terminal.
    scrollToBottom();

    VTermModifier vmod = VTERM_MOD_NONE;
    if (mods & Qt::ShiftModifier)   vmod = static_cast<VTermModifier>(vmod | VTERM_MOD_SHIFT);
    if (mods & Qt::ControlModifier) vmod = static_cast<VTermModifier>(vmod | VTERM_MOD_CTRL);
    if (mods & Qt::AltModifier)     vmod = static_cast<VTermModifier>(vmod | VTERM_MOD_ALT);

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

    if ((mods & Qt::ControlModifier) && key >= Qt::Key_A && key <= Qt::Key_Z) {
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

void TerminalWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        setFocus(Qt::MouseFocusReason);
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
        m_selStart = pixelToAbsoluteCell(event->pos());
        m_selEnd = m_selStart;
        m_selecting = true;
        if (m_hasSelection) {
            m_hasSelection = false;
            markAllDirty();
            scheduleFrame();
        }
    } else if (event->button() == Qt::MiddleButton) {
        const QClipboard *cb = QApplication::clipboard();
        const QString sel = cb->text(QClipboard::Selection);
        if (!sel.isEmpty()) {
            writeToPty(sel.toUtf8());
        }
    }
    QAbstractScrollArea::mousePressEvent(event);
}

void TerminalWidget::mouseMoveEvent(QMouseEvent *event)
{
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

    if (m_selecting) {
        const QPoint end = pixelToAbsoluteCell(event->pos());
        if (end != m_selEnd || m_hasSelection != (m_selStart != end)) {
            m_selEnd = end;
            m_hasSelection = (m_selStart != m_selEnd);
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

    if (event->button() == Qt::LeftButton && m_selecting) {
        m_selecting = false;
        m_selEnd = pixelToAbsoluteCell(event->pos());
        m_hasSelection = (m_selStart != m_selEnd);
        markAllDirty();
        scheduleFrame();
        if (m_hasSelection) {
            QClipboard *cb = QApplication::clipboard();
            if (cb->supportsSelection()) {
                cb->setText(selectionText(), QClipboard::Selection);
            }
        }
    }
    QAbstractScrollArea::mouseReleaseEvent(event);
}

void TerminalWidget::focusInEvent(QFocusEvent *event)
{
    m_hasFocus = true;
    if (m_focusReporting && m_pty) {
        writeToPty(QByteArray("\x1b[I", 3));
    }
    markLiveRowsDirty(m_cursor.y(), m_cursor.y() + 1);
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
    QAbstractScrollArea::wheelEvent(event);
}

void TerminalWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && !isMouseReportingActive(event->modifiers())) {
        setFocus(Qt::MouseFocusReason);
        selectWord(pixelToAbsoluteCell(event->pos()));
        event->accept();
        return;
    }
    QAbstractScrollArea::mouseDoubleClickEvent(event);
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

QString TerminalWidget::selectionText() const
{
    if (!m_screen || !m_hasSelection) return QString();
    WidgetCellSource src(m_scrollback, m_screen, m_rows, m_cols);
    return extractSelectionText(src, m_selStart, m_selEnd);
}

void TerminalWidget::copySelection()
{
    if (!m_hasSelection) return;
    QApplication::clipboard()->setText(selectionText());
}

void TerminalWidget::pasteClipboard()
{
    const QString text = QApplication::clipboard()->text();
    if (text.isEmpty() || !m_vt) return;
    scrollToBottom();
    vterm_keyboard_start_paste(m_vt);
    for (uint cp : text.toUcs4()) {
        vterm_keyboard_unichar(m_vt, static_cast<uint32_t>(cp), VTERM_MOD_NONE);
    }
    vterm_keyboard_end_paste(m_vt);
}

QString TerminalWidget::selectedText() const
{
    return selectionText();
}

void TerminalWidget::contextMenuEvent(QContextMenuEvent *event)
{
    if (isMouseReportingActive(QGuiApplication::keyboardModifiers())) {
        event->ignore();
        return;
    }

    QMenu menu(this);

    QAction *copyAct = menu.addAction(tr("Copy"));
    copyAct->setEnabled(m_hasSelection);
    connect(copyAct, &QAction::triggered, this, [this]() { copySelection(); });

    QAction *pasteAct = menu.addAction(tr("Paste"));
    connect(pasteAct, &QAction::triggered, this, [this]() { pasteClipboard(); });

    emit contextMenuAboutToShow(&menu);

    menu.exec(event->globalPos());
}

void TerminalWidget::selectWord(const QPoint &absCell)
{
    if (!m_screen) return;
    WidgetCellSource src(m_scrollback, m_screen, m_rows, m_cols);
    const int row = absCell.y();
    const int cols = src.cols();
    if (row < 0 || row >= src.rowCount()) return;

    static const auto isWordChar = [](uint32_t ch) {
        if (ch == 0 || ch == ' ' || ch == '\t') return false;
        static constexpr char32_t delims[] = U"!@#$%^&*(){}[]|;:'\",.<>?/`~-=+\\";
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
        if (cb->supportsSelection()) {
            cb->setText(selectionText(), QClipboard::Selection);
        }
    }
}
