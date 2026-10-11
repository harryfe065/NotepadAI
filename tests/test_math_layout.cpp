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
#include <QGuiApplication>

#include "markdown/MathLayout.h"

using namespace Math;

namespace {

constexpr double kPx = 20.0;

Layout lay(const char *src, bool display = false)
{
    return layoutFormula(QString::fromUtf8(src), display, kPx);
}

} // namespace

class TestMathLayout : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void atomHasPositiveBox();
    void unsupportedIsNotOk();
    void fractionIsTallerThanAtom();
    void superscriptRaisesAndSubscriptLowers();
    void displayLimitsStackVertically();
    void textStyleLimitsGoBesideNotAbove();
    void integralKeepsSideScriptsInDisplay();
    void delimitersGrowWithContent();
    void matrixGrowsWithRows();
    void squareRootIsLargerThanRadicand();
    void binaryOperatorsAddSpace();
    void unaryMinusHasNoLeadingSpace();
    void spacingCommandsWiden();
    void scriptsAreSmallerThanBase();
    void accentsAddHeight();
    void sizeScalesWithPixelSize();
    void axisIsAboveBaseline();
    void boldGreekUsesBoldAlphabetGlyphs();
    void wideVectorArrowSpansItsContent();
    void normBarsAreTallerThanTheirContent();

    void renderProducesTintedImage();
    void renderIsSymmetricAboutAxis();
    void renderHonoursDevicePixelRatio();
    void renderUnsupportedIsNullopt();
    void bundledFontFamilyIsNotEmpty();
};

void TestMathLayout::initTestCase()
{
#ifdef MATH_FONT_FILE
    registerMathFontFile(QStringLiteral(MATH_FONT_FILE));
#endif
}

void TestMathLayout::atomHasPositiveBox()
{
    const Layout l = lay("x");
    QVERIFY2(l.ok, qPrintable(l.error));
    QVERIFY(l.width > 0);
    QVERIFY(l.ascent > 0);
    QVERIFY(l.descent >= 0);
    QVERIFY(!l.prims.empty());
}

void TestMathLayout::unsupportedIsNotOk()
{
    const Layout l = lay("\\foo{x}");
    QVERIFY(!l.ok);
    QVERIFY(!l.error.isEmpty());
}

void TestMathLayout::fractionIsTallerThanAtom()
{
    const Layout atom = lay("a");
    const Layout frac = lay("\\frac{a}{b}");
    QVERIFY(frac.ok);
    QVERIFY(frac.height() > atom.height() * 1.4);
}

void TestMathLayout::superscriptRaisesAndSubscriptLowers()
{
    const Layout base = lay("x");
    const Layout sup = lay("x^2");
    const Layout sub = lay("x_2");
    QVERIFY(sup.ascent > base.ascent);
    QVERIFY(sub.descent > base.descent);
    QVERIFY(sup.width > base.width);
    QVERIFY(sub.width > base.width);
}

void TestMathLayout::displayLimitsStackVertically()
{
    const Layout disp = lay("\\sum_{i=0}^{n} i", true);
    const Layout text = lay("\\sum_{i=0}^{n} i", false);
    QVERIFY(disp.ok && text.ok);
    // display: limits above/below -> taller and narrower than side scripts
    QVERIFY(disp.height() > text.height());
    QVERIFY(disp.width < text.width);
}

void TestMathLayout::textStyleLimitsGoBesideNotAbove()
{
    const Layout plain = lay("\\sum", false);
    const Layout scripted = lay("\\sum_{i}^{n}", false);
    QVERIFY(scripted.width > plain.width);
}

void TestMathLayout::integralKeepsSideScriptsInDisplay()
{
    const Layout plain = lay("\\int", true);
    const Layout scripted = lay("\\int_0^1", true);
    QVERIFY(scripted.width > plain.width + kPx * 0.3);
}

void TestMathLayout::delimitersGrowWithContent()
{
    const Layout small = lay("\\left( a \\right)");
    const Layout tall = lay("\\left( \\frac{a}{b} \\right)");
    const Layout inner = lay("\\frac{a}{b}");
    QVERIFY(small.ok && tall.ok);
    QVERIFY(tall.height() >= inner.height());
    QVERIFY(tall.height() > small.height());
}

void TestMathLayout::matrixGrowsWithRows()
{
    const Layout one = lay("\\begin{pmatrix} a & b \\end{pmatrix}");
    const Layout two = lay("\\begin{pmatrix} a & b \\\\ c & d \\end{pmatrix}");
    QVERIFY(one.ok && two.ok);
    QVERIFY(two.height() > one.height() * 1.3);
    QVERIFY(two.width > 0);
}

void TestMathLayout::squareRootIsLargerThanRadicand()
{
    const Layout body = lay("x");
    const Layout root = lay("\\sqrt{x}");
    QVERIFY(root.ok);
    QVERIFY(root.width > body.width + kPx * 0.4);
    QVERIFY(root.ascent > body.ascent);
}

void TestMathLayout::binaryOperatorsAddSpace()
{
    const Layout ab = lay("ab");
    const Layout apb = lay("a+b");
    const Layout aeb = lay("a=b");
    QVERIFY(apb.width > ab.width);
    QVERIFY(aeb.width > ab.width);
}

void TestMathLayout::unaryMinusHasNoLeadingSpace()
{
    // "-a" : the minus is unary (Ord), "b-a" : binary (spaced both sides).
    const Layout un = lay("-a");
    const Layout bin = lay("b-a");
    const Layout b = lay("b");
    QVERIFY(un.width < bin.width - b.width);
}

void TestMathLayout::spacingCommandsWiden()
{
    const Layout tight = lay("ab");
    const Layout quad = lay("a\\quad b");
    QVERIFY(quad.width > tight.width + kPx * 0.9);
}

