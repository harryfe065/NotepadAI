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

#include "markdown/MathParser.h"

using namespace Math;

namespace {

// Compact s-expression of an AST so tests compare shape, not struct fields.
QString dump(const Node &n);

QString dumpKids(const Node &n, const QString &sep)
{
    QStringList parts;
    for (const Node &k : n.kids)
        parts << dump(k);
    return parts.join(sep);
}

QString dump(const Node &n)
{
    switch (n.kind) {
    case Node::Kind::Row:
        return QLatin1Char('[') + dumpKids(n, QStringLiteral(" ")) + QLatin1Char(']');
    case Node::Kind::Atom: {
        QString prefix;
        if (n.style == FontStyle::Bold)
            prefix = QStringLiteral("B:");
        else if (n.style == FontStyle::Roman && n.text.size() == 1 && n.text.at(0).isLetter()
                 && n.text.at(0).unicode() < 0x80)
            prefix = QStringLiteral("R:");
        return prefix + n.text;
    }
    case Node::Kind::LargeOp: {
        QString s = QStringLiteral("op(") + n.text + QLatin1Char(')');
        if (n.limits == LimitsMode::Limits) s += QStringLiteral("+L");
        if (n.limits == LimitsMode::NoLimits) s += QStringLiteral("-L");
        return s;
    }
    case Node::Kind::Script:
        return QStringLiteral("script(") + dump(n.kids.at(0)) + QLatin1Char(',')
               + (n.hasSub ? dump(n.kids.at(1)) : QStringLiteral("-")) + QLatin1Char(',')
               + (n.hasSup ? dump(n.kids.at(n.hasSub ? 2 : 1)) : QStringLiteral("-"))
               + QLatin1Char(')');
    case Node::Kind::Frac: {
        QString name = n.bar ? QStringLiteral("frac") : QStringLiteral("frac0");
        if (n.styleOverride == 0) name += QStringLiteral("@t");
        if (n.styleOverride == 1) name += QStringLiteral("@d");
        return name + QLatin1Char('(') + dumpKids(n, QStringLiteral(",")) + QLatin1Char(')');
    }
    case Node::Kind::Sqrt:
        if (n.kids.size() == 2)
            return QStringLiteral("sqrt[") + dump(n.kids.at(1)) + QStringLiteral("](")
                   + dump(n.kids.at(0)) + QLatin1Char(')');
        return QStringLiteral("sqrt(") + dump(n.kids.at(0)) + QLatin1Char(')');
    case Node::Kind::Delimited:
        return QStringLiteral("delim(") + n.left + QLatin1Char(',') + n.right + QLatin1Char(',')
               + dump(n.kids.at(0)) + QLatin1Char(')');
    case Node::Kind::Matrix:
        return QStringLiteral("mat:%1:%2x%3(").arg(n.env).arg(n.rows).arg(n.cols)
               + dumpKids(n, QStringLiteral(",")) + QLatin1Char(')');
    case Node::Kind::Text:
        return QStringLiteral("text\"") + n.text + QLatin1Char('"');
    case Node::Kind::Accent:
        return QStringLiteral("acc:") + n.text + QLatin1Char('(') + dump(n.kids.at(0))
               + QLatin1Char(')');
    case Node::Kind::Space:
        return QStringLiteral("sp(%1)").arg(n.em, 0, 'g', 4);
    }
    return QString();
}

QString d(const char *src)
{
    const ParseResult r = parse(QString::fromUtf8(src));
    return r.ok ? dump(r.root) : QStringLiteral("ERR:") + r.error;
}

} // namespace

class TestMathParser : public QObject
{
    Q_OBJECT

private slots:
    void atomsAndOperators();
    void numbersMerge();
    void scripts();
    void primeIsSuperscript();
    void groups();
    void fractions();
    void roots();
    void greekAndSymbols();
    void largeOperators();
    void namedOperators();
    void limitsModifiers();
    void integralWithSpacing();
    void delimiters();
    void matrices();
    void realWorldChatFormulas();
    void normsAndAbsoluteValuesAsAtoms();
    void angleBracketsAndFloorsAsAtoms();
    void normDelimitersWithLeftRight();
    void vectorNotations();
    void boldAndTypewriterFonts();
    void alignedEnvironments();
    void textAndFonts();
    void accents();
    void spacingCommands();
    void whitespaceIgnored();
    void errors();
    void depthLimit();
    void sourceLimit();
};

