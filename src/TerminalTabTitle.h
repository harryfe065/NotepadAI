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

#ifndef TERMINALTABTITLE_H
#define TERMINALTABTITLE_H

#include <QDir>
#include <QFileInfo>
#include <QString>

// A terminal cwd as shown to the user (tooltip, and the base of
// terminalCwdLabel): native separators, drive roots keep their slash.
inline QString terminalTabTitle(const QString &cwd)
{
    if (cwd.trimmed().isEmpty())
        return QStringLiteral("Terminal");

    QString native = QDir::toNativeSeparators(QDir::cleanPath(cwd));
#ifdef Q_OS_WIN
    // cleanPath("d:/") collapses to "d:"; keep the root slash the user expects.
    if (native.size() == 2 && native.at(1) == QLatin1Char(':'))
        return native + QLatin1Char('\\');
#endif
    return native;
}

// Short cwd label for a tab: the last path component ("proj"), or the root
// itself (`D:\`, `/`).
inline QString terminalCwdLabel(const QString &cwd)
{
    const QString native = terminalTabTitle(cwd);
    if (cwd.trimmed().isEmpty())
        return native;
    const QString name = QFileInfo(QDir::cleanPath(cwd)).fileName();
    return name.isEmpty() ? native : name;
}

// Shells (and Windows ConPTY) often set the window title to the shell image,
// e.g. "C:\Program Files\PowerShell\7\pwsh.exe". That says nothing the tab
// icon does not; such titles are ignored (Zed does the same).
inline bool isShellImageTitle(const QString &title, const QString &shellPath)
{
    QString t = title.trimmed();
    if (t.isEmpty())
        return true;
    const QString elevated = QStringLiteral("Administrator: ");
    if (t.startsWith(elevated, Qt::CaseInsensitive))
        t = t.mid(elevated.size()).trimmed();
    if (t.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive))
        return true;
    if (shellPath.isEmpty())
        return false;
    const QFileInfo shell(shellPath);
    const QString name = QFileInfo(t).fileName();
    return t.compare(shellPath, Qt::CaseInsensitive) == 0
        || name.compare(shell.fileName(), Qt::CaseInsensitive) == 0
        || t.compare(shell.completeBaseName(), Qt::CaseInsensitive) == 0;
}

inline QString elideTerminalTitle(const QString &title, int maxChars = 25)
{
    if (title.size() <= maxChars)
        return title;
    return title.left(maxChars - 1) + QChar(0x2026);
}

// Tab text: a user-chosen name, else the program's title (OSC 0/2) unless it
// is just the shell image, else the cwd folder name.
inline QString terminalDisplayTitle(const QString &customName, const QString &programTitle,
                                    const QString &shellPath, const QString &cwd)
{
    if (!customName.trimmed().isEmpty())
        return elideTerminalTitle(customName.trimmed());
    if (!isShellImageTitle(programTitle, shellPath))
        return elideTerminalTitle(programTitle.trimmed());
    return elideTerminalTitle(terminalCwdLabel(cwd));
}

#endif
