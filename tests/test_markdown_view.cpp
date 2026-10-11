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
#include <QAbstractTextDocumentLayout>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTextBlock>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QUrl>

#include "MarkdownView.h"

namespace {

int imageCount(MarkdownView &v)
{
    int n = 0;
    for (QTextBlock b = v.document()->begin(); b.isValid(); b = b.next())
        for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it)
            if (it.fragment().charFormat().isImageFormat())
                ++n;
    return n;
}

QString shown(MarkdownView &v)
{
    return v.toPlainText();
}

// A long prose paragraph, so the layout actually uses the available width.
QString longParagraph()
{
    QString s;
    for (int i = 0; i < 120; ++i)
        s += QStringLiteral("word%1 ").arg(i);
    return s;
}

void showAt(MarkdownView &v, int w, int h = 400)
{
    v.setAttribute(Qt::WA_DontShowOnScreen, true);
    v.resize(w, h);
    v.show();
    QApplication::processEvents();
}

} // namespace

class TestMarkdownView : public QObject
{
    Q_OBJECT

private slots:
    void rendersMathAndCodeThroughTheSharedPipeline();
    void emitsRenderedOnSourceChange();
    void emitsRenderedOnDetailsToggle();
    void chatFontWithoutRerenderDoesNotRender();

    void maxContentWidthCapsTheTextColumn();
    void maxContentWidthCentersTheColumn();
    void narrowViewIsNotPadded();
    void resizingRecomputesTheColumn();
    void removingTheCapRestoresFullWidth();
    void capKeepsMathAndCodeSurfacesInsideTheColumn();

    void codeSurfaceFollowsTheColumn();

    void flatSpacingIsTheDefault();
    void documentSpacingSeparatesParagraphs();
    void documentSpacingBindsHeadingsToWhatTheyTitle();
    void documentSpacingKeepsListItemsTight();
    void documentSpacingLeavesCodeSurfacesAlone();
    void documentSpacingHasNoLeadingOrTrailingGap();
    void togglingDocumentSpacingReRenders();
};

void TestMarkdownView::rendersMathAndCodeThroughTheSharedPipeline()
{
    MarkdownView v;
    showAt(v, 600);
    v.setMarkdownSource(QStringLiteral("Energy $E = mc^2$\n\n```math\n\\frac{a}{b}\n```\n\n"
                                       "<details><summary>More</summary>hidden</details>"));
    QCOMPARE(imageCount(v), 2);
    QVERIFY(shown(v).contains(QStringLiteral("More")));
    QVERIFY(!shown(v).contains(QStringLiteral("hidden")));
    QVERIFY(!shown(v).contains(QLatin1Char('$')));
}

void TestMarkdownView::emitsRenderedOnSourceChange()
{
    MarkdownView v;
    QSignalSpy spy(&v, &MarkdownView::rendered);
    v.setMarkdownSource(QStringLiteral("one"));
    QCOMPARE(spy.count(), 1);
    v.setMarkdownSource(QStringLiteral("two"));
    QCOMPARE(spy.count(), 2);
    QCOMPARE(v.markdownSource(), QStringLiteral("two"));
}

void TestMarkdownView::emitsRenderedOnDetailsToggle()
{
    MarkdownView v;
    v.setMarkdownSource(QStringLiteral("<details><summary>T</summary>body</details>"));
    QSignalSpy spy(&v, &MarkdownView::rendered);
    emit v.anchorClicked(QUrl(QStringLiteral("nn-details:0")));
    QCOMPARE(spy.count(), 1);
    QVERIFY(shown(v).contains(QStringLiteral("body")));
}

void TestMarkdownView::chatFontWithoutRerenderDoesNotRender()
{
    MarkdownView v;
    v.setMarkdownSource(QStringLiteral("text"));
    QSignalSpy spy(&v, &MarkdownView::rendered);
    QFont f(QStringLiteral("Segoe UI"));
    f.setPixelSize(15);
    v.setChatFont(f, false);
    QCOMPARE(spy.count(), 0);
    v.setChatFont(f, true);
    QCOMPARE(spy.count(), 1);
}

void TestMarkdownView::maxContentWidthCapsTheTextColumn()
{
    MarkdownView v;
    v.setMaxContentWidth(700);
    showAt(v, 1500);
    v.setMarkdownSource(longParagraph());
    QVERIFY2(v.viewport()->width() <= 700, qPrintable(QString::number(v.viewport()->width())));
    QVERIFY(v.viewport()->width() >= 650); // the column is as wide as allowed, not collapsed
    // the document wraps inside the capped viewport
    QVERIFY2(v.document()->size().width() <= 700.5, qPrintable(QString::number(v.document()->size().width())));
}

