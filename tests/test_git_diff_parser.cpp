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
#include <QElapsedTimer>

#include "GitDiffParser.h"

class TestGitDiffParser : public QObject
{
    Q_OBJECT

private slots:
    void empty_returnsEmpty();
    void singleHunk_textAndStats();
    void crlf_normalizedOut();
    void binaryMarker_setsFlag();
    void noNewlineMarker_classified();
    void multipleFiles_allFileHeadersCounted();
    void fileTable_modifiedFile();
    void fileTable_variants_data();
    void fileTable_variants();
    void fileTable_multiFileBoundaries();
    void hunk_deletedLineLookingLikeHeaderStaysBody();
    void hunk_strippedEmptyContextLine();
    void perf_5kLineDiff_under60ms();
};

void TestGitDiffParser::fileTable_variants_data()
{
    QTest::addColumn<QByteArray>("diff");
    QTest::addColumn<QByteArray>("oldPath");
    QTest::addColumn<QByteArray>("newPath");
    QTest::addColumn<QString>("status");
    QTest::addColumn<uint>("oldMode");
    QTest::addColumn<uint>("newMode");
    QTest::addColumn<bool>("binary");
    QTest::addColumn<bool>("submodule");
    QTest::addColumn<int>("similarity");

    QTest::newRow("added")
        << QByteArray("diff --git a/n.txt b/n.txt\n"
                      "new file mode 100644\n"
                      "index 0000000..e69de29\n"
                      "--- /dev/null\n"
                      "+++ b/n.txt\n"
                      "@@ -0,0 +1 @@\n"
                      "+hi\n")
        << QByteArray() << QByteArray("n.txt") << "A" << 0u << 0100644u << false << false << 0;
    QTest::newRow("deleted")
        << QByteArray("diff --git a/d.txt b/d.txt\n"
                      "deleted file mode 100644\n"
                      "index e69de29..0000000\n"
                      "--- a/d.txt\n"
                      "+++ /dev/null\n"
                      "@@ -1 +0,0 @@\n"
                      "-bye\n")
        << QByteArray("d.txt") << QByteArray() << "D" << 0100644u << 0u << false << false << 0;
    QTest::newRow("rename with edits")
        << QByteArray("diff --git a/old dir/a.cpp b/new dir/b.cpp\n"
                      "similarity index 92%\n"
                      "rename from old dir/a.cpp\n"
                      "rename to new dir/b.cpp\n"
                      "index 1111111..2222222 100644\n"
                      "--- a/old dir/a.cpp\n"
                      "+++ b/new dir/b.cpp\n"
                      "@@ -1 +1 @@\n"
                      "-x\n"
                      "+y\n")
        << QByteArray("old dir/a.cpp") << QByteArray("new dir/b.cpp") << "R"
        << 0100644u << 0100644u << false << false << 92;
    QTest::newRow("pure rename")
        << QByteArray("diff --git a/a.cpp b/b.cpp\n"
                      "similarity index 100%\n"
                      "rename from a.cpp\n"
                      "rename to b.cpp\n")
        << QByteArray("a.cpp") << QByteArray("b.cpp") << "R" << 0u << 0u << false << false << 100;
    QTest::newRow("mode only")
        << QByteArray("diff --git a/run.sh b/run.sh\n"
                      "old mode 100644\n"
                      "new mode 100755\n")
        << QByteArray("run.sh") << QByteArray("run.sh") << "M" << 0100644u << 0100755u
        << false << false << 0;
    QTest::newRow("type change")
        << QByteArray("diff --git a/l b/l\n"
                      "old mode 100644\n"
                      "new mode 120000\n")
        << QByteArray("l") << QByteArray("l") << "T" << 0100644u << 0120000u << false << false << 0;
    QTest::newRow("binary")
        << QByteArray("diff --git a/img.png b/img.png\n"
                      "index 1111111..2222222 100644\n"
                      "Binary files a/img.png and b/img.png differ\n")
        << QByteArray("img.png") << QByteArray("img.png") << "M" << 0100644u << 0100644u
        << true << false << 0;
    QTest::newRow("submodule")
        << QByteArray("diff --git a/lib b/lib\n"
                      "index 1111111..2222222 160000\n"
                      "--- a/lib\n"
                      "+++ b/lib\n"
                      "@@ -1 +1 @@\n"
                      "-Subproject commit 1111111\n"
                      "+Subproject commit 2222222\n")
        << QByteArray("lib") << QByteArray("lib") << "M" << 0160000u << 0160000u
        << false << true << 0;
    QTest::newRow("quoted utf-8 path")
        << QByteArray("diff --git \"a/f\\303\\266o\\tx.txt\" \"b/f\\303\\266o\\tx.txt\"\n"
                      "--- \"a/f\\303\\266o\\tx.txt\"\n"
                      "+++ \"b/f\\303\\266o\\tx.txt\"\n"
                      "@@ -1 +1 @@\n"
                      "-a\n"
                      "+b\n")
        << QByteArray("f\xc3\xb6o\tx.txt") << QByteArray("f\xc3\xb6o\tx.txt") << "M" << 0u << 0u
        << false << false << 0;
}

