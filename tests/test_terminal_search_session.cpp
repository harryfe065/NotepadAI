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

#include "ScrollbackBuffer.h"
#include "TerminalSearchSession.h"

#include <cstring>
#include <vector>

namespace {

using State = TerminalSearchSession::State;

std::vector<VTermScreenCell> row(const QString &s, int cols)
{
    std::vector<VTermScreenCell> cells(static_cast<std::size_t>(cols));
    for (int i = 0; i < cols; ++i) {
        VTermScreenCell &c = cells[static_cast<std::size_t>(i)];
        std::memset(&c, 0, sizeof(c));
        c.width = 1;
        c.fg.type = VTERM_COLOR_DEFAULT_FG;
        c.bg.type = VTERM_COLOR_DEFAULT_BG;
        if (i < s.size() && s.at(i) != QLatin1Char(' ')) c.chars[0] = s.at(i).unicode();
    }
    return cells;
}

// History in a real ScrollbackBuffer plus a few live rows, like the widget.
class FakeTerminal final : public TerminalSearchSession::Source
{
public:
    FakeTerminal(int capacity, int cols, int liveRows)
        : m_history(capacity), m_cols(cols), m_live(static_cast<std::size_t>(liveRows), row(QString(), cols))
    {
    }

    void push(const QString &text)
    {
        const auto cells = row(text, m_cols);
        m_dropped += m_history.pushLine(m_cols, cells.data());
    }
    void setLive(int r, const QString &text) { m_live[static_cast<std::size_t>(r)] = row(text, m_cols); }

    qint64 firstLine() const override { return m_dropped; }
    qint64 historyEnd() const override { return m_dropped + m_history.size(); }
    int columns() const override { return m_cols; }
    int liveRows() const override { return static_cast<int>(m_live.size()); }
    bool historyLine(qint64 line, VTermScreenCell *cells) const override
    {
        return m_history.lineAt(static_cast<int>(line - m_dropped), m_cols, cells);
    }
    bool liveRow(int r, VTermScreenCell *cells) const override
    {
        const auto &src = m_live[static_cast<std::size_t>(r)];
        std::copy(src.begin(), src.end(), cells);
        return true;
    }

private:
    ScrollbackBuffer m_history;
    qint64 m_dropped = 0;
    int m_cols;
    std::vector<std::vector<VTermScreenCell>> m_live;
};

TerminalSearchOptions query(const QString &q, bool regex = false)
{
    TerminalSearchOptions o;
    o.query = q;
    o.regex = regex;
    return o;
}

State scanAll(TerminalSearchSession &s, const FakeTerminal &t)
{
    State state = s.state();
    for (int i = 0; i < 100000 && state == State::Scanning; ++i) {
        state = s.scan(t, std::chrono::milliseconds(4));
    }
    return state;
}

} // namespace

class TestTerminalSearchSession : public QObject
{
    Q_OBJECT

private slots:
    void emptyQueryIsOffAndBadRegexInvalid();
    void findsHistoryAndLiveRowsWithStableIds();
    void zeroBudgetStillProgresses();
    void newOutputReplacesLiveMatches();
    void largeBacklogGoesBackToScanning();
    void evictionDropsMatchesAndKeepsCurrent();
    void historyShrinkRescansPoppedLines();
    void stepWrapsAndActivateLast();
    void catastrophicRegexGivesUp();
    void highlightsMarkTheCurrentMatch();
};

void TestTerminalSearchSession::emptyQueryIsOffAndBadRegexInvalid()
{
    TerminalSearchSession s;
    QCOMPARE(s.start(query(QString()), 0), State::Off);
    QVERIFY(!s.isActive());
    QCOMPARE(s.start(query(QStringLiteral("(unclosed"), true), 0), State::Invalid);
    QVERIFY(!s.isActive());
    QCOMPARE(s.start(query(QStringLiteral("x")), 0), State::Scanning);
    QVERIFY(s.isActive());
    s.stop();
    QCOMPARE(s.state(), State::Off);
}

void TestTerminalSearchSession::findsHistoryAndLiveRowsWithStableIds()
{
    FakeTerminal t(3, 8, 2);
    for (int i = 0; i < 5; ++i) t.push(i % 2 ? QStringLiteral("hit") : QStringLiteral("miss"));
    t.setLive(1, QStringLiteral("  hit"));
    // Lines 0 and 1 were evicted: history is lines 2..4.
    TerminalSearchSession s;
    s.start(query(QStringLiteral("hit")), t.firstLine());
    QCOMPARE(scanAll(s, t), State::Ready);
    QCOMPARE(s.matches().size(), std::size_t(2));
    QCOMPARE(s.matches()[0].line, qint64(3));
    QCOMPARE(s.matches()[1].line, t.historyEnd() + 1);
    QCOMPARE(s.matches()[1].col, 2);
}

void TestTerminalSearchSession::zeroBudgetStillProgresses()
{
    FakeTerminal t(10000, 6, 1);
    for (int i = 0; i < 5000; ++i) t.push(QStringLiteral("hit"));
    TerminalSearchSession s;
    s.start(query(QStringLiteral("hit")), t.firstLine());
    int slices = 0;
    while (s.state() == State::Scanning && slices < 100000) {
        s.scan(t, std::chrono::nanoseconds(0));
        ++slices;
    }
    QCOMPARE(s.state(), State::Ready);
    QVERIFY(slices > 1);   // really sliced
    QCOMPARE(s.matches().size(), std::size_t(5000));
}

