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

#include "TerminalEmptyPanelPolicy.h"

#include "remote/SshProfile.h"

#include <QDir>

QString TerminalEmptyPanelPolicy::workspaceKey(const QString &workspaceRoot)
{
    if (workspaceRoot.isEmpty())
        return {};
    if (remote::isSshUri(workspaceRoot)) {
        QString key = workspaceRoot;
        while (key.endsWith(QLatin1Char('/')))
            key.chop(1);
        return key;
    }
    QString key = QDir::cleanPath(workspaceRoot);
#ifdef Q_OS_WIN
    key = key.toLower();
#endif
    return key;
}
