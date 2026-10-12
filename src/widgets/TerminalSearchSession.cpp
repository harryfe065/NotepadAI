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

#include "TerminalSearchSession.h"

#include <algorithm>

namespace {

using Clock = std::chrono::steady_clock;

auto lineLess = [](const TerminalSearchMatch &m, qint64 line) { return m.line < line; };

} // namespace

void TerminalSearchSession::cancelJob()
{
    if (m_cancel) m_cancel->store(true, std::memory_order_relaxed);
    m_cancel.reset();
    m_jobRunning = false;
    ++m_generation;
}

TerminalSearchSession::State TerminalSearchSession::start(const TerminalSearchOptions &options, qint64 firstLine)
{
    stop();
    m_options = options;
    if (options.query.isEmpty()) return m_state;
    QRegularExpression re = terminalSearchRegex(options);
    if (!re.isValid()) {
        m_state = State::Invalid;
        return m_state;
    }
    m_re = std::move(re);
    // Blank rows cannot hold a literal match without spaces; regexes might.
    m_skipBlank = !options.regex && !options.query.contains(QLatin1Char(' '));
    m_scanNext = firstLine;
    m_searchedHistoryEnd = firstLine;
    m_state = State::Scanning;
    return m_state;
}

void TerminalSearchSession::stop()
{
    cancelJob();
    m_state = State::Off;
    m_re = QRegularExpression();
    m_matches.clear();
    m_active = -1;
}

bool TerminalSearchSession::searchRow(qint64 line, int cols)
{
    if (!m_options.regex) {
        // Literal patterns are escaped: linear time, nothing to watch.
        searchTerminalRow(m_re, line, m_cells.data(), cols, m_matches, m_text, m_colOf, m_skipBlank);
        return true;
    }
    const auto start = Clock::now();
    searchTerminalRow(m_re, line, m_cells.data(), cols, m_matches, m_text, m_colOf, m_skipBlank);
    if (Clock::now() - start <= kRegexRowLimit) return true;
    giveUp();
    return false;
}

void TerminalSearchSession::giveUp()
{
    cancelJob();
    m_state = State::TooSlow;
    m_re = QRegularExpression();
    m_matches.clear();
    m_active = -1;
}

std::optional<TerminalSearchSession::HistoryJob>
TerminalSearchSession::takeHistoryJob(const ScrollbackBuffer &history, qint64 firstLine, int columns)
{
    if (m_state != State::Scanning || m_jobRunning) return std::nullopt;
    HistoryJob job;
    job.history = history.snapshot();
    job.firstLine = firstLine;
    job.from = std::max(m_scanNext, firstLine);
    job.columns = columns;
    job.re = m_re;
    job.regex = m_options.regex;
    job.skipBlank = m_skipBlank;
    job.generation = m_generation;
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    job.cancel = m_cancel;
    m_jobRunning = true;
    return job;
}

TerminalSearchSession::HistoryResult TerminalSearchSession::runHistoryJob(const HistoryJob &job)
{
    HistoryResult result;
    result.generation = job.generation;
    const ScrollbackStore &history = *job.history;
    result.end = job.firstLine + history.size();
    if (job.columns <= 0) return result;
    std::vector<VTermScreenCell> cells(static_cast<std::size_t>(job.columns));
    QString text;
    std::vector<int> colOf;
    qint64 scanned = 0;
    for (qint64 id = std::max(job.from, job.firstLine); id < result.end; ++id) {
        if ((++scanned & 63) == 0 && job.cancel && job.cancel->load(std::memory_order_relaxed)) {
            result.cancelled = true;
            return result;
        }
        if (!history.lineAt(static_cast<int>(id - job.firstLine), job.columns, cells.data())) continue;
        if (!job.regex) {
            // Literal patterns are escaped: linear time, nothing to watch.
            searchTerminalRow(job.re, id, cells.data(), job.columns, result.matches, text, colOf, job.skipBlank);
            continue;
        }
        // Off the UI thread, but a regex stuck on one row would still burn
        // a core for nothing: give up like the incremental pass does.
        const auto start = Clock::now();
        searchTerminalRow(job.re, id, cells.data(), job.columns, result.matches, text, colOf, job.skipBlank);
        if (Clock::now() - start > kRegexRowLimit) {
            result.tooSlow = true;
            result.matches.clear();
            return result;
        }
    }
    return result;
}

