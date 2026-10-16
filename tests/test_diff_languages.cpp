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

#include "DiffLanguages.h"

using namespace DiffLanguages;

namespace {

LanguageDef lang(const QString &name, const QString &lexer, QStringList exts,
                 QStringList filenames = {}, QStringList patterns = {}, int styleCount = 3)
{
    LanguageDef d;
    d.name = name;
    d.lexer = lexer;
    d.extensions = std::move(exts);
    d.filenames = std::move(filenames);
    d.patterns = std::move(patterns);
    d.styles.append(StyleDef{0, 0x000000, 0});
    for (int i = 1; i <= styleCount; ++i)
        d.styles.append(StyleDef{quint8(i), quint32(0x010101 * i), 0});
    return d;
}

Catalog sampleCatalog()
{
    return Catalog({
        lang("Text", "null", {"", "txt"}),
        lang("C++", "cpp", {"cpp", "h"}),
        lang("Env", "props", {"env"}, {".env"}, {"^%.env%."}),
        lang("Justfile", "makefile", {"just"}, {"justfile", "Justfile"}),
        lang("Python", "python", {"py"}),
    });
}

QByteArray serialize(const QVector<QList<QByteArray>> &langs)
{
    QByteArray out;
    for (const auto &fields : langs) {
        if (!out.isEmpty()) out.append('\x1e');
        out.append(fields.join('\x1f'));
    }
    return out;
}

} // namespace

class TestDiffLanguages : public QObject
{
    Q_OBJECT

private slots:
    void detect_data();
    void detect();
    void luaPattern_data();
    void luaPattern();
    void fromSerialized_roundTrip();
    void compact_skipsDefaultAndPredefined();
    void compact_foldsInactiveStyles();
    void compact_overflow();
    void assign_sharesRangePerLanguage();
    void assign_overflowLeavesNoSyntax();
};

void TestDiffLanguages::detect_data()
{
    QTest::addColumn<QByteArray>("path");
    QTest::addColumn<QString>("language");
    QTest::newRow("extension") << QByteArray("src/a.cpp") << QStringLiteral("C++");
    QTest::newRow("header") << QByteArray("a.h") << QStringLiteral("C++");
    QTest::newRow("filename beats extension") << QByteArray("tools/justfile") << QStringLiteral("Justfile");
    QTest::newRow("exact filename") << QByteArray(".env") << QStringLiteral("Env");
    QTest::newRow("pattern") << QByteArray("cfg/.env.local") << QStringLiteral("Env");
    QTest::newRow("pattern anchored") << QByteArray("a.env.local") << QString();
    QTest::newRow("extension case-sensitive") << QByteArray("A.CPP") << QString();
    QTest::newRow("null lexer") << QByteArray("notes.txt") << QString();
    QTest::newRow("no extension") << QByteArray("LICENSE") << QString();
    QTest::newRow("unknown") << QByteArray("a.zig") << QString();
}

void TestDiffLanguages::detect()
{
    QFETCH(QByteArray, path);
    QFETCH(QString, language);
    const Catalog c = sampleCatalog();
    const int i = c.detect(path);
    QCOMPARE(i < 0 ? QString() : c.languages().at(i).name, language);
}

void TestDiffLanguages::luaPattern_data()
{
    QTest::addColumn<QString>("pattern");
    QTest::addColumn<QString>("subject");
    QTest::addColumn<bool>("matches");
    QTest::newRow("escape dot") << "^%.env%." << ".env.prod" << true;
    QTest::newRow("escape dot, no match") << "^%.env%." << "xenv.prod" << false;
    QTest::newRow("digits") << "^log%d+%.txt$" << "log12.txt" << true;
    QTest::newRow("digits, none") << "^log%d+%.txt$" << "log.txt" << false;
    QTest::newRow("lazy dash") << "^a.-b$" << "axxb" << true;
    QTest::newRow("set") << "^[Mm]akefile$" << "makefile" << true;
    QTest::newRow("unanchored") << "rc" << ".bashrc" << true;
    QTest::newRow("regex metachar literal") << "^a+b$" << "aab" << true;
    QTest::newRow("brace is literal") << "^{x}$" << "{x}" << true;
}

