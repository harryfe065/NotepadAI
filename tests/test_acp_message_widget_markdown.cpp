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
#include <QFontInfo>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QUrl>

#include "AcpMessageWidget.h"
#include "markdown/MarkdownPreprocessor.h"

namespace {

QTextBrowser *browserOf(AcpMessageWidget &w)
{
    return w.findChild<QTextBrowser *>();
}

QString shown(AcpMessageWidget &w)
{
    return browserOf(w)->toPlainText();
}

struct ImageInfo
{
    int count = 0;
    QString firstName;
    double firstWidth = 0;
    QTextCharFormat::VerticalAlignment align = QTextCharFormat::AlignNormal;
};

ImageInfo images(AcpMessageWidget &w)
{
    ImageInfo info;
    QTextDocument *doc = browserOf(w)->document();
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it) {
            const QTextCharFormat cf = it.fragment().charFormat();
            if (!cf.isImageFormat()) continue;
            const QTextImageFormat f = cf.toImageFormat();
            if (info.count++ == 0) {
                info.firstName = f.name();
                info.firstWidth = f.width();
                info.align = f.verticalAlignment();
            }
        }
    }
    return info;
}

Qt::Alignment alignmentOfBlockContaining(AcpMessageWidget &w, const QString &needle)
{
    QTextDocument *doc = browserOf(w)->document();
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
        if (b.text().contains(needle))
            return b.blockFormat().alignment();
    return Qt::Alignment();
}

void clickAnchor(AcpMessageWidget &w, const QString &url)
{
    emit browserOf(w)->anchorClicked(QUrl(url));
}

const QString kDetails = QStringLiteral(
    "<details>\n<summary>Bấm để mở</summary>\nNội dung ẩn\n</details>");

} // namespace

class TestAcpMessageWidgetMarkdown : public QObject
{
    Q_OBJECT

private slots:
    void plainTextTakesTheUnchangedPath();
    void genericsStayLiteral();

    void inlineMathBecomesAMiddleAlignedImage();
    void vectorAndNormMessageRendersBothFormulas();
    void displayMathIsCentered();
    void mathFenceRendersAsDisplayMath();
    void backtickHeadingDoesNotBreakFollowingMathBlocks();
    void plainCodeFenceIsNotMath();
    void codeBlocksGetSurfaceFill();
    void adjacentFencesAreSeparateRegions();
    void inlineCodeGetsSurfaceFill();
    void codeBlockFontIsFixedPitchEvenWhenChatFontIsNot();
    void codeSurfaceIsPaintedPaddedAndRounded();
    void blockMarginsAreZeroedLikeBefore();
    void unsupportedMathStaysAsSource();
    void priceIsNotMath();
    void mathIsNotRenderedInsideCode();

    void detailsCollapsedByDefault();
    void detailsOpenAttribute();
    void detailsToggleOnClick();
    void detailsStateSurvivesStreamedChunks();
    void detailsBodyRendersMarkdownAndMath();

    void alignCenterAppliesAndRemovesSentinels();
    void forgedSentinelDoesNotAlign();

    void unknownSchemeLinkIsIgnored();
    void thoughtRoleStaysRawText();
};

void TestAcpMessageWidgetMarkdown::plainTextTakesTheUnchangedPath()
{
    AcpMessageWidget w(QStringLiteral("assistant"));
    w.setText(QStringLiteral("hello **world**"));
    QCOMPARE(shown(w).trimmed(), QStringLiteral("hello world"));
    QCOMPARE(images(w).count, 0);
}

void TestAcpMessageWidgetMarkdown::genericsStayLiteral()
{
    AcpMessageWidget w(QStringLiteral("assistant"));
    w.setText(QStringLiteral("Use Surreal<Db> and a < b."));
    QVERIFY(shown(w).contains(QStringLiteral("Surreal<Db>")));
    QVERIFY(shown(w).contains(QStringLiteral("a < b")));
}

// Reported: "Vector: $\vec{v}$, $\lVert v \rVert$" rendered only the first formula.
void TestAcpMessageWidgetMarkdown::vectorAndNormMessageRendersBothFormulas()
{
    AcpMessageWidget w(QStringLiteral("assistant"));
    w.setText(QStringLiteral("Vector: $\\vec{v}$, $\\lVert v \\rVert$ and $\\langle u, v \\rangle$"));
    QCOMPARE(images(w).count, 3);
    QVERIFY(!shown(w).contains(QStringLiteral("lVert")));
    QVERIFY(!shown(w).contains(QStringLiteral("langle")));
    QVERIFY(!shown(w).contains(QLatin1Char('$')));
}

