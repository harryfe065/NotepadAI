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

#include "DiffDocumentBuilder.h"
#include "GitCommitDetail.h"

using namespace DiffDoc;
using Reason = DiffFileClassifier::Reason;

namespace {

const QByteArray kModified =
    "diff --git a/src/a.cpp b/src/a.cpp\n"
    "index 1111111..2222222 100644\n"
    "--- a/src/a.cpp\n"
    "+++ b/src/a.cpp\n"
    "@@ -1,3 +1,3 @@ int main()\n"
    " int a = 1;\n"
    "-int b = 2;\n"
    "+int b = 3;\n"
    " return a;\n";

const QByteArray kDeleted =
    "diff --git a/src/gone.cpp b/src/gone.cpp\n"
    "deleted file mode 100644\n"
    "index 3333333..0000000\n"
    "--- a/src/gone.cpp\n"
    "+++ /dev/null\n"
    "@@ -1,3 +0,0 @@\n"
    "-one\n"
    "-two\n"
    "-three\n";

const QByteArray kAdded =
    "diff --git a/src/new.cpp b/src/new.cpp\n"
    "new file mode 100644\n"
    "index 0000000..4444444\n"
    "--- /dev/null\n"
    "+++ b/src/new.cpp\n"
    "@@ -0,0 +1,2 @@\n"
    "+one\n"
    "+two\n";

const QByteArray kLock =
    "diff --git a/package-lock.json b/package-lock.json\n"
    "index 5555555..6666666 100644\n"
    "--- a/package-lock.json\n"
    "+++ b/package-lock.json\n"
    "@@ -1,2 +1,2 @@\n"
    "-\"a\": 1\n"
    "+\"a\": 2\n"
    " }\n";

std::shared_ptr<Input> inputFor(const QByteArray &diff)
{
    auto in = std::make_shared<Input>();
    in->parsed = std::make_shared<const GitDiffParser::Result>(GitDiffParser::parse(diff));
    in->reasons = DiffDocumentBuilder::classifyFiles(*in->parsed, {});
    return in;
}

std::shared_ptr<const Document> docFor(const QByteArray &diff)
{
    return DiffDocumentBuilder::build(inputFor(diff));
}

// Byte offset of the start of `row` in `text`.
qsizetype rowStart(const Rows &rows, qint32 row)
{
    qsizetype pos = 0;
    for (qint32 r = 0; r < row; ++r) pos = rows.text.indexOf('\n', pos) + 1;
    return pos;
}

QString rowText(const Rows &rows, qint32 row)
{
    const qsizetype s = rowStart(rows, row);
    return QString::fromUtf8(rows.text.mid(s, rows.text.indexOf('\n', s) - s));
}

int styleAt(const Rows &rows, qint32 row, const QString &needle, int offset = 0)
{
    const qsizetype s = rowStart(rows, row);
    const QByteArray line = rows.text.mid(s, rows.text.indexOf('\n', s) - s);
    const qsizetype col = line.indexOf(needle.toUtf8());
    if (col < 0) return -1;
    return static_cast<unsigned char>(rows.styles.at(s + col + offset));
}

int findRow(const Rows &rows, const QString &prefix)
{
    for (qint32 r = 0; r < rows.kinds.size(); ++r)
        if (rowText(rows, r).startsWith(prefix)) return r;
    return -1;
}

const Link *findLink(const Document &doc, LinkKind kind, int nth = 0)
{
    for (const Link &l : doc.links)
        if (l.kind == kind && nth-- == 0) return &l;
    return nullptr;
}

QString linkText(const Rows &rows, const Link &l)
{
    return QString::fromUtf8(rows.text.mid(l.pos, l.len));
}

void verifyConsistent(const Rows &rows)
{
    QCOMPARE(rows.styles.size(), rows.text.size());
    QCOMPARE(qsizetype(rows.text.count('\n')), rows.kinds.size());
    QCOMPARE(rows.oldLn.size(), rows.kinds.size());
    QCOMPARE(rows.newLn.size(), rows.kinds.size());
    QVERIFY(rows.text.isEmpty() || rows.text.endsWith('\n'));
}

std::shared_ptr<const GitCommitDetail> commit(const QByteArray &parents, const QByteArray &body,
                                              const QByteArray &committer = "Alice")
{
    auto c = std::make_shared<GitCommitDetail>();
    c->sha = "1234567890abcdef1234567890abcdef12345678";
    c->parents = parents;
    c->authorName = "Alice";
    c->authorTime = 1'700'000'000;
    c->committerName = committer;
    c->commitTime = 1'700'000'000;
    c->subject = "Fix the parser";
    c->body = "Fix the parser\n\n" + body;
    return c;
}

} // namespace

