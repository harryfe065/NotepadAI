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

#include "GitDiffSyntaxMapper.h"

#include "ILexer.h"
#include "Lexilla.h"
#include "ProfileScope.h"

#include <QHash>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// LexerDocument: minimal Scintilla::IDocument shim for one-shot Lexilla
// styling of a flat byte buffer. Folding, decorations, and indentation are
// no-ops since we only read back the per-byte style array.
//
// All Sci_Position arguments are clamped to valid ranges so a misbehaving
// lexer can't read OOB.
// ---------------------------------------------------------------------------
class LexerDocument final : public Scintilla::IDocument
{
public:
    explicit LexerDocument(QByteArray content)
        : m_content(std::move(content)),
          m_styles(m_content.size(), 0)
    {
        m_lineStarts.reserve(static_cast<size_t>(m_content.size() / 32 + 8));
        m_lineStarts.push_back(0);
        const char *data = m_content.constData();
        const Sci_Position n = m_content.size();
        for (Sci_Position i = 0; i < n; ++i) {
            if (data[i] == '\n') m_lineStarts.push_back(i + 1);
        }
        m_lineStarts.push_back(n);  // sentinel for LineStart(N)
        m_lineState.assign(m_lineStarts.size(), 0);
    }

    const QByteArray &styles() const { return m_styles; }
    Sci_Position lineStartFor(Sci_Position line) const
    {
        if (line < 0) return 0;
        if (line >= static_cast<Sci_Position>(m_lineStarts.size()))
            return m_content.size();
        return m_lineStarts[static_cast<size_t>(line)];
    }
    Sci_Position lineEndFor(Sci_Position line) const
    {
        if (line < 0) return 0;
        const Sci_Position next = lineStartFor(line + 1);
        if (next > 0 && next <= m_content.size() && m_content[static_cast<qsizetype>(next - 1)] == '\n')
            return next - 1;
        return next;
    }
    Sci_Position lineCount() const
    {
        return static_cast<Sci_Position>(m_lineStarts.size()) - 1;
    }