void TestAcpMessageWidgetMarkdown::inlineMathBecomesAMiddleAlignedImage()
{
    AcpMessageWidget w(QStringLiteral("assistant"));
    w.resize(400, 100);
    w.setText(QStringLiteral("Energy $E = mc^2$ here"));
    const ImageInfo img = images(w);
    QCOMPARE(img.count, 1);
    QCOMPARE(img.firstName, QStringLiteral("math:0"));
    QVERIFY(img.firstWidth > 10);
    QCOMPARE(img.align, QTextCharFormat::AlignMiddle);
    QVERIFY(!shown(w).contains(QLatin1Char('$')));
    QVERIFY(shown(w).contains(QStringLiteral("Energy")));
    QVERIFY(shown(w).contains(QStringLiteral("here")));
}

void TestAcpMessageWidgetMarkdown::displayMathIsCentered()
{
    AcpMessageWidget w(QStringLiteral("assistant"));
    w.setText(QStringLiteral("before\n\n$$\\int_0^1 x^2\\,dx$$\n\nafter"));
    QCOMPARE(images(w).count, 1);
    QTextDocument *doc = browserOf(w)->document();
    bool found = false;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it) {
            if (it.fragment().charFormat().isImageFormat()) {
                found = true;
                QVERIFY(b.blockFormat().alignment() & Qt::AlignHCenter);
            }
        }
    }
    QVERIFY(found);
}

void TestAcpMessageWidgetMarkdown::mathFenceRendersAsDisplayMath()
{
    AcpMessageWidget w(QStringLiteral("assistant"));
    w.setText(QStringLiteral("intro\n\n```latex\n$$\n\\Delta t = \\frac{1}{60}\n$$\n```\n\nafter"));
    QCOMPARE(images(w).count, 1);
    QVERIFY(!shown(w).contains(QStringLiteral("Delta")));
}

void TestAcpMessageWidgetMarkdown::backtickHeadingDoesNotBreakFollowingMathBlocks()
{
    // Reported: "### Block (cú pháp ```math)" made the next fence render as code.
    AcpMessageWidget w(QStringLiteral("assistant"));
    w.setText(QStringLiteral(
        "### Block (c\u00fa ph\u00e1p ```math)\n\n"
        "```math\nv_{\\text{bevy}} = \\frac{v_{\\text{C\\#}} \\times 60}{1}, \\qquad y_{\\text{bevy}} = -y_{\\text{C\\#}}\n```\n\n"
        "### Ma tr\u1eadn\n\n"
        "```\n$$\nR(\\theta) = \\begin{pmatrix}\\cos\\theta & -\\sin\\theta \\\\ \\sin\\theta & \\cos\\theta \\end{pmatrix}\n$$\n```\n"));
    QCOMPARE(images(w).count, 2);
    QVERIFY(!shown(w).contains(QStringLiteral("pmatrix")));
    QVERIFY(!shown(w).contains(QStringLiteral("\\frac")));
    QVERIFY(shown(w).contains(QStringLiteral("```math")));  // the heading text itself
}

// Regression: on Windows Qt resolves CSS "monospace" to the system fixed font
// ("Courier New") and reports fontFixedPitch()==false, so code detection keyed on
// the name "monospace" found nothing — no fill, no spacing, no copy region.
void TestAcpMessageWidgetMarkdown::adjacentFencesAreSeparateRegions()
{
    AcpMessageWidget w(QStringLiteral("assistant"));
    w.setText(QStringLiteral("```rust\nlet a = 1;\n```\n\n```\njust check\n```"));
    QTextDocument *doc = browserOf(w)->document();
    QTextBlock first, second;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        if (b.text().contains(QStringLiteral("let a"))) first = b;
        if (b.text().contains(QStringLiteral("just check"))) second = b;
    }
    QVERIFY(first.isValid() && second.isValid());
    // some non-code block sits between the two code blocks ...
    bool gap = false;
    for (QTextBlock b = first.next(); b.isValid() && b != second; b = b.next())
        gap = gap || b.blockFormat().background().style() == Qt::NoBrush;
    QVERIFY(gap);
    // ... and it only keeps them apart: the space between the two code lines is
    // the two surfaces' padding plus a modest gap, not an extra blank text line
    const double apart = doc->documentLayout()->blockBoundingRect(second).top()
                         - doc->documentLayout()->blockBoundingRect(first).bottom();
    const QFontMetricsF fm(browserOf(w)->document()->defaultFont());
    QVERIFY2(apart < 2 * 14 + fm.height() * 0.5, qPrintable(QString::number(apart)));
}