void TestMarkdownView::maxContentWidthCentersTheColumn()
{
    MarkdownView v;
    v.setMaxContentWidth(700);
    showAt(v, 1500);
    v.setMarkdownSource(longParagraph());
    const int left = v.viewport()->geometry().left();
    const int right = v.width() - v.viewport()->geometry().right() - 1;
    QVERIFY(left > 200);
    QVERIFY2(qAbs(left - right) <= 2, qPrintable(QStringLiteral("%1 vs %2").arg(left).arg(right)));
}

void TestMarkdownView::narrowViewIsNotPadded()
{
    MarkdownView v;
    v.setMaxContentWidth(900);
    showAt(v, 600);
    v.setMarkdownSource(longParagraph());
    // narrower than the cap: use all of it
    QVERIFY(v.viewport()->width() >= 580);
}

void TestMarkdownView::resizingRecomputesTheColumn()
{
    MarkdownView v;
    v.setMaxContentWidth(600);
    showAt(v, 1000);
    v.setMarkdownSource(longParagraph());
    const int wide = v.viewport()->geometry().left();
    v.resize(1600, 400);
    QApplication::processEvents();
    QVERIFY(v.viewport()->geometry().left() > wide);
    QVERIFY(v.viewport()->width() <= 600);
    v.resize(500, 400);
    QApplication::processEvents();
    QVERIFY(v.viewport()->width() >= 480);
}

void TestMarkdownView::removingTheCapRestoresFullWidth()
{
    MarkdownView v;
    v.setMaxContentWidth(500);
    showAt(v, 1200);
    v.setMarkdownSource(longParagraph());
    QVERIFY(v.viewport()->width() <= 500);
    v.setMaxContentWidth(0);
    QApplication::processEvents();
    QVERIFY(v.viewport()->width() >= 1100);
}

void TestMarkdownView::capKeepsMathAndCodeSurfacesInsideTheColumn()
{
    MarkdownView v;
    v.setMaxContentWidth(600);
    showAt(v, 1400, 600);
    v.setMarkdownSource(QStringLiteral("```\nlet a = 1;\n```\n\n$$\n\\frac{a}{b}\n$$\n"));
    QCOMPARE(imageCount(v), 1);
    QTextDocument *doc = v.document();
    QTextBlock code;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
        if (b.text().contains(QStringLiteral("let a"))) code = b;
    QVERIFY(code.isValid());
    const QRectF r = doc->documentLayout()->blockBoundingRect(code);
    QVERIFY2(r.right() <= 600.5, qPrintable(QStringLiteral("right=%1 vp=%2").arg(r.right()).arg(v.viewport()->width())));
}

void TestMarkdownView::codeSurfaceFollowsTheColumn()
{
    // The surface is painted across the VIEWPORT, so with a cap it spans the
    // column, not the whole window.
    MarkdownView v;
    v.setMaxContentWidth(500);
    QPalette pal = v.palette();
    pal.setColor(QPalette::Base, Qt::white);
    pal.setColor(QPalette::Text, Qt::black);
    v.setPalette(pal);
    showAt(v, 1200, 300);
    v.setMarkdownSource(QStringLiteral("```\nlet a = 1;\nlet b = 2;\n```"));
    QTextBlock first;
    for (QTextBlock b = v.document()->begin(); b.isValid(); b = b.next())
        if (b.text().contains(QStringLiteral("let a"))) first = b;
    QVERIFY(first.isValid());
    const QRectF a = v.document()->documentLayout()->blockBoundingRect(first);

    QImage img(v.size(), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);
    v.render(&img);
    const QPoint origin = v.viewport()->mapTo(&v, QPoint(0, 0));
    auto px = [&](double x, double y) { return img.pixelColor(origin.x() + int(x), origin.y() + int(y)); };
    const int row = int(a.top()) - 3; // padding row above the first code line
    QVERIFY2(px(v.viewport()->width() - 20, row).red() < 245, "surface reaches the column's right edge");
    // ... and nothing is painted left of the column
    QVERIFY(img.pixelColor(origin.x() - 10, origin.y() + row).red() >= 250);
}

QTextBlock blockWithText(MarkdownView &v, const QString &needle)
{
    for (QTextBlock b = v.document()->begin(); b.isValid(); b = b.next())
        if (b.text().contains(needle))
            return b;
    return QTextBlock();
}

void TestMarkdownView::flatSpacingIsTheDefault()
{
    MarkdownView v;
    v.setMarkdownSource(QStringLiteral("# Title\n\nfirst\n\nsecond\n\n- a\n- b"));
    QVERIFY(!v.documentSpacing());
    for (QTextBlock b = v.document()->begin(); b.isValid(); b = b.next()) {
        if (b.text().trimmed().isEmpty()) continue;
        QCOMPARE(b.blockFormat().topMargin(), 0.0);
        QCOMPARE(b.blockFormat().bottomMargin(), 0.0);
    }
}

