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

#include "TerminalScrollBar.h"

#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace {
constexpr qreal kMinThumb = 24;
} // namespace

TerminalScrollBar::TerminalScrollBar(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_NoSystemBackground, true);
    setMouseTracking(true);
    setCursor(Qt::ArrowCursor);
    setFixedWidth(kWidth);
}

void TerminalScrollBar::setState(int history, int page, int offset)
{
    history = std::max(0, history);
    page = std::max(1, page);
    offset = std::clamp(offset, 0, history);
    if (history == m_history && page == m_page && offset == m_offset) return;
    m_history = history;
    m_page = page;
    m_offset = offset;
    update();
}

void TerminalScrollBar::setThumbColor(QRgb color)
{
    if (color == m_thumb) return;
    m_thumb = color;
    update();
}

void TerminalScrollBar::setMarks(std::vector<int> rows, int active, QRgb color, QRgb activeColor)
{
    m_marks = std::move(rows);
    m_activeMark = active;
    m_markColor = color;
    m_activeMarkColor = activeColor;
    update();
}

QRectF TerminalScrollBar::thumbRect() const
{
    const qreal h = height();
    const qreal total = m_history + m_page;
    const qreal thumbH = std::min(h, std::max(kMinThumb, h * m_page / total));
    const qreal travel = h - thumbH;
    const qreal top = m_history > 0 ? travel * (m_history - m_offset) / m_history : 0;
    return QRectF(0, top, width(), thumbH);
}

int TerminalScrollBar::offsetForThumbTop(qreal top) const
{
    const QRectF t = thumbRect();
    const qreal travel = height() - t.height();
    if (travel <= 0 || m_history <= 0) return 0;
    const qreal frac = std::clamp(top / travel, 0.0, 1.0);
    return m_history - static_cast<int>(std::lround(frac * m_history));
}

void TerminalScrollBar::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Match ticks span the full track so they line up with content position.
    if (!m_marks.empty() && m_history + m_page > 0) {
        const qreal scale = static_cast<qreal>(height()) / (m_history + m_page);
        const qreal tick = std::max<qreal>(2.0, scale);
        QColor mark = QColor::fromRgba(m_markColor);
        for (std::size_t i = 0; i < m_marks.size(); ++i) {
            if (static_cast<int>(i) == m_activeMark) continue;
            p.fillRect(QRectF(0, m_marks[i] * scale, width(), tick), mark);
        }
        if (m_activeMark >= 0 && m_activeMark < static_cast<int>(m_marks.size())) {
            p.fillRect(QRectF(0, m_marks[static_cast<std::size_t>(m_activeMark)] * scale, width(), tick),
                       QColor::fromRgba(m_activeMarkColor));
        }
    }

    QColor thumb = QColor::fromRgb(m_thumb);
    thumb.setAlpha(m_dragging ? 150 : (m_hover ? 110 : 60));
    const QRectF t = thumbRect();
    const qreal w = m_hover || m_dragging ? width() - 2.0 : width() - 4.0;
    const QRectF r(width() - w - 1.0, t.top() + 1.0, w, t.height() - 2.0);
    p.setPen(Qt::NoPen);
    p.setBrush(thumb);
    p.drawRoundedRect(r, w / 2, w / 2);
}

void TerminalScrollBar::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) return;
    const QRectF t = thumbRect();
    const qreal y = event->position().y();
    if (y >= t.top() && y <= t.bottom()) {
        m_dragging = true;
        m_dragGrab = y - t.top();
    } else {
        // Page toward the click, like a classic scrollbar track.
        emit offsetRequested(y < t.top() ? m_offset + m_page : std::max(0, m_offset - m_page));
    }
    update();
    event->accept();
}

void TerminalScrollBar::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging) {
        emit offsetRequested(offsetForThumbTop(event->position().y() - m_dragGrab));
        event->accept();
    }
}

void TerminalScrollBar::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        update();
    }
    event->accept();
}

void TerminalScrollBar::enterEvent(QEnterEvent *event)
{
    m_hover = true;
    update();
    QWidget::enterEvent(event);
}

void TerminalScrollBar::leaveEvent(QEvent *event)
{
    m_hover = false;
    update();
    QWidget::leaveEvent(event);
}
