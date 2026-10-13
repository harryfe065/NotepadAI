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

#ifndef TERMINALSEARCHSESSION_H
#define TERMINALSEARCHSESSION_H

#include "ScrollbackBuffer.h"
#include "TerminalRowLayout.h"
#include "TerminalSearch.h"

#include <QRegularExpression>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <vector>

// One find-in-terminal search, apart from the widget: its matches and how far
// the scan has got. History is scanned on a worker thread over a snapshot of
// the scrollback (one pointer per chunk; output arriving meanwhile copies at
// most the chunk it lands in), so the UI thread never scans 100k lines; output
// that arrives later is searched incrementally on the caller's thread (only
// lines new since the last pass, plus the screen). A user regex that
// backtracks catastrophically ends the search (TooSlow). Live rows are read
// through Source, so tests drive it without a terminal.
class TerminalSearchSession
{
public:
    // Where rows come from. Line ids are absolute: they survive eviction.
    class Source
    {
    public:
        virtual ~Source() = default;
        virtual qint64 firstLine() const = 0;    // oldest line still in history
        virtual qint64 historyEnd() const = 0;   // line id of the first live row
        virtual int columns() const = 0;
        virtual int liveRows() const = 0;
        virtual bool historyLine(qint64 line, VTermScreenCell *cells) const = 0;
        virtual bool liveRow(int row, VTermScreenCell *cells) const = 0;
        // History from firstLine() to historyEnd(), unchanging, for a worker.
        virtual ScrollbackSnapshot historySnapshot() const = 0;
    };

    enum class State : uint8_t { Off, Invalid, Scanning, Ready, TooSlow };

    // Lines of new history searched in one incremental pass; more goes back
    // to slicing.
    static constexpr qint64 kSyncLines = 5000;
    // A regex row taking this long, or an incremental pass taking
    // kRegexPassLimit, ends the search. The check runs between rows: one row
    // is bounded by PCRE2's own match limit, not interrupted.
    static constexpr std::chrono::milliseconds kRegexRowLimit{20};
    static constexpr std::chrono::milliseconds kRegexPassLimit{50};

    // A new search (Scanning: history still to scan). Empty query: Off.
    // Starting or stopping cancels a history job still running.
    State start(const TerminalSearchOptions &options, qint64 firstLine);
    void stop();

    // The history scan, as a job for a worker thread.
    struct HistoryJob {
        ScrollbackSnapshot history;   // never changes
        qint64 firstLine = 0;   // line id of history index 0
        qint64 from = 0;        // first line id to search
        int columns = 0;
        QRegularExpression re;
        bool regex = false;
        bool skipBlank = false;
        quint64 generation = 0;
        std::shared_ptr<std::atomic<bool>> cancel;
    };
    struct HistoryResult {
        std::vector<TerminalSearchMatch> matches;
        qint64 end = 0;   // line id after the last line searched
        quint64 generation = 0;
        bool tooSlow = false;
        bool cancelled = false;
    };
    // The next job while Scanning and none is running, else nothing.
    std::optional<HistoryJob> takeHistoryJob(const Source &source);
    // Pure and thread-safe: reads only the job's snapshot. Cancelling is
    // seen between rows (every row for a regex, so a stuck one holds a
    // worker, and app exit, for at most one row).
    static HistoryResult runHistoryJob(const HistoryJob &job);
    // Merges a finished job (results of a replaced search are ignored), then
    // searches the output that arrived meanwhile; Scanning again when that
    // was more than kSyncLines.
    State finishHistoryJob(const HistoryResult &result, const Source &source);
    // Searches the lines that reached history since the last pass and the
    // live rows, replacing matches on rows that were live (they may have
    // changed). A backlog over kSyncLines goes back to Scanning.
    State searchNewOutput(const Source &source);
    // Lines before `firstLine` were evicted from history.
    void linesDropped(qint64 firstLine);
    // History shrank to `historyEnd` (lines popped back onto the screen).
    // Lines popped while a history job runs are searched again once it
    // finishes: the job saw their old text.
    void historyShrunk(qint64 historyEnd);
    // Moves the current match by `delta` (wrapping); returns it, or -1.
    int step(int delta);
    // Makes the newest match current, like a search from the prompt.
    void activateLast();

    State state() const { return m_state; }
    bool isActive() const { return m_state == State::Scanning || m_state == State::Ready; }
    const std::vector<TerminalSearchMatch> &matches() const { return m_matches; }
    int activeMatch() const { return m_active; }

    // Columns of the current match on a row ([from, to)), or -1 when it is
    // elsewhere.
    struct ActiveSpan {
        int from = -1;
        int to = -1;
    };
    // Background highlights of `line` into `out`: `normal`, and `strong` for
    // the current match, whose columns are returned.
    ActiveSpan highlightsFor(qint64 line, QRgb normal, QRgb strong,
                             std::vector<TerminalRowLayout::Highlight> &out) const;

private:
    bool searchRow(qint64 line, int cols);
    void giveUp();
    void cancelJob();

    TerminalSearchOptions m_options;
    QRegularExpression m_re;
    State m_state = State::Off;
    std::vector<TerminalSearchMatch> m_matches;
    int m_active = -1;
    qint64 m_scanNext = 0;
    qint64 m_searchedHistoryEnd = 0;
    bool m_skipBlank = false;
    std::vector<VTermScreenCell> m_cells;
    QString m_text;
    std::vector<int> m_colOf;
    quint64 m_generation = 0;
    bool m_jobRunning = false;
    qint64 m_jobShrunkTo = 0;   // lowest history end seen while the job ran
    std::shared_ptr<std::atomic<bool>> m_cancel;
};

#endif