void TestMathParser::atomsAndOperators()
{
    QCOMPARE(d("x"), QStringLiteral("[x]"));
    QCOMPARE(d("x+y"), QStringLiteral("[x + y]"));
    QCOMPARE(d("a-b"), QStringLiteral("[a − b]"));
    QCOMPARE(d("a=b"), QStringLiteral("[a = b]"));
    QCOMPARE(d("(a,b)"), QStringLiteral("[( a , b )]"));
    QCOMPARE(d("a<b"), QStringLiteral("[a < b]"));
    QCOMPARE(d("|x|"), QStringLiteral("[| x |]"));
}

void TestMathParser::numbersMerge()
{
    QCOMPARE(d("12.5x"), QStringLiteral("[12.5 x]"));
    QCOMPARE(d("007"), QStringLiteral("[007]"));
}

void TestMathParser::scripts()
{
    QCOMPARE(d("x^2"), QStringLiteral("[script(x,-,[2])]"));
    QCOMPARE(d("x_i"), QStringLiteral("[script(x,[i],-)]"));
    QCOMPARE(d("x_i^2"), QStringLiteral("[script(x,[i],[2])]"));
    QCOMPARE(d("x^2_i"), QStringLiteral("[script(x,[i],[2])]"));
    QCOMPARE(d("x^{ab}"), QStringLiteral("[script(x,-,[a b])]"));
    QCOMPARE(d("x^{y^z}"), QStringLiteral("[script(x,-,[script(y,-,[z])])]"));
    QCOMPARE(d("{ab}^2"), QStringLiteral("[script([a b],-,[2])]"));
    // a script with no base attaches to an empty group
    QCOMPARE(d("^2"), QStringLiteral("[script([],-,[2])]"));
}

void TestMathParser::primeIsSuperscript()
{
    QCOMPARE(d("x'"), QStringLiteral("[script(x,-,[′])]"));
}

void TestMathParser::groups()
{
    QCOMPARE(d("{x}"), QStringLiteral("[[x]]"));
    QCOMPARE(d("a{b}c"), QStringLiteral("[a [b] c]"));
}

void TestMathParser::fractions()
{
    QCOMPARE(d("\\frac{1}{2}"), QStringLiteral("[frac([1],[2])]"));
    QCOMPARE(d("\\frac12"), QStringLiteral("[frac([1],[2])]"));
    QCOMPARE(d("\\tfrac{1}{3}"), QStringLiteral("[frac@t([1],[3])]"));
    QCOMPARE(d("\\dfrac{a}{b}"), QStringLiteral("[frac@d([a],[b])]"));
    QCOMPARE(d("\\binom{n}{k}"), QStringLiteral("[delim((,),[frac0([n],[k])])]"));
    QCOMPARE(d("\\frac{a}{\\frac{b}{c}}"),
             QStringLiteral("[frac([a],[frac([b],[c])])]"));
}

void TestMathParser::roots()
{
    QCOMPARE(d("\\sqrt{x}"), QStringLiteral("[sqrt([x])]"));
    QCOMPARE(d("\\sqrt x"), QStringLiteral("[sqrt([x])]"));
    QCOMPARE(d("\\sqrt[3]{x}"), QStringLiteral("[sqrt[[3]]([x])]"));
}

void TestMathParser::greekAndSymbols()
{
    QCOMPARE(d("\\alpha+\\beta"), QStringLiteral("[α + β]"));
    QCOMPARE(d("\\Gamma"), QStringLiteral("[Γ]"));
    QCOMPARE(d("\\infty"), QStringLiteral("[∞]"));
    QCOMPARE(d("\\partial f"), QStringLiteral("[∂ f]"));
    QCOMPARE(d("a\\leq b"), QStringLiteral("[a ≤ b]"));
    QCOMPARE(d("a\\le b"), QStringLiteral("[a ≤ b]"));
    QCOMPARE(d("x\\to y"), QStringLiteral("[x → y]"));
    QCOMPARE(d("a\\times b\\cdot c"), QStringLiteral("[a × b ⋅ c]"));
    QCOMPARE(d("x\\in A"), QStringLiteral("[x ∈ A]"));
    QCOMPARE(d("\\$ \\% \\& \\#"), QStringLiteral("[$ % & #]"));
    QCOMPARE(d("\\{x\\}"), QStringLiteral("[{ x }]"));
}

