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

#include "MarkdownPreviewSupport.h"

using namespace MarkdownPreviewSupport;

class TestMarkdownPreviewSupport : public QObject
{
    Q_OBJECT

private slots:
    void shortSourceIsUnchanged();
    void sourceAtTheCapIsUnchanged();
    void longSourceIsCutAtALineBoundary();
    void longSourceWithoutNewlinesIsCutAtTheCap();
    void cutNeverTearsTheLastLineFarFromTheCap();
    void truncationNoticeIsAppended();
    void resultStaysNearTheCap();

    void scrollFractionEndpoints();
    void scrollFractionMidpoint();
    void scrollFractionClamps();
    void scrollFractionDegenerateDocuments();
};

void TestMarkdownPreviewSupport::shortSourceIsUnchanged()
{
    const QString s = QStringLiteral("# hi\n\ntext\n");
    QCOMPARE(clampSource(s, 1000), s);
}

void TestMarkdownPreviewSupport::sourceAtTheCapIsUnchanged()
{
    const QString s(100, QLatin1Char('a'));
    QCOMPARE(clampSource(s, 100), s);
}

void TestMarkdownPreviewSupport::longSourceIsCutAtALineBoundary()
{
    QString s;
    for (int i = 0; i < 50; ++i)
        s += QStringLiteral("line number %1\n").arg(i);
    const QString r = clampSource(s, 200);
    QVERIFY(r.size() < s.size());
    // every kept source line is complete: the text before the notice ends at a
    // newline boundary and each line is one of the originals
    const int notice = int(r.indexOf(QStringLiteral("\n\n---")));
    QVERIFY(notice > 0);
    const QString kept = r.left(notice);
    for (const QString &line : kept.split(QLatin1Char('\n')))
        QVERIFY2(line.startsWith(QStringLiteral("line number ")) && line.size() >= 13,
                 qPrintable(line));
}

void TestMarkdownPreviewSupport::longSourceWithoutNewlinesIsCutAtTheCap()
{
    const QString s(5000, QLatin1Char('x'));
    const QString r = clampSource(s, 1000);
    const int notice = int(r.indexOf(QStringLiteral("\n\n---")));
    QCOMPARE(notice, 1000);
}

void TestMarkdownPreviewSupport::cutNeverTearsTheLastLineFarFromTheCap()
{
    // one early newline then a huge line: cutting at that early newline would drop
    // almost everything, so the cap wins instead
    const QString s = QStringLiteral("a\n") + QString(5000, QLatin1Char('y'));
    const QString r = clampSource(s, 1000);
    const int notice = int(r.indexOf(QStringLiteral("\n\n---")));
    QVERIFY(notice >= 500);
}

void TestMarkdownPreviewSupport::truncationNoticeIsAppended()
{
    const QString r = clampSource(QString(2000, QLatin1Char('z')), 500);
    QVERIFY(r.contains(QStringLiteral("truncated")));
    QVERIFY(r.endsWith(QLatin1Char('\n')));
}

void TestMarkdownPreviewSupport::resultStaysNearTheCap()
{
    const QString r = clampSource(QString(10'000, QLatin1Char('q')), 1000);
    QVERIFY(r.size() < 1000 + 120);
}

void TestMarkdownPreviewSupport::scrollFractionEndpoints()
{
    QCOMPARE(scrollFraction(1, 100), 0.0);
    QCOMPARE(scrollFraction(100, 100), 1.0);
}

void TestMarkdownPreviewSupport::scrollFractionMidpoint()
{
    QVERIFY(qAbs(scrollFraction(51, 101) - 0.5) < 1e-9);
    QVERIFY(qAbs(scrollFraction(26, 101) - 0.25) < 1e-9);
}

void TestMarkdownPreviewSupport::scrollFractionClamps()
{
    QCOMPARE(scrollFraction(0, 100), 0.0);
    QCOMPARE(scrollFraction(-5, 100), 0.0);
    QCOMPARE(scrollFraction(500, 100), 1.0);
}

void TestMarkdownPreviewSupport::scrollFractionDegenerateDocuments()
{
    QCOMPARE(scrollFraction(1, 1), 0.0);
    QCOMPARE(scrollFraction(7, 1), 0.0);
    QCOMPARE(scrollFraction(3, 0), 0.0);
    QCOMPARE(scrollFraction(3, -2), 0.0);
}

QTEST_APPLESS_MAIN(TestMarkdownPreviewSupport)
#include "test_markdown_preview_support.moc"