class TestDiffDocumentBuilder : public QObject
{
    Q_OBJECT

private slots:
    void rowTables_consistent();
    void rawHeaders_replacedByOneHeaderRow();
    void header_modified();
    void header_deletedIsStruckAndCollapsed();
    void header_renameSameDir();
    void header_renameOtherDir();
    void header_badges();
    void squares_data();
    void squares();
    void hunkHeader_splitsRangeAndContext();
    void body_prefixesAndLineNumbers();
    void collapsed_headerAndPlaceholderOnly();
    void singleFile_neverCollapsed();
    void fragment_equalsInlineBuild();
    void placeholder_equalsCollapsedInlineRow();
    void separators_betweenFiles();
    void card_mergeWithTwoParents();
    void card_committerDiffers();
    void card_emptyCommit();
    void card_bodyLinks();
    void wordSpans_absolute();
    void syntax_remapsLexerStyles();
};

void TestDiffDocumentBuilder::rowTables_consistent()
{
    auto in = inputFor(kModified + kDeleted + kAdded + kLock);
    in->commit = commit("abcdef0123456789abcdef0123456789abcdef01", "Body line.\n");
    in->nowSecs = 1'700'000'100;
    const auto doc = DiffDocumentBuilder::build(in);
    verifyConsistent(*doc);
    QCOMPARE(doc->files.size(), 4);
    for (const File &f : doc->files) {
        QCOMPARE(doc->kinds.at(f.headerRow), RowKind::FileHeader);
        QCOMPARE(rowText(*doc, f.headerRow).toUtf8(), f.headerText);
    }
    for (qint32 h : doc->hunkRows) QCOMPARE(doc->kinds.at(h), RowKind::HunkHeader);
}

void TestDiffDocumentBuilder::rawHeaders_replacedByOneHeaderRow()
{
    const auto doc = docFor(kModified);
    QVERIFY(!doc->text.contains("diff --git"));
    QVERIFY(!doc->text.contains("index 1111111"));
    QVERIFY(!doc->text.contains("--- a/"));
    QVERIFY(!doc->text.contains("+++ b/"));
    // header, hunk, 4 body rows
    QCOMPARE(doc->kinds.size(), 6);
    QCOMPARE(doc->files.at(0).headerRow, 0);
    QCOMPARE(doc->files.at(0).bodyRows, 5);
}

void TestDiffDocumentBuilder::header_modified()
{
    const auto doc = docFor(kModified);
    QCOMPARE(rowText(*doc, 0), QString::fromUtf8("▾ M  src/a.cpp  +1 −1 ■■■■■"));
    QCOMPARE(styleAt(*doc, 0, "▾"), int(Style::Chevron));
    QCOMPARE(styleAt(*doc, 0, "M"), int(Style::StatusModified));
    QCOMPARE(styleAt(*doc, 0, "src/"), int(Style::Dir));
    QCOMPARE(styleAt(*doc, 0, "a.cpp"), int(Style::Name));
    QCOMPARE(styleAt(*doc, 0, "+1"), int(Style::Plus));
    QCOMPARE(styleAt(*doc, 0, "−1"), int(Style::Minus));
    QCOMPARE(styleAt(*doc, 0, "■"), int(Style::SqAdd));
    QCOMPARE(styleAt(*doc, 0, "■", 3), int(Style::SqDel));
    QCOMPARE(styleAt(*doc, 0, "■", 6), int(Style::SqNeutral));

    const Link *chevron = findLink(*doc, LinkKind::Chevron);
    QVERIFY(chevron);
    QCOMPARE(linkText(*doc, *chevron), QString::fromUtf8("▾"));
    const Link *path = findLink(*doc, LinkKind::Path);
    QVERIFY(path);
    QCOMPARE(linkText(*doc, *path), QStringLiteral("src/a.cpp"));
    QCOMPARE(path->file, 0);
}

