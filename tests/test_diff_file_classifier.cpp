/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <QtTest>

#include "DiffFileClassifier.h"

using Reason = DiffFileClassifier::Reason;

namespace {

GitDiffParser::FileSpan modified(const QByteArray &path, qint32 rows = 10)
{
    GitDiffParser::FileSpan f;
    f.oldPath = path;
    f.newPath = path;
    f.status = 'M';
    f.bodyRows = rows;
    f.bodyBytes = rows * 40;
    f.maxLineBytes = 40;
    f.added = rows / 2;
    f.deleted = rows / 2;
    return f;
}

} // namespace

class TestDiffFileClassifier : public QObject
{
    Q_OBJECT

private slots:
    void reasons_data();
    void reasons();
    void shape_reasons();
    void collapse_onlyWhenSeveralFiles();
    void collapse_perReason_data();
    void collapse_perReason();
    void large_thresholds_data();
    void large_thresholds();
};

void TestDiffFileClassifier::reasons_data()
{
    QTest::addColumn<QByteArray>("path");
    QTest::addColumn<int>("expected");

    QTest::newRow("plain source") << QByteArray("src/a.cpp") << int(Reason::None);
    QTest::newRow("package-lock") << QByteArray("web/package-lock.json") << int(Reason::Lockfile);
    QTest::newRow("yarn.lock") << QByteArray("yarn.lock") << int(Reason::Lockfile);
    QTest::newRow("pnpm") << QByteArray("pnpm-lock.yaml") << int(Reason::Lockfile);
    QTest::newRow("Cargo.lock") << QByteArray("Cargo.lock") << int(Reason::Lockfile);
    QTest::newRow("go.sum") << QByteArray("svc/go.sum") << int(Reason::Lockfile);
    QTest::newRow("any .lock") << QByteArray("deps/thing.lock") << int(Reason::Lockfile);
    QTest::newRow("lock in name only") << QByteArray("src/lock.cpp") << int(Reason::None);
    QTest::newRow("min.js") << QByteArray("static/app.min.js") << int(Reason::Generated);
    QTest::newRow("min.css") << QByteArray("a.min.css") << int(Reason::Generated);
    QTest::newRow("source map") << QByteArray("out/app.js.map") << int(Reason::Generated);
    QTest::newRow("pb2") << QByteArray("proto/x_pb2.py") << int(Reason::Generated);
    QTest::newRow("pb.go") << QByteArray("api/x.pb.go") << int(Reason::Generated);
    QTest::newRow("pb.h") << QByteArray("api/x.pb.h") << int(Reason::Generated);
    QTest::newRow("g.dart") << QByteArray("lib/x.g.dart") << int(Reason::Generated);
    QTest::newRow(".generated.") << QByteArray("src/Api.generated.ts") << int(Reason::Generated);
    QTest::newRow("dist at root") << QByteArray("dist/index.js") << int(Reason::Generated);
    QTest::newRow("dist nested") << QByteArray("pkg/dist/index.js") << int(Reason::Generated);
    QTest::newRow("distance is not dist") << QByteArray("src/distance.cpp") << int(Reason::None);
}

void TestDiffFileClassifier::reasons()
{
    QFETCH(QByteArray, path);
    QFETCH(int, expected);
    QCOMPARE(int(DiffFileClassifier::classify(modified(path), false)), expected);
}

void TestDiffFileClassifier::shape_reasons()
{
    auto bin = modified("yarn.lock");
    bin.binary = true;
    QCOMPARE(DiffFileClassifier::classify(bin, false), Reason::Binary);   // binary beats lockfile

    auto sub = modified("lib");
    sub.submodule = true;
    QCOMPARE(DiffFileClassifier::classify(sub, false), Reason::Submodule);

    auto ren = modified("b.cpp", 0);
    ren.oldPath = "a.cpp";
    ren.status = 'R';
    ren.added = ren.deleted = 0;
    QCOMPARE(DiffFileClassifier::classify(ren, false), Reason::RenamedOnly);

    auto mode = modified("run.sh", 0);
    mode.oldMode = 0100644;
    mode.newMode = 0100755;
    QCOMPARE(DiffFileClassifier::classify(mode, false), Reason::ModeOnly);

    QCOMPARE(DiffFileClassifier::classify(modified("src/a.cpp"), true), Reason::Generated);
    QCOMPARE(DiffFileClassifier::classify(modified("yarn.lock"), true), Reason::Lockfile);

    auto del = modified("src/gone.cpp");
    del.status = 'D';
    del.newPath.clear();
    QCOMPARE(DiffFileClassifier::classify(del, false), Reason::Deleted);

    auto delLock = modified("yarn.lock");
    delLock.status = 'D';
    QCOMPARE(DiffFileClassifier::classify(delLock, false), Reason::Lockfile);
}

void TestDiffFileClassifier::collapse_onlyWhenSeveralFiles()
{
    const auto lock = modified("yarn.lock");
    const Reason r = DiffFileClassifier::classify(lock, false);
    QVERIFY(DiffFileClassifier::startsCollapsed(r, 2));
    QVERIFY(!DiffFileClassifier::startsCollapsed(r, 1));
}

void TestDiffFileClassifier::collapse_perReason_data()
{
    QTest::addColumn<int>("reason");
    QTest::addColumn<bool>("collapsed");
    QTest::newRow("none") << int(Reason::None) << false;
    QTest::newRow("binary") << int(Reason::Binary) << true;
    QTest::newRow("submodule") << int(Reason::Submodule) << false;
    QTest::newRow("renamed only") << int(Reason::RenamedOnly) << false;
    QTest::newRow("mode only") << int(Reason::ModeOnly) << false;
    QTest::newRow("lockfile") << int(Reason::Lockfile) << true;
    QTest::newRow("generated") << int(Reason::Generated) << true;
    QTest::newRow("deleted") << int(Reason::Deleted) << true;
    QTest::newRow("large") << int(Reason::Large) << true;
}

void TestDiffFileClassifier::collapse_perReason()
{
    QFETCH(int, reason);
    QFETCH(bool, collapsed);
    QCOMPARE(DiffFileClassifier::startsCollapsed(Reason(reason), 3), collapsed);
}

void TestDiffFileClassifier::large_thresholds_data()
{
    QTest::addColumn<int>("rows");
    QTest::addColumn<qint64>("bytes");
    QTest::addColumn<int>("maxLine");
    QTest::addColumn<bool>("large");
    QTest::newRow("1500 rows") << 1500 << qint64(1000) << 10 << false;
    QTest::newRow("1501 rows") << 1501 << qint64(1000) << 10 << true;
    QTest::newRow("256 KB") << 10 << qint64(256 * 1024) << 10 << false;
    QTest::newRow("256 KB + 1") << 10 << qint64(256 * 1024 + 1) << 10 << true;
    QTest::newRow("5000-byte line") << 10 << qint64(6000) << 5000 << false;
    QTest::newRow("5001-byte line") << 10 << qint64(6000) << 5001 << true;
}

void TestDiffFileClassifier::large_thresholds()
{
    QFETCH(int, rows);
    QFETCH(qint64, bytes);
    QFETCH(int, maxLine);
    QFETCH(bool, large);
    auto f = modified("src/a.cpp", rows);
    f.bodyBytes = bytes;
    f.maxLineBytes = maxLine;
    QCOMPARE(DiffFileClassifier::classify(f, false) == Reason::Large, large);
}

QTEST_GUILESS_MAIN(TestDiffFileClassifier)
#include "test_diff_file_classifier.moc"
