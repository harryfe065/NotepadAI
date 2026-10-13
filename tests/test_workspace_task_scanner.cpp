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
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "WorkspaceTaskScanner.h"

// Seam: WorkspaceTaskScanner::scanRoot() — the root-level task discovery the
// terminal empty panel lists next to the configured workspace tasks. Pure
// filesystem in, task groups out; no UI, no PTY.
class TestWorkspaceTaskScanner : public QObject
{
    Q_OBJECT

private slots:
    void emptyDir_noGroups();
    void packageJson_scriptsInDeclarationOrder_npmRun();
    void packageJson_yarnLock_yarnCommand();
    void packageJson_invalidJson_noGroup();
    void justfile_recipesAndRequiredParams();
    void justfile_recipeWithDefaultParam_notRequired();
    void makefile_targets_skipDotAndAssignments();
    void denoJson_tasksInDeclarationOrder();
    void allFiles_groupsInJustMakePackageDenoOrder();
    void subdirFiles_ignored();

private:
    void writeFile(const QString &name, const QByteArray &content);

    QTemporaryDir tmp;
};

void TestWorkspaceTaskScanner::writeFile(const QString &name, const QByteArray &content)
{
    QFile f(QDir(tmp.path()).filePath(name));
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(content);
    f.close();
}

void TestWorkspaceTaskScanner::emptyDir_noGroups()
{
    QVERIFY(tmp.isValid());
    QCOMPARE(WorkspaceTaskScanner::scanRoot(tmp.path()).size(), 0);
}

void TestWorkspaceTaskScanner::packageJson_scriptsInDeclarationOrder_npmRun()
{
    writeFile(QStringLiteral("package.json"),
              "{\"scripts\":{\"dev\":\"vite\",\"build\":\"tsc\",\"test\":\"vitest\"}}");

    const auto groups = WorkspaceTaskScanner::scanRoot(tmp.path());
    QCOMPARE(groups.size(), 1);
    QCOMPARE(groups.at(0).source, QStringLiteral("package.json"));
    QCOMPARE(groups.at(0).tasks.size(), 3);
    // Declaration order, not the alphabetical order QJsonObject::keys() gives.
    QCOMPARE(groups.at(0).tasks.at(0).name, QStringLiteral("dev"));
    QCOMPARE(groups.at(0).tasks.at(0).command, QStringLiteral("npm run dev"));
    QCOMPARE(groups.at(0).tasks.at(1).command, QStringLiteral("npm run build"));
    QCOMPARE(groups.at(0).tasks.at(2).command, QStringLiteral("npm run test"));
    QCOMPARE(groups.at(0).tasks.at(0).hasRequiredParams, false);
}

void TestWorkspaceTaskScanner::packageJson_yarnLock_yarnCommand()
{
    writeFile(QStringLiteral("package.json"), "{\"scripts\":{\"dev\":\"vite\"}}");
    writeFile(QStringLiteral("yarn.lock"), "# yarn lockfile v1\n");

    const auto groups = WorkspaceTaskScanner::scanRoot(tmp.path());
    QCOMPARE(groups.size(), 1);
    QCOMPARE(groups.at(0).tasks.at(0).command, QStringLiteral("yarn dev"));
}

void TestWorkspaceTaskScanner::packageJson_invalidJson_noGroup()
{
    writeFile(QStringLiteral("package.json"), "{not json");

    QCOMPARE(WorkspaceTaskScanner::scanRoot(tmp.path()).size(), 0);
}

void TestWorkspaceTaskScanner::justfile_recipesAndRequiredParams()
{
    writeFile(QStringLiteral("justfile"),
              "build:\n    cargo build\n\nserve port:\n    ./serve {{port}}\n");

    const auto groups = WorkspaceTaskScanner::scanRoot(tmp.path());
    QCOMPARE(groups.size(), 1);
    QCOMPARE(groups.at(0).source, QStringLiteral("Justfile"));
    QCOMPARE(groups.at(0).tasks.size(), 2);
    QCOMPARE(groups.at(0).tasks.at(0).name, QStringLiteral("build"));
    QCOMPARE(groups.at(0).tasks.at(0).command, QStringLiteral("just build"));
    QCOMPARE(groups.at(0).tasks.at(0).hasRequiredParams, false);
    QCOMPARE(groups.at(0).tasks.at(1).name, QStringLiteral("serve"));
    QCOMPARE(groups.at(0).tasks.at(1).command, QStringLiteral("just serve"));
    QCOMPARE(groups.at(0).tasks.at(1).hasRequiredParams, true);
}