void TestAcpMessageWidgetMarkdown::inlineCodeGetsSurfaceFill()
{
    AcpMessageWidget w(QStringLiteral("assistant"));
    QFont chat(QStringLiteral("Segoe UI"));
    chat.setPixelSize(14);
    w.setChatFont(chat);
    w.setText(QStringLiteral("use `inline_call()` here"));
    QTextDocument *doc = browserOf(w)->document();
    bool found = false;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (f.text().contains(QStringLiteral("inline_call"))) {
                found = true;
                QVERIFY(f.charFormat().background().style() != Qt::NoBrush);
            } else if (f.text().contains(QStringLiteral("use"))) {
                QVERIFY(f.charFormat().background().style() == Qt::NoBrush);
            }
        }
    }
    QVERIFY(found);
}

void TestAcpMessageWidgetMarkdown::codeBlocksGetSurfaceFill()
{
    AcpMessageWidget w(QStringLiteral("assistant"));
    QFont chat(QStringLiteral("Segoe UI"));
    chat.setPixelSize(14);
    w.setChatFont(chat);
    w.setText(QStringLiteral("before\n\n```rust\nfn main() {}\n\nlet a = 1;\n```\n\nafter"));
    QTextDocument *doc = browserOf(w)->document();
    int filled = 0, prose = 0;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        const bool codeLine = b.text().contains(QStringLiteral("fn main"))
                              || b.text().contains(QStringLiteral("let a"));
        const bool hasFill = b.blockFormat().background().style() != Qt::NoBrush;
        if (codeLine) {
            QVERIFY2(hasFill, qPrintable(b.text()));
            QCOMPARE(b.blockFormat().lineHeightType(), int(QTextBlockFormat::SingleHeight));
            ++filled;
        } else if (b.text() == QStringLiteral("before") || b.text() == QStringLiteral("after")) {
            QVERIFY2(!hasFill, qPrintable(b.text()));
            ++prose;
        }
    }
    QCOMPARE(filled, 2);
    QCOMPARE(prose, 2);
}

void TestAcpMessageWidgetMarkdown::codeBlockFontIsFixedPitchEvenWhenChatFontIsNot()
{
    AcpMessageWidget w(QStringLiteral("assistant"));
    QFont chat(QStringLiteral("Segoe UI"));
    chat.setPixelSize(14);
    w.setChatFont(chat);
    w.setText(QStringLiteral("prose\n\n```\nlet a = 1;\n```\n"));
    QTextDocument *doc = browserOf(w)->document();
    bool checked = false;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        if (!b.text().contains(QStringLiteral("let a"))) continue;
        for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it) {
            const QFont f = it.fragment().charFormat().font();
            QVERIFY2(QFontInfo(f).fixedPitch(), qPrintable(QFontInfo(f).family()));
            checked = true;
        }
    }
    QVERIFY(checked);
}

// The code surface is painted by the browser itself (Qt block backgrounds can
// neither pad nor round), so check real pixels, not block formats.
void TestAcpMessageWidgetMarkdown::codeSurfaceIsPaintedPaddedAndRounded()
{
    AcpMessageWidget w(QStringLiteral("assistant"));
    w.setAttribute(Qt::WA_DontShowOnScreen, true);
    QFont chat(QStringLiteral("Segoe UI"));
    chat.setPixelSize(14);
    w.setChatFont(chat);
    w.resize(400, 100);
    w.setText(QStringLiteral("before\n\n```\nlet a = 1;\nlet b = 2;\n```\n\nafter"));
    w.show();
    QApplication::processEvents();
    w.resize(400, w.height());
    QApplication::processEvents();

    QTextBrowser *br = browserOf(w);
    QTextDocument *doc = br->document();
    QTextBlock first, last;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        if (b.text().contains(QStringLiteral("let a"))) first = b;
        if (b.text().contains(QStringLiteral("let b"))) last = b;
    }
    QVERIFY(first.isValid() && last.isValid());

    // padding: text is inset from the surface on the left, and the surface extends
    // above the first line and below the last
    QVERIFY(first.blockFormat().leftMargin() >= 8);
    QVERIFY(first.blockFormat().topMargin() >= 8);
    QVERIFY(last.blockFormat().bottomMargin() >= 8);
    // no extra leading between the code lines (they read as one block)
    QCOMPARE(first.blockFormat().lineHeightType(), int(QTextBlockFormat::SingleHeight));

    const QRectF a = doc->documentLayout()->blockBoundingRect(first);
    const QRectF z = doc->documentLayout()->blockBoundingRect(last);
    QImage img(w.size(), QImage::Format_ARGB32_Premultiplied);
    img.fill(w.palette().color(QPalette::Base));
    w.render(&img);
    const QPoint origin = br->viewport()->mapTo(&w, QPoint(0, 0));
    auto px = [&](double x, double y) {
        return img.pixelColor(origin.x() + int(x), origin.y() + int(y));
    };
    auto dist = [](const QColor &p, const QColor &q) {
        return qAbs(p.red() - q.red()) + qAbs(p.green() - q.green()) + qAbs(p.blue() - q.blue());
    };
    const QColor bg = w.palette().color(QPalette::Base);
    const int vw = br->viewport()->width();

    // the surface covers the padding above the first line and below the last
    const int padTop = int(a.top()) - 3;
    const int padBottom = int(z.bottom()) + 3;
    QVERIFY2(dist(px(vw / 2, padTop), bg) >= 24, "padding above the first line is filled");
    QVERIFY2(dist(px(vw / 2, padBottom), bg) >= 24, "padding below the last line is filled");
    // ... and the seam between two code lines is filled too (no stripes)
    QVERIFY2(dist(px(vw - 10, (a.bottom() + z.top()) / 2.0), bg) >= 24, "no seam between lines");
    // beyond the surface it is the bubble background again
    QVERIFY2(dist(px(vw / 2, int(a.top()) - 14), bg) < 8, "gap above the surface");

    // rounded: the very corner of the surface is NOT painted, while the same row
    // a few pixels in is
    const int surfaceTop = int(a.top()) - 6;
    QVERIFY2(dist(px(1, surfaceTop + 1), bg) < 8, "top-left corner is rounded off");
    QVERIFY2(dist(px(10, surfaceTop + 3), bg) >= 8, "surface edge row is painted");
}

