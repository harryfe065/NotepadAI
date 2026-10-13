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

#include <QList>
#include <QString>

// One runnable entry parsed out of a task file (Justfile recipe, Makefile
// target, package.json script, deno.json task).
struct WorkspaceTask
{
    QString name;
    QString command; // full command line, e.g. "npm run dev"
    bool hasRequiredParams = false; // just recipes with a param lacking a default
    int line = -1; // 0-based line in the source file (gutter markers), -1 if unknown
};

// Parsers for the four task-file kinds behind TaskRunnerGutter's run icons.
// Pure: file contents in, tasks out — no UI, no editor, no file I/O.
class WorkspaceTaskScanner
{
public:
    static QList<WorkspaceTask> parsePackageJson(const QByteArray &text,
                                                 const QString &packageManager);
    static QList<WorkspaceTask> parseJustfile(const QByteArray &text);
    static QList<WorkspaceTask> parseMakefile(const QByteArray &text);
    static QList<WorkspaceTask> parseDenoJson(const QByteArray &text);
};
