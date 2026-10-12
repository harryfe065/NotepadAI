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

#include "TerminalSearch.h"

#include <cstring>
#include <vector>

namespace {

std::vector<VTermScreenCell> row(const QString &s, int cols)
{
    std::vector<VTermScreenCell> cells(static_cast<std::size_t>(cols));
    for (int i = 0; i < cols; ++i) {
        VTermScreenCell &c = cells[static_cast<std::size_t>(i)];
        std::memset(&c, 0, sizeof(c));
        c.width = 1;
        c.fg.type = VTERM_COLOR_DEFAULT_FG;
        c.bg.type = VTERM_COLOR_DEFAULT_BG;
        if (i < s.size() && s.at(i) != QLatin1Char(' ')) {
            c.chars[0] = s.at(i).unicode();
        }
    }
    return cells;
}

TerminalSearchOptions plain(const QString &q)
{
    TerminalSearchOptions o;
    o.query = q;
    return o;
}

} // namespace

class TestTerminalSearch : public QObject
{
    Q_OBJECT

private slots:
    void emptyOrBrokenQueryIsInvalid();
    void plainIsLiteralAndCaseInsensitive();
    void caseSensitiveOption();
    void wholeWordOption();
    void regexOption();
    void matchColumnsSkipWideSpacer();
    void scrollbackSearchUsesStableLineIds();
    void cancelStopsEarly();
};

void TestTerminalSearch::emptyOrBrokenQueryIsInvalid()
{
    QVERIFY(!terminalSearchRegex(plain(QString())).isValid());
    TerminalSearchOptions o = plain(QStringLiteral("(unclosed"));
    o.regex = true;
    QVERIFY(!terminalSearchRegex(o).isValid());
    // The same text as a literal is fine.
    QVERIFY(terminalSearchRegex(plain(QStringLiteral("(unclosed"))).isValid());
}

void TestTerminalSearch::plainIsLiteralAndCaseInsensitive()
{
    const auto cells = row(QStringLiteral("Error: a.b error"), 20);
    std::vector<TerminalSearchMatch> out;
    QString text;
    std::vector<int> colOf;
    searchTerminalRow(terminalSearchRegex(plain(QStringLiteral("error"))), 7, cells.data(), 20, out, text, colOf);
    QCOMPARE(out.size(), std::size_t(2));
    QCOMPARE(out[0].line, qint64(7));
    QCOMPARE(out[0].col, 0);
    QCOMPARE(out[0].width, 5);
    QCOMPARE(out[1].col, 11);

    out.clear();
    searchTerminalRow(terminalSearchRegex(plain(QStringLiteral("a.b"))), 0, cells.data(), 20, out, text, colOf);
    QCOMPARE(out.size(), std::size_t(1));
    QCOMPARE(out[0].col, 7);
}

void TestTerminalSearch::caseSensitiveOption()
{
    const auto cells = row(QStringLiteral("Error error"), 12);
    TerminalSearchOptions o = plain(QStringLiteral("Error"));
    o.caseSensitive = true;
    std::vector<TerminalSearchMatch> out;
    QString text;
    std::vector<int> colOf;
    searchTerminalRow(terminalSearchRegex(o), 0, cells.data(), 12, out, text, colOf);
    QCOMPARE(out.size(), std::size_t(1));
}

void TestTerminalSearch::wholeWordOption()
{
    const auto cells = row(QStringLiteral("cat concat cat."), 16);
    TerminalSearchOptions o = plain(QStringLiteral("cat"));
    o.wholeWord = true;
    std::vector<TerminalSearchMatch> out;
    QString text;
    std::vector<int> colOf;
    searchTerminalRow(terminalSearchRegex(o), 0, cells.data(), 16, out, text, colOf);
    QCOMPARE(out.size(), std::size_t(2));
    QCOMPARE(out[1].col, 11);
}

void TestTerminalSearch::regexOption()
{
    const auto cells = row(QStringLiteral("took 120ms and 7ms"), 20);
    TerminalSearchOptions o = plain(QStringLiteral("\\d+ms"));
    o.regex = true;
    std::vector<TerminalSearchMatch> out;
    QString text;
    std::vector<int> colOf;
    searchTerminalRow(terminalSearchRegex(o), 0, cells.data(), 20, out, text, colOf);
    QCOMPARE(out.size(), std::size_t(2));
    QCOMPARE(out[0].col, 5);
    QCOMPARE(out[0].width, 5);
}

void TestTerminalSearch::matchColumnsSkipWideSpacer()
{
    // 中 occupies columns 0-1; "x" sits in column 2.
    auto cells = row(QStringLiteral("  x"), 3);
    cells[0].chars[0] = 0x4E2D;
    cells[0].width = 2;
    cells[1].chars[0] = 0xFFFFFFFFu;
    std::vector<TerminalSearchMatch> out;
    QString text;
    std::vector<int> colOf;
    searchTerminalRow(terminalSearchRegex(plain(QStringLiteral("x"))), 0, cells.data(), 3, out, text, colOf);
    QCOMPARE(out.size(), std::size_t(1));
    QCOMPARE(out[0].col, 2);

    out.clear();
    const char32_t han = 0x4E2D;
    searchTerminalRow(terminalSearchRegex(plain(QString::fromUcs4(&han, 1))), 0, cells.data(), 3, out, text, colOf);
    QCOMPARE(out.size(), std::size_t(1));
    QCOMPARE(out[0].col, 0);
    QCOMPARE(out[0].width, 2);
}

void TestTerminalSearch::scrollbackSearchUsesStableLineIds()
{
    ScrollbackBuffer sb(3);
    for (int i = 0; i < 5; ++i) {
        const auto cells = row(i % 2 ? QStringLiteral("hit") : QStringLiteral("miss"), 6);
        sb.pushLine(6, cells.data());
    }
    // Lines 0 and 1 were evicted: history index 0 is line id 2.
    const auto found = searchScrollback(sb, 2, 0, 6, terminalSearchRegex(plain(QStringLiteral("hit"))));
    QCOMPARE(found.size(), std::size_t(1));
    QCOMPARE(found[0].line, qint64(3));
}

void TestTerminalSearch::cancelStopsEarly()
{
    ScrollbackBuffer sb(100);
    const auto cells = row(QStringLiteral("hit"), 4);
    for (int i = 0; i < 50; ++i) sb.pushLine(4, cells.data());
    std::atomic<bool> cancel(true);
    QVERIFY(searchScrollback(sb, 0, 0, 4, terminalSearchRegex(plain(QStringLiteral("hit"))), &cancel).empty());
}

QTEST_APPLESS_MAIN(TestTerminalSearch)

#include "test_terminal_search.moc"
