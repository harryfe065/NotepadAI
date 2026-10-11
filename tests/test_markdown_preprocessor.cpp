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
#include <QString>

#include "markdown/MarkdownPreprocessor.h"

using namespace MarkdownPreprocessor;

namespace {

QString alignBegin(QChar kind)
{
    return QStringLiteral("\n\n") + kAlignBegin + kind + QStringLiteral("\n\n");
}
QString alignEnd()
{
    return QStringLiteral("\n\n") + kAlignEnd + QStringLiteral("\n\n");
}

} // namespace

class TestMarkdownPreprocessor : public QObject
{
    Q_OBJECT

private slots:
    // fast path / untouched text
    void plainTextIsUnchanged();
    void genericsAndComparisonsStayLiteral();

    // math delimiters
    void inlineDollar();
    void inlineParens();
    void displayDollarMultiLine();
    void displayBrackets();
    void displayInsideSentenceGetsOwnParagraph();
    void pricesAndShellVariablesStayLiteral();
    void escapedDollarStaysLiteral();
    void dollarWithInnerSpacesIsNotMath();
    void closerFollowedByDigitIsNotMath();
    void emptyDisplayIsNotMath();
    void twoInlineFormulasOnOneLine();
    void priceBeforeFormulaDoesNotSwallowIt();
    void inlineMathDoesNotSpanLines();
    void mathAcceptCallbackCanReject();
    void formulaCountAndLengthAreCapped();

    // math fences (GitHub ```math convention)
    void mathFenceBecomesDisplayMath();
    void latexAndTexFencesAreMath();
    void mathFenceMayWrapDollarDelimiters();
    void unlabeledFenceWithOnlyDisplayMathIsMath();
    void unlabeledFenceWithOtherContentStaysCode();
    void unsupportedMathFenceStaysCode();
    void unclosedMathFenceStaysLiteral();
    void tildeMathFence();
    void mathFenceInsideTextKeepsSurroundings();

    // inline code spans must not swallow later constructs
    void headingWithBacktickRunDoesNotSwallowNextFence();
    void codeSpanDoesNotCrossBlankLine();
    void codeSpanDoesNotCrossHeadingLine();
    void fenceInterruptsParagraphWithOpenBacktick();
    void spanAcrossSingleNewlineStillCloses();
    void matrixInUnlabelledFenceRenders();
    void unclosedBacktickRunDoesNotHideFollowingMath();

    // adjacent fences must stay visually separate blocks
    void adjacentFencesGetASeparatorParagraph();
    void singleFenceIsUntouched();
    void fenceFollowedByTextIsUntouched();

    // code and link contexts
    void fencedCodeIsVerbatim();
    void inlineCodeIsVerbatim();
    void linkUrlIsNotMath();

    // streaming partials
    void unclosedDisplayStaysLiteral();
    void unclosedBlockTagsStayLiteral();

    // inline html
    void boldItalicStrike();
    void kbdAndCode();
    void emphasisWhitespaceMovesOutsideMarkers();
    void lineBreak();
    void commentsAreRemoved();
    void linksAllowlist();
    void unsafeHtmlStaysLiteral();
    void subSupUnicode();

    // blocks
    void alignDivCenter();
    void alignVariants();
    void divWithoutAlignStaysLiteral();
    void blockTagMustStartLine();
    void forgedSentinelsAreStripped();
    void detailsCollapsedByDefault();
    void detailsOpenAttribute();
    void detailsFlippedState();
    void detailsOrdinalsSurviveCollapsedNesting();
    void detailsBodyIsProcessedRecursively();
    void detailsSummaryBracketsEscaped();
};

void TestMarkdownPreprocessor::plainTextIsUnchanged()
{
    const QString in = QStringLiteral("just some **markdown**\nwith lines");
    const Result r = run(in);
    QCOMPARE(r.markdown, in);
    QVERIFY(!r.changed);
    QVERIFY(!r.hasDetails);
    QVERIFY(r.math.isEmpty());
}

