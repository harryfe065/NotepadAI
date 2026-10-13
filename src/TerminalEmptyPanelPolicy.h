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

#pragma once

#include <QString>

// The show/hide decision for the terminal area's empty panel, isolated as a
// pure function so the policy (setting, per-workspace suppression, terminal
// visibility) is testable without a MainWindow.
class TerminalEmptyPanelPolicy
{
public:
    struct Input
    {
        bool settingEnabled = false;
        bool hasActiveWorkspace = false;
        // The user closed the panel for this workspace and the workspace has
        // not been closed since (suppression lives for the workspace's
        // lifetime in this session only).
        bool suppressedForActiveWorkspace = false;
        bool anyTerminalVisible = false;
        bool shuttingDown = false;
    };

    static bool shouldShow(const Input &in)
    {
        return in.settingEnabled && in.hasActiveWorkspace
               && !in.suppressedForActiveWorkspace && !in.anyTerminalVisible
               && !in.shuttingDown;
    }

    // Stable per-workspace key for the suppression set. Local paths mirror
    // TerminalTaskRegistry::normalizeWorkspacePath (clean path, lower-cased
    // on Windows). SSH roots keep their ssh:// URI verbatim (only the
    // trailing slash goes) — remote paths are case-sensitive and two profiles
    // may serve the same path.
    static QString workspaceKey(const QString &workspaceRoot);
};