void TestMathLayout::scriptsAreSmallerThanBase()
{
    const Layout base = lay("x");
    const Layout sup = lay("x^x");
    // the script glyph adds well under a full base glyph's advance
    QVERIFY(sup.width - base.width < base.width);
}

void TestMathLayout::accentsAddHeight()
{
    const Layout plain = lay("x");
    const Layout hat = lay("\\hat{x}");
    QVERIFY(hat.ok);
    QVERIFY(hat.ascent > plain.ascent);
    const Layout under = lay("\\underline{x}");
    QVERIFY(under.descent > plain.descent);
}

void TestMathLayout::sizeScalesWithPixelSize()
{
    const Layout a = layoutFormula(QStringLiteral("\\frac{a}{b}"), false, 10.0);
    const Layout b = layoutFormula(QStringLiteral("\\frac{a}{b}"), false, 40.0);
    QVERIFY(a.ok && b.ok);
    QVERIFY(b.width > a.width * 2.5);
    QVERIFY(b.height() > a.height() * 2.5);
}

void TestMathLayout::boldGreekUsesBoldAlphabetGlyphs()
{
    // \boldsymbol{\theta} must use the Mathematical Bold Greek code point, not
    // the plain (regular weight) letter.
    const Layout l = lay("\\boldsymbol{\\theta}");
    QVERIFY(l.ok);
    bool sawBold = false;
    for (const Prim &p : l.prims) {
        if (p.kind == PrimKind::Text && p.text == QString::fromUcs4(U"\U0001D6C9"))
            sawBold = true;
    }
    QVERIFY(sawBold);
}

void TestMathLayout::wideVectorArrowSpansItsContent()
{
    const Layout wide = lay("\\overrightarrow{ABCD}");
    const Layout narrow = lay("\\overrightarrow{A}");
    QVERIFY(wide.ok && narrow.ok);
    // the arrow is a path as wide as its content, above the baseline
    auto arrowWidth = [](const Layout &l) {
        double w = 0;
        for (const Prim &p : l.prims)
            if (p.kind == PrimKind::Path) w = std::max(w, p.path.boundingRect().width());
        return w;
    };
    QVERIFY(arrowWidth(wide) > arrowWidth(narrow) * 2.0);
    QVERIFY(wide.ascent > lay("ABCD").ascent);
}

void TestMathLayout::normBarsAreTallerThanTheirContent()
{
    const Layout plain = lay("\\lVert v \\rVert");
    QVERIFY(plain.ok);
    QVERIFY(plain.width > lay("v").width);
    const Layout tall = lay("\\left\\lVert \\frac{a}{b} \\right\\rVert");
    QVERIFY(tall.ok);
    QVERIFY(tall.height() >= lay("\\frac{a}{b}").height());
}

void TestMathLayout::axisIsAboveBaseline()
{
    const Layout l = lay("x");
    QVERIFY(l.axis > 0);
    QVERIFY(l.axis < kPx);
}

void TestMathLayout::renderProducesTintedImage()
{
    const QColor color(200, 40, 60);
    const auto r = renderFormula(QStringLiteral("x^2 + y"), false, kPx, color, 1.0);
    QVERIFY(r.has_value());
    QVERIFY(r->image.width() > 0 && r->image.height() > 0);
    bool sawInk = false;
    for (int y = 0; y < r->image.height() && !sawInk; ++y) {
        for (int x = 0; x < r->image.width(); ++x) {
            const QColor c = r->image.pixelColor(x, y);
            if (c.alpha() > 200) {
                sawInk = true;
                QVERIFY(qAbs(c.red() - color.red()) < 12);
                QVERIFY(qAbs(c.green() - color.green()) < 12);
                QVERIFY(qAbs(c.blue() - color.blue()) < 12);
                break;
            }
        }
    }
    QVERIFY(sawInk);
    // background stays transparent
    QCOMPARE(r->image.pixelColor(0, 0).alpha(), 0);
}

void TestMathLayout::renderIsSymmetricAboutAxis()
{
    for (const char *src : {"x", "\\frac{a}{b}", "x^2", "\\sum_{i=0}^n i", "y_j"}) {
        const auto r = renderFormula(QString::fromUtf8(src), false, kPx, Qt::black, 1.0);
        QVERIFY2(r.has_value(), src);
        const double h = r->image.height() / r->image.devicePixelRatio();
        QVERIFY2(qAbs(r->axisFromTop - h / 2.0) < 1.0, src);
        QVERIFY2(r->baselineFromTop > r->axisFromTop, src);
    }
}

void TestMathLayout::renderHonoursDevicePixelRatio()
{
    const auto one = renderFormula(QStringLiteral("x"), false, kPx, Qt::black, 1.0);
    const auto two = renderFormula(QStringLiteral("x"), false, kPx, Qt::black, 2.0);
    QVERIFY(one && two);
    QCOMPARE(two->image.devicePixelRatio(), 2.0);
    QVERIFY(two->image.width() >= one->image.width() * 2 - 2);
    // same device-independent size
    QVERIFY(qAbs(two->image.width() / 2.0 - one->image.width()) <= 1.5);
}

void TestMathLayout::renderUnsupportedIsNullopt()
{
    QVERIFY(!renderFormula(QStringLiteral("\\foo"), false, kPx, Qt::black, 1.0).has_value());
    QVERIFY(!renderFormula(QStringLiteral(""), false, kPx, Qt::black, 1.0).has_value());
}

void TestMathLayout::bundledFontFamilyIsNotEmpty()
{
    QVERIFY(!mathFontFamily().isEmpty());
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    TestMathLayout t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_math_layout.moc"