void TestDiffDocumentBuilder::header_deletedIsStruckAndCollapsed()
{
    const auto doc = docFor(kModified + kDeleted);
    const File &f = doc->files.at(1);
    QCOMPARE(f.reason, Reason::Deleted);
    QVERIFY(f.collapsed);
    QCOMPARE(rowText(*doc, f.headerRow), QString::fromUtf8("▸ D  src/gone.cpp  +0 −3 ■■■■■"));
    QCOMPARE(rowText(*doc, f.headerRow + 1), QStringLiteral("  This file was deleted. Load diff"));
    QCOMPARE(styleAt(*doc, f.headerRow, "gone.cpp"), int(Style::NameStruck));
    QCOMPARE(doc->strikes.size(), 1);
    QCOMPARE(QString::fromUtf8(doc->text.mid(doc->strikes[0].pos, doc->strikes[0].len)),
             QStringLiteral("src/gone.cpp"));
}

void TestDiffDocumentBuilder::header_renameSameDir()
{
    const auto doc = docFor(
        "diff --git a/a/old.cpp b/a/new.cpp\n"
        "similarity index 90%\n"
        "rename from a/old.cpp\n"
        "rename to a/new.cpp\n"
        "--- a/a/old.cpp\n"
        "+++ b/a/new.cpp\n"
        "@@ -1 +1 @@\n"
        "-x\n"
        "+y\n");
    QCOMPARE(rowText(*doc, 0), QString::fromUtf8("▾ R  a/old.cpp → new.cpp  +1 −1 ■■■■■"));
    QCOMPARE(styleAt(*doc, 0, "R"), int(Style::StatusRenamed));
    QCOMPARE(styleAt(*doc, 0, "old.cpp"), int(Style::Dir));
    QCOMPARE(styleAt(*doc, 0, "new.cpp"), int(Style::Name));
    QCOMPARE(doc->files[0].oldPath, QByteArray("a/old.cpp"));
    QCOMPARE(doc->files[0].path, QByteArray("a/new.cpp"));
}

void TestDiffDocumentBuilder::header_renameOtherDir()
{
    const auto doc = docFor(
        "diff --git a/x/old.cpp b/y/new.cpp\n"
        "similarity index 100%\n"
        "rename from x/old.cpp\n"
        "rename to y/new.cpp\n");
    QCOMPARE(rowText(*doc, 0), QString::fromUtf8("▾ R  x/old.cpp → y/new.cpp"));
    QCOMPARE(styleAt(*doc, 0, "y/"), int(Style::Dir));
    QCOMPARE(styleAt(*doc, 0, "new.cpp"), int(Style::Name));
    QCOMPARE(rowText(*doc, 1), QStringLiteral("  File renamed without changes."));
    QCOMPARE(doc->kinds.at(1), RowKind::Placeholder);
}