void TestMarkdownView::documentSpacingSeparatesParagraphs()
{
    MarkdownView v;
    v.setDocumentSpacing(true);
    v.setMarkdownSource(QStringLiteral("first paragraph\n\nsecond paragraph\n\nthird paragraph"));
    const QTextBlock first = blockWithText(v, QStringLiteral("first"));
    const QTextBlock second = blockWithText(v, QStringLiteral("second"));
    QVERIFY(first.isValid() && second.isValid());
    QVERIFY(first.blockFormat().bottomMargin() >= 4.0);
    QVERIFY(second.blockFormat().bottomMargin() >= 4.0);
}

void TestMarkdownView::documentSpacingBindsHeadingsToWhatTheyTitle()
{
    MarkdownView v;
    v.setDocumentSpacing(true);
    v.setMarkdownSource(QStringLiteral("intro\n\n## Section\n\nbody text"));
    const QTextBlock intro = blockWithText(v, QStringLiteral("intro"));
    const QTextBlock heading = blockWithText(v, QStringLiteral("Section"));
    QVERIFY(heading.isValid());
    // more air above a heading than below it: it belongs to the text after it
    QVERIFY(heading.blockFormat().topMargin() > heading.blockFormat().bottomMargin());
    QVERIFY(heading.blockFormat().topMargin() >= 6.0);
    QVERIFY(intro.blockFormat().bottomMargin() >= 4.0);
}

void TestMarkdownView::documentSpacingKeepsListItemsTight()
{
    MarkdownView v;
    v.setDocumentSpacing(true);
    v.setMarkdownSource(QStringLiteral("before\n\n- alpha\n- beta\n- gamma\n\nafter"));
    const QTextBlock alpha = blockWithText(v, QStringLiteral("alpha"));
    const QTextBlock gamma = blockWithText(v, QStringLiteral("gamma"));
    const QTextBlock before = blockWithText(v, QStringLiteral("before"));
    QVERIFY(alpha.isValid() && gamma.isValid());
    // items sit close together, the list as a whole is separated from what follows
    QVERIFY(alpha.blockFormat().bottomMargin() < before.blockFormat().bottomMargin());
    QVERIFY(gamma.blockFormat().bottomMargin() >= before.blockFormat().bottomMargin() - 0.5);
}

void TestMarkdownView::documentSpacingLeavesCodeSurfacesAlone()
{
    MarkdownView v;
    v.setDocumentSpacing(true);
    v.setMarkdownSource(QStringLiteral("text\n\n```\nlet a = 1;\nlet b = 2;\n```\n\nafter"));
    const QTextBlock a = blockWithText(v, QStringLiteral("let a"));
    const QTextBlock b = blockWithText(v, QStringLiteral("let b"));
    QVERIFY(a.isValid() && b.isValid());
    // surface geometry (gap + padding) is not overwritten by the prose spacing
    QVERIFY(a.blockFormat().topMargin() >= 12.0);
    QVERIFY(b.blockFormat().bottomMargin() >= 12.0);
    QVERIFY(a.blockFormat().leftMargin() >= 8.0);
}

void TestMarkdownView::documentSpacingHasNoLeadingOrTrailingGap()
{
    MarkdownView v;
    v.setDocumentSpacing(true);
    v.setMarkdownSource(QStringLiteral("# Title\n\nonly paragraph"));
    const QTextBlock title = blockWithText(v, QStringLiteral("Title"));
    const QTextBlock last = blockWithText(v, QStringLiteral("only paragraph"));
    QCOMPARE(title.blockFormat().topMargin(), 0.0);   // nothing above the first block
    QCOMPARE(last.blockFormat().bottomMargin(), 0.0); // nothing below the last
}

void TestMarkdownView::togglingDocumentSpacingReRenders()
{
    MarkdownView v;
    v.setMarkdownSource(QStringLiteral("first\n\nsecond"));
    QCOMPARE(blockWithText(v, QStringLiteral("first")).blockFormat().bottomMargin(), 0.0);
    v.setDocumentSpacing(true);
    QVERIFY(blockWithText(v, QStringLiteral("first")).blockFormat().bottomMargin() >= 4.0);
    v.setDocumentSpacing(false);
    QCOMPARE(blockWithText(v, QStringLiteral("first")).blockFormat().bottomMargin(), 0.0);
}

QTEST_MAIN(TestMarkdownView)
#include "test_markdown_view.moc"