void TestMarkdownPreprocessor::genericsAndComparisonsStayLiteral()
{
    const QString in = QStringLiteral("Use Surreal<Db> and vector<int>; also a < b and x > y.");
    const Result r = run(in);
    QCOMPARE(r.markdown, in);
    QVERIFY(!r.changed);
}

void TestMarkdownPreprocessor::inlineDollar()
{
    const Result r = run(QStringLiteral("Energy $E = mc^2$ here"));
    QCOMPARE(r.markdown, QStringLiteral("Energy ![formula](math:0) here"));
    QCOMPARE(r.math.size(), 1);
    QCOMPARE(r.math.at(0).source, QStringLiteral("E = mc^2"));
    QVERIFY(!r.math.at(0).display);
    QVERIFY(r.changed);
}

void TestMarkdownPreprocessor::inlineParens()
{
    const Result r = run(QStringLiteral("so \\(x_1 + y\\) holds"));
    QCOMPARE(r.markdown, QStringLiteral("so ![formula](math:0) holds"));
    QCOMPARE(r.math.at(0).source, QStringLiteral("x_1 + y"));
    QVERIFY(!r.math.at(0).display);
}

void TestMarkdownPreprocessor::displayDollarMultiLine()
{
    const Result r = run(QStringLiteral("$$\n\\int_0^1 x^2\\,dx\n$$"));
    QCOMPARE(r.markdown, QStringLiteral("\n\n![formula](math:0)\n\n"));
    QCOMPARE(r.math.size(), 1);
    QCOMPARE(r.math.at(0).source, QStringLiteral("\\int_0^1 x^2\\,dx"));
    QVERIFY(r.math.at(0).display);
}

void TestMarkdownPreprocessor::displayBrackets()
{
    const Result r = run(QStringLiteral("\\[ a + b \\]"));
    QCOMPARE(r.markdown, QStringLiteral("\n\n![formula](math:0)\n\n"));
    QCOMPARE(r.math.at(0).source, QStringLiteral("a + b"));
    QVERIFY(r.math.at(0).display);
}

void TestMarkdownPreprocessor::displayInsideSentenceGetsOwnParagraph()
{
    const Result r = run(QStringLiteral("x $$a$$ y"));
    QCOMPARE(r.markdown, QStringLiteral("x \n\n![formula](math:0)\n\n y"));
}

void TestMarkdownPreprocessor::pricesAndShellVariablesStayLiteral()
{
    for (const QString &in : {QStringLiteral("costs $5 and $10"),
                              QStringLiteral("echo $HOME $PATH"),
                              QStringLiteral("a single $ sign")}) {
        const Result r = run(in);
        QCOMPARE(r.markdown, in);
        QVERIFY(r.math.isEmpty());
    }
}

void TestMarkdownPreprocessor::escapedDollarStaysLiteral()
{
    const QString in = QStringLiteral("\\$5 and \\$6");
    const Result r = run(in);
    QCOMPARE(r.markdown, in);
    QVERIFY(r.math.isEmpty());
}

void TestMarkdownPreprocessor::dollarWithInnerSpacesIsNotMath()
{
    const QString in = QStringLiteral("$ x $ and $y $");
    const Result r = run(in);
    QCOMPARE(r.markdown, in);
}

void TestMarkdownPreprocessor::closerFollowedByDigitIsNotMath()
{
    const QString in = QStringLiteral("$a$5");
    const Result r = run(in);
    QCOMPARE(r.markdown, in);
}

void TestMarkdownPreprocessor::emptyDisplayIsNotMath()
{
    const QString in = QStringLiteral("$$ $$");
    const Result r = run(in);
    QCOMPARE(r.markdown, in);
}

void TestMarkdownPreprocessor::twoInlineFormulasOnOneLine()
{
    const Result r = run(QStringLiteral("$a$ and $b$"));
    QCOMPARE(r.markdown, QStringLiteral("![formula](math:0) and ![formula](math:1)"));
    QCOMPARE(r.math.size(), 2);
    QCOMPARE(r.math.at(1).source, QStringLiteral("b"));
}