void TestDiffDocumentBuilder::header_badges()
{
    const auto mode = docFor(
        "diff --git a/run.sh b/run.sh\n"
        "old mode 100644\n"
        "new mode 100755\n");
    QCOMPARE(rowText(*mode, 0), QString::fromUtf8("▾ M  run.sh  100644 → 100755"));
    QCOMPARE(styleAt(*mode, 0, "100644"), int(Style::Badge));
    QCOMPARE(rowText(*mode, 1), QStringLiteral("  File mode changed."));

    const auto bin = docFor(
        "diff --git a/img.png b/img.png\n"
        "index 1111111..2222222 100644\n"
        "Binary files a/img.png and b/img.png differ\n");
    QCOMPARE(rowText(*bin, 0), QString::fromUtf8("▾ M  img.png  binary"));
    QCOMPARE(styleAt(*bin, 0, "binary"), int(Style::Badge));
    QCOMPARE(rowText(*bin, 1), QStringLiteral("  Binary file not shown."));
    // Nothing to fold: the chevron is not clickable.
    QVERIFY(!findLink(*bin, LinkKind::Chevron));
    QVERIFY(!findLink(*mode, LinkKind::Chevron));
    QVERIFY(findLink(*bin, LinkKind::Path));

    const auto sub = docFor(
        "diff --git a/lib b/lib\n"
        "index 1111111..2222222 160000\n"
        "--- a/lib\n"
        "+++ b/lib\n"
        "@@ -1 +1 @@\n"
        "-Subproject commit 1111111111111111111111111111111111111111\n"
        "+Subproject commit 2222222222222222222222222222222222222222\n");
    QCOMPARE(rowText(*sub, 0), QString::fromUtf8("▾ M  lib  +1 −1 ■■■■■  submodule"));
    QCOMPARE(styleAt(*sub, 0, "submodule"), int(Style::Badge));
}

void TestDiffDocumentBuilder::squares_data()
{
    QTest::addColumn<int>("added");
    QTest::addColumn<int>("deleted");
    QTest::addColumn<int>("green");
    QTest::addColumn<int>("red");
    QTest::newRow("30/10") << 30 << 10 << 3 << 1;
    QTest::newRow("0/0") << 0 << 0 << 0 << 0;
    QTest::newRow("1/0") << 1 << 0 << 1 << 0;
    QTest::newRow("5000/1") << 5000 << 1 << 4 << 0;
    QTest::newRow("2/3") << 2 << 3 << 2 << 3;
    QTest::newRow("0/7") << 0 << 7 << 0 << 5;
}

void TestDiffDocumentBuilder::squares()
{
    QFETCH(int, added);
    QFETCH(int, deleted);
    QFETCH(int, green);
    QFETCH(int, red);
    const auto s = DiffDocumentBuilder::squares(added, deleted);
    QCOMPARE(s.green, green);
    QCOMPARE(s.red, red);
    QCOMPARE(s.grey, 5 - green - red);
}

void TestDiffDocumentBuilder::hunkHeader_splitsRangeAndContext()
{
    const auto doc = docFor(kModified);
    QCOMPARE(doc->hunkRows, QVector<qint32>{1});
    QCOMPARE(rowText(*doc, 1), QStringLiteral("@@ -1,3 +1,3 @@ int main()"));
    QCOMPARE(styleAt(*doc, 1, "@@ -1"), int(Style::HunkRange));
    QCOMPARE(styleAt(*doc, 1, "+1,3 @@"), int(Style::HunkRange));
    QCOMPARE(styleAt(*doc, 1, "int main"), int(Style::HunkContext));
}

void TestDiffDocumentBuilder::body_prefixesAndLineNumbers()
{
    const auto doc = docFor(kModified);
    QCOMPARE(rowText(*doc, 2), QStringLiteral(" int a = 1;"));
    QCOMPARE(rowText(*doc, 3), QStringLiteral("-int b = 2;"));
    QCOMPARE(rowText(*doc, 4), QStringLiteral("+int b = 3;"));
    QCOMPARE(doc->kinds.at(2), RowKind::Context);
    QCOMPARE(doc->kinds.at(3), RowKind::Deleted);
    QCOMPARE(doc->kinds.at(4), RowKind::Added);
    QCOMPARE(doc->oldLn.at(3), 2);
    QCOMPARE(doc->newLn.at(3), -1);
    QCOMPARE(doc->oldLn.at(4), -1);
    QCOMPARE(doc->newLn.at(4), 2);
    QCOMPARE(doc->oldLn.at(0), -1);
    QCOMPARE(styleAt(*doc, 3, "-"), int(Style::Deleted));
    QCOMPARE(styleAt(*doc, 4, "+"), int(Style::Added));
    QCOMPARE(styleAt(*doc, 2, "int"), int(Style::Context));
    QCOMPARE(doc->maxOldLn, 3);
    QCOMPARE(doc->maxNewLn, 3);
}