void TestGitDiffParser::fileTable_variants()
{
    QFETCH(QByteArray, diff);
    QFETCH(QByteArray, oldPath);
    QFETCH(QByteArray, newPath);
    QFETCH(QString, status);
    QFETCH(uint, oldMode);
    QFETCH(uint, newMode);
    QFETCH(bool, binary);
    QFETCH(bool, submodule);
    QFETCH(int, similarity);

    const auto r = GitDiffParser::parse(diff);
    QCOMPARE(r.files.size(), 1);
    const auto &f = r.files.at(0);
    QCOMPARE(f.oldPath, oldPath);
    QCOMPARE(f.newPath, newPath);
    QCOMPARE(QString(QChar(f.status)), status);
    QCOMPARE(uint(f.oldMode), oldMode);
    QCOMPARE(uint(f.newMode), newMode);
    QCOMPARE(f.binary, binary);
    QCOMPARE(f.submodule, submodule);
    QCOMPARE(int(f.similarity), similarity);
    QCOMPARE(f.endRow, r.kinds.size());
}

void TestGitDiffParser::fileTable_multiFileBoundaries()
{
    const QByteArray diff =
        "diff --git a/a.cpp b/a.cpp\n"   // 0
        "--- a/a.cpp\n"                  // 1
        "+++ b/a.cpp\n"                  // 2
        "@@ -1,2 +1,2 @@\n"              // 3
        "-x\n"                           // 4
        "+y\n"                           // 5
        " z\n"                           // 6
        "@@ -9 +9 @@\n"                  // 7
        "-p\n"                           // 8
        "+q\n"                           // 9
        "diff --git a/b.cpp b/b.cpp\n"   // 10
        "--- a/b.cpp\n"                  // 11
        "+++ b/b.cpp\n"                  // 12
        "@@ -1 +1,2 @@\n"                // 13
        " k\n"                           // 14
        "+n\n";                          // 15
    const auto r = GitDiffParser::parse(diff);
    QCOMPARE(r.files.size(), 2);
    QCOMPARE(r.files.at(0).firstRow, 0);
    QCOMPARE(r.files.at(0).endRow, 10);
    QCOMPARE(r.files.at(0).added, 2);
    QCOMPARE(r.files.at(0).deleted, 2);
    QCOMPARE(r.files.at(1).firstRow, 10);
    QCOMPARE(r.files.at(1).endRow, 16);
    QCOMPARE(r.files.at(1).added, 1);
    QCOMPARE(r.files.at(1).deleted, 0);
    QCOMPARE(r.hunks.size(), 3);
    QCOMPARE(r.hunks.at(0).headerRow, 3);
    QCOMPARE(r.hunks.at(1).headerRow, 7);
    QCOMPARE(r.hunks.at(1).fileIndex, 0);
    QCOMPARE(r.hunks.at(2).headerRow, 13);
    QCOMPARE(r.hunks.at(2).fileIndex, 1);
    QCOMPARE(r.newLn.at(15), 2);
}