void TestMarkdownPreprocessor::priceBeforeFormulaDoesNotSwallowIt()
{
    const Result r = run(QStringLiteral("costs $5 and $10, then $x$"));
    QCOMPARE(r.markdown, QStringLiteral("costs $5 and $10, then ![formula](math:0)"));
    QCOMPARE(r.math.at(0).source, QStringLiteral("x"));
}

void TestMarkdownPreprocessor::inlineMathDoesNotSpanLines()
{
    const QString in = QStringLiteral("first $a\nb$ second");
    const Result r = run(in);
    QCOMPARE(r.markdown, in);
}

void TestMarkdownPreprocessor::mathAcceptCallbackCanReject()
{
    int calls = 0;
    const MathAccept reject = [&calls](const QString &, bool) {
        ++calls;
        return false;
    };
    const QString in = QStringLiteral("$\\foo{x}$ and $$y$$");
    const Result r = run(in, {}, reject);
    QCOMPARE(r.markdown, in);
    QVERIFY(r.math.isEmpty());
    QCOMPARE(calls, 2);
}

void TestMarkdownPreprocessor::formulaCountAndLengthAreCapped()
{
    QString many;
    for (int i = 0; i < kMaxFormulas + 5; ++i)
        many += QStringLiteral("$x$ ");
    const Result r = run(many);
    QCOMPARE(r.math.size(), kMaxFormulas);
    QVERIFY(r.markdown.endsWith(QStringLiteral("$x$ $x$ $x$ $x$ $x$ ")));

    const QString big = QStringLiteral("$") + QString(kMaxFormulaChars + 1, QLatin1Char('a'))
                        + QStringLiteral("$");
    const Result r2 = run(big);
    QCOMPARE(r2.markdown, big);
    QVERIFY(r2.math.isEmpty());
}

void TestMarkdownPreprocessor::mathFenceBecomesDisplayMath()
{
    const Result r = run(QStringLiteral("```math\n\\frac{a}{b}\n```"));
    QCOMPARE(r.markdown, QStringLiteral("\n\n![formula](math:0)\n\n"));
    QCOMPARE(r.math.size(), 1);
    QCOMPARE(r.math.at(0).source, QStringLiteral("\\frac{a}{b}"));
    QVERIFY(r.math.at(0).display);
}

void TestMarkdownPreprocessor::latexAndTexFencesAreMath()
{
    for (const char *lang : {"latex", "tex", "LaTeX", "katex"}) {
        const Result r = run(QStringLiteral("```%1\nx^2\n```").arg(QLatin1String(lang)));
        QCOMPARE(r.math.size(), 1);
        QVERIFY(r.math.at(0).display);
    }
}

void TestMarkdownPreprocessor::mathFenceMayWrapDollarDelimiters()
{
    const Result r = run(QStringLiteral("```latex\n$$\n\\Delta t = 1\n$$\n```"));
    QCOMPARE(r.math.size(), 1);
    QCOMPARE(r.math.at(0).source, QStringLiteral("\\Delta t = 1"));
}

void TestMarkdownPreprocessor::unlabeledFenceWithOnlyDisplayMathIsMath()
{
    const Result r = run(QStringLiteral("```\n$$\nv_{a} = \\frac{b}{c}\n$$\n```"));
    QCOMPARE(r.math.size(), 1);
    QCOMPARE(r.math.at(0).source, QStringLiteral("v_{a} = \\frac{b}{c}"));

    const Result bracket = run(QStringLiteral("```\n\\[ x^2 \\]\n```"));
    QCOMPARE(bracket.math.size(), 1);
}

void TestMarkdownPreprocessor::unlabeledFenceWithOtherContentStaysCode()
{
    for (const QString &in : {QStringLiteral("```\n$x$ and more\n```"),
                              QStringLiteral("```\nx^2\n```"),
                              QStringLiteral("```\n$$a$$ then $$b$$\n```"),
                              QStringLiteral("```rust\n$$a$$\n```"),
                              QStringLiteral("```bash\necho $$\n```")}) {
        const Result r = run(in);
        QCOMPARE(r.markdown, in);
        QVERIFY(r.math.isEmpty());
    }
}

