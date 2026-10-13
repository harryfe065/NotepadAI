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

#ifndef TERMINALEMPTYPANEL_H
#define TERMINALEMPTYPANEL_H

#include <QColor>
#include <QList>
#include <QWidget>

#include "TerminalTaskRegistry.h"
#include "WorkspaceTaskScanner.h"

class QLabel;
class QScrollArea;
class QVBoxLayout;

// Empty state for the bottom terminal area: shown in place of terminal tabs
// when the active workspace has no visible terminal. Offers one-click
// terminal creation and the workspace's tasks (configured first, then the
// ones discovered in root task files). Pure presentation — TerminalManager
// owns placement, suppression and execution.
class TerminalEmptyPanel : public QWidget
{
    Q_OBJECT

public:
    explicit TerminalEmptyPanel(QWidget *parent = nullptr);

    // Rebuilds the whole content for a workspace. `remote` hides every task
    // affordance (tasks are local-only); the New Terminal button stays.
    void setContent(const QString &workspaceDisplayPath, bool remote,
                    const QList<TerminalTask> &configuredTasks,
                    const QList<WorkspaceTaskGroup> &discoveredGroups);
    // Replaces only the discovered task groups (a worker's rescan landing);
    // no-op when they did not change. Private just recipes are not listed.
    void setDiscoveredGroups(QList<WorkspaceTaskGroup> groups);
    // Terminal colour scheme colours (background/foreground/selection), so
    // the panel reads as the terminal surface it reserves space for.
    void setSchemeColors(const QColor &background, const QColor &foreground,
                         const QColor &selection);

signals:
    void newTerminalRequested();
    void configuredTaskRequested(const TerminalTask &task);
    void discoveredTaskRequested(const WorkspaceTask &task);
    void editTasksRequested();

private:
    void rebuild();
    // One style sheet on the panel for every row and button (selected by
    // object name / property), so a rebuild never parses per-widget sheets.
    void applyStyleSheet();
    QWidget *makeTaskRow(const QString &name, const QString &command, const QString &source);
    void clearContent();

    QScrollArea *m_scroll = nullptr;
    QWidget *m_content = nullptr;
    QVBoxLayout *m_contentLayout = nullptr;

    QString m_workspaceDisplayPath;
    bool m_remote = false;
    QList<TerminalTask> m_configuredTasks;
    QList<WorkspaceTaskGroup> m_discoveredGroups;

    QColor m_background;
    QColor m_foreground;
    QColor m_selection;
};

#endif
