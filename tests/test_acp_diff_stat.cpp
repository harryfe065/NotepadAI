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

#include "AcpDiffStat.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QTest>

class TestAcpDiffStat : public QObject
{
    Q_OBJECT

private slots:
    void unifiedCompactDiffCountsMarkedLines();
    void unifiedFileHeadersAreNotCounted();
    void numberedCompactDiffCountsMarkedLines();
    void compactDiffWinsOverOldNew();
    void oldNewTextCountsMinimalLineEdit();
    void snippetWithoutTrailingNewlineCountsOnlyNewLine();
    void crlfAgainstLfIsUnchanged();
    void newFileCountsEveryLine();
    void identicalTextIsEmpty();
    void hugeTextFallsBackToPrefixSuffix();
    void contentSumsDiffBlocksAndSkipsOthers();
    void onlyCompletedToolCallsCount();
    void labelFormatsBothSides();
};

namespace {

QJsonObject compactBlock(const QString &diff)
{
    QJsonObject b;
    b.insert(QStringLiteral("type"), QStringLiteral("diff"));
    b.insert(QStringLiteral("path"), QStringLiteral("a.cpp"));
    b.insert(QStringLiteral("diff"), diff);
    return b;
}

QJsonObject textBlock(const QJsonValue &oldText, const QString &newText)
{
    QJsonObject b;
    b.insert(QStringLiteral("type"), QStringLiteral("diff"));
    b.insert(QStringLiteral("path"), QStringLiteral("a.cpp"));
    b.insert(QStringLiteral("oldText"), oldText);
    b.insert(QStringLiteral("newText"), newText);
    return b;
}

void compareLines(const AcpDiffStat::Lines &got, int added, int removed)
{
    QCOMPARE(got.added, added);
    QCOMPARE(got.removed, removed);
}

} // namespace

void TestAcpDiffStat::unifiedCompactDiffCountsMarkedLines()
{
    compareLines(AcpDiffStat::forBlock(compactBlock(
                     QStringLiteral("@@ -1,3 +1,4 @@\n keep\n-old\n+new\n+more\n keep2\n"))),
                 2, 1);
}

void TestAcpDiffStat::unifiedFileHeadersAreNotCounted()
{
    compareLines(AcpDiffStat::forBlock(compactBlock(
                     QStringLiteral("diff --git a/x b/x\nindex 1..2\n--- a/x\n+++ b/x\n"
                                    "@@ -1 +1 @@\n-a\n+b\n"))),
                 1, 1);
}

void TestAcpDiffStat::numberedCompactDiffCountsMarkedLines()
{
    compareLines(AcpDiffStat::forBlock(compactBlock(
                     QStringLiteral("  10|keep\n-11|old line\n+11|new line\n+12|x\n  13|tail\n"))),
                 2, 1);
    // omp's box-drawing gutter.
    compareLines(AcpDiffStat::forBlock(compactBlock(
                     QStringLiteral(" 1│a\n-2│b\n"))),
                 0, 1);
}

void TestAcpDiffStat::compactDiffWinsOverOldNew()
{
    QJsonObject b = textBlock(QStringLiteral("a\nb\nc\n"), QStringLiteral("x\ny\nz\n"));
    b.insert(QStringLiteral("diff"), QStringLiteral("@@ -2 +2 @@\n-b\n+y\n"));
    compareLines(AcpDiffStat::forBlock(b), 1, 1);
}

void TestAcpDiffStat::oldNewTextCountsMinimalLineEdit()
{
    compareLines(AcpDiffStat::forBlock(textBlock(QStringLiteral("a\nb\nc\n"),
                                                 QStringLiteral("a\nB\nc\nd\n"))),
                 2, 1);
}

