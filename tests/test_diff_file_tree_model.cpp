/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <QAbstractItemModelTester>
#include <QSortFilterProxyModel>
#include <QtTest>

#include "DiffFileTreeModel.h"
#include "GitStatusModel.h"

namespace {

GitDiffParser::FileSpan file(const QByteArray &path, char status = 'M', qint32 added = 1, qint32 deleted = 0)
{
    GitDiffParser::FileSpan f;
    f.oldPath = status == 'A' ? QByteArray() : path;
    f.newPath = status == 'D' ? QByteArray() : path;
    f.status = status;
    f.added = added;
    f.deleted = deleted;
    return f;
}

// The tree as indented display names, one row per line ("  " per level).
QString outline(const QAbstractItemModel &m, const QModelIndex &parent = {}, int depth = 0)
{
    QString out;
    for (int r = 0; r < m.rowCount(parent); ++r) {
        const QModelIndex i = m.index(r, 0, parent);
        out += QString(depth * 2, QLatin1Char(' ')) + i.data(DiffFileTreeModel::NameRole).toString() + QLatin1Char('\n');
        out += outline(m, i, depth + 1);
    }
    return out;
}

DiffFileTreeModel *modelOf(const QVector<GitDiffParser::FileSpan> &files, QObject *parent)
{
    auto *m = new DiffFileTreeModel(parent);
    m->setTree(DiffFileTreeModel::build(files));
    return m;
}

} // namespace

class TestDiffFileTreeModel : public QObject
{
    Q_OBJECT

private slots:
    void compaction_singleChain();
    void compaction_branching();
    void ordering_dirsFirstCaseInsensitive();
    void roles_file();
    void roles_directoryAndBinary();
    void indexOfFile_roundTrips();
    void filter_keepsAncestors();
    void modelTester_clean();
};

void TestDiffFileTreeModel::compaction_singleChain()
{
    QObject owner;
    auto *m = modelOf({file("src/git/A.cpp"), file("src/git/B.cpp")}, &owner);
    QCOMPARE(outline(*m), QStringLiteral("src/git\n  A.cpp\n  B.cpp\n"));
}

void TestDiffFileTreeModel::compaction_branching()
{
    QObject owner;
    auto *m = modelOf({file("README.md"), file("src/git/A.cpp"), file("src/ui/B.cpp"),
                       file("src/main.cpp"), file("docs/adr/0002.md")}, &owner);
    QCOMPARE(outline(*m), QStringLiteral(
        "docs/adr\n"
        "  0002.md\n"
        "src\n"
        "  git\n"
        "    A.cpp\n"
        "  ui\n"
        "    B.cpp\n"
        "  main.cpp\n"
        "README.md\n"));
}

void TestDiffFileTreeModel::ordering_dirsFirstCaseInsensitive()
{
    QObject owner;
    auto *m = modelOf({file("b.txt"), file("Zeta/x"), file("a.txt"), file("alpha/y"), file("C.txt")}, &owner);
    QCOMPARE(outline(*m), QStringLiteral("alpha\n  y\nZeta\n  x\na.txt\nb.txt\nC.txt\n"));
}

void TestDiffFileTreeModel::roles_file()
{
    QObject owner;
    GitDiffParser::FileSpan renamed = file("src/new.cpp", 'R', 4, 2);
    renamed.oldPath = "src/old.cpp";
    auto *m = modelOf({file("src/a.cpp", 'M', 30, 10), renamed}, &owner);
    const QModelIndex dir = m->index(0, 0);
    const QModelIndex a = m->index(0, 0, dir);
    QCOMPARE(a.data(Qt::DisplayRole).toString(), QStringLiteral("M  a.cpp"));
    QCOMPARE(a.data(GitStatusModel::AddedLinesRole).toInt(), 30);
    QCOMPARE(a.data(GitStatusModel::DeletedLinesRole).toInt(), 10);
    QCOMPARE(a.data(GitStatusModel::IsBinaryRole).toBool(), false);
    QCOMPARE(a.data(GitStatusModel::IsSectionRole).toBool(), false);
    QCOMPARE(a.data(DiffFileTreeModel::FileIndexRole).toInt(), 0);
    QCOMPARE(a.data(DiffFileTreeModel::PathRole).toString(), QStringLiteral("src/a.cpp"));
    QCOMPARE(a.data(Qt::ToolTipRole).toString(), QStringLiteral("src/a.cpp"));
    QVERIFY(a.data(Qt::ForegroundRole).value<QBrush>().style() != Qt::NoBrush);

    const QModelIndex r = m->index(1, 0, dir);
    QCOMPARE(r.data(Qt::DisplayRole).toString(), QStringLiteral("R  new.cpp"));
    QCOMPARE(r.data(Qt::ToolTipRole).toString(), QStringLiteral("src/old.cpp → src/new.cpp"));
    QCOMPARE(r.data(DiffFileTreeModel::FileIndexRole).toInt(), 1);
}

