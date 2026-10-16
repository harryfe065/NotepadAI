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

#ifndef AI_DOCK_TITLE_BAR_H
#define AI_DOCK_TITLE_BAR_H

#include <QIcon>
#include <QPoint>
#include <QString>
#include <QWidget>

class QDockWidget;
class QLabel;
class QToolButton;

// Title bar of an AI project dock: the dock title, the open session's agent
// button (Switch agent), then float and close like Qt's own title bar. A pure
// view: the dock pushes the agent and shows the menu on agentMenuRequested.
// Mouse presses it does not use fall through to the dock (drag to move,
// double-click to float); a middle click closes the dock like the native bar.
class AiDockTitleBar : public QWidget
{
    Q_OBJECT

public:
    static constexpr char kAgentButtonName[] = "nn_aiAgentButton";

    struct AgentButton
    {
        QString name;        // configured agent name; empty hides the button
        QIcon icon;
        QString tooltip;
        QString switchingTo; // Switch agent in progress: reads "→ name…"
    };

    explicit AiDockTitleBar(QDockWidget *dock);

    void setAgentButton(const AgentButton &agent);
    QString agentButtonText() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void agentMenuRequested(const QPoint &globalPos);

protected:
    void changeEvent(QEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    void restyleAgentButton();
    void refreshDockButtons();

    QDockWidget *m_dock = nullptr;
    QLabel *m_title = nullptr;
    QToolButton *m_agentButton = nullptr;
    QToolButton *m_floatButton = nullptr;
    QToolButton *m_closeButton = nullptr;
    AgentButton m_agent;
};

#endif // AI_DOCK_TITLE_BAR_H