void TestGitDiffParser::hunk_deletedLineLookingLikeHeaderStaysBody()
{
    // A deleted SQL comment "-- note" is printed as "--- note"; inside a
    // hunk that still owes lines it is a deletion, not a file header.
    const QByteArray diff =
        "diff --git a/q.sql b/q.sql\n"
        "--- a/q.sql\n"
        "+++ b/q.sql\n"
        "@@ -1,2 +1,1 @@\n"
        "--- note\n"
        " select 1;\n";
    const auto r = GitDiffParser::parse(diff);
    QCOMPARE(r.kinds.at(4), GitDiffParser::LineKind::Deleted);
    QCOMPARE(r.texts.at(4), QByteArray("-- note"));
    QCOMPARE(r.oldLn.at(4), 1);
    QCOMPARE(r.files.at(0).deleted, 1);
}

void TestGitDiffParser::hunk_strippedEmptyContextLine()
{
    const QByteArray diff =
        "@@ -1,3 +1,3 @@\n"
        " a\n"
        "\n"
        "-b\n"
        "+c\n";
    const auto r = GitDiffParser::parse(diff);
    QCOMPARE(r.kinds.at(2), GitDiffParser::LineKind::Context);
    QCOMPARE(r.oldLn.at(2), 2);
    QCOMPARE(r.newLn.at(2), 2);
    QCOMPARE(r.oldLn.at(3), 3);
}

void TestGitDiffParser::fileTable_modifiedFile()
{
    const QByteArray diff =
        "diff --git a/src/foo.cpp b/src/foo.cpp\n"   // row 0
        "index 1234567..89abcde 100644\n"            // 1
        "--- a/src/foo.cpp\n"                        // 2
        "+++ b/src/foo.cpp\n"                        // 3
        "@@ -1,3 +1,4 @@ int main()\n"              // 4
        " keep1\n"                                   // 5
        "-removed\n"                                 // 6
        "+added1\n"                                  // 7
        "+added2 is the longest\n"                   // 8
        " keep2\n";                                  // 9
    auto r = GitDiffParser::parse(diff);
    QCOMPARE(r.files.size(), 1);
    const auto &f = r.files.at(0);
    QCOMPARE(f.firstRow, 0);
    QCOMPARE(f.endRow, 10);
    QCOMPARE(f.oldPath, QByteArray("src/foo.cpp"));
    QCOMPARE(f.newPath, QByteArray("src/foo.cpp"));
    QCOMPARE(f.status, 'M');
    QCOMPARE(f.oldMode, 0100644u);
    QCOMPARE(f.newMode, 0100644u);
    QVERIFY(!f.binary);
    QVERIFY(!f.submodule);
    QCOMPARE(f.added, 2);
    QCOMPARE(f.deleted, 1);
    QCOMPARE(f.maxLineBytes, qint32(sizeof("added2 is the longest") - 1));
    QCOMPARE(r.hunks.size(), 1);
    QCOMPARE(r.hunks.at(0).headerRow, 4);
    QCOMPARE(r.hunks.at(0).fileIndex, 0);
}

void TestGitDiffParser::empty_returnsEmpty()
{
    auto r = GitDiffParser::parse(QByteArray());
    QCOMPARE(r.texts.size(), 0);
    QCOMPARE(r.addedCount, 0);
    QCOMPARE(r.deletedCount, 0);
    QVERIFY(!r.isBinary);
}

void TestGitDiffParser::singleHunk_textAndStats()
{
    const QByteArray diff =
        "diff --git a/foo.cpp b/foo.cpp\n"
        "index 1234567..89abcde 100644\n"
        "--- a/foo.cpp\n"
        "+++ b/foo.cpp\n"
        "@@ -1,3 +1,4 @@\n"
        " keep1\n"
        "-removed\n"
        "+added1\n"
        "+added2\n"
        " keep2\n";
    auto r = GitDiffParser::parse(diff);
    QCOMPARE(r.addedCount, 2);
    QCOMPARE(r.deletedCount, 1);
    QVERIFY(!r.isBinary);
    QVERIFY(r.texts.size() >= 8); // headers + 5 body lines
    QCOMPARE(r.texts.size(), r.kinds.size());
    QCOMPARE(r.texts.size(), r.oldLn.size());
    QCOMPARE(r.texts.size(), r.newLn.size());
}