void TestAcpMessageWidgetMarkdown::plainCodeFenceIsNotMath()
{
    AcpMessageWidget w(QStringLiteral("assistant"));
    w.setText(QStringLiteral("```\nlet a = 1;\n```"));
    QCOMPARE(images(w).count, 0);
    QVERIFY(shown(w).contains(QStringLiteral("let a = 1;")));
}

void TestAcpMessageWidgetMarkdown::blockMarginsAreZeroedLikeBefore()
{
    // Prose blocks keep the original flat layout: no added paragraph gap and no
    // proportional line height (that spacing pass was reverted).
    AcpMessageWidget w(QStringLiteral("assistant"));
    w.setText(QStringLiteral("## Title\n\nfirst paragraph\n\nsecond paragraph\n\n- one\n- two"));
    QTextDocument *doc = browserOf(w)->document();
    int checked = 0;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        if (b.text().trimmed().isEmpty()) continue;
        QCOMPARE(b.blockFormat().topMargin(), 0.0);
        QCOMPARE(b.blockFormat().bottomMargin(), 0.0);
        QCOMPARE(b.blockFormat().lineHeightType(), int(QTextBlockFormat::SingleHeight));
        ++checked;
    }
    QVERIFY(checked >= 5);
}

void TestAcpMessageWidgetMarkdown::unsupportedMathStaysAsSource()
{
    AcpMessageWidget w(QStringLiteral("assistant"));
    w.setText(QStringLiteral("see $\\foo{x}$ and $y$"));
    QVERIFY(shown(w).contains(QStringLiteral("\\foo{x}")));
    QCOMPARE(images(w).count, 1); // only $y$
}

void TestAcpMessageWidgetMarkdown::priceIsNotMath()
{
    AcpMessageWidget w(QStringLiteral("assistant"));
    w.setText(QStringLiteral("costs $5 and $10"));
    QCOMPARE(images(w).count, 0);
    QVERIFY(shown(w).contains(QStringLiteral("$5 and $10")));
}

void TestAcpMessageWidgetMarkdown::mathIsNotRenderedInsideCode()
{
    AcpMessageWidget w(QStringLiteral("assistant"));
    w.setText(QStringLiteral("```\n$x$ <b>y</b>\n```\nand `$z$`"));
    QCOMPARE(images(w).count, 0);
    QVERIFY(shown(w).contains(QStringLiteral("$x$ <b>y</b>")));
    QVERIFY(shown(w).contains(QStringLiteral("$z$")));
}

void TestAcpMessageWidgetMarkdown::detailsCollapsedByDefault()
{
    AcpMessageWidget w(QStringLiteral("assistant"));
    w.setText(kDetails);
    QVERIFY(shown(w).contains(QStringLiteral("Bấm để mở")));
    QVERIFY(shown(w).contains(QChar(0x25B8)));
    QVERIFY(!shown(w).contains(QStringLiteral("Nội dung ẩn")));
    QVERIFY(!shown(w).contains(QStringLiteral("<details")));
}