    // ----- IDocument -------------------------------------------------------
    int SCI_METHOD Version() const override { return Scintilla::dvRelease4; }
    void SCI_METHOD SetErrorStatus(int) override {}
    Sci_Position SCI_METHOD Length() const override { return m_content.size(); }
    void SCI_METHOD GetCharRange(char *buffer, Sci_Position position, Sci_Position lengthRetrieve) const override
    {
        if (!buffer || lengthRetrieve <= 0) return;
        const Sci_Position end = std::min<Sci_Position>(position + lengthRetrieve, m_content.size());
        const Sci_Position start = std::max<Sci_Position>(position, 0);
        if (end <= start) return;
        std::memcpy(buffer, m_content.constData() + start, static_cast<size_t>(end - start));
    }
    char SCI_METHOD StyleAt(Sci_Position position) const override
    {
        if (position < 0 || position >= m_styles.size()) return 0;
        return m_styles[static_cast<qsizetype>(position)];
    }
    Sci_Position SCI_METHOD LineFromPosition(Sci_Position position) const override
    {
        if (position <= 0) return 0;
        if (position >= m_content.size()) return std::max<Sci_Position>(lineCount() - 1, 0);
        auto it = std::upper_bound(m_lineStarts.begin(), m_lineStarts.end(),
                                   static_cast<Sci_Position>(position));
        return static_cast<Sci_Position>((it - m_lineStarts.begin()) - 1);
    }
    Sci_Position SCI_METHOD LineStart(Sci_Position line) const override { return lineStartFor(line); }
    int SCI_METHOD GetLevel(Sci_Position) const override { return 0x400; /* SC_FOLDLEVELBASE */ }
    int SCI_METHOD SetLevel(Sci_Position, int) override { return 0; }
    int SCI_METHOD GetLineState(Sci_Position line) const override
    {
        if (line < 0 || line >= static_cast<Sci_Position>(m_lineState.size())) return 0;
        return m_lineState[static_cast<size_t>(line)];
    }
    int SCI_METHOD SetLineState(Sci_Position line, int state) override
    {
        if (line < 0) return 0;
        if (static_cast<size_t>(line) >= m_lineState.size())
            m_lineState.resize(static_cast<size_t>(line) + 1, 0);
        m_lineState[static_cast<size_t>(line)] = state;
        return state;
    }
    void SCI_METHOD StartStyling(Sci_Position position) override
    {
        m_stylingPos = std::clamp<Sci_Position>(position, 0, m_styles.size());
    }
    bool SCI_METHOD SetStyleFor(Sci_Position length, char style) override
    {
        if (length <= 0) return true;
        const Sci_Position end = std::min<Sci_Position>(m_stylingPos + length, m_styles.size());
        if (end > m_stylingPos) {
            std::memset(m_styles.data() + m_stylingPos, static_cast<unsigned char>(style),
                        static_cast<size_t>(end - m_stylingPos));
        }
        m_stylingPos = end;
        return true;
    }
    bool SCI_METHOD SetStyles(Sci_Position length, const char *styles) override
    {
        if (length <= 0 || !styles) return true;
        const Sci_Position end = std::min<Sci_Position>(m_stylingPos + length, m_styles.size());
        if (end > m_stylingPos) {
            std::memcpy(m_styles.data() + m_stylingPos, styles,
                        static_cast<size_t>(end - m_stylingPos));
        }
        m_stylingPos = end;
        return true;
    }
    void SCI_METHOD DecorationSetCurrentIndicator(int) override {}
    void SCI_METHOD DecorationFillRange(Sci_Position, int, Sci_Position) override {}
    void SCI_METHOD ChangeLexerState(Sci_Position, Sci_Position) override {}
    int SCI_METHOD CodePage() const override { return 65001; /* UTF-8 */ }
    bool SCI_METHOD IsDBCSLeadByte(char) const override { return false; }
    const char * SCI_METHOD BufferPointer() override { return m_content.constData(); }
    int SCI_METHOD GetLineIndentation(Sci_Position) override { return 0; }
    Sci_Position SCI_METHOD LineEnd(Sci_Position line) const override { return lineEndFor(line); }
    Sci_Position SCI_METHOD GetRelativePosition(Sci_Position positionStart, Sci_Position characterOffset) const override
    {
        return std::clamp<Sci_Position>(positionStart + characterOffset, 0, m_content.size());
    }
    int SCI_METHOD GetCharacterAndWidth(Sci_Position position, Sci_Position *pWidth) const override
    {
        if (pWidth) *pWidth = 1;
        if (position < 0 || position >= m_content.size()) return 0;
        return static_cast<unsigned char>(m_content[static_cast<qsizetype>(position)]);
    }

private:
    QByteArray m_content;
    QByteArray m_styles;                 // parallel to m_content
    std::vector<Sci_Position> m_lineStarts;
    std::vector<int> m_lineState;
    Sci_Position m_stylingPos = 0;
};

// ---------------------------------------------------------------------------
// Lex `slice` (concatenation of source lines joined with '\n') with
// `lexerName`, and write the resulting per-byte styles into the rows of
// `outPerRow` at indices listed in `rowsInSlice` (one row per source line).
// `outPerRow` must be pre-sized to the parsed result row count.
//
// The lexer styles are offset by kLexerStyleBase so they never collide
// with the painter's fixed styles.
// ---------------------------------------------------------------------------
void lexSliceIntoRows(const QByteArray &slice,
                      const QString &lexerName,
                      const GitDiffSyntaxMapper::LexerSetup *setup,
                      const QVector<int> &rowsInSlice,
                      QVector<QByteArray> &outPerRow)
{
    if (slice.isEmpty() || lexerName.isEmpty() || rowsInSlice.isEmpty()) return;

    Scintilla::ILexer5 *lexer = CreateLexer(lexerName.toLatin1().constData());
    if (!lexer) return;
    if (setup) {
        for (const auto &[key, value] : setup->properties)
            lexer->PropertySet(key.constData(), value.constData());
        for (int set = 0; set < setup->keywords.size(); ++set)
            if (!setup->keywords[set].isEmpty())
                lexer->WordListSet(set, setup->keywords[set].constData());
    }

    LexerDocument doc(slice);
    lexer->Lex(0, static_cast<Sci_Position>(slice.size()), 0, &doc);
    lexer->Release();

    const QByteArray &styles = doc.styles();
    const Sci_Position lc = doc.lineCount();

    const int n = std::min<int>(rowsInSlice.size(), static_cast<int>(lc));
    for (int i = 0; i < n; ++i) {
        const int row = rowsInSlice[i];
        const Sci_Position s = doc.lineStartFor(i);
        const Sci_Position e = doc.lineEndFor(i);
        if (e <= s) continue;
        const Sci_Position len = e - s;

        QByteArray bytes(static_cast<int>(len), Qt::Uninitialized);
        const char *src = styles.constData() + s;
        char *dst = bytes.data();
        for (Sci_Position k = 0; k < len; ++k) {
            // Lexilla styles fit in 7 bits; mask + offset keeps us in [40, 167].
            const std::uint8_t st = static_cast<std::uint8_t>(src[k]) & 0x7F;
            dst[k] = static_cast<char>(GitDiffSyntaxMapper::kLexerStyleBase + st);
        }
        if (row >= 0 && row < outPerRow.size()) {
            outPerRow[row] = std::move(bytes);
        }
    }
}

