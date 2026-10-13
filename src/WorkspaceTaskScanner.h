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
    // just recipes hidden from `just --list`: `_name` or a `[private]`
    // attribute. The empty panel skips them; the gutter still marks them.
    bool isPrivate = false;
    int line = -1; // 0-based line in the source file (gutter markers), -1 if unknown

    bool operator==(const WorkspaceTask &) const = default;
};

// All tasks from one source file, in declaration order.
struct WorkspaceTaskGroup
{
    QString source; // display label: "Justfile" | "Makefile" | "package.json" | "deno.json"
    QList<WorkspaceTask> tasks;

    bool operator==(const WorkspaceTaskGroup &) const = default;
};

// Parsers for the four task-file kinds plus the workspace-root scan the
// terminal empty panel uses. Pure: file contents (or a directory) in, tasks
// out — no UI, no editor, no PTY. TaskRunnerGutter shares these parsers, so
// the gutter and the panel always agree on names/commands/required-params.
// (Package-manager detection differs by scope on purpose: the gutter walks
// up from the open file with a cache; the scanner only sniffs the root.)
class WorkspaceTaskScanner
{
public:
    // Root-level discovery only (no recursion): the first existing Justfile
    // variant, the first existing Makefile variant, package.json, deno.json.
    // Groups come back in that order and only when they hold at least one task.
    static QList<WorkspaceTaskGroup> scanRoot(const QString &workspaceRoot);

    static QList<WorkspaceTask> parsePackageJson(const QByteArray &text,
                                                 const QString &packageManager);
    static QList<WorkspaceTask> parseJustfile(const QByteArray &text);
    static QList<WorkspaceTask> parseMakefile(const QByteArray &text);
    static QList<WorkspaceTask> parseDenoJson(const QByteArray &text);

    // Lockfile sniff in `dir` itself (no upward walk — the scanner only ever
    // asks about a workspace root): yarn.lock → yarn, pnpm-lock.yaml → pnpm,
    // bun.lockb / bun.lock → bun, otherwise npm.
    static QString detectPackageManager(const QString &dir);
};