void TestWorkspaceTaskScanner::justfile_recipeWithDefaultParam_notRequired()
{
    writeFile(QStringLiteral("justfile"), "serve port=\"8080\":\n    ./serve\n");

    const auto groups = WorkspaceTaskScanner::scanRoot(tmp.path());
    QCOMPARE(groups.size(), 1);
    QCOMPARE(groups.at(0).tasks.at(0).hasRequiredParams, false);
}

void TestWorkspaceTaskScanner::makefile_targets_skipDotAndAssignments()
{
    writeFile(QStringLiteral("Makefile"),
              "CC := gcc\n.PHONY: test\nbuild: main.o\n\t$(CC) -o app main.o\ntest:\n\t./test\n");

    const auto groups = WorkspaceTaskScanner::scanRoot(tmp.path());
    QCOMPARE(groups.size(), 1);
    QCOMPARE(groups.at(0).source, QStringLiteral("Makefile"));
    QCOMPARE(groups.at(0).tasks.size(), 2);
    QCOMPARE(groups.at(0).tasks.at(0).name, QStringLiteral("build"));
    QCOMPARE(groups.at(0).tasks.at(0).command, QStringLiteral("make build"));
    QCOMPARE(groups.at(0).tasks.at(1).command, QStringLiteral("make test"));
}

void TestWorkspaceTaskScanner::denoJson_tasksInDeclarationOrder()
{
    writeFile(QStringLiteral("deno.json"),
              "{\"tasks\":{\"start\":\"deno run main.ts\",\"lint\":\"deno lint\"}}");

    const auto groups = WorkspaceTaskScanner::scanRoot(tmp.path());
    QCOMPARE(groups.size(), 1);
    QCOMPARE(groups.at(0).source, QStringLiteral("deno.json"));
    QCOMPARE(groups.at(0).tasks.size(), 2);
    QCOMPARE(groups.at(0).tasks.at(0).command, QStringLiteral("deno task start"));
    QCOMPARE(groups.at(0).tasks.at(1).command, QStringLiteral("deno task lint"));
}

void TestWorkspaceTaskScanner::allFiles_groupsInJustMakePackageDenoOrder()
{
    writeFile(QStringLiteral("deno.json"), "{\"tasks\":{\"d\":\"x\"}}");
    writeFile(QStringLiteral("package.json"), "{\"scripts\":{\"p\":\"x\"}}");
    writeFile(QStringLiteral("Makefile"), "m:\n\techo m\n");
    writeFile(QStringLiteral("justfile"), "j:\n    echo j\n");

    const auto groups = WorkspaceTaskScanner::scanRoot(tmp.path());
    QCOMPARE(groups.size(), 4);
    QCOMPARE(groups.at(0).source, QStringLiteral("Justfile"));
    QCOMPARE(groups.at(1).source, QStringLiteral("Makefile"));
    QCOMPARE(groups.at(2).source, QStringLiteral("package.json"));
    QCOMPARE(groups.at(3).source, QStringLiteral("deno.json"));
}

void TestWorkspaceTaskScanner::subdirFiles_ignored()
{
    QVERIFY(QDir(tmp.path()).mkpath(QStringLiteral("sub")));
    QFile f(QDir(tmp.path()).filePath(QStringLiteral("sub/package.json")));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("{\"scripts\":{\"dev\":\"vite\"}}");
    f.close();

    QCOMPARE(WorkspaceTaskScanner::scanRoot(tmp.path()).size(), 0);
}

QTEST_APPLESS_MAIN(TestWorkspaceTaskScanner)
#include "test_workspace_task_scanner.moc"
