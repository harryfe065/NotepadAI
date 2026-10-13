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

#ifndef TERMINALTASKLAUNCHER_H
#define TERMINALTASKLAUNCHER_H

#include <QString>
#include <QStringList>

#include <cstdint>

// Private OSC a wrapped task prints with its exit code: ESC ] 6973 ; <code> BEL.
constexpr int kTerminalTaskExitOsc = 6973;

// How to start a local task terminal so the tab can show the task's result.
// The command is handed to the shell as an argument (never typed, so it is
// not echoed), followed by a line that reports the exit code through the
// private OSC and prints a faint "finished" line; the shell then stays
// interactive (PowerShell -NoExit, cmd /k, `exec $SHELL -i`).
struct TerminalTaskLaunch
{
    // Arguments for IPtyProcess::setArguments. Empty: unknown shell, so the
    // caller types the command into a plain interactive shell instead.
    QStringList arguments;
    bool reportsExit = false;
};

enum class TerminalShellKind : uint8_t { PowerShell, Cmd, Posix, Fish, Other };

TerminalShellKind terminalShellKind(const QString &shellPath);

// `finishedText` contains %1 for the exit code, e.g. "Task finished (exit code %1)".
// `windowsCommandLine` selects how arguments are quoted (Windows appends them
// verbatim to one command line; POSIX passes argv).
TerminalTaskLaunch terminalTaskLaunch(const QString &shellPath, const QString &command,
                                      const QString &finishedText, bool windowsCommandLine);

// CommandLineToArgvW-compatible quoting of one argument.
QString quoteWindowsArgument(const QString &arg);

// Text about to be pasted into a terminal with everything that could act as
// a control sequence removed: ESC (so a paste cannot end bracketed paste and
// run the rest), other C0 controls except tab, and C1 controls. Line breaks
// become CR, as a typed Enter would be.
QString sanitizeTerminalPaste(const QString &text);

// A path typed into an interactive shell of `kind` as one word: PowerShell
// single quotes, cmd double quotes, POSIX/fish single quotes. Plain paths
// (no spaces or shell metacharacters) stay bare.
QString quotePathForShell(const QString &path, TerminalShellKind kind);

#endif