void TestAcpMessageWidgetMarkdown::detailsOpenAttribute()
{
    AcpMessageWidget w(QStringLiteral("assistant"));
    w.setText(QStringLiteral("<details open>\n<summary>T</summary>\nbody text\n</details>"));
    QVERIFY(shown(w).contains(QChar(0x25BE)));
    QVERIFY(shown(w).contains(QStringLiteral("body text")));
}

void TestAcpMessageWidgetMarkdown::detailsToggleOnClick()
{
    AcpMessageWidget w(QStringLiteral("assistant"));
    w.setText(kDetails);
    clickAnchor(w, QStringLiteral("nn-details:0"));
    QVERIFY(shown(w).contains(QStringLiteral("Nội dung ẩn")));
    QVERIFY(shown(w).contains(QChar(0x25BE)));
    clickAnchor(w, QStringLiteral("nn-details:0"));
    QVERIFY(!shown(w).contains(QStringLiteral("Nội dung ẩn")));
    QVERIFY(shown(w).contains(QChar(0x25B8)));
}

void TestAcpMessageWidgetMarkdown::detailsStateSurvivesStreamedChunks()
{
    AcpMessageWidget w(QStringLiteral("assistant"));
    w.setText(kDetails);
    clickAnchor(w, QStringLiteral("nn-details:0"));
    w.appendChunk(QStringLiteral("\n\nmore streamed text"));
    QTest::qWait(150); // debounce
    QVERIFY(shown(w).contains(QStringLiteral("more streamed text")));
    QVERIFY(shown(w).contains(QStringLiteral("Nội dung ẩn")));
}

void TestAcpMessageWidgetMarkdown::detailsBodyRendersMarkdownAndMath()
{
    AcpMessageWidget w(QStringLiteral("assistant"));
    w.setText(QStringLiteral("<details open><summary>T</summary>\nsee $x$ and <b>bold</b>\n</details>"));
    QCOMPARE(images(w).count, 1);
    QVERIFY(shown(w).contains(QStringLiteral("bold")));
    QVERIFY(!shown(w).contains(QStringLiteral("<b>")));
}

void TestAcpMessageWidgetMarkdown::alignCenterAppliesAndRemovesSentinels()
{
    AcpMessageWidget w(QStringLiteral("assistant"));
    w.setText(QStringLiteral("left text\n\n<div align=\"center\">Khối căn giữa</div>\n\nafter"));
    QVERIFY(alignmentOfBlockContaining(w, QStringLiteral("Khối căn giữa")) & Qt::AlignHCenter);
    QVERIFY(!(alignmentOfBlockContaining(w, QStringLiteral("left text")) & Qt::AlignHCenter));
    QVERIFY(!(alignmentOfBlockContaining(w, QStringLiteral("after")) & Qt::AlignHCenter));
    const QString s = shown(w);
    QVERIFY(!s.contains(MarkdownPreprocessor::kAlignBegin));
    QVERIFY(!s.contains(MarkdownPreprocessor::kAlignEnd));
    QVERIFY(!s.contains(QStringLiteral("<div")));
}

void TestAcpMessageWidgetMarkdown::forgedSentinelDoesNotAlign()
{
    AcpMessageWidget w(QStringLiteral("assistant"));
    w.setText(QString(MarkdownPreprocessor::kAlignBegin) + QStringLiteral("c\n\nplain paragraph"));
    QVERIFY(!(alignmentOfBlockContaining(w, QStringLiteral("plain paragraph")) & Qt::AlignHCenter));
}

void TestAcpMessageWidgetMarkdown::unknownSchemeLinkIsIgnored()
{
    AcpMessageWidget w(QStringLiteral("assistant"));
    w.setText(kDetails);
    const QString before = shown(w);
    clickAnchor(w, QStringLiteral("javascript:alert(1)"));
    clickAnchor(w, QStringLiteral("file:///etc/passwd"));
    clickAnchor(w, QStringLiteral("nn-details:notanumber"));
    QCOMPARE(shown(w), before);
}

void TestAcpMessageWidgetMarkdown::thoughtRoleStaysRawText()
{
    AcpMessageWidget w(QStringLiteral("thought"));
    w.setText(QStringLiteral("think $x$ and <b>y</b>"));
    QVERIFY(shown(w).contains(QStringLiteral("$x$ and <b>y</b>")));
    QCOMPARE(images(w).count, 0);
}

QTEST_MAIN(TestAcpMessageWidgetMarkdown)
#include "test_acp_message_widget_markdown.moc"