void TestDiffDocumentBuilder::collapsed_headerAndPlaceholderOnly()
{
    const auto doc = docFor(kModified + kLock);
    const File &lock = doc->files.at(1);
    QCOMPARE(lock.reason, Reason::Lockfile);
    QVERIFY(lock.collapsed);
    QCOMPARE(lock.bodyRows, 1);
    QVERIFY(rowText(*doc, lock.headerRow).startsWith(QString::fromUtf8("▸ M  package-lock.json")));
    QCOMPARE(rowText(*doc, lock.headerRow + 1), QStringLiteral("  Lockfile not shown. Load diff"));
    QCOMPARE(doc->kinds.at(lock.headerRow + 1), RowKind::Placeholder);
    QVERIFY(!doc->text.contains("\"a\": 2"));
    QCOMPARE(doc->hunkRows.size(), 1);       // the lockfile's hunk is not in the buffer
    QCOMPARE(doc->kinds.size(), lock.headerRow + 2);

    const Link *load = findLink(*doc, LinkKind::LoadDiff);
    QVERIFY(load);
    QCOMPARE(linkText(*doc, *load), QStringLiteral("Load diff"));
    QCOMPARE(load->file, 1);
    QCOMPARE(styleAt(*doc, lock.headerRow + 1, "Load diff"), int(Style::Link));
    QCOMPARE(styleAt(*doc, lock.headerRow + 1, "Lockfile"), int(Style::Placeholder));
}

void TestDiffDocumentBuilder::singleFile_neverCollapsed()
{
    const auto doc = docFor(kLock);
    QVERIFY(!doc->files.at(0).collapsed);
    QVERIFY(doc->text.contains("\"a\": 2"));
}

void TestDiffDocumentBuilder::fragment_equalsInlineBuild()
{
    auto in = inputFor(kModified + kLock + kAdded);
    const auto collapsedDoc = DiffDocumentBuilder::build(in);
    QVERIFY(collapsedDoc->files.at(1).collapsed);

    auto expanded = std::make_shared<Input>(*in);
    expanded->collapsed = {false, false, false};
    const auto inlineDoc = DiffDocumentBuilder::build(expanded);
    const File &f = inlineDoc->files.at(1);
    QVERIFY(!f.collapsed);

    const Rows frag = DiffDocumentBuilder::buildBody(*in, 1);
    verifyConsistent(frag);
    QCOMPARE(frag.kinds.size(), f.bodyRows);
    const qsizetype s = rowStart(*inlineDoc, f.headerRow + 1);
    const qsizetype e = rowStart(*inlineDoc, f.headerRow + 1 + f.bodyRows);
    QCOMPARE(frag.text, inlineDoc->text.mid(s, e - s));
    QCOMPARE(frag.styles, inlineDoc->styles.mid(s, e - s));
    QCOMPARE(frag.kinds, inlineDoc->kinds.mid(f.headerRow + 1, f.bodyRows));
    QCOMPARE(frag.oldLn, inlineDoc->oldLn.mid(f.headerRow + 1, f.bodyRows));
    QCOMPARE(frag.newLn, inlineDoc->newLn.mid(f.headerRow + 1, f.bodyRows));
    QCOMPARE(frag.hunkRows, QVector<qint32>{0});
}

void TestDiffDocumentBuilder::placeholder_equalsCollapsedInlineRow()
{
    auto in = inputFor(kModified + kLock);
    const auto doc = DiffDocumentBuilder::build(in);
    const File &f = doc->files.at(1);
    const Rows ph = DiffDocumentBuilder::buildPlaceholder(*in, 1);
    verifyConsistent(ph);
    QCOMPARE(ph.kinds.size(), 1);
    const qsizetype s = rowStart(*doc, f.headerRow + 1);
    QCOMPARE(ph.text, doc->text.mid(s));
    QCOMPARE(ph.styles, doc->styles.mid(s));
    QCOMPARE(ph.links.size(), 1);
    QCOMPARE(ph.links[0].kind, LinkKind::LoadDiff);
}