void TestAcpDiffStat::snippetWithoutTrailingNewlineCountsOnlyNewLine()
{
    // Claude Code's Edit sends old_string/new_string, usually without a final
    // newline: appending a line must not also count the old last line.
    compareLines(AcpDiffStat::forBlock(textBlock(QStringLiteral("a\nb"),
                                                 QStringLiteral("a\nb\nc"))),
                 1, 0);
    compareLines(AcpDiffStat::forBlock(textBlock(QStringLiteral("foo"), QStringLiteral("bar"))),
                 1, 1);
}

void TestAcpDiffStat::crlfAgainstLfIsUnchanged()
{
    QVERIFY(AcpDiffStat::forBlock(textBlock(QStringLiteral("a\r\nb\r\n"),
                                            QStringLiteral("a\nb\n"))).isEmpty());
}

void TestAcpDiffStat::newFileCountsEveryLine()
{
    compareLines(AcpDiffStat::forBlock(textBlock(QJsonValue(), QStringLiteral("x\ny\n"))), 2, 0);
    compareLines(AcpDiffStat::forBlock(textBlock(QStringLiteral(""), QStringLiteral("x\ny"))), 2, 0);
    // Deleting everything.
    compareLines(AcpDiffStat::forBlock(textBlock(QStringLiteral("x\ny\n"), QString())), 0, 2);
}

void TestAcpDiffStat::identicalTextIsEmpty()
{
    QVERIFY(AcpDiffStat::forBlock(textBlock(QStringLiteral("a\nb\n"),
                                            QStringLiteral("a\nb\n"))).isEmpty());
}

void TestAcpDiffStat::hugeTextFallsBackToPrefixSuffix()
{
    // Over the size cap: common prefix/suffix trimmed, the middle counted as
    // one replace, in linear time.
    QString oldText;
    QString newText;
    oldText.reserve(1 << 21);
    newText.reserve(1 << 21);
    for (int i = 0; i < 60000; ++i) {
        const QString line = QStringLiteral("line-%1-abcdefghij\n").arg(i);
        oldText += line;
        newText += (i == 30000) ? QStringLiteral("changed\nadded\n") : line;
    }
    QVERIFY(oldText.size() > AcpDiffStat::kMaxExactDiffChars);
    compareLines(AcpDiffStat::forBlock(textBlock(oldText, newText)), 2, 1);
}

void TestAcpDiffStat::contentSumsDiffBlocksAndSkipsOthers()
{
    QJsonArray content;
    QJsonObject text;
    text.insert(QStringLiteral("type"), QStringLiteral("content"));
    text.insert(QStringLiteral("content"), QStringLiteral("+not a diff\n-nope\n"));
    content.append(text);
    content.append(compactBlock(QStringLiteral("@@ -1 +1 @@\n-a\n+b\n")));
    content.append(textBlock(QJsonValue(), QStringLiteral("x\ny\nz\n")));
    compareLines(AcpDiffStat::forContent(content), 4, 1);
}

void TestAcpDiffStat::onlyCompletedToolCallsCount()
{
    AcpProtocol::AcpToolCall tc;
    tc.id = QStringLiteral("t1");
    tc.content.append(compactBlock(QStringLiteral("@@ -1 +1 @@\n-a\n+b\n")));

    tc.status = QStringLiteral("pending"); // proposed edit awaiting permission
    QVERIFY(AcpDiffStat::forToolCall(tc).isEmpty());
    tc.status = QStringLiteral("failed");
    QVERIFY(AcpDiffStat::forToolCall(tc).isEmpty());
    tc.status = QStringLiteral("completed");
    compareLines(AcpDiffStat::forToolCall(tc), 1, 1);
}

void TestAcpDiffStat::labelFormatsBothSides()
{
    QCOMPARE(AcpDiffStat::label({}), QString());
    QCOMPARE(AcpDiffStat::label({6, 10}), QStringLiteral("+6 -10"));
    QCOMPARE(AcpDiffStat::label({3, 0}), QStringLiteral("+3 -0"));
}

QTEST_APPLESS_MAIN(TestAcpDiffStat)
#include "test_acp_diff_stat.moc"
