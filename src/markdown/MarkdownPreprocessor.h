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

#ifndef MARKDOWN_PREPROCESSOR_H
#define MARKDOWN_PREPROCESSOR_H

#include <QChar>
#include <QSet>
#include <QString>
#include <QVector>

#include <functional>

// Rewrites the small set of HTML constructs and math delimiters that chat
// messages use into plain markdown the existing QTextDocument pipeline already
// renders. Runs before setMarkdown, which keeps MarkdownNoHTML on: anything not
// recognised here (Surreal<Db>, a < b, <script>, <table>, unbalanced tags,
// unclosed $$ while streaming) is returned byte-for-byte untouched.
//
// Math fences (```math / ```latex / ```tex / ```katex, or an unlabelled fence
// holding exactly one $$…$$ / \[…\]) become display formulas; any other fence
// is verbatim.
//
// Complexity: O(n) single hand-written scan, no regex. Text with none of
// '$', '<', "\(", "\[", "```", "~~~" returns immediately without scanning.
namespace MarkdownPreprocessor {

// Alignment sentinels: private-use code points. A begin paragraph is
// kAlignBegin + one of l/c/r/j; the end paragraph is kAlignEnd. Input text can
// not forge them — run() strips both from its input.
constexpr QChar kAlignBegin(0xE000);
constexpr QChar kAlignEnd(0xE001);

// Scheme of the <details> toggle link: "[▸ Title](nn-details:<ordinal>)".
constexpr const char *kDetailsScheme = "nn-details:";

// Per-message limits (the formula itself is left as text beyond them).
constexpr int kMaxFormulas = 200;
constexpr int kMaxFormulaChars = 2048;

struct MathSpan
{
    QString source; // TeX between the delimiters, trimmed
    bool display = false;
};

struct Result
{
    // Markdown to feed setMarkdown. Math is emitted as "![formula](math:<index>)",
    // index into `math`; display math sits in its own paragraph.
    QString markdown;
    QVector<MathSpan> math;
    bool hasAlign = false;   // sentinel paragraphs were emitted
    bool hasDetails = false; // at least one <details> toggle row was emitted
    bool changed = false;  // markdown differs from the input
};

// Decides whether a candidate formula is renderable. Returning false leaves the
// original "$…$" text in place. Default (empty function) accepts everything.
using MathAccept = std::function<bool(const QString &source, bool display)>;

// `flippedDetails`: ordinals (document order, counted over the raw source) of
// <details> whose open state differs from their default (`open` attribute).
Result run(const QString &input,
           const QSet<int> &flippedDetails = {},
           const MathAccept &accept = {});

} // namespace MarkdownPreprocessor

#endif // MARKDOWN_PREPROCESSOR_H
