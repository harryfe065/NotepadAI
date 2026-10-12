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

#include <QtTest>

#include "TerminalLinkDetector.h"

namespace {

TerminalLink at(const QString &text, const QString &needle, int offset = 0)
{
    const int index = static_cast<int>(text.indexOf(needle)) + offset;
    return findTerminalLinkAt(text, index);
}

} // namespace

class TestTerminalLinks : public QObject
{
    Q_OBJECT

private slots:
    void url_plain();
    void url_trailingPunctuationTrimmed();
    void url_balancedParensKept();
    void url_otherSchemes();
    void path_lineAndColumn();
    void path_msvcParens();
    void path_windowsDrive();
    void path_pythonTraceback();
    void path_relativeWithExtension();
    void plainWordsAreNotLinks();
    void versionsAreNotPaths();
    void outsideTheSpanIsNone();
};

void TestTerminalLinks::url_plain()
{
    const QString text = QStringLiteral("see https://zed.dev/docs/terminal for more");
    const TerminalLink link = at(text, QStringLiteral("zed"));
    QCOMPARE(link.kind, TerminalLink::Url);
    QCOMPARE(link.target, QStringLiteral("https://zed.dev/docs/terminal"));
    QCOMPARE(link.start, static_cast<int>(text.indexOf(QStringLiteral("https"))));
}

void TestTerminalLinks::url_trailingPunctuationTrimmed()
{
    const QString text = QStringLiteral("(docs: https://example.com/a.)");
    QCOMPARE(at(text, QStringLiteral("example")).target, QStringLiteral("https://example.com/a"));
}

void TestTerminalLinks::url_balancedParensKept()
{
    const QString text = QStringLiteral("https://en.wikipedia.org/wiki/Foo_(bar) end");
    QCOMPARE(at(text, QStringLiteral("wiki")).target, QStringLiteral("https://en.wikipedia.org/wiki/Foo_(bar)"));
}

void TestTerminalLinks::url_otherSchemes()
{
    QCOMPARE(at(QStringLiteral("mailto:me@example.com"), QStringLiteral("me")).kind, TerminalLink::Url);
    QCOMPARE(at(QStringLiteral("file:///tmp/x.txt"), QStringLiteral("tmp")).kind, TerminalLink::Url);
}

void TestTerminalLinks::path_lineAndColumn()
{
    const QString text = QStringLiteral("src/main.cpp:42:7: error: expected ';'");
    const TerminalLink link = at(text, QStringLiteral("main"));
    QCOMPARE(link.kind, TerminalLink::Path);
    QCOMPARE(link.target, QStringLiteral("src/main.cpp"));
    QCOMPARE(link.line, 42);
    QCOMPARE(link.column, 7);
    QCOMPARE(link.start, 0);
}

void TestTerminalLinks::path_msvcParens()
{
    const QString text = QStringLiteral("C:\\proj\\a.cpp(12,5): error C2065");
    const TerminalLink link = at(text, QStringLiteral("a.cpp"));
    QCOMPARE(link.kind, TerminalLink::Path);
    QCOMPARE(link.target, QStringLiteral("C:\\proj\\a.cpp"));
    QCOMPARE(link.line, 12);
    QCOMPARE(link.column, 5);
}

void TestTerminalLinks::path_windowsDrive()
{
    const TerminalLink link = at(QStringLiteral("at D:/x/y.ts:10"), QStringLiteral("y.ts"));
    QCOMPARE(link.target, QStringLiteral("D:/x/y.ts"));
    QCOMPARE(link.line, 10);
    QCOMPARE(link.column, 0);
}

void TestTerminalLinks::path_pythonTraceback()
{
    const QString text = QStringLiteral("  File \"/srv/app/views.py\", line 88, in handler");
    const TerminalLink link = at(text, QStringLiteral("views"));
    QCOMPARE(link.kind, TerminalLink::Path);
    QCOMPARE(link.target, QStringLiteral("/srv/app/views.py"));
    QCOMPARE(link.line, 88);
}

void TestTerminalLinks::path_relativeWithExtension()
{
    const TerminalLink link = at(QStringLiteral("modified:   README.md"), QStringLiteral("README"));
    QCOMPARE(link.kind, TerminalLink::Path);
    QCOMPARE(link.target, QStringLiteral("README.md"));
}

void TestTerminalLinks::plainWordsAreNotLinks()
{
    QCOMPARE(at(QStringLiteral("hello world"), QStringLiteral("world")).kind, TerminalLink::None);
    QCOMPARE(at(QStringLiteral("a // comment"), QStringLiteral("//")).kind, TerminalLink::None);
}

void TestTerminalLinks::versionsAreNotPaths()
{
    QCOMPARE(at(QStringLiteral("version 1.2.3 ready"), QStringLiteral("1.2")).kind, TerminalLink::None);
}

void TestTerminalLinks::outsideTheSpanIsNone()
{
    const QString text = QStringLiteral("open src/a.cpp now");
    QCOMPARE(findTerminalLinkAt(text, 1).kind, TerminalLink::None);
    QCOMPARE(findTerminalLinkAt(text, -1).kind, TerminalLink::None);
    QCOMPARE(findTerminalLinkAt(text, 999).kind, TerminalLink::None);
}

QTEST_APPLESS_MAIN(TestTerminalLinks)

#include "test_terminal_links.moc"