// ---------------------------------------------------------------------------
// Token-LCS word diff
// ---------------------------------------------------------------------------
//
// Background. The previous design ran per-pair char-LCS with a "shift-best"
// line-pairing heuristic. When content reflows across line boundaries (a
// comment block re-wrapped onto a different number of lines, with the same
// words in the same order), shift-best forced one del line to pair with
// one add line; the inner char-LCS then highlighted bytes that hadn't
// actually been deleted — they had simply moved to the next add line.
//
// New design. For each contiguous block of N deleted lines followed by M
// added lines, concatenate the TOKEN streams of both sides (carrying each
// token's row/col origin) and run a single token-level LCS across the
// whole block. Tokens that the backtrack does not match become spans on
// the row where they originated. Span placement is exact even when the
// reflow shifts a phrase to the next row, because the matched/unmatched
// flag is per-token and tokens know their own row.
//
// Token kinds.
//   ALNUM_RUN  one token per maximal [A-Za-z0-9_]+ run.
//   UTF8_RUN   one token per UTF-8 codepoint (1–4 bytes). Codepoints don't
//              fuse so individual CJK glyphs and emoji align cleanly.
//   PUNCT_BYTE one token per single ASCII punctuation byte. Per-byte so
//              `"hello"` -> ["\"", "hello", "\""] — quotes match
//              independently of the word inside.
//   WS_RUN     one token per maximal run of ASCII space/tab. The intern key
//              is the exact bytes, so `"    "` (4 spaces) and `"  "` are
//              different tokens and indent-level mismatches don't trivially
//              LCS-match.
//
// Determinism. The intern table assigns IDs by insertion order in a
// deterministic walk (del tokens left-to-right, then add tokens
// left-to-right). The DP and backtrack compare interned IDs only. No
// iteration over QHash anywhere. Same input bytes -> bit-identical spans
// regardless of QHash's seed.
//
// Backtrack tie-break: `dp[i-1][j] >= dp[i][j-1]` prefers DEL. This biases
// toward "matches at the end of the block", which is GitHub's diff-view
// convention and matches the painter's existing expectation.
//
// Span emission per row.
//   If every non-WS token on the row is unmatched ->
//       emit one full-line span [0, line.size()).
//   Else ->
//       walk tokens in order; matched tokens flush the open run; unmatched
//       non-WS tokens open or extend it; unmatched WS tokens extend the
//       run if one is open (visual contiguity) but do not push the
//       lastNonWsEnd marker (so trailing WS is trimmed at flush time).
//
// Caps.
//   kMaxTokensPerSide        per-block hard cap. If either side exceeds,
//                            the entire block falls back to full-line spans
//                            (no LCS attempt).
//   kMaxLcsCellsPerOverlay   cumulative DP cell budget across all blocks
//                            in one map() call. If a block would push us
//                            past it, that block also falls back to
//                            full-line spans.
//
// Worst-case wall time at the per-overlay cap (4M cells, uint16_t):
//   Forward fill: row-strip access pattern, ~8KB working set, L1-resident.
//                 ~1.5 ns/cell -> ~6 ms total.
//   Backtrack:    N+M reads on one diagonal -> ~20 us.
//   Per-block fill clear: std::fill of (N+1)*(M+1) cells, ~30 GB/s memset.
//
// Buffer lifetime. dpScratch is a member of State, owned by the caller
// (typically GitDiffFetcher). When the Git dock closes the fetcher
// destructs and the buffer is freed. Lazy-grows on first use; auto-shrinks
// after a call whose peak stayed below 25% of the buffer's current size.

enum class TokenKind : std::uint8_t {
    Alnum = 0,
    Utf8  = 1,
    Punct = 2,
    Ws    = 3,
};

struct Token {
    qint32 row;
    qint32 colStart;
    qint32 colEnd;
    qint32 id;
    TokenKind kind;
};

inline bool isAlnumByte(unsigned char c)
{
    return (c >= '0' && c <= '9') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           c == '_';
}