void TestMathParser::largeOperators()
{
    QCOMPARE(d("\\sum_{i=0}^n i"),
             QStringLiteral("[script(op(∑),[i = 0],[n]) i]"));
    QCOMPARE(d("\\prod"), QStringLiteral("[op(∏)]"));
    QCOMPARE(d("\\oint"), QStringLiteral("[op(∮)]"));
}

void TestMathParser::namedOperators()
{
    QCOMPARE(d("\\sin x"), QStringLiteral("[op(sin) x]"));
    QCOMPARE(d("\\lim_{x\\to0}"),
             QStringLiteral("[script(op(lim),[x → 0],-)]"));
    QCOMPARE(d("\\log_2 n"), QStringLiteral("[script(op(log),[2],-) n]"));
    QCOMPARE(d("\\operatorname{rank} A"), QStringLiteral("[op(rank) A]"));
    QCOMPARE(d("\\max"), QStringLiteral("[op(max)]"));
}

void TestMathParser::limitsModifiers()
{
    QCOMPARE(d("\\int\\limits_0^1"), QStringLiteral("[script(op(∫)+L,[0],[1])]"));
    QCOMPARE(d("\\sum\\nolimits_i"), QStringLiteral("[script(op(∑)-L,[i],-)]"));
}

void TestMathParser::integralWithSpacing()
{
    QCOMPARE(d("\\int_0^1 x^2\\,dx"),
             QStringLiteral("[script(op(∫),[0],[1]) script(x,-,[2]) sp(0.1667) d x]"));
}

void TestMathParser::delimiters()
{
    QCOMPARE(d("\\left( \\frac{a}{b} \\right)"),
             QStringLiteral("[delim((,),[frac([a],[b])])]"));
    QCOMPARE(d("\\left. x \\right|"), QStringLiteral("[delim(,|,[x])]"));
    QCOMPARE(d("\\left\\langle x \\right\\rangle"),
             QStringLiteral("[delim(⟨,⟩,[x])]"));
    QCOMPARE(d("\\left[ a \\right\\}"), QStringLiteral("[delim([,},[a])]"));
    // nesting
    QCOMPARE(d("\\left( \\left[ x \\right] \\right)"),
             QStringLiteral("[delim((,),[delim([,],[x])])]"));
}

void TestMathParser::matrices()
{
    QCOMPARE(d("\\begin{pmatrix} a & b \\\\ c & d \\end{pmatrix}"),
             QStringLiteral("[mat:pmatrix:2x2([a],[b],[c],[d])]"));
    QCOMPARE(d("\\begin{bmatrix} 1 \\\\ 2 \\end{bmatrix}"),
             QStringLiteral("[mat:bmatrix:2x1([1],[2])]"));
    QCOMPARE(d("\\begin{cases} x & y \\\\ z \\end{cases}"),
             QStringLiteral("[mat:cases:2x2([x],[y],[z],[])]"));
    QCOMPARE(d("\\begin{vmatrix} a & b \\end{vmatrix}"),
             QStringLiteral("[mat:vmatrix:1x2([a],[b])]"));
}

void TestMathParser::realWorldChatFormulas()
{
    const char *formulas[] = {
        "R(\\theta) = \\begin{pmatrix}\\cos\\theta & -\\sin\\theta \\\\ \\sin\\theta & \\cos\\theta \\end{pmatrix}",
        "v_{\\text{bevy}} = \\frac{v_{\\text{C\\#}} \\times 60}{1}, \\qquad y_{\\text{bevy}} = -y_{\\text{C\\#}}",
        "\\int_{0}^{\\infty} e^{-x^2}\\,dx = \\frac{\\sqrt{\\pi}}{2}",
        "\\Delta t = \\frac{1}{60} \\approx 0.01667\\ \\text{s}",
        "A = \\begin{bmatrix} 1 & 2 \\\\ 3 & 4 \\end{bmatrix}, \\quad \\det A = ad - bc",
        "\\begin{pmatrix} a & b & c \\\\ d & e & f \\\\ g & h & i \\end{pmatrix}\\begin{pmatrix} x \\\\ y \\\\ z \\end{pmatrix}",
    };
    for (const char *f : formulas) {
        const ParseResult r = parse(QString::fromUtf8(f));
        QVERIFY2(r.ok, f);
    }
}