void TestMarkdownPreprocessor::unsupportedMathFenceStaysCode()
{
    const QString in = QStringLiteral("```latex\n\\documentclass{article}\n```");
    const MathAccept reject = [](const QString &, bool) { return false; };
    const Result r = run(in, {}, reject);
    QCOMPARE(r.markdown, in);
    QVERIFY(r.math.isEmpty());
}

void TestMarkdownPreprocessor::unclosedMathFenceStaysLiteral()
{
    const QString in = QStringLiteral("```math\n\\frac{a}{b}");
    const Result r = run(in);
    QCOMPARE(r.markdown, in);
    QVERIFY(r.math.isEmpty());
}

void TestMarkdownPreprocessor::tildeMathFence()
{
    const Result r = run(QStringLiteral("~~~math\nx\n~~~"));
    QCOMPARE(r.math.size(), 1);
}

void TestMarkdownPreprocessor::mathFenceInsideTextKeepsSurroundings()
{
    const Result r = run(QStringLiteral("before\n```math\nx\n```\nafter"));
    QCOMPARE(r.markdown, QStringLiteral("before\n\n\n![formula](math:0)\n\n\nafter"));
}

void TestMarkdownPreprocessor::headingWithBacktickRunDoesNotSwallowNextFence()
{
    // The heading's literal ``` used to pair with the next fence's opener.
    const QString in = QStringLiteral(
        "### Block (c\u00fa ph\u00e1p ```math)\n\n```math\n\\frac{a}{b}\n```\n\n### Next\n");
    const Result r = run(in);
    QCOMPARE(r.math.size(), 1);
    QVERIFY(r.markdown.contains(QStringLiteral("### Block (c\u00fa ph\u00e1p ```math)")));
    QVERIFY(r.markdown.contains(QStringLiteral("![formula](math:0)")));
    QVERIFY(!r.markdown.contains(QStringLiteral("\\frac")));
}

void TestMarkdownPreprocessor::codeSpanDoesNotCrossBlankLine()
{
    const Result r = run(QStringLiteral("text `unclosed\n\nthen $x$ here `later`"));
    QCOMPARE(r.math.size(), 1);
    QVERIFY(r.markdown.contains(QStringLiteral("`later`")));
}

void TestMarkdownPreprocessor::codeSpanDoesNotCrossHeadingLine()
{
    const Result r = run(QStringLiteral("## a ` b\n$x$ and ` c"));
    QCOMPARE(r.math.size(), 1);
}

void TestMarkdownPreprocessor::fenceInterruptsParagraphWithOpenBacktick()
{
    const Result r = run(QStringLiteral("intro `` oops\n```math\nx\n```"));
    QCOMPARE(r.math.size(), 1);
}

void TestMarkdownPreprocessor::spanAcrossSingleNewlineStillCloses()
{
    // a code span may wrap one line within a paragraph: its content stays verbatim
    const QString in = QStringLiteral("see `a $b$\nc` end");
    const Result r = run(in);
    QCOMPARE(r.markdown, in);
    QVERIFY(r.math.isEmpty());
}

void TestMarkdownPreprocessor::matrixInUnlabelledFenceRenders()
{
    const Result r = run(QStringLiteral(
        "## Ma tr\u1eadn\n\n```\n$$\nR(\\theta) = \\begin{pmatrix}\\cos\\theta & -\\sin\\theta \\\\ "
        "\\sin\\theta & \\cos\\theta \\end{pmatrix}\n$$\n```\n"));
    QCOMPARE(r.math.size(), 1);
    QVERIFY(r.math.at(0).source.startsWith(QStringLiteral("R(\\theta)")));
    QVERIFY(r.math.at(0).source.contains(QStringLiteral("pmatrix")));
}

void TestMarkdownPreprocessor::unclosedBacktickRunDoesNotHideFollowingMath()
{
    const Result r = run(QStringLiteral("a ``` b\n\n$$\nx\n$$"));
    QCOMPARE(r.math.size(), 1);
}