inline bool isWsByte(unsigned char c)
{
    return c == ' ' || c == '\t';
}

// Tokenize one line into `out`. The line content holds NO line terminator
// (GitDiffParser strips \n and \r before storage), so we never see stray
// newlines.
void tokenizeLine(const QByteArray &line, qint32 row, std::vector<Token> &out)
{
    const char *data = line.constData();
    const qint32 n = line.size();
    qint32 i = 0;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(data[i]);
        const qint32 start = i;
        TokenKind kind;

        if (isWsByte(c)) {
            kind = TokenKind::Ws;
            do { ++i; } while (i < n && isWsByte(static_cast<unsigned char>(data[i])));
        }
        else if (c >= 0x80) {
            // One UTF-8 codepoint per token. Lead byte determines length;
            // truncate at the first non-continuation byte (malformed input
            // becomes a 1-byte token rather than swallowing ASCII).
            kind = TokenKind::Utf8;
            qint32 cpLen = 1;
            if ((c & 0xE0) == 0xC0)      cpLen = 2;
            else if ((c & 0xF0) == 0xE0) cpLen = 3;
            else if ((c & 0xF8) == 0xF0) cpLen = 4;
            cpLen = std::min<qint32>(cpLen, n - i);
            qint32 k = i + 1;
            while (k < i + cpLen) {
                if ((static_cast<unsigned char>(data[k]) & 0xC0) != 0x80) {
                    cpLen = k - i;
                    break;
                }
                ++k;
            }
            i += cpLen;
        }
        else if (isAlnumByte(c)) {
            kind = TokenKind::Alnum;
            do { ++i; } while (i < n && isAlnumByte(static_cast<unsigned char>(data[i])));
        }
        else {
            kind = TokenKind::Punct;
            ++i;
        }

        out.push_back({row, start, i, -1, kind});
    }
}

// Intern token bytes into an ID. delTokens are walked left-to-right, then
// addTokens — this fixes a deterministic id assignment that is independent
// of QHash's per-process seed (we never iterate the QHash; we only insert
// and find, both of which are content-deterministic).
void internTokens(QHash<QByteArray, qint32> &intern,
                  qint32 &nextId,
                  const QByteArray &line,
                  std::vector<Token> &tokens,
                  size_t firstNew)
{
    for (size_t i = firstNew; i < tokens.size(); ++i) {
        Token &t = tokens[i];
        const QByteArray key = QByteArray::fromRawData(
            line.constData() + t.colStart, t.colEnd - t.colStart);
        auto it = intern.find(key);
        if (it == intern.end()) {
            // .insert() takes ownership of the bytes — pass a deep copy
            // since `key` is a fromRawData view over `line` which is the
            // parsed.texts entry (stable for the duration of map(), but
            // QHash's key must outlive the find()s in this call too).
            intern.insert(QByteArray(key.constData(), key.size()), nextId);
            t.id = nextId++;
        } else {
            t.id = it.value();
        }
    }
}

// Emit per-row spans from a token stream plus a parallel `matched` bitmask.
//   - If every non-WS token on a row is unmatched -> full-line span.
//   - Otherwise -> trim emitter: skip leading WS, extend through internal
//     WS, trim trailing WS via lastNonWsEnd.
void emitSpansForSide(const std::vector<Token> &tokens,
                      const std::vector<std::uint8_t> &matched,
                      const GitDiffParser::Result &parsed,
                      QVector<GitDiffSyntaxMapper::WordSpan> &out)
{
    const qint32 nTokens = static_cast<qint32>(tokens.size());
    qint32 t = 0;
    while (t < nTokens) {
        const qint32 rowStart = t;
        const qint32 row = tokens[t].row;
        while (t < nTokens && tokens[t].row == row) ++t;
        const qint32 rowEnd = t;

        bool anyUnmatchedNonWs = false;
        bool anyMatched        = false;
        for (qint32 i = rowStart; i < rowEnd; ++i) {
            if (tokens[i].kind == TokenKind::Ws) continue;
            if (matched[static_cast<size_t>(i)]) anyMatched = true;
            else                                 anyUnmatchedNonWs = true;
        }
        if (!anyUnmatchedNonWs) continue;

        if (!anyMatched) {
            const qint32 n = parsed.texts.at(row).size();
            if (n > 0) out.push_back({row, 0, n});
            continue;
        }

        qint32 curStart = -1;
        qint32 lastNonWsEnd = -1;
        auto flush = [&]() {
            if (curStart >= 0 && lastNonWsEnd > curStart) {
                out.push_back({row, curStart, lastNonWsEnd});
            }
            curStart = -1;
            lastNonWsEnd = -1;
        };
        for (qint32 i = rowStart; i < rowEnd; ++i) {
            const Token &tok = tokens[i];
            if (matched[static_cast<size_t>(i)]) {
                flush();
                continue;
            }
            if (curStart < 0) {
                if (tok.kind == TokenKind::Ws) continue;  // skip leading WS
                curStart = tok.colStart;
                lastNonWsEnd = tok.colEnd;
            } else {
                if (tok.kind != TokenKind::Ws) lastNonWsEnd = tok.colEnd;
                // WS: extend implicitly via lastNonWsEnd not advancing.
            }
        }
        flush();
    }
}

