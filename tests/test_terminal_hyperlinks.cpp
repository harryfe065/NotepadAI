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

#include "TerminalHyperlinks.h"

#include <map>
#include <utility>

namespace {

// Screen text by (line, column): what textBetween would read.
struct FakeText {
    std::map<std::pair<qint64, int>, QChar> cells;
    int reads = 0;

    void write(qint64 line, int col, const QString &s)
    {
        for (int i = 0; i < s.size(); ++i) cells[{line, col + i}] = s.at(i);
    }
    TerminalHyperlinks::TextReader reader()
    {
        return [this](qint64 startLine, int startCol, qint64 endLine, int endCol) {
            ++reads;
            QString out;
            for (qint64 line = startLine; line <= endLine; ++line) {
                const int from = line == startLine ? startCol : 0;
                const int to = line == endLine ? endCol : 80;
                for (int col = from; col < to; ++col) {
                    const auto it = cells.find({line, col});
                    out.append(it == cells.end() ? QLatin1Char(' ') : it->second);
                }
            }
            return out;
        };
    }
};

} // namespace

class TestTerminalHyperlinks : public QObject
{
    Q_OBJECT

private slots:
    void onlySafeSchemesOpen();
    void openThenCloseRecordsTheSpan();
    void lookupFindsTheNewestLiveSpan();
    void overwrittenTextKillsTheLinkOnce();
    void unchangedTextNeverRechecks();
    void altScreenAndEvictionDropLinks();
    void visibleSpansCoverTheirColumns();
};

void TestTerminalHyperlinks::onlySafeSchemesOpen()
{
    QVERIFY(TerminalHyperlinks::isAllowed(QStringLiteral("https://zed.dev")));
    QVERIFY(TerminalHyperlinks::isAllowed(QStringLiteral("MAILTO:a@b.c")));
    QVERIFY(TerminalHyperlinks::isAllowed(QStringLiteral("file:///tmp/x")));
    QVERIFY(!TerminalHyperlinks::isAllowed(QStringLiteral("ms-settings:privacy")));
    QVERIFY(!TerminalHyperlinks::isAllowed(QStringLiteral("javascript:alert(1)")));
    QVERIFY(!TerminalHyperlinks::isAllowed(QString()));

    FakeText text;
    TerminalHyperlinks links;
    QVERIFY(!links.handle(QStringLiteral("ms-settings:privacy"), 0, 0, text.reader()));
    QVERIFY(!links.handle(QString(), 0, 5, text.reader()));   // nothing was open
    QVERIFY(links.isEmpty());
}

void TestTerminalHyperlinks::openThenCloseRecordsTheSpan()
{
    FakeText text;
    text.write(3, 2, QStringLiteral("click"));
    TerminalHyperlinks links;
    QVERIFY(!links.handle(QStringLiteral("https://a.example"), 3, 2, text.reader()));
    const TerminalHyperlinks::Span *span = links.handle(QString(), 3, 7, text.reader());
    QVERIFY(span);
    QCOMPARE(span->uri, QStringLiteral("https://a.example"));
    QCOMPARE(span->text, QStringLiteral("click"));
    QCOMPARE(span->startCol, 2);
    QCOMPARE(span->endCol, 7);
    // An empty link (closed where it opened) is not recorded.
    links.handle(QStringLiteral("https://b.example"), 4, 0, text.reader());
    QVERIFY(!links.handle(QString(), 4, 0, text.reader()));
    QCOMPARE(links.spans().size(), std::size_t(1));
}

void TestTerminalHyperlinks::lookupFindsTheNewestLiveSpan()
{
    FakeText text;
    text.write(0, 0, QStringLiteral("one two"));
    TerminalHyperlinks links;
    links.handle(QStringLiteral("https://one"), 0, 0, text.reader());
    links.handle(QStringLiteral("https://two"), 0, 4, text.reader());   // closes one, opens two
    links.handle(QString(), 0, 7, text.reader());
    QCOMPARE(links.at(0, 1, text.reader())->uri, QStringLiteral("https://one"));
    QCOMPARE(links.at(0, 5, text.reader())->uri, QStringLiteral("https://two"));
    QVERIFY(!links.at(0, 7, text.reader()));   // end is exclusive
    QVERIFY(!links.at(1, 0, text.reader()));
}

