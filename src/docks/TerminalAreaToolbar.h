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

#ifndef TERMINALAREATOOLBAR_H
#define TERMINALAREATOOLBAR_H

#include <QColor>
#include <QIcon>
#include <QPointer>
#include <QWidget>

class QLabel;
class QToolButton;
class TerminalDock;
namespace ads { class CDockAreaWidget; }

// SVG with stroke="currentColor" rendered in `color` at every device pixel
// ratio Qt asks for; `dotColor` (if valid) adds a small badge dot top-right.
QIcon terminalTintedIcon(const QString &svgPath, const QColor &color, const QColor &dotColor = QColor());

// Lives in an ADS dock area's title bar (Zed-style tab-bar controls). Shows
// the CPU/RAM of the area's current terminal and its actions, and hides itself
// whenever the current tab is not a terminal. One per area, created lazily.
class TerminalAreaToolbar : public QWidget
{
    Q_OBJECT

public:
    explicit TerminalAreaToolbar(ads::CDockAreaWidget *area);

    // The toolbar already installed in `area`, if any.
    static TerminalAreaToolbar *find(ads::CDockAreaWidget *area);

    ads::CDockAreaWidget *area() const { return m_area.data(); }
    TerminalDock *dock() const { return m_dock.data(); }
    bool cpuLabelUnderMouse() const;
    // Re-read the current tab (after a tab moved in or out).
    void syncToCurrent();
    void setMaximized(bool maximized);

signals:
    void findRequested(TerminalDock *dock);
    void clearRequested(TerminalDock *dock);
    void newTerminalRequested(TerminalDock *dock);
    void restartRequested(TerminalDock *dock);
    void maximizeRequested(TerminalAreaToolbar *toolbar);
    void cpuHoverChanged();
    void currentDockChanged(TerminalDock *dock);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    QToolButton *makeButton(const QString &toolTip);
    void updateStats();
    void retint();

    QPointer<ads::CDockAreaWidget> m_area;
    QPointer<TerminalDock> m_dock;
    QMetaObject::Connection m_statsConnection;
    QMetaObject::Connection m_presentationConnection;
    QLabel *m_stats = nullptr;
    QToolButton *m_find = nullptr;
    QToolButton *m_clear = nullptr;
    QToolButton *m_new = nullptr;
    QToolButton *m_restart = nullptr;
    QToolButton *m_maximize = nullptr;
    bool m_maximized = false;
    QPoint m_coreTipAnchor;
};

#endif