void TestMathParser::normsAndAbsoluteValuesAsAtoms()
{
    // reported: $\lVert v \rVert$ stayed as source text
    QCOMPARE(d("\\lVert v \\rVert"), QStringLiteral("[\u2016 v \u2016]"));
    QCOMPARE(d("\\lvert x \\rvert"), QStringLiteral("[| x |]"));
    QCOMPARE(d("\\vert x \\Vert"), QStringLiteral("[| x \u2016]"));
    QCOMPARE(d("\\|v\\|"), QStringLiteral("[\u2016 v \u2016]"));
    // scripts attach to the closing bar
    QCOMPARE(d("\\lVert v \\rVert_2"), QStringLiteral("[\u2016 v script(\u2016,[2],-)]"));
}

void TestMathParser::angleBracketsAndFloorsAsAtoms()
{
    QCOMPARE(d("\\langle u, v \\rangle"), QStringLiteral("[\u27E8 u , v \u27E9]"));
    QCOMPARE(d("\\lfloor x \\rfloor \\lceil x \\rceil"),
             QStringLiteral("[\u230A x \u230B \u2308 x \u2309]"));
    QCOMPARE(d("\\lbrace a \\rbrace"), QStringLiteral("[{ a }]"));
}

void TestMathParser::normDelimitersWithLeftRight()
{
    QCOMPARE(d("\\left\\lVert v \\right\\rVert"), QStringLiteral("[delim(\u2016,\u2016,[v])]"));
    QCOMPARE(d("\\left\\| v \\right\\|"), QStringLiteral("[delim(\u2016,\u2016,[v])]"));
    QCOMPARE(d("\\left\\langle u, v \\right\\rangle"),
             QStringLiteral("[delim(\u27E8,\u27E9,[u , v])]"));
}

void TestMathParser::vectorNotations()
{
    QCOMPARE(d("\\vec{v}"), QStringLiteral("[acc:vec([v])]"));
    QCOMPARE(d("\\overrightarrow{AB}"), QStringLiteral("[acc:overrightarrow([A B])]"));
    QCOMPARE(d("\\overleftarrow{x}"), QStringLiteral("[acc:overleftarrow([x])]"));
    QCOMPARE(d("\\hat{\\imath} + \\hat{\\jmath}"),
             QStringLiteral("[acc:hat([\u0131]) + acc:hat([\u0237])]"));
    QCOMPARE(d("\\mathbf{u} \\cdot \\mathbf{v}"), QStringLiteral("[B:u \u22C5 B:v]"));
    QCOMPARE(d("\\boldsymbol{\\theta}"), QStringLiteral("[B:\u03B8]"));
    QCOMPARE(d("\\nabla \\times \\mathbf{F}"), QStringLiteral("[\u2207 \u00D7 B:F]"));
    QCOMPARE(d("\\mathbf{a} \\times \\mathbf{b} \\perp \\mathbf{a}"),
             QStringLiteral("[B:a \u00D7 B:b \u22A5 B:a]"));
}

void TestMathParser::boldAndTypewriterFonts()
{
    QCOMPARE(d("\\mathsf{A}"), QStringLiteral("[R:A]"));
    QCOMPARE(d("\\mathtt{x}"), QStringLiteral("[R:x]"));
}

void TestMathParser::alignedEnvironments()
{
    QCOMPARE(d("\\begin{aligned} a &= b \\\\ c &= d \\end{aligned}"),
             QStringLiteral("[mat:aligned:2x2([a],[= b],[c],[= d])]"));
}