void TestDiffDocumentBuilder::separators_betweenFiles()
{
    const auto doc = docFor(kModified + kAdded);
    QCOMPARE(doc->files.at(0).headerRow, 0);   // no separator above the first file
    const qint32 h = doc->files.at(1).headerRow;
    QCOMPARE(doc->kinds.at(h - 1), RowKind::Separator);
    QCOMPARE(rowText(*doc, h - 1), QString());
    QCOMPARE(doc->files.at(0).bodyRows, h - 1 - doc->files.at(0).headerRow - 1);
}

void TestDiffDocumentBuilder::card_mergeWithTwoParents()
{
    auto in = inputFor(kModified);
    in->commit = commit("abcdef0123456789abcdef0123456789abcdef01 9876543210fedcba9876543210fedcba98765432", {});
    in->nowSecs = 1'700'000'000 + 3 * 3600 + 5;
    const auto doc = DiffDocumentBuilder::build(in);
    verifyConsistent(*doc);
    QCOMPARE(rowText(*doc, 0), QStringLiteral("Fix the parser"));
    QCOMPARE(doc->kinds.at(0), RowKind::CardTitle);
    const int commitRow = findRow(*doc, QStringLiteral("commit "));
    QVERIFY(commitRow > 0);
    QCOMPARE(rowText(*doc, commitRow),
             QString::fromUtf8("commit 1234567 Copy · parent abcdef0 · parent 9876543"));
    const Link *p0 = findLink(*doc, LinkKind::Parent, 0);
    const Link *p1 = findLink(*doc, LinkKind::Parent, 1);
    QVERIFY(p0 && p1);
    QCOMPARE(linkText(*doc, *p0), QStringLiteral("abcdef0"));
    QCOMPARE(p0->payload, QByteArray("abcdef0123456789abcdef0123456789abcdef01"));
    QCOMPARE(p1->payload, QByteArray("9876543210fedcba9876543210fedcba98765432"));
    const Link *copy = findLink(*doc, LinkKind::CopySha, 1);
    QVERIFY(copy);
    QCOMPARE(linkText(*doc, *copy), QStringLiteral("Copy"));
    QCOMPARE(copy->payload, in->commit->sha);

    const int metaRow = findRow(*doc, QStringLiteral("Alice"));
    QCOMPARE(rowText(*doc, metaRow), QString::fromUtf8("Alice · authored 3 hours ago"));
    QCOMPARE(doc->tips.size(), 1);
    QCOMPARE(QString::fromUtf8(doc->text.mid(doc->tips[0].pos, doc->tips[0].len)),
             QStringLiteral("3 hours ago"));
    QVERIFY(!doc->tips[0].text.isEmpty());

    QCOMPARE(rowText(*doc, findRow(*doc, QStringLiteral("1 file"))),
             QString::fromUtf8("1 file changed  +1 −1 ■■■■■"));
    QCOMPARE(doc->kinds.at(doc->files.at(0).headerRow - 1), RowKind::Separator);
}

void TestDiffDocumentBuilder::card_committerDiffers()
{
    auto in = inputFor(kModified);
    in->commit = commit("abcdef0123456789abcdef0123456789abcdef01", {}, "Bob");
    in->nowSecs = 1'700'000'030;
    const auto doc = DiffDocumentBuilder::build(in);
    QCOMPARE(rowText(*doc, findRow(*doc, QStringLiteral("Alice"))),
             QString::fromUtf8("Alice · authored just now · committed by Bob"));
}

void TestDiffDocumentBuilder::card_emptyCommit()
{
    auto in = inputFor({});
    in->commit = commit("abcdef0123456789abcdef0123456789abcdef01", {});
    in->nowSecs = 1'700'000'000;
    const auto doc = DiffDocumentBuilder::build(in);
    verifyConsistent(*doc);
    QVERIFY(doc->files.isEmpty());
    const int row = findRow(*doc, QStringLiteral("  No changes to show"));
    QVERIFY(row > 0);
    QCOMPARE(doc->kinds.at(row), RowKind::Placeholder);
    QCOMPARE(findRow(*doc, QStringLiteral("0 files")), -1);
}