void TestMarkdownPreprocessor::adjacentFencesGetASeparatorParagraph()
{
    // Qt renders back-to-back fences as one run of code lines; a zero-width
    // paragraph between them keeps them two blocks.
    const QString sep = QStringLiteral("\n\n\u200B\n");
    const QString in = QStringLiteral("```rust\nlet a = 1;\n```\n\n```\njust check\n```");
    QCOMPARE(run(in).markdown,
             QStringLiteral("```rust\nlet a = 1;\n```") + sep
                 + QStringLiteral("\n\n```\njust check\n```"));
    // three in a row, no blank line between them
    const QString three = QStringLiteral("```\na\n```\n```\nb\n```\n```\nc\n```");
    QCOMPARE(run(three).markdown.count(QChar(0x200B)), 2);
}

void TestMarkdownPreprocessor::singleFenceIsUntouched()
{
    const QString in = QStringLiteral("text\n\n```rust\nlet a = 1;\n```\n");
    QCOMPARE(run(in).markdown, in);
}

void TestMarkdownPreprocessor::fenceFollowedByTextIsUntouched()
{
    const QString in = QStringLiteral("```\na\n```\n\nparagraph\n\n```\nb\n```");
    QCOMPARE(run(in).markdown, in);
}

void TestMarkdownPreprocessor::fencedCodeIsVerbatim()
{
    const QString in = QStringLiteral("```\n$x$ <b>y</b> $$z$$\n```\nafter");
    const Result r = run(in);
    QCOMPARE(r.markdown, in);
    QVERIFY(r.math.isEmpty());

    const QString tilde = QStringLiteral("~~~rust\nlet a = $b$;\n~~~");
    QCOMPARE(run(tilde).markdown, tilde);
}

void TestMarkdownPreprocessor::inlineCodeIsVerbatim()
{
    const QString in = QStringLiteral("use `$x$` and `<b>y</b>` here");
    const Result r = run(in);
    QCOMPARE(r.markdown, in);

    const QString dbl = QStringLiteral("``a $b$ ` c``");
    QCOMPARE(run(dbl).markdown, dbl);
}

void TestMarkdownPreprocessor::linkUrlIsNotMath()
{
    const QString in = QStringLiteral("[t](http://e.com/$a$)");
    const Result r = run(in);
    QCOMPARE(r.markdown, in);
}

void TestMarkdownPreprocessor::unclosedDisplayStaysLiteral()
{
    const QString in = QStringLiteral("so $$ \\frac{1}{");
    const Result r = run(in);
    QCOMPARE(r.markdown, in);
    QVERIFY(r.math.isEmpty());
}

void TestMarkdownPreprocessor::unclosedBlockTagsStayLiteral()
{
    for (const QString &in : {QStringLiteral("<details><summary>x</summary>body"),
                              QStringLiteral("<div align=\"center\">text"),
                              QStringLiteral("<b>bold never closed")}) {
        const Result r = run(in);
        QCOMPARE(r.markdown, in);
    }
}

void TestMarkdownPreprocessor::boldItalicStrike()
{
    QCOMPARE(run(QStringLiteral("<b>bold</b> <strong>b2</strong>")).markdown,
             QStringLiteral("**bold** **b2**"));
    QCOMPARE(run(QStringLiteral("<i>x</i> <EM>y</EM>")).markdown,
             QStringLiteral("*x* *y*"));
    QCOMPARE(run(QStringLiteral("<s>a</s><del>b</del>")).markdown,
             QStringLiteral("~~a~~~~b~~"));
}

void TestMarkdownPreprocessor::kbdAndCode()
{
    QCOMPARE(run(QStringLiteral("press <kbd>Ctrl</kbd>+<code>C</code>")).markdown,
             QStringLiteral("press `Ctrl`+`C`"));
}

void TestMarkdownPreprocessor::emphasisWhitespaceMovesOutsideMarkers()
{
    QCOMPARE(run(QStringLiteral("<b> x </b>")).markdown, QStringLiteral(" **x** "));
}