void TestMathParser::textAndFonts()
{
    QCOMPARE(d("\\text{if } x"), QStringLiteral("[text\"if \" x]"));
    QCOMPARE(d("\\mathbb{R}"), QStringLiteral("[ℝ]"));
    QCOMPARE(d("\\mathbb{N}\\mathbb{Z}"), QStringLiteral("[ℕ ℤ]"));
    QCOMPARE(d("\\mathbf{v}"), QStringLiteral("[B:v]"));
    QCOMPARE(d("\\mathrm{d}x"), QStringLiteral("[R:d x]"));
    QCOMPARE(d("\\mathcal{L}"), QStringLiteral("[ℒ]"));
    QCOMPARE(d("\\textbf{ab}"), QStringLiteral("[text\"ab\"]"));
    // TeX escapes inside \text lose their backslash
    QCOMPARE(d("\\text{C\\# and 50\\% \\$ \\& a\\_b \\{x\\}}"),
             QStringLiteral("[text\"C# and 50% $ & a_b {x}\"]"));
}

void TestMathParser::accents()
{
    QCOMPARE(d("\\hat{x}"), QStringLiteral("[acc:hat([x])]"));
    QCOMPARE(d("\\vec v"), QStringLiteral("[acc:vec([v])]"));
    QCOMPARE(d("\\overline{ab}"), QStringLiteral("[acc:overline([a b])]"));
    QCOMPARE(d("\\underline{x}"), QStringLiteral("[acc:underline([x])]"));
    QCOMPARE(d("\\bar{x}\\tilde{y}\\dot{z}"),
             QStringLiteral("[acc:bar([x]) acc:tilde([y]) acc:dot([z])]"));
}

void TestMathParser::spacingCommands()
{
    QCOMPARE(d("a\\quad b"), QStringLiteral("[a sp(1) b]"));
    QCOMPARE(d("a\\qquad b"), QStringLiteral("[a sp(2) b]"));
    QCOMPARE(d("a\\; b"), QStringLiteral("[a sp(0.2778) b]"));
    QCOMPARE(d("a\\! b"), QStringLiteral("[a sp(-0.1667) b]"));
    QCOMPARE(d("a\\ b"), QStringLiteral("[a sp(0.3333) b]"));
}

void TestMathParser::whitespaceIgnored()
{
    QCOMPARE(d("  x   +\n y "), QStringLiteral("[x + y]"));
}

void TestMathParser::errors()
{
    for (const char *bad : {"", "   ", "\\foo", "\\frac{1}", "{x", "}", "x^", "x_", "a_1_2",
                            "a^1^2", "\\begin{foo}x\\end{foo}", "\\left( x", "\\right)",
                            "\\end{pmatrix}", "a & b", "a \\\\ b",
                            "\\begin{pmatrix} a \\end{bmatrix}", "\\sqrt", "\\color{red}x",
                            "\\left( x \\right", "\\begin{pmatrix} a"}) {
        const ParseResult r = parse(QString::fromUtf8(bad));
        QVERIFY2(!r.ok, bad);
        QVERIFY2(!r.error.isEmpty(), bad);
    }
}

void TestMathParser::depthLimit()
{
    const int ok = kMaxDepth - 2;
    QString good = QString(ok, QLatin1Char('{')) + QStringLiteral("x") + QString(ok, QLatin1Char('}'));
    QVERIFY(parse(good).ok);

    const int deep = kMaxDepth + 8;
    QString bad = QString(deep, QLatin1Char('{')) + QStringLiteral("x") + QString(deep, QLatin1Char('}'));
    QVERIFY(!parse(bad).ok);

    QString fracs;
    for (int i = 0; i < deep; ++i) fracs += QStringLiteral("\\frac{1}{");
    fracs += QStringLiteral("x");
    fracs += QString(deep, QLatin1Char('}'));
    for (int i = 0; i < deep; ++i) fracs += QStringLiteral("}");
    QVERIFY(!parse(fracs).ok);
}

void TestMathParser::sourceLimit()
{
    const QString big = QString(kMaxSourceChars + 1, QLatin1Char('x'));
    QVERIFY(!parse(big).ok);
    const QString edge = QString(kMaxSourceChars, QLatin1Char('x'));
    QVERIFY(parse(edge).ok);
}

QTEST_APPLESS_MAIN(TestMathParser)
#include "test_math_parser.moc"
