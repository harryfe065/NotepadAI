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

#ifndef TERMINALSCROLLBAR_H
#define TERMINALSCROLLBAR_H

#include <QRgb>
#include <QWidget>

#include <vector>

// Thin overlay scrollbar for the terminal. It sits in the right padding so it
// never changes the grid width (a classic scrollbar appearing would resize the
// terminal and send SIGWINCH), and can show search-match ticks.
//
// Positions are in terminal terms: `history` lines above the live screen,
// `page` visible rows, `offset` lines scrolled up from the bottom.
class TerminalScrollBar : public QWidget
{
    Q_OBJECT

public:
    static constexpr int kWidth = 8;

    explicit TerminalScrollBar(QWidget *parent = nullptr);

    void setState(int history, int page, int offset);
    void setThumbColor(QRgb color);
    // Match positions as absolute rows (0 = oldest history line); `active` is
    // an index into `rows` or -1.
    void setMarks(std::vector<int> rows, int active, QRgb color, QRgb activeColor);

signals:
    void offsetRequested(int offset);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QRectF thumbRect() const;
    int offsetForThumbTop(qreal top) const;

    int m_history = 0;
    int m_page = 1;
    int m_offset = 0;
    QRgb m_thumb = 0xFF808080;
    std::vector<int> m_marks;
    int m_activeMark = -1;
    QRgb m_markColor = 0;
    QRgb m_activeMarkColor = 0;
    bool m_hover = false;
    bool m_dragging = false;
    qreal m_dragGrab = 0;
};

#endif