void TestDiffLanguages::luaPattern()
{
    QFETCH(QString, pattern);
    QFETCH(QString, subject);
    QFETCH(bool, matches);
    const QRegularExpression re = regexFromLuaPattern(pattern);
    QVERIFY2(re.isValid(), qPrintable(re.pattern()));
    QCOMPARE(re.match(subject).hasMatch(), matches);
}

void TestDiffLanguages::fromSerialized_roundTrip()
{
    // name, lexer, filenames, patterns, extensions, styles (id,fg,font), keywords (set=words), properties (k=v)
    const QByteArray data = serialize({
        {"C++", "cpp", "", "", "cpp\x1dh", "0,0,0\x1d" "5,16711680,1\x1d" "1,32768,2",
         "0=int return\x1d" "1=class", "lexer.cpp.track.preprocessor=0"},
        {"Env", "props", ".env", "^%.env%.", "env", "0,0,0", "", ""},
    });
    const auto c = Catalog::fromSerialized(data);
    QVERIFY(c);
    QCOMPARE(c->languages().size(), 2);
    const LanguageDef &cpp = c->languages().at(0);
    QCOMPARE(cpp.lexer, QStringLiteral("cpp"));
    QCOMPARE(cpp.extensions, (QStringList{"cpp", "h"}));
    QCOMPARE(cpp.styles.size(), 3);
    QCOMPARE(cpp.styles[1].id, quint8(5));
    QCOMPARE(cpp.styles[1].fg, quint32(0x0000FF));   // Scintilla BGR 0xFF0000 -> RGB blue
    QCOMPARE(cpp.styles[1].fontStyle, quint8(1));
    QCOMPARE(cpp.setup.keywords.size(), 2);
    QCOMPARE(cpp.setup.keywords[0], QByteArray("int return"));
    QCOMPARE(cpp.setup.keywords[1], QByteArray("class"));
    QCOMPARE(cpp.setup.properties.size(), 1);
    QCOMPARE(cpp.setup.properties[0].first, QByteArray("lexer.cpp.track.preprocessor"));
    QCOMPARE(c->detect(".env.x"), 1);
    QCOMPARE(c->detect("a.h"), 0);
}

void TestDiffLanguages::compact_skipsDefaultAndPredefined()
{
    LanguageDef d = lang("C++", "cpp", {"cpp"}, {}, {}, 0);
    d.styles.append(StyleDef{11, 0x000000, 0});   // identifier, same as default: no slot
    d.styles.append(StyleDef{5, 0x0000FF, 1});
    d.styles.append(StyleDef{2, 0x008000, 0});
    d.styles.append(StyleDef{34, 0x0000FF, 0});   // brace light: predefined
    DiffDoc::Language out;
    QCOMPARE(compact(d, 40, out), 2);
    QCOMPARE(out.styleSlots[0], quint8(0));
    QCOMPARE(out.styleSlots[11], quint8(0));
    QCOMPARE(out.styleSlots[34], quint8(0));
    QCOMPARE(out.styleSlots[2], quint8(40));   // ascending id order
    QCOMPARE(out.styleSlots[5], quint8(41));
    QCOMPARE(out.slotStyles.size(), 2);
    QCOMPARE(out.slotStyles[1].slot, quint8(41));
    QCOMPARE(out.slotStyles[1].fg, quint32(0x0000FF));
    QCOMPARE(out.slotStyles[1].fontStyle, quint8(1));
    QCOMPARE(out.lexer, QStringLiteral("cpp"));
}

