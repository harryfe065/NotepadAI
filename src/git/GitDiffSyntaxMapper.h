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

#ifndef GIT_DIFF_SYNTAX_MAPPER_H
#define GIT_DIFF_SYNTAX_MAPPER_H

#include "GitDiffParser.h"

#include <QByteArray>
#include <QString>
#include <QVector>

#include <memory>
#include <utility>

// Computes a "syntax overlay" for a parsed unified diff: per-row lexer style
// bytes (for the diff body, indexed by display row) plus word-level
// intra-line diff spans (added/deleted token bg fills).
//
// Strategy: instead of fetching OLD/NEW blobs via additional `git show`
// invocations (cost: 2 extra QProcess spawns per diff), the mapper rebuilds
// per-hunk OLD/NEW slices from the parsed unified diff itself:
//   OLD slice  = concatenation of (Context | Deleted) row texts in the hunk
//   NEW slice  = concatenation of (Context | Added)   row texts in the hunk
// Each hunk is lexed independently with initStyle=0. This loses multi-line
// constructs that begin outside the hunk (a string opened above context
// won't be styled correctly), but ships the feature without new IO and
// matches GitHub's diff-view behavior closely for the common case.
//
// Pure namespace, no QObject, no Scintilla document: runs on the pool, one
// call per file (DiffDocumentBuilder), with one State per worker thread.
namespace GitDiffSyntaxMapper {

// Hard caps. Conservative for v1 and meant to be raised based on the bench
// (see tests/bench_lexilla_throughput.cpp).
//   kMaxBytesPerSide        — refuse to lex if total hunk content bytes per
//                             side exceed this. Skipping yields ok=false.
//   kMaxTokensPerSide       — per-Del/Add-block cap on tokens. Above this,
//                             the block falls back to full-line spans (no
//                             token-LCS attempt).
//   kMaxLcsCellsPerOverlay  — cumulative DP cells across all blocks in one
//                             map() call. Bounds total UI-thread work.
//   kLexerStyleBase         — lexer styles are offset by this so they never
//                             collide with the fixed diff styles (0..31) or
//                             Scintilla's predefined styles
//                             (STYLE_LASTPREDEFINED == 39). DiffLanguages
//                             remaps them into per-language slots.
inline constexpr int       kMaxBytesPerSide        = 256 * 1024;
inline constexpr qint32    kMaxTokensPerSide       = 4096;
inline constexpr qsizetype kMaxLcsCellsPerOverlay  = 4'000'000;
inline constexpr std::uint8_t kLexerStyleBase      = 40;

// Word-level fill span. row is the diff display row (0-based, lines up with
// GitDiffParser::Result::texts indices); colStart/colEnd are byte offsets
// within the line content (excluding the '+'/'-' prefix the painter adds).
struct WordSpan {
    qint32 row;
    qint32 colStart;
    qint32 colEnd;
};

// Keyword lists and properties a language sets on its lexer (Lua
// `L.keywords` / `L.properties`), copied on the UI thread so the lexer can be
// configured on any thread.
struct LexerSetup {
    QVector<QByteArray> keywords;                               // index = keyword set
    QVector<std::pair<QByteArray, QByteArray>> properties;
};

struct Input {
    QString  oldLexerName;        // Lexilla lexer name for OLD; empty -> no lex on OLD side
    QString  newLexerName;        // Lexilla lexer name for NEW; empty -> no lex on NEW side
    const LexerSetup *oldSetup = nullptr;          // optional, borrowed
    const LexerSetup *newSetup = nullptr;
    const GitDiffParser::Result *parsed = nullptr; // borrowed, non-owning
};

// Output overlay. styleBytesPerRow has the same length as parsed->texts
// (one QByteArray per display row). For Context/Added rows the bytes
// mirror the new-side lex; for Deleted rows they mirror the old-side
// lex; for header rows they are empty (painter keeps the fixed style).
// When ok=false the painter must ignore styleBytesPerRow and word spans.
struct Overlay {
    QVector<QByteArray> styleBytesPerRow;
    QVector<WordSpan>   addWordSpans;
    QVector<WordSpan>   delWordSpans;
    bool                ok = false;
};

Overlay map(const Input &in);

// Reusable workspace for map(). DiffDocumentBuilder keeps one per worker
// thread (thread_local); peak buffers — worst case ~8MB for the DP table
// at kMaxLcsCellsPerOverlay — shrink back by the rule below.
//
// Lazy: nothing is allocated until the first non-trivial block is
// processed. High-water-mark within a call. Auto-shrink after a call
// whose peak usage stays below 25% of the buffer it currently holds.
//
// Not thread-safe: one State per thread.
struct State {
    State();
    ~State();
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    State(State&&) noexcept;
    State& operator=(State&&) noexcept;

    struct Impl;
private:
    std::unique_ptr<Impl> d;
    friend Overlay map(const Input &in, State &state);
};

// Stateful overload — the one used by GitDiffFetcher. The state-less
// overload above creates a one-shot State internally and is kept only
// for tests / one-off callers that don't care about pool reuse.
Overlay map(const Input &in, State &state);

} // namespace GitDiffSyntaxMapper

#endif // GIT_DIFF_SYNTAX_MAPPER_H
