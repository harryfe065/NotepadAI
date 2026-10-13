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

#include <QtTest>

#include "WorkspaceTaskScanner.h"

// Seam: the WorkspaceTaskScanner parsers behind TaskRunnerGutter's run
// icons. Pure: file contents in, tasks out; no UI, no file I/O.
class TestWorkspaceTaskScanner : public QObject
{
    Q_OBJECT

private slots:
    void packageJson_scriptsInDeclarationOrder_npmRun();
    void packageJson_yarn_yarnCommand();
    void packageJson_invalidJson_noTasks();
    void packageJson_valueEqualToKey_placedByKey();
    void packageJson_blockKeyInNestedObject_ignored();
    void justfile_recipesAndRequiredParams();
    void justfile_recipeWithDefaultParam_notRequired();
    void justfile_variadicParams();
    void makefile_targets_skipDotAndAssignments();
    void denoJson_tasksInDeclarationOrder();
};

void TestWorkspaceTaskScanner::packageJson_scriptsInDeclarationOrder_npmRun()
{
    const QList<WorkspaceTask> tasks = WorkspaceTaskScanner::parsePackageJson(
        "{\"scripts\":{\"dev\":\"vite\",\"build\":\"tsc\",\"test\":\"vitest\"}}",
        QStringLiteral("npm"));
    QCOMPARE(tasks.size(), 3);
    // Declaration order, not the alphabetical order QJsonObject::keys() gives.
    QCOMPARE(tasks.at(0).name, QStringLiteral("dev"));
    QCOMPARE(tasks.at(0).command, QStringLiteral("npm run dev"));
    QCOMPARE(tasks.at(1).command, QStringLiteral("npm run build"));
    QCOMPARE(tasks.at(2).command, QStringLiteral("npm run test"));
    QCOMPARE(tasks.at(0).hasRequiredParams, false);
}

void TestWorkspaceTaskScanner::packageJson_yarn_yarnCommand()
{
    const QList<WorkspaceTask> tasks = WorkspaceTaskScanner::parsePackageJson(
        "{\"scripts\":{\"dev\":\"vite\"}}", QStringLiteral("yarn"));
    QCOMPARE(tasks.size(), 1);
    QCOMPARE(tasks.at(0).command, QStringLiteral("yarn dev"));
}

void TestWorkspaceTaskScanner::packageJson_invalidJson_noTasks()
{
    QCOMPARE(WorkspaceTaskScanner::parsePackageJson("{not json", QStringLiteral("npm")).size(), 0);
}

void TestWorkspaceTaskScanner::packageJson_valueEqualToKey_placedByKey()
{
    // "lint"'s value is the text "test": the key "test" must be found as a
    // key on its own line, not inside that value.
    const QList<WorkspaceTask> tasks = WorkspaceTaskScanner::parsePackageJson(
        "{\n  \"scripts\": {\n    \"lint\": \"test\",\n    \"test\": \"vitest\"\n  }\n}\n",
        QStringLiteral("npm"));
    QCOMPARE(tasks.size(), 2);
    QCOMPARE(tasks.at(0).name, QStringLiteral("lint"));
    QCOMPARE(tasks.at(0).line, 2);
    QCOMPARE(tasks.at(1).name, QStringLiteral("test"));
    QCOMPARE(tasks.at(1).line, 3);
}

void TestWorkspaceTaskScanner::packageJson_blockKeyInNestedObject_ignored()
{
    const QList<WorkspaceTask> tasks = WorkspaceTaskScanner::parsePackageJson(
        "{\n  \"config\": {\"scripts\": {\"b\": 1}},\n  \"scripts\": {\n    \"b\": \"y\"\n  }\n}\n",
        QStringLiteral("npm"));
    QCOMPARE(tasks.size(), 1);
    QCOMPARE(tasks.at(0).name, QStringLiteral("b"));
    QCOMPARE(tasks.at(0).line, 3);
}

void TestWorkspaceTaskScanner::justfile_recipesAndRequiredParams()
{
    const QList<WorkspaceTask> tasks = WorkspaceTaskScanner::parseJustfile(
        "build:\n    cargo build\n\nserve port:\n    ./serve {{port}}\n");
    QCOMPARE(tasks.size(), 2);
    QCOMPARE(tasks.at(0).name, QStringLiteral("build"));
    QCOMPARE(tasks.at(0).command, QStringLiteral("just build"));
    QCOMPARE(tasks.at(0).line, 0);
    QCOMPARE(tasks.at(0).hasRequiredParams, false);
    QCOMPARE(tasks.at(1).name, QStringLiteral("serve"));
    QCOMPARE(tasks.at(1).command, QStringLiteral("just serve"));
    QCOMPARE(tasks.at(1).line, 3);
    QCOMPARE(tasks.at(1).hasRequiredParams, true);
}

void TestWorkspaceTaskScanner::justfile_recipeWithDefaultParam_notRequired()
{
    const QList<WorkspaceTask> tasks =
        WorkspaceTaskScanner::parseJustfile("serve port=\"8080\":\n    ./serve\n");
    QCOMPARE(tasks.size(), 1);
    QCOMPARE(tasks.at(0).hasRequiredParams, false);
}

void TestWorkspaceTaskScanner::justfile_variadicParams()
{
    const QList<WorkspaceTask> tasks = WorkspaceTaskScanner::parseJustfile(
        "star *args:\n    echo\n"
        "plus +args:\n    echo\n"
        "exported $x:\n    echo\n"
        "mixed a=\"1\" *rest:\n    echo\n"
        "starExported *$rest:\n    echo\n");
    QCOMPARE(tasks.size(), 5);
    QCOMPARE(tasks.at(0).hasRequiredParams, false);
    QCOMPARE(tasks.at(1).hasRequiredParams, true);
    QCOMPARE(tasks.at(2).hasRequiredParams, true);
    QCOMPARE(tasks.at(3).hasRequiredParams, false);
    QCOMPARE(tasks.at(4).hasRequiredParams, false);
}

void TestWorkspaceTaskScanner::makefile_targets_skipDotAndAssignments()
{
    const QList<WorkspaceTask> tasks = WorkspaceTaskScanner::parseMakefile(
        "CC := gcc\n.PHONY: test\nbuild: main.o\n\t$(CC) -o app main.o\ntest:\n\t./test\n");
    QCOMPARE(tasks.size(), 2);
    QCOMPARE(tasks.at(0).name, QStringLiteral("build"));
    QCOMPARE(tasks.at(0).command, QStringLiteral("make build"));
    QCOMPARE(tasks.at(0).line, 2);
    QCOMPARE(tasks.at(1).command, QStringLiteral("make test"));
}

void TestWorkspaceTaskScanner::denoJson_tasksInDeclarationOrder()
{
    const QList<WorkspaceTask> tasks = WorkspaceTaskScanner::parseDenoJson(
        "{\"tasks\":{\"start\":\"deno run main.ts\",\"lint\":\"deno lint\"}}");
    QCOMPARE(tasks.size(), 2);
    QCOMPARE(tasks.at(0).command, QStringLiteral("deno task start"));
    QCOMPARE(tasks.at(1).command, QStringLiteral("deno task lint"));
}

QTEST_APPLESS_MAIN(TestWorkspaceTaskScanner)
#include "test_workspace_task_scanner.moc"