TerminalSearchSession::State TerminalSearchSession::finishHistoryJob(const HistoryResult &result,
                                                                     const Source &source)
{
    if (result.generation != m_generation || m_state != State::Scanning) return m_state;
    m_jobRunning = false;
    m_cancel.reset();
    if (result.tooSlow) {
        giveUp();
        return m_state;
    }
    if (result.cancelled) return m_state;
    // Earlier rounds' matches are all before this job's range; lines evicted
    // while it ran are gone.
    const qint64 first = source.firstLine();
    m_matches.insert(m_matches.end(), result.matches.begin(), result.matches.end());
    m_matches.erase(m_matches.begin(), std::lower_bound(m_matches.begin(), m_matches.end(), first, lineLess));
    m_scanNext = result.end;
    m_searchedHistoryEnd = result.end;
    m_state = State::Ready;
    return searchNewOutput(source);
}

TerminalSearchSession::State TerminalSearchSession::searchNewOutput(const Source &source)
{
    if (m_state != State::Ready) return m_state;
    const qint64 first = source.firstLine();
    const qint64 historyEnd = source.historyEnd();
    // History can shrink (a taller window pops lines back): clamp.
    const qint64 from = std::clamp(m_searchedHistoryEnd, first, historyEnd);

    TerminalSearchMatch active{};
    const bool hadActive = m_active >= 0 && m_active < static_cast<int>(m_matches.size());
    if (hadActive) active = m_matches[static_cast<std::size_t>(m_active)];
    m_matches.erase(std::remove_if(m_matches.begin(), m_matches.end(),
                                   [from, first](const TerminalSearchMatch &m) {
                                       return m.line >= from || m.line < first;
                                   }),
                    m_matches.end());

    if (historyEnd - from > kSyncLines) {
        m_active = -1;
        m_scanNext = from;
        m_state = State::Scanning;
        return m_state;
    }

    const int cols = source.columns();
    if (static_cast<int>(m_cells.size()) < cols) m_cells.resize(static_cast<std::size_t>(cols));
    const auto passStart = Clock::now();
    const auto overPassLimit = [this, passStart]() {
        return m_options.regex && Clock::now() - passStart > kRegexPassLimit;
    };
    for (qint64 id = from; id < historyEnd; ++id) {
        if (source.historyLine(id, m_cells.data()) && !searchRow(id, cols)) return m_state;
        if (overPassLimit()) {
            giveUp();
            return m_state;
        }
    }
    m_searchedHistoryEnd = historyEnd;
    const int rows = source.liveRows();
    for (int row = 0; row < rows; ++row) {
        if (source.liveRow(row, m_cells.data()) && !searchRow(historyEnd + row, cols)) return m_state;
        if (overPassLimit()) {
            giveUp();
            return m_state;
        }
    }

    m_active = -1;
    if (hadActive) {
        for (std::size_t i = 0; i < m_matches.size(); ++i) {
            if (m_matches[i].line == active.line && m_matches[i].col == active.col) {
                m_active = static_cast<int>(i);
                break;
            }
        }
        if (m_active < 0 && !m_matches.empty()) m_active = static_cast<int>(m_matches.size()) - 1;
    }
    return m_state;
}

void TerminalSearchSession::linesDropped(qint64 firstLine)
{
    if (m_matches.empty() || m_matches.front().line >= firstLine) return;
    const auto keep = std::lower_bound(m_matches.begin(), m_matches.end(), firstLine, lineLess);
    const int removed = static_cast<int>(keep - m_matches.begin());
    m_matches.erase(m_matches.begin(), keep);
    // An evicted current match leaves none current.
    m_active = m_active < removed ? -1 : m_active - removed;
}

void TerminalSearchSession::historyShrunk(qint64 historyEnd)
{
    // The popped lines are live again: searched history ends before them.
    m_searchedHistoryEnd = std::min(m_searchedHistoryEnd, historyEnd);
    m_scanNext = std::min(m_scanNext, historyEnd);
}

int TerminalSearchSession::step(int delta)
{
    if (m_matches.empty()) return -1;
    const int n = static_cast<int>(m_matches.size());
    m_active = m_active < 0 ? (delta > 0 ? 0 : n - 1) : ((m_active + delta) % n + n) % n;
    return m_active;
}

void TerminalSearchSession::activateLast()
{
    if (!m_matches.empty()) m_active = static_cast<int>(m_matches.size()) - 1;
}

TerminalSearchSession::ActiveSpan TerminalSearchSession::highlightsFor(
    qint64 line, QRgb normal, QRgb strong, std::vector<TerminalRowLayout::Highlight> &out) const
{
    out.clear();
    ActiveSpan active;
    auto it = std::lower_bound(m_matches.begin(), m_matches.end(), line, lineLess);
    for (; it != m_matches.end() && it->line == line; ++it) {
        const bool isActive = (it - m_matches.begin()) == m_active;
        out.push_back({it->col, it->col + it->width, isActive ? strong : normal});
        if (isActive) {
            active = {it->col, it->col + it->width};
        }
    }
    return active;
}