void emitFullLineSpansForRows(const GitDiffParser::Result &parsed,
                              int rowStart, int rowEnd,
                              GitDiffParser::LineKind kind,
                              QVector<GitDiffSyntaxMapper::WordSpan> &out)
{
    for (int r = rowStart; r < rowEnd; ++r) {
        if (parsed.kinds.at(r) != kind) continue;
        const qint32 n = static_cast<qint32>(parsed.texts.at(r).size());
        if (n > 0) out.push_back({r, 0, n});
    }
}

} // namespace

namespace GitDiffSyntaxMapper {

struct State::Impl {
    std::vector<std::uint16_t> dpScratch;
    std::vector<Token>         delTokens;
    std::vector<Token>         addTokens;
    std::vector<std::uint8_t>  delMatched;
    std::vector<std::uint8_t>  addMatched;
    QHash<QByteArray, qint32>  intern;

    qsizetype currentCallPeakCells = 0;
    qsizetype cellsUsedThisCall    = 0;
    qint32    internNextId         = 0;
};

State::State() : d(std::make_unique<Impl>()) {}
State::~State() = default;
State::State(State&&) noexcept = default;
State& State::operator=(State&&) noexcept = default;

} // namespace GitDiffSyntaxMapper

namespace {

// Per-block: collect tokens for the N del rows and M add rows, intern
// them through the shared id table, run a token-LCS, backtrack to fill
// the matched[] bitmasks, then route through emitSpansForSide.
//
// Falls back to full-line spans on either side when:
//   (a) either side exceeds kMaxTokensPerSide,
//   (b) (N+1)*(M+1) would push us past the cumulative cell budget,
//   (c) weak match: non-WS LCS < 40% of the shorter non-WS token count.
void tokenLcsBlock(const GitDiffParser::Result &parsed,
                   int delStart, int delEnd,
                   int addStart, int addEnd,
                   GitDiffSyntaxMapper::State::Impl &state,
                   QVector<GitDiffSyntaxMapper::WordSpan> &delOut,
                   QVector<GitDiffSyntaxMapper::WordSpan> &addOut)
{
    PROFILE_SCOPE("GitDiffSyntaxMapper::tokenLcsBlock");

    // Per-block buffers: reuse capacity but reset content.
    state.delTokens.clear();
    state.addTokens.clear();
    state.delMatched.clear();
    state.addMatched.clear();
    state.intern.clear();
    state.internNextId = 0;

    for (int r = delStart; r < delEnd; ++r) {
        if (parsed.kinds.at(r) != GitDiffParser::LineKind::Deleted) continue;
        const size_t before = state.delTokens.size();
        tokenizeLine(parsed.texts.at(r), r, state.delTokens);
        internTokens(state.intern, state.internNextId,
                     parsed.texts.at(r), state.delTokens, before);
    }
    for (int r = addStart; r < addEnd; ++r) {
        if (parsed.kinds.at(r) != GitDiffParser::LineKind::Added) continue;
        const size_t before = state.addTokens.size();
        tokenizeLine(parsed.texts.at(r), r, state.addTokens);
        internTokens(state.intern, state.internNextId,
                     parsed.texts.at(r), state.addTokens, before);
    }

    const qint32 N = static_cast<qint32>(state.delTokens.size());
    const qint32 M = static_cast<qint32>(state.addTokens.size());

    auto fallbackFull = [&]() {
        emitFullLineSpansForRows(parsed, delStart, delEnd,
                                 GitDiffParser::LineKind::Deleted, delOut);
        emitFullLineSpansForRows(parsed, addStart, addEnd,
                                 GitDiffParser::LineKind::Added, addOut);
    };

    if (N == 0 && M == 0) return;
    if (N == 0) {
        emitFullLineSpansForRows(parsed, addStart, addEnd,
                                 GitDiffParser::LineKind::Added, addOut);
        return;
    }
    if (M == 0) {
        emitFullLineSpansForRows(parsed, delStart, delEnd,
                                 GitDiffParser::LineKind::Deleted, delOut);
        return;
    }
    if (N > GitDiffSyntaxMapper::kMaxTokensPerSide ||
        M > GitDiffSyntaxMapper::kMaxTokensPerSide) {
        fallbackFull();
        return;
    }

    const qsizetype need = static_cast<qsizetype>(N + 1) *
                           static_cast<qsizetype>(M + 1);
    if (state.cellsUsedThisCall + need > GitDiffSyntaxMapper::kMaxLcsCellsPerOverlay) {
        fallbackFull();
        return;
    }
    state.cellsUsedThisCall += need;
    state.currentCallPeakCells = std::max(state.currentCallPeakCells, need);

    if (static_cast<qsizetype>(state.dpScratch.size()) < need) {
        state.dpScratch.resize(static_cast<size_t>(need));
    }
    std::uint16_t *dp = state.dpScratch.data();
    const auto idx = [M](qint32 i, qint32 j) -> size_t {
        return static_cast<size_t>(i) * static_cast<size_t>(M + 1) +
               static_cast<size_t>(j);
    };

    // Forward fill. Row strip pattern -> L1-resident at b=2048.
    // Clear column 0 / row 0 explicitly; rest is written by the recurrence.
    for (qint32 i = 0; i <= N; ++i) dp[idx(i, 0)] = 0;
    for (qint32 j = 0; j <= M; ++j) dp[idx(0, j)] = 0;
    for (qint32 i = 1; i <= N; ++i) {
        const qint32 idi = state.delTokens[static_cast<size_t>(i - 1)].id;
        for (qint32 j = 1; j <= M; ++j) {
            if (idi == state.addTokens[static_cast<size_t>(j - 1)].id) {
                dp[idx(i, j)] = static_cast<std::uint16_t>(dp[idx(i - 1, j - 1)] + 1);
            } else {
                const std::uint16_t up = dp[idx(i - 1, j)];
                const std::uint16_t lt = dp[idx(i, j - 1)];
                dp[idx(i, j)] = up >= lt ? up : lt;
            }
        }
    }

    state.delMatched.assign(static_cast<size_t>(N), 0);
    state.addMatched.assign(static_cast<size_t>(M), 0);

    // Backtrack. `>=` on the DEL branch biases toward "matches near the
    // bottom-right" which is GitHub's convention for shifted duplicates.
    qint32 i = N, j = M;
    qint32 matchedNonWs = 0;
    while (i > 0 && j > 0) {
        const Token &dt = state.delTokens[static_cast<size_t>(i - 1)];
        const Token &at = state.addTokens[static_cast<size_t>(j - 1)];
        if (dt.id == at.id) {
            state.delMatched[static_cast<size_t>(i - 1)] = 1;
            state.addMatched[static_cast<size_t>(j - 1)] = 1;
            if (dt.kind != TokenKind::Ws) ++matchedNonWs;
            --i; --j;
        } else if (dp[idx(i - 1, j)] >= dp[idx(i, j - 1)]) {
            --i;
        } else {
            --j;
        }
    }

    // Weak-match guard. Compare against the shorter non-WS token count
    // on either side. If the matching is too thin, treat the entire
    // block as unrelated and emit full-line spans.
    qint32 nonWsDel = 0, nonWsAdd = 0;
    for (const auto &tt : state.delTokens) if (tt.kind != TokenKind::Ws) ++nonWsDel;
    for (const auto &tt : state.addTokens) if (tt.kind != TokenKind::Ws) ++nonWsAdd;
    const qint32 nonWsMin = std::min(nonWsDel, nonWsAdd);
    if (nonWsMin > 0 && matchedNonWs * 5 < nonWsMin * 2) {
        fallbackFull();
        return;
    }

    emitSpansForSide(state.delTokens, state.delMatched, parsed, delOut);
    emitSpansForSide(state.addTokens, state.addMatched, parsed, addOut);
}

// Walk parsed rows and process each contiguous Deleted block followed by
// an Added block as one token-LCS unit. Unpaired Deleted-only or
// Added-only runs emit full-line spans directly.
void wordDiffPairedBlocks(const GitDiffParser::Result &parsed,
                          GitDiffSyntaxMapper::State::Impl &state,
                          QVector<GitDiffSyntaxMapper::WordSpan> &delOut,
                          QVector<GitDiffSyntaxMapper::WordSpan> &addOut)
{
    const int rows = parsed.kinds.size();
    int r = 0;
    while (r < rows) {
        if (parsed.kinds[r] != GitDiffParser::LineKind::Deleted) { ++r; continue; }
        const int delStart = r;
        while (r < rows && parsed.kinds[r] == GitDiffParser::LineKind::Deleted) ++r;
        const int delEnd = r;
        if (r >= rows || parsed.kinds[r] != GitDiffParser::LineKind::Added) {
            emitFullLineSpansForRows(parsed, delStart, delEnd,
                                     GitDiffParser::LineKind::Deleted, delOut);
            continue;
        }
        const int addStart = r;
        while (r < rows && parsed.kinds[r] == GitDiffParser::LineKind::Added) ++r;
        const int addEnd = r;

        tokenLcsBlock(parsed, delStart, delEnd, addStart, addEnd,
                      state, delOut, addOut);
    }

    // Trailing Added-only runs (no preceding Deleted) are full-line ADDs.
    for (int rr = 0; rr < rows; ++rr) {
        if (parsed.kinds[rr] != GitDiffParser::LineKind::Added) continue;
        // Skip Added rows that were already processed by a Del+Add pair.
        // We detect "already processed" by checking the preceding non-body
        // row: if it's Deleted, the row is inside a pair and was handled
        // above; otherwise it's a standalone Added run.
        int prev = rr - 1;
        while (prev >= 0) {
            const auto k = parsed.kinds[prev];
            if (k == GitDiffParser::LineKind::Added)   { --prev; continue; }
            if (k == GitDiffParser::LineKind::Deleted) break;
            // Context / HunkHeader / FileHeader -> standalone Added run.
            const qint32 n = static_cast<qint32>(parsed.texts.at(rr).size());
            if (n > 0) addOut.push_back({rr, 0, n});
            break;
        }
        if (prev < 0) {
            // Added at file head with no preceding rows.
            const qint32 n = static_cast<qint32>(parsed.texts.at(rr).size());
            if (n > 0) addOut.push_back({rr, 0, n});
        }
    }
}

// Snap span boundaries to UTF-8 codepoint edges. Token-LCS spans are
// codepoint-safe by construction (UTF-8 codepoints are one token each, so
// boundaries always land on codepoint edges); this is a defensive
// no-op for them. Full-line fallback spans use line.size() which is the
// raw byte count — also codepoint-safe — but we keep the snap pass so
// the painter contract holds even if a future caller hand-builds a span.
void snapSpansToUtf8(const GitDiffParser::Result &parsed,
                     QVector<GitDiffSyntaxMapper::WordSpan> &spans)
{
    for (auto &s : spans) {
        if (s.row < 0 || s.row >= parsed.texts.size()) continue;
        const QByteArray &line = parsed.texts.at(s.row);
        const qint32 n = static_cast<qint32>(line.size());
        if (s.colStart < 0) s.colStart = 0;
        if (s.colEnd > n) s.colEnd = n;
        if (s.colStart >= s.colEnd) continue;
        while (s.colStart > 0 &&
               (static_cast<unsigned char>(line[s.colStart]) & 0xC0) == 0x80) {
            --s.colStart;
        }
        while (s.colEnd < n &&
               (static_cast<unsigned char>(line[s.colEnd]) & 0xC0) == 0x80) {
            ++s.colEnd;
        }
    }
}

} // namespace