void TestGitDiffParser::crlf_normalizedOut()
{
    const QByteArray diff =
        "@@ -1,1 +1,2 @@\r\n"
        " keep\r\n"
        "+new\r\n";
    auto r = GitDiffParser::parse(diff);
    QCOMPARE(r.addedCount, 1);
    // The text fields should not contain the trailing CR.
    for (const QByteArray &t : r.texts) {
        QVERIFY2(!t.endsWith('\r'),
                 ("CR survived in payload: " + t).constData());
    }
}

void TestGitDiffParser::binaryMarker_setsFlag()
{
    const QByteArray diff =
        "diff --git a/img.png b/img.png\n"
        "Binary files a/img.png and b/img.png differ\n";
    auto r = GitDiffParser::parse(diff);
    QVERIFY(r.isBinary);
}

void TestGitDiffParser::noNewlineMarker_classified()
{
    const QByteArray diff =
        "@@ -1,1 +1,1 @@\n"
        "-old\n"
        "\\ No newline at end of file\n"
        "+new\n";
    auto r = GitDiffParser::parse(diff);
    bool sawNoNewline = false;
    for (auto k : r.kinds) {
        if (k == GitDiffParser::LineKind::NoNewline) {
            sawNoNewline = true;
            break;
        }
    }
    QVERIFY(sawNoNewline);
}

void TestGitDiffParser::multipleFiles_allFileHeadersCounted()
{
    const QByteArray diff =
        "diff --git a/a.cpp b/a.cpp\n"
        "--- a/a.cpp\n"
        "+++ b/a.cpp\n"
        "@@ -1,1 +1,1 @@\n"
        "-x\n"
        "+y\n"
        "diff --git a/b.cpp b/b.cpp\n"
        "--- a/b.cpp\n"
        "+++ b/b.cpp\n"
        "@@ -1,1 +1,1 @@\n"
        "-p\n"
        "+q\n";
    auto r = GitDiffParser::parse(diff);
    QCOMPARE(r.addedCount, 2);
    QCOMPARE(r.deletedCount, 2);
    int hunkHeaders = 0;
    for (auto k : r.kinds) {
        if (k == GitDiffParser::LineKind::HunkHeader) ++hunkHeaders;
    }
    QCOMPARE(hunkHeaders, 2);
}

void TestGitDiffParser::perf_5kLineDiff_under60ms()
{
    // Build a synthetic 5k-line unified diff: alternating + / - / context lines
    // across one large hunk, with realistic line widths.
    QByteArray diff;
    diff.reserve(5000 * 80);
    diff.append("diff --git a/big.txt b/big.txt\n");
    diff.append("--- a/big.txt\n");
    diff.append("+++ b/big.txt\n");
    diff.append("@@ -1,5000 +1,5000 @@\n");
    const char *filler = "abcdefghij abcdefghij abcdefghij abcdefghij abcdefghij abcdefghij";
    for (int i = 0; i < 5000; ++i) {
        char prefix;
        switch (i % 3) {
            case 0: prefix = ' '; break;
            case 1: prefix = '+'; break;
            default: prefix = '-'; break;
        }
        diff.append(prefix);
        diff.append(filler);
        diff.append('\n');
    }

    // Warm up branch predictor / cache.
    (void)GitDiffParser::parse(diff);

    QElapsedTimer t;
    qint64 best = std::numeric_limits<qint64>::max();
    for (int run = 0; run < 3; ++run) {
        t.start();
        auto r = GitDiffParser::parse(diff);
        const qint64 elapsed = t.nsecsElapsed();
        best = std::min(best, elapsed);
        // Sanity: the parsed result must contain all source lines.
        QVERIFY(r.texts.size() >= 5000);
    }

    const double ms = best / 1.0e6;
    qInfo() << "GitDiffParser 5k-line best:" << ms << "ms";
    // Hard budget: parser must finish well under 60ms so the full
    // parse+paint+focus chain stays under the user-perceived threshold.
    QVERIFY2(ms < 60.0,
             qPrintable(QString("Parser too slow: %1ms (>60ms)").arg(ms)));
}

QTEST_GUILESS_MAIN(TestGitDiffParser)
#include "test_git_diff_parser.moc"