void TestTerminalHyperlinks::overwrittenTextKillsTheLinkOnce()
{
    FakeText text;
    text.write(0, 0, QStringLiteral("link"));
    TerminalHyperlinks links;
    links.handle(QStringLiteral("https://x"), 0, 0, text.reader());
    links.handle(QString(), 0, 4, text.reader());
    text.write(0, 0, QStringLiteral("gone"));
    links.markStale(0, 1);
    QVERIFY(!links.at(0, 1, text.reader()));
    const int reads = text.reads;
    QVERIFY(!links.at(0, 1, text.reader()));   // dead for good, no re-read
    QCOMPARE(text.reads, reads);
}

void TestTerminalHyperlinks::unchangedTextNeverRechecks()
{
    FakeText text;
    text.write(5, 0, QStringLiteral("stay"));
    TerminalHyperlinks links;
    links.handle(QStringLiteral("https://x"), 5, 0, text.reader());
    links.handle(QString(), 5, 4, text.reader());
    const int reads = text.reads;
    for (int i = 0; i < 10; ++i) QVERIFY(links.at(5, 0, text.reader()));
    QCOMPARE(text.reads, reads);   // not stale: no reads at all
    links.markStale(0, 5);         // other lines only
    QVERIFY(links.at(5, 0, text.reader()));
    QCOMPARE(text.reads, reads);
    links.markStale(5, 6);         // its line: one re-read, still alive
    QVERIFY(links.at(5, 0, text.reader()));
    QCOMPARE(text.reads, reads + 1);
}

void TestTerminalHyperlinks::altScreenAndEvictionDropLinks()
{
    FakeText text;
    text.write(1, 0, QStringLiteral("aa"));
    text.write(9, 0, QStringLiteral("bb"));
    TerminalHyperlinks links;
    links.handle(QStringLiteral("https://a"), 1, 0, text.reader());
    links.handle(QString(), 1, 2, text.reader());
    links.handle(QStringLiteral("https://b"), 9, 0, text.reader());
    links.handle(QString(), 9, 2, text.reader());
    links.killFrom(5);   // the screen swapped below line 5
    QVERIFY(links.at(1, 0, text.reader()));
    QVERIFY(!links.at(9, 0, text.reader()));
    links.dropBefore(2);
    QCOMPARE(links.spans().size(), std::size_t(1));
    QVERIFY(!links.at(1, 0, text.reader()));
}

void TestTerminalHyperlinks::visibleSpansCoverTheirColumns()
{
    FakeText text;
    text.write(2, 6, QStringLiteral("abc"));
    text.write(3, 0, QStringLiteral("de"));
    TerminalHyperlinks links;
    links.handle(QStringLiteral("https://multi"), 2, 6, text.reader());   // wraps onto line 3
    links.handle(QString(), 3, 2, text.reader());
    links.gatherVisible(2, 3);
    QList<QPair<int, int>> seen;
    const auto visit = [&](int from, int to) { seen.append({from, to}); };
    links.forEachOnLine(2, text.reader(), visit);
    links.forEachOnLine(3, text.reader(), visit);
    links.forEachOnLine(4, text.reader(), visit);
    QCOMPARE(seen.size(), 2);
    QCOMPARE(seen[0], qMakePair(6, -1));   // to the end of the row
    QCOMPARE(seen[1], qMakePair(0, 2));
    links.gatherVisible(10, 20);
    seen.clear();
    links.forEachOnLine(2, text.reader(), visit);
    QVERIFY(seen.isEmpty());
}

QTEST_APPLESS_MAIN(TestTerminalHyperlinks)

#include "test_terminal_hyperlinks.moc"