namespace GitDiffSyntaxMapper {

Overlay map(const Input &in, State &state)
{
    PROFILE_SCOPE("GitDiffSyntaxMapper::map");
    Overlay out;
    if (!in.parsed) return out;

    const GitDiffParser::Result &p = *in.parsed;
    if (p.isBinary || p.kinds.isEmpty()) return out;

    State::Impl &impl = *state.d;
    impl.cellsUsedThisCall    = 0;
    impl.currentCallPeakCells = 0;

    const int rows = p.kinds.size();
    out.styleBytesPerRow.resize(rows);

    // Walk hunks. For each contiguous span between HunkHeaders (or between a
    // HunkHeader and EOF), build an OLD slice (Context + Deleted) and a NEW
    // slice (Context + Added). Lex each independently, then route styles
    // back into out.styleBytesPerRow.
    int r = 0;
    qsizetype oldBytesTotal = 0;
    qsizetype newBytesTotal = 0;
    while (r < rows) {
        // Skip non-body rows until we find a body row.
        while (r < rows) {
            const auto k = p.kinds[r];
            if (k == GitDiffParser::LineKind::Context ||
                k == GitDiffParser::LineKind::Added ||
                k == GitDiffParser::LineKind::Deleted) break;
            ++r;
        }
        if (r >= rows) break;
        const int hunkStart = r;
        // Body rows continue until the next non-body row (HunkHeader,
        // FileHeader, NoNewline, Empty).
        while (r < rows) {
            const auto k = p.kinds[r];
            if (k != GitDiffParser::LineKind::Context &&
                k != GitDiffParser::LineKind::Added &&
                k != GitDiffParser::LineKind::Deleted) break;
            ++r;
        }
        const int hunkEnd = r; // exclusive

        // Build slices.
        QByteArray oldSlice;
        QByteArray newSlice;
        QVector<int> oldRows; oldRows.reserve(hunkEnd - hunkStart);
        QVector<int> newRows; newRows.reserve(hunkEnd - hunkStart);
        qsizetype oldCap = 0;
        qsizetype newCap = 0;
        for (int i = hunkStart; i < hunkEnd; ++i) {
            const auto k = p.kinds[i];
            const QByteArray &t = p.texts.at(i);
            if (k == GitDiffParser::LineKind::Context || k == GitDiffParser::LineKind::Deleted) {
                oldCap += t.size() + 1;
            }
            if (k == GitDiffParser::LineKind::Context || k == GitDiffParser::LineKind::Added) {
                newCap += t.size() + 1;
            }
        }
        oldSlice.reserve(oldCap);
        newSlice.reserve(newCap);

        for (int i = hunkStart; i < hunkEnd; ++i) {
            const auto k = p.kinds[i];
            const QByteArray &t = p.texts.at(i);
            if (k == GitDiffParser::LineKind::Context || k == GitDiffParser::LineKind::Deleted) {
                oldSlice.append(t);
                oldSlice.append('\n');
                oldRows.push_back(i);
            }
            if (k == GitDiffParser::LineKind::Context || k == GitDiffParser::LineKind::Added) {
                newSlice.append(t);
                newSlice.append('\n');
                newRows.push_back(i);
            }
        }

        // Cap total per-side bytes across all hunks. If we exceed the cap on
        // either side, stop processing further hunks (we already have partial
        // results for earlier hunks).
        oldBytesTotal += oldSlice.size();
        newBytesTotal += newSlice.size();
        if (oldBytesTotal > kMaxBytesPerSide && newBytesTotal > kMaxBytesPerSide) {
            break;
        }

        if (oldBytesTotal <= kMaxBytesPerSide) {
            lexSliceIntoRows(oldSlice, in.oldLexerName, in.oldSetup, oldRows, out.styleBytesPerRow);
        }
        if (newBytesTotal <= kMaxBytesPerSide) {
            lexSliceIntoRows(newSlice, in.newLexerName, in.newSetup, newRows, out.styleBytesPerRow);
        }
    }

    // Token-level word diff on paired Del+Add blocks (independent of lex).
    wordDiffPairedBlocks(p, impl, out.delWordSpans, out.addWordSpans);

    // Defensive UTF-8 boundary snap. Token-LCS spans are already codepoint-
    // safe; this is a no-op for them but guards against future regressions.
    snapSpansToUtf8(p, out.delWordSpans);
    snapSpansToUtf8(p, out.addWordSpans);

    // Auto-shrink: if the peak usage of this call stayed below 25% of the
    // current buffer, drop it. The buffer regrows on the next big block.
    // Floor at 128KB so small day-to-day diffs don't churn realloc.
    constexpr qsizetype kShrinkFloor = 64 * 1024;  // cells -> 128KB
    if (impl.dpScratch.size() > kShrinkFloor &&
        impl.currentCallPeakCells * 4 < static_cast<qsizetype>(impl.dpScratch.size())) {
        impl.dpScratch.clear();
        impl.dpScratch.shrink_to_fit();
    }

    // Overlay is useful if we produced any styled row OR any word span.
    bool any = !out.delWordSpans.isEmpty() || !out.addWordSpans.isEmpty();
    if (!any) {
        for (const auto &b : out.styleBytesPerRow) {
            if (!b.isEmpty()) { any = true; break; }
        }
    }
    out.ok = any;
    return out;
}

Overlay map(const Input &in)
{
    State scratch;
    return map(in, scratch);
}

} // namespace GitDiffSyntaxMapper