void TestTerminalSearchSession::newOutputReplacesLiveMatches()
{
    FakeTerminal t(100, 8, 2);
    t.setLive(0, QStringLiteral("hit"));
    TerminalSearchSession s;
    s.start(query(QStringLiteral("hit")), t.firstLine());
    QCOMPARE(scanAll(s, t), State::Ready);
    QCOMPARE(s.matches().size(), std::size_t(1));

    // The live row changed and a line scrolled into history.
    t.setLive(0, QStringLiteral("nothing"));
    t.push(QStringLiteral("hit hit"));
    QCOMPARE(s.searchNewOutput(t), State::Ready);
    QCOMPARE(s.matches().size(), std::size_t(2));
    QCOMPARE(s.matches()[0].line, qint64(0));
    QCOMPARE(s.matches()[1].col, 4);
}

void TestTerminalSearchSession::largeBacklogGoesBackToScanning()
{
    FakeTerminal t(20000, 6, 1);
    TerminalSearchSession s;
    s.start(query(QStringLiteral("hit")), t.firstLine());
    QCOMPARE(scanAll(s, t), State::Ready);
    for (qint64 i = 0; i < TerminalSearchSession::kSyncLines + 10; ++i) t.push(QStringLiteral("hit"));
    QCOMPARE(s.searchNewOutput(t), State::Scanning);
    QCOMPARE(scanAll(s, t), State::Ready);
    QCOMPARE(s.matches().size(), std::size_t(TerminalSearchSession::kSyncLines + 10));
}

void TestTerminalSearchSession::evictionDropsMatchesAndKeepsCurrent()
{
    FakeTerminal t(100, 6, 1);
    for (int i = 0; i < 4; ++i) t.push(QStringLiteral("hit"));
    TerminalSearchSession s;
    s.start(query(QStringLiteral("hit")), t.firstLine());
    QCOMPARE(scanAll(s, t), State::Ready);
    s.step(1);
    s.step(1);
    s.step(1);
    QCOMPARE(s.activeMatch(), 2);
    s.linesDropped(2);
    QCOMPARE(s.matches().size(), std::size_t(2));
    QCOMPARE(s.activeMatch(), 0);   // still line 2
    QCOMPARE(s.matches()[0].line, qint64(2));
    s.linesDropped(3);
    QCOMPARE(s.activeMatch(), -1);  // the current one was evicted
}

void TestTerminalSearchSession::historyShrinkRescansPoppedLines()
{
    FakeTerminal t(100, 6, 1);
    for (int i = 0; i < 3; ++i) t.push(QStringLiteral("hit"));
    TerminalSearchSession s;
    s.start(query(QStringLiteral("hit")), t.firstLine());
    QCOMPARE(scanAll(s, t), State::Ready);
    QCOMPARE(s.matches().size(), std::size_t(3));
    // Line 2 is popped back onto the screen: its old history match goes,
    // and the next pass finds it as a live row.
    s.historyShrunk(2);
    QCOMPARE(s.searchNewOutput(t), State::Ready);
    QCOMPARE(s.matches().size(), std::size_t(3));
}

void TestTerminalSearchSession::stepWrapsAndActivateLast()
{
    FakeTerminal t(100, 6, 1);
    for (int i = 0; i < 3; ++i) t.push(QStringLiteral("hit"));
    TerminalSearchSession s;
    s.start(query(QStringLiteral("hit")), t.firstLine());
    scanAll(s, t);
    QCOMPARE(s.step(-1), 2);
    QCOMPARE(s.step(1), 0);
    QCOMPARE(s.step(-1), 2);
    s.activateLast();
    QCOMPARE(s.activeMatch(), 2);
}

// (a+)+$ backtracks exponentially on "aaa...ab": the session must give up
// after one row instead of spending PCRE's whole match limit on every row.
void TestTerminalSearchSession::catastrophicRegexGivesUp()
{
    FakeTerminal t(1000, 40, 1);
    for (int i = 0; i < 1000; ++i) t.push(QString(30, QLatin1Char('a')) + QLatin1Char('b'));
    TerminalSearchSession s;
    QCOMPARE(s.start(query(QStringLiteral("(a+)+$"), true), t.firstLine()), State::Scanning);
    QElapsedTimer timer;
    timer.start();
    const State state = scanAll(s, t);
    if (state == State::Ready) QSKIP("this PCRE build fails the pattern fast; nothing to guard");
    QCOMPARE(state, State::TooSlow);
    QVERIFY(timer.elapsed() < 1000);
    QVERIFY(s.matches().empty());
    QVERIFY(!s.isActive());
}

void TestTerminalSearchSession::highlightsMarkTheCurrentMatch()
{
    FakeTerminal t(100, 12, 1);
    t.push(QStringLiteral("ab ab ab"));
    TerminalSearchSession s;
    s.start(query(QStringLiteral("ab")), t.firstLine());
    scanAll(s, t);
    s.step(1);
    s.step(1);
    std::vector<TerminalRowLayout::Highlight> out;
    TerminalSearchSession::ActiveSpan active = s.highlightsFor(0, 1, 2, out);
    QCOMPARE(out.size(), std::size_t(3));
    QCOMPARE(out[1].color, QRgb(2));
    QCOMPARE(out[0].color, QRgb(1));
    QCOMPARE(active.from, 3);
    QCOMPARE(active.to, 5);
    active = s.highlightsFor(1, 1, 2, out);
    QVERIFY(out.empty());
    QCOMPARE(active.from, -1);
}

QTEST_APPLESS_MAIN(TestTerminalSearchSession)

#include "test_terminal_search_session.moc"
