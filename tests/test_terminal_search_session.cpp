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
    void pop()
    {
        std::vector<VTermScreenCell> cells(static_cast<std::size_t>(m_cols));
        m_history.popLine(m_cols, cells.data());
    }

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
    ScrollbackSnapshot historySnapshot() const override { return m_history.snapshot(); }

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

// What the widget does with its worker, synchronously.
State scanAll(TerminalSearchSession &s, const FakeTerminal &t)
{
    State state = s.state();
    for (int i = 0; i < 100 && state == State::Scanning; ++i) {
        const auto job = s.takeHistoryJob(t);
        if (!job) break;
        state = s.finishHistoryJob(TerminalSearchSession::runHistoryJob(*job), t);
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
    void historyChangingDuringTheJob();
    void replacedSearchIgnoresOldResults();
    void cancelStopsTheJob();
    void newOutputReplacesLiveMatches();
    void largeBacklogGoesBackToScanning();
    void evictionDropsMatchesAndKeepsCurrent();
    void historyShrinkRescansPoppedLines();
    void lineRepushedDuringTheJobIsRescanned();
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

// The worker reads a snapshot: output and eviction while it runs change
// neither its view nor the merged result.
void TestTerminalSearchSession::historyChangingDuringTheJob()
{
    FakeTerminal t(10, 6, 1);
    for (int i = 0; i < 10; ++i) t.push(QStringLiteral("hit"));
    TerminalSearchSession s;
    s.start(query(QStringLiteral("hit")), t.firstLine());
    const auto job = s.takeHistoryJob(t);
    QVERIFY(job.has_value());
    QVERIFY(!s.takeHistoryJob(t).has_value());   // one at a time
    // Meanwhile: 4 more lines, which evict lines 0..3.
    for (int i = 0; i < 4; ++i) t.push(QStringLiteral("hit"));
    const TerminalSearchSession::HistoryResult result = TerminalSearchSession::runHistoryJob(*job);
    QCOMPARE(result.matches.size(), std::size_t(10));   // the snapshot, untouched
    QCOMPARE(s.finishHistoryJob(result, t), State::Ready);
    // Lines 4..13 are in history now: evicted ones dropped, new ones found.
    QCOMPARE(s.matches().size(), std::size_t(10));
    QCOMPARE(s.matches().front().line, qint64(4));
    QCOMPARE(s.matches().back().line, qint64(13));
}

void TestTerminalSearchSession::replacedSearchIgnoresOldResults()
{
    FakeTerminal t(100, 6, 1);
    for (int i = 0; i < 5; ++i) t.push(QStringLiteral("hit"));
    TerminalSearchSession s;
    s.start(query(QStringLiteral("hit")), t.firstLine());
    const auto old = s.takeHistoryJob(t);
    s.start(query(QStringLiteral("zzz")), t.firstLine());
    QVERIFY(old->cancel->load());
    QCOMPARE(s.finishHistoryJob(TerminalSearchSession::runHistoryJob(*old), t), State::Scanning);
    QVERIFY(s.matches().empty());
    QCOMPARE(scanAll(s, t), State::Ready);
    QVERIFY(s.matches().empty());
}

void TestTerminalSearchSession::cancelStopsTheJob()
{
    FakeTerminal t(10000, 6, 1);
    for (int i = 0; i < 5000; ++i) t.push(QStringLiteral("hit"));
    TerminalSearchSession s;
    s.start(query(QStringLiteral("hit")), t.firstLine());
    const auto job = s.takeHistoryJob(t);
    s.stop();
    const TerminalSearchSession::HistoryResult result = TerminalSearchSession::runHistoryJob(*job);
    QVERIFY(result.cancelled);
    QVERIFY(result.matches.size() < 64);
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

// Lines popped back onto the screen while the worker runs and pushed again
// with other text: the job's matches there are stale, the new text is found.
void TestTerminalSearchSession::lineRepushedDuringTheJobIsRescanned()
{
    FakeTerminal t(100, 12, 1);
    for (int i = 0; i < 5; ++i) t.push(QStringLiteral("old"));
    TerminalSearchSession s;
    s.start(query(QStringLiteral("old|brand-new"), true), t.firstLine());
    const auto job = s.takeHistoryJob(t);
    QVERIFY(job.has_value());
    t.pop();
    t.pop();
    s.historyShrunk(t.historyEnd());
    t.push(QStringLiteral("brand-new"));
    t.push(QStringLiteral("brand-new"));
    QCOMPARE(s.finishHistoryJob(TerminalSearchSession::runHistoryJob(*job), t), State::Ready);
    QCOMPARE(s.matches().size(), std::size_t(5));
    for (int i = 0; i < 5; ++i) {
        QCOMPARE(s.matches()[static_cast<std::size_t>(i)].line, qint64(i));
        QCOMPARE(s.matches()[static_cast<std::size_t>(i)].width, i < 3 ? 3 : 9);
    }
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