void TestDiffLanguages::compact_foldsInactiveStyles()
{
    LanguageDef d = lang("C++", "cpp", {"cpp"}, {}, {}, 5);
    d.styles.append(StyleDef{quint8(1 | 0x40), 0x808080, 0});   // explicitly defined inactive comment
    DiffDoc::Language out;
    QCOMPARE(compact(d, 100, out), 6);
    QCOMPARE(out.styleSlots[5 | 0x40], out.styleSlots[5]);       // folded
    QVERIFY(out.styleSlots[1 | 0x40] != out.styleSlots[1]);      // defined: own slot
}

void TestDiffLanguages::compact_overflow()
{
    LanguageDef d = lang("Big", "cpp", {"x"}, {}, {}, 20);
    DiffDoc::Language out;
    QCOMPARE(compact(d, 236, out), 20);    // 236..255
    DiffDoc::Language none;
    QCOMPARE(compact(d, 237, none), -1);
    for (quint8 s : none.styleSlots) QCOMPARE(s, quint8(0));
}

void TestDiffLanguages::assign_sharesRangePerLanguage()
{
    const Catalog c = sampleCatalog();
    const auto parsed = GitDiffParser::parse(
        "diff --git a/a.cpp b/a.cpp\n--- a/a.cpp\n+++ b/a.cpp\n@@ -1 +1 @@\n-x\n+y\n"
        "diff --git a/b.py b/b.py\n--- a/b.py\n+++ b/b.py\n@@ -1 +1 @@\n-x\n+y\n"
        "diff --git a/c.h b/c.h\n--- a/c.h\n+++ b/c.h\n@@ -1 +1 @@\n-x\n+y\n"
        "diff --git a/old.cpp b/new.py\nsimilarity index 90%\nrename from old.cpp\nrename to new.py\n"
        "--- a/old.cpp\n+++ b/new.py\n@@ -1 +1 @@\n-x\n+y\n"
        "diff --git a/n.txt b/n.txt\n--- a/n.txt\n+++ b/n.txt\n@@ -1 +1 @@\n-x\n+y\n");
    const Assignment a = assign(c, parsed);
    QCOMPARE(a.languages.size(), 2);
    QCOMPARE(a.languages[0].lexer, QStringLiteral("cpp"));
    QCOMPARE(a.languages[1].lexer, QStringLiteral("python"));
    QCOMPARE(a.oldLanguage, (QVector<qint16>{0, 1, 0, 0, -1}));
    QCOMPARE(a.newLanguage, (QVector<qint16>{0, 1, 0, 1, -1}));
    QCOMPARE(a.languages[0].styleSlots[1], quint8(40));
    QCOMPARE(a.languages[1].styleSlots[1], quint8(43));   // after cpp's 3 slots
}

void TestDiffLanguages::assign_overflowLeavesNoSyntax()
{
    QVector<LanguageDef> defs;
    QByteArray diff;
    for (int i = 0; i < 12; ++i) {
        const QString ext = QStringLiteral("e%1").arg(i);
        defs.append(lang(QStringLiteral("L%1").arg(i), QStringLiteral("cpp"), {ext}, {}, {}, 20));
        const QByteArray f = "f." + ext.toUtf8();
        diff += "diff --git a/" + f + " b/" + f + "\n--- a/" + f + "\n+++ b/" + f + "\n@@ -1 +1 @@\n-x\n+y\n";
    }
    const Catalog c(defs);
    const Assignment a = assign(c, GitDiffParser::parse(diff));
    QCOMPARE(a.languages.size(), 10);              // 10 * 20 slots fit in 40..255 (216)
    QCOMPARE(a.newLanguage.at(9), qint16(9));
    QCOMPARE(a.newLanguage.at(10), qint16(-1));
    QCOMPARE(a.newLanguage.at(11), qint16(-1));
    QCOMPARE(a.languages[9].styleSlots[20], quint8(40 + 9 * 20 + 19));
}

QTEST_GUILESS_MAIN(TestDiffLanguages)
#include "test_diff_languages.moc"