void TestMarkdownPreprocessor::lineBreak()
{
    QCOMPARE(run(QStringLiteral("a<br>b<BR/>c<br />d")).markdown,
             QStringLiteral("a  \nb  \nc  \nd"));
}

void TestMarkdownPreprocessor::commentsAreRemoved()
{
    QCOMPARE(run(QStringLiteral("a<!-- hidden -->b")).markdown, QStringLiteral("ab"));
    // unterminated comment stays visible (streaming)
    const QString in = QStringLiteral("a<!-- not closed");
    QCOMPARE(run(in).markdown, in);
}

void TestMarkdownPreprocessor::linksAllowlist()
{
    QCOMPARE(run(QStringLiteral("<a href=\"https://x.y/z\">site</a>")).markdown,
             QStringLiteral("[site](https://x.y/z)"));
    QCOMPARE(run(QStringLiteral("<a href='mailto:a@b.c'>mail</a>")).markdown,
             QStringLiteral("[mail](mailto:a@b.c)"));
    const QString js = QStringLiteral("<a href=\"javascript:alert(1)\">x</a>");
    QCOMPARE(run(js).markdown, js);
    const QString bad = QStringLiteral("<a href=\"https://x.y/a b\">x</a>");
    QCOMPARE(run(bad).markdown, bad);
}

void TestMarkdownPreprocessor::unsafeHtmlStaysLiteral()
{
    for (const QString &in : {QStringLiteral("<script>alert(1)</script>"),
                              QStringLiteral("<img src=\"http://x/y.png\">"),
                              QStringLiteral("<table><tr><td>1</td></tr></table>"),
                              QStringLiteral("<style>a{}</style>"),
                              QStringLiteral("<span onclick=\"x()\">y</span>")}) {
        const Result r = run(in);
        QCOMPARE(r.markdown, in);
    }
}

void TestMarkdownPreprocessor::subSupUnicode()
{
    QCOMPARE(run(QStringLiteral("H<sub>2</sub>O and x<sup>2</sup>")).markdown,
             QStringLiteral("H₂O and x²"));
    const QString in = QStringLiteral("a<sup>hello</sup>");
    QCOMPARE(run(in).markdown, in);
}

void TestMarkdownPreprocessor::alignDivCenter()
{
    const Result r = run(QStringLiteral("<div align=\"center\">Khối HTML căn giữa</div>"));
    QCOMPARE(r.markdown, alignBegin(QLatin1Char('c')) + QStringLiteral("Khối HTML căn giữa")
                             + alignEnd());
    QVERIFY(r.hasAlign);
}

void TestMarkdownPreprocessor::alignVariants()
{
    QCOMPARE(run(QStringLiteral("<p align=right>x</p>")).markdown,
             alignBegin(QLatin1Char('r')) + QStringLiteral("x") + alignEnd());
    QCOMPARE(run(QStringLiteral("<center>x</center>")).markdown,
             alignBegin(QLatin1Char('c')) + QStringLiteral("x") + alignEnd());
    QCOMPARE(run(QStringLiteral("<DIV ALIGN='left'>x</DIV>")).markdown,
             alignBegin(QLatin1Char('l')) + QStringLiteral("x") + alignEnd());
    QCOMPARE(run(QStringLiteral("<div align=\"justify\">x</div>")).markdown,
             alignBegin(QLatin1Char('j')) + QStringLiteral("x") + alignEnd());
}

void TestMarkdownPreprocessor::divWithoutAlignStaysLiteral()
{
    for (const QString &in : {QStringLiteral("<div>plain</div>"),
                              QStringLiteral("<div align=\"diagonal\">x</div>"),
                              QStringLiteral("<div class=\"a\" align=\"center\">x</div>")}) {
        const Result r = run(in);
        QCOMPARE(r.markdown, in);
        QVERIFY(!r.hasAlign);
    }
}

void TestMarkdownPreprocessor::blockTagMustStartLine()
{
    const QString in = QStringLiteral("text <details><summary>x</summary>y</details>");
    QCOMPARE(run(in).markdown, in);
    const QString in2 = QStringLiteral("text <div align=\"center\">x</div>");
    QCOMPARE(run(in2).markdown, in2);
}

