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

#include "markdown/MathRenderCache.h"

using namespace Math;

namespace {
const QColor kBlack(Qt::black);
}

class TestMathRenderCache : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void secondGetIsAHit();
    void keyIncludesDisplayPxColorAndDpr();
    void unsupportedFormulasAreCachedNegatively();
    void evictsLeastRecentlyUsedByCount();
    void getRefreshesRecency();
    void evictsByByteBudget();
    void bytesAreTrackedAndReleased();
    void clearEmptiesTheCache();
    void hitSharesTheSameImageData();
    void instanceIsASingleton();
};

void TestMathRenderCache::initTestCase()
{
#ifdef MATH_FONT_FILE
    registerMathFontFile(QStringLiteral(MATH_FONT_FILE));
#endif
}

void TestMathRenderCache::secondGetIsAHit()
{
    RenderCache c;
    const auto a = c.get(QStringLiteral("x^2"), false, 20, kBlack, 1.0);
    QVERIFY(a.has_value());
    QCOMPARE(c.misses(), 1);
    QCOMPARE(c.hits(), 0);
    const auto b = c.get(QStringLiteral("x^2"), false, 20, kBlack, 1.0);
    QVERIFY(b.has_value());
    QCOMPARE(c.misses(), 1);
    QCOMPARE(c.hits(), 1);
    QCOMPARE(c.size(), 1);
}

void TestMathRenderCache::keyIncludesDisplayPxColorAndDpr()
{
    RenderCache c;
    const QString s = QStringLiteral("a+b");
    c.get(s, false, 20, kBlack, 1.0);
    c.get(s, true, 20, kBlack, 1.0);              // display flag
    c.get(s, false, 24, kBlack, 1.0);             // size
    c.get(s, false, 20, QColor(Qt::white), 1.0);  // theme colour
    c.get(s, false, 20, kBlack, 2.0);             // dpr
    QCOMPARE(c.size(), 5);
    QCOMPARE(c.misses(), 5);
    QCOMPARE(c.hits(), 0);
    QVERIFY(c.contains(s, false, 20, QColor(Qt::white), 1.0));
    QVERIFY(!c.contains(s, false, 20, QColor(Qt::red), 1.0));
}

void TestMathRenderCache::unsupportedFormulasAreCachedNegatively()
{
    RenderCache c;
    QVERIFY(!c.get(QStringLiteral("\\foo"), false, 20, kBlack, 1.0).has_value());
    QVERIFY(!c.get(QStringLiteral("\\foo"), false, 20, kBlack, 1.0).has_value());
    QCOMPARE(c.misses(), 1);
    QCOMPARE(c.hits(), 1);
    QCOMPARE(c.size(), 1);
}

void TestMathRenderCache::evictsLeastRecentlyUsedByCount()
{
    RenderCache c(3, 64 * 1024 * 1024);
    for (const char *s : {"a", "b", "c", "d"})
        c.get(QString::fromLatin1(s), false, 20, kBlack, 1.0);
    QCOMPARE(c.size(), 3);
    QVERIFY(!c.contains(QStringLiteral("a"), false, 20, kBlack, 1.0));
    QVERIFY(c.contains(QStringLiteral("b"), false, 20, kBlack, 1.0));
    QVERIFY(c.contains(QStringLiteral("d"), false, 20, kBlack, 1.0));
}

void TestMathRenderCache::getRefreshesRecency()
{
    RenderCache c(3, 64 * 1024 * 1024);
    for (const char *s : {"a", "b", "c"})
        c.get(QString::fromLatin1(s), false, 20, kBlack, 1.0);
    c.get(QStringLiteral("a"), false, 20, kBlack, 1.0); // touch: b is now the oldest
    c.get(QStringLiteral("d"), false, 20, kBlack, 1.0);
    QVERIFY(c.contains(QStringLiteral("a"), false, 20, kBlack, 1.0));
    QVERIFY(!c.contains(QStringLiteral("b"), false, 20, kBlack, 1.0));
}

void TestMathRenderCache::evictsByByteBudget()
{
    RenderCache probe;
    probe.get(QStringLiteral("\\frac{a}{b}"), false, 20, kBlack, 1.0);
    const qint64 one = probe.bytes();
    QVERIFY(one > 0);

    RenderCache c(1000, one * 2 + one / 2); // room for two, not three
    c.get(QStringLiteral("\\frac{a}{b}"), false, 20, kBlack, 1.0);
    c.get(QStringLiteral("\\frac{c}{d}"), false, 20, kBlack, 1.0);
    c.get(QStringLiteral("\\frac{e}{f}"), false, 20, kBlack, 1.0);
    QVERIFY(c.size() <= 2);
    QVERIFY(c.bytes() <= one * 2 + one / 2);
    QVERIFY(c.contains(QStringLiteral("\\frac{e}{f}"), false, 20, kBlack, 1.0));
}

void TestMathRenderCache::bytesAreTrackedAndReleased()
{
    RenderCache c(2, 64 * 1024 * 1024);
    QCOMPARE(c.bytes(), 0);
    c.get(QStringLiteral("x"), false, 20, kBlack, 1.0);
    const qint64 one = c.bytes();
    QVERIFY(one > 0);
    c.get(QStringLiteral("y"), false, 20, kBlack, 1.0);
    c.get(QStringLiteral("z"), false, 20, kBlack, 1.0); // evicts x
    QVERIFY(c.bytes() > one);
    QVERIFY(c.bytes() < one * 3);
}

void TestMathRenderCache::clearEmptiesTheCache()
{
    RenderCache c;
    c.get(QStringLiteral("x"), false, 20, kBlack, 1.0);
    c.clear();
    QCOMPARE(c.size(), 0);
    QCOMPARE(c.bytes(), 0);
    QVERIFY(!c.contains(QStringLiteral("x"), false, 20, kBlack, 1.0));
}

void TestMathRenderCache::hitSharesTheSameImageData()
{
    RenderCache c;
    const auto a = c.get(QStringLiteral("x^2"), false, 20, kBlack, 1.0);
    const auto b = c.get(QStringLiteral("x^2"), false, 20, kBlack, 1.0);
    QVERIFY(a && b);
    // implicit sharing: the hit must not have copied pixels
    QCOMPARE(a->image.constBits(), b->image.constBits());
}

void TestMathRenderCache::instanceIsASingleton()
{
    QVERIFY(&RenderCache::instance() == &RenderCache::instance());
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    TestMathRenderCache t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_math_render_cache.moc"