void TestDiffFileTreeModel::roles_directoryAndBinary()
{
    QObject owner;
    GitDiffParser::FileSpan bin = file("img/logo.png", 'A', 0, 0);
    bin.binary = true;
    auto *m = modelOf({bin}, &owner);
    const QModelIndex dir = m->index(0, 0);
    QCOMPARE(dir.data(Qt::DisplayRole).toString(), QStringLiteral("img"));
    QCOMPARE(dir.data(DiffFileTreeModel::FileIndexRole).toInt(), -1);
    QCOMPARE(dir.data(DiffFileTreeModel::PathRole).toString(), QString());
    QCOMPARE(dir.data(GitStatusModel::AddedLinesRole).toInt(), -1);
    const QModelIndex png = m->index(0, 0, dir);
    QCOMPARE(png.data(GitStatusModel::IsBinaryRole).toBool(), true);
}

void TestDiffFileTreeModel::indexOfFile_roundTrips()
{
    QObject owner;
    auto *m = modelOf({file("z.txt"), file("a/b/c.txt"), file("a/d.txt")}, &owner);
    for (int i = 0; i < 3; ++i) {
        const QModelIndex idx = m->indexOfFile(i);
        QVERIFY(idx.isValid());
        QCOMPARE(idx.data(DiffFileTreeModel::FileIndexRole).toInt(), i);
    }
    QVERIFY(!m->indexOfFile(3).isValid());
    QVERIFY(!m->indexOfFile(-1).isValid());
}

void TestDiffFileTreeModel::filter_keepsAncestors()
{
    QObject owner;
    auto *m = modelOf({file("src/git/GitController.cpp"), file("src/git/GitView.cpp"),
                       file("src/ui/Main.cpp"), file("README.md")}, &owner);
    QSortFilterProxyModel *proxy = DiffFileTreeModel::createFilterProxy(&owner);
    proxy->setSourceModel(m);

    proxy->setFilterFixedString(QStringLiteral("controller"));
    QCOMPARE(outline(*proxy), QStringLiteral("src\n  git\n    GitController.cpp\n"));

    // The directory part of the path matches too.
    proxy->setFilterFixedString(QStringLiteral("SRC/UI"));
    QCOMPARE(outline(*proxy), QStringLiteral("src\n  ui\n    Main.cpp\n"));

    proxy->setFilterFixedString(QStringLiteral("nothing"));
    QCOMPARE(proxy->rowCount(), 0);

    proxy->setFilterFixedString(QString());
    QCOMPARE(outline(*proxy), outline(*m));
}

void TestDiffFileTreeModel::modelTester_clean()
{
    QObject owner;
    auto *m = new DiffFileTreeModel(&owner);
    QAbstractItemModelTester tester(m, QAbstractItemModelTester::FailureReportingMode::QtTest);
    m->setTree(DiffFileTreeModel::build({file("a/b/c.txt"), file("a/d.txt"), file("e.txt")}));
    m->setDarkPalette(true);
    m->setTree(DiffFileTreeModel::build({}));
    m->setTree(DiffFileTreeModel::build({file("x/y.txt")}));
}

QTEST_MAIN(TestDiffFileTreeModel)
#include "test_diff_file_tree_model.moc"