void TestDiffDocumentBuilder::card_bodyLinks()
{
    auto in = inputFor(kModified);
    in->commit = commit("abcdef0123456789abcdef0123456789abcdef01",
                        "Fixes #12 and (#7).\nSee https://example.com/x?a=1.\nNot a#3 link.\n");
    in->issueUrlBase = "https://github.com/o/r/issues/";
    in->nowSecs = 1'700'000'000;
    const auto doc = DiffDocumentBuilder::build(in);
    QVector<QString> texts;
    QVector<QByteArray> payloads;
    for (const Link &l : doc->links) {
        if (l.kind != LinkKind::Url) continue;
        texts << linkText(*doc, l);
        payloads << l.payload;
    }
    QCOMPARE(texts, (QVector<QString>{QStringLiteral("#12"), QStringLiteral("#7"),
                                      QStringLiteral("https://example.com/x?a=1")}));
    QCOMPARE(payloads, (QVector<QByteArray>{"https://github.com/o/r/issues/12",
                                            "https://github.com/o/r/issues/7",
                                            "https://example.com/x?a=1"}));
    const int row = findRow(*doc, QStringLiteral("Fixes"));
    QCOMPARE(doc->kinds.at(row), RowKind::CardBody);
    QCOMPARE(styleAt(*doc, row, "#12"), int(Style::Link));
    QCOMPARE(styleAt(*doc, row, "Fixes"), int(Style::CardBody));

    // Without a recognised forge, #N stays plain text.
    in->issueUrlBase.clear();
    const auto plain = DiffDocumentBuilder::build(in);
    int urls = 0;
    for (const Link &l : plain->links) urls += l.kind == LinkKind::Url;
    QCOMPARE(urls, 1);
}

void TestDiffDocumentBuilder::wordSpans_absolute()
{
    const auto doc = docFor(kModified);
    QCOMPARE(doc->delWords.size(), 1);
    QCOMPARE(doc->addWords.size(), 1);
    QCOMPARE(QString::fromUtf8(doc->text.mid(doc->delWords[0].pos, doc->delWords[0].len)),
             QStringLiteral("2"));
    QCOMPARE(QString::fromUtf8(doc->text.mid(doc->addWords[0].pos, doc->addWords[0].len)),
             QStringLiteral("3"));
}

void TestDiffDocumentBuilder::syntax_remapsLexerStyles()
{
    auto in = inputFor(kModified);
    Language cpp;
    cpp.lexer = QStringLiteral("cpp");
    cpp.setup.keywords = {"int return"};
    cpp.styleSlots.fill(0);
    cpp.styleSlots[5] = 41;            // SCE_C_WORD
    cpp.styleSlots[5 | 0x40] = 41;     // inactive keyword folded onto the same slot
    cpp.styleSlots[4] = 42;            // SCE_C_NUMBER
    in->languages = {cpp};
    in->oldLanguage = {0};
    in->newLanguage = {0};
    const auto doc = DiffDocumentBuilder::build(in);
    QCOMPARE(styleAt(*doc, 2, "int"), 41);   // context row, new side
    QCOMPARE(styleAt(*doc, 2, "1"), 42);
    QCOMPARE(styleAt(*doc, 3, "int"), 41);   // deleted row, old side
    QCOMPARE(styleAt(*doc, 3, "-"), int(Style::Deleted));
    QCOMPARE(styleAt(*doc, 3, "b"), int(Style::Deleted));   // identifier: no slot → base style
    QCOMPARE(styleAt(*doc, 4, "+"), int(Style::Added));

    // A file without a language keeps base styles but still gets word spans.
    in->oldLanguage = {-1};
    in->newLanguage = {-1};
    const auto plain = DiffDocumentBuilder::build(in);
    QCOMPARE(styleAt(*plain, 2, "int"), int(Style::Context));
    QCOMPARE(plain->addWords.size(), 1);
}

QTEST_GUILESS_MAIN(TestDiffDocumentBuilder)
#include "test_diff_document_builder.moc"