void TestMarkdownPreprocessor::forgedSentinelsAreStripped()
{
    const QString in = QString(kAlignBegin) + QStringLiteral("c forged ") + QString(kAlignEnd)
                       + QStringLiteral(" <b>x</b>");
    const Result r = run(in);
    QVERIFY(!r.markdown.contains(kAlignBegin));
    QVERIFY(!r.markdown.contains(kAlignEnd));
    QVERIFY(r.markdown.contains(QStringLiteral("**x**")));
    QVERIFY(!r.hasAlign);
}

void TestMarkdownPreprocessor::detailsCollapsedByDefault()
{
    const Result r = run(QStringLiteral(
        "<details>\n<summary>Bấm để mở</summary>\nNội dung ẩn\n</details>"));
    QCOMPARE(r.markdown, QStringLiteral("\n\n[▸ Bấm để mở](nn-details:0)\n\n"));
    QVERIFY(r.hasDetails);
    QVERIFY(!r.markdown.contains(QStringLiteral("Nội dung")));
}

void TestMarkdownPreprocessor::detailsOpenAttribute()
{
    const Result r = run(QStringLiteral(
        "<details open>\n<summary>T</summary>\nbody text\n</details>"));
    QVERIFY(r.markdown.contains(QStringLiteral("[▾ T](nn-details:0)")));
    QVERIFY(r.markdown.contains(QStringLiteral("body text")));
}

void TestMarkdownPreprocessor::detailsFlippedState()
{
    const QString in = QStringLiteral("<details><summary>T</summary>body</details>");
    const Result opened = run(in, QSet<int>{0});
    QVERIFY(opened.markdown.contains(QStringLiteral("[▾ T](nn-details:0)")));
    QVERIFY(opened.markdown.contains(QStringLiteral("body")));

    const QString inOpen = QStringLiteral("<details open><summary>T</summary>body</details>");
    const Result closed = run(inOpen, QSet<int>{0});
    QVERIFY(closed.markdown.contains(QStringLiteral("[▸ T](nn-details:0)")));
    QVERIFY(!closed.markdown.contains(QStringLiteral("body")));
}

void TestMarkdownPreprocessor::detailsOrdinalsSurviveCollapsedNesting()
{
    const QString in = QStringLiteral(
        "<details><summary>outer</summary>\n"
        "<details><summary>inner</summary>x</details>\n"
        "</details>\n\n"
        "<details><summary>sibling</summary>y</details>");
    const Result r = run(in);
    // outer = 0, inner = 1 (skipped while collapsed), sibling = 2 either way.
    QVERIFY(r.markdown.contains(QStringLiteral("(nn-details:0)")));
    QVERIFY(r.markdown.contains(QStringLiteral("[▸ sibling](nn-details:2)")));
    QVERIFY(!r.markdown.contains(QStringLiteral("inner")));

    const Result open = run(in, QSet<int>{0});
    QVERIFY(open.markdown.contains(QStringLiteral("[▸ inner](nn-details:1)")));
    QVERIFY(open.markdown.contains(QStringLiteral("[▸ sibling](nn-details:2)")));
}

void TestMarkdownPreprocessor::detailsBodyIsProcessedRecursively()
{
    const Result r = run(QStringLiteral(
        "<details open><summary>T</summary>\nsee $x$ and <b>y</b>\n</details>"));
    QVERIFY(r.markdown.contains(QStringLiteral("![formula](math:0)")));
    QVERIFY(r.markdown.contains(QStringLiteral("**y**")));
    QCOMPARE(r.math.size(), 1);
}

void TestMarkdownPreprocessor::detailsSummaryBracketsEscaped()
{
    const Result r = run(QStringLiteral("<details><summary>a [b] c</summary>x</details>"));
    QVERIFY(r.markdown.contains(QStringLiteral("[▸ a &#91;b&#93; c](nn-details:0)")));
}

QTEST_APPLESS_MAIN(TestMarkdownPreprocessor)
#include "test_markdown_preprocessor.moc"
