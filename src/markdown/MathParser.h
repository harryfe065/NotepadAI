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

#ifndef MATH_PARSER_H
#define MATH_PARSER_H

#include <QString>

#include <vector>

// Parser for the TeX math subset used in chat messages (see the
// add-chat-html-math-render design). Pure: no QtGui, no fonts. Anything
// outside the subset is a parse error, never a best-effort guess — the caller
// then leaves the formula as source text.
//
// Complexity: O(n) over the source; recursion depth bounded by kMaxDepth.
namespace Math {

constexpr int kMaxDepth = 32;
constexpr int kMaxSourceChars = 2048;

// TeX atom classes (drive inter-atom spacing in layout).
enum class AtomClass { Ord, Op, Bin, Rel, Open, Close, Punct };

enum class FontStyle { Italic, Roman, Bold };

enum class LimitsMode { Default, Limits, NoLimits };

struct Node
{
    enum class Kind {
        Row,       // kids = items in order
        Atom,      // text = glyph(s); cls/style
        LargeOp,   // text = glyph or function name; named = name set in roman
        Script,    // kids = [base, sub?, sup?] (sub/sup are Rows), hasSub/hasSup
        Frac,      // kids = [numerator, denominator] (Rows)
        Sqrt,      // kids = [radicand] or [radicand, index] (Rows)
        Delimited, // left/right (empty = none), kids = [content Row]
        Matrix,    // env, rows x cols, kids = cells row-major (Rows)
        Text,      // text = literal run (spaces kept), style
        Accent,    // text = hat|bar|vec|dot|ddot|tilde|overline|underline, kids = [content Row]
        Space      // em = horizontal advance in em (may be negative)
    };

    Kind kind = Kind::Row;
    QString text;
    AtomClass cls = AtomClass::Ord;
    FontStyle style = FontStyle::Italic;
    std::vector<Node> kids;

    bool hasSub = false;
    bool hasSup = false;

    bool named = false;                       // LargeOp: `text` is a function name
    LimitsMode limits = LimitsMode::Default;  // LargeOp: \limits / \nolimits

    bool bar = true;                          // Frac: draw the rule (false for \binom)
    int styleOverride = -1;                   // Frac: -1 inherit, 0 \tfrac (text), 1 \dfrac (display)

    QString left;                             // Delimited
    QString right;

    QString env;                              // Matrix environment name
    int rows = 0;
    int cols = 0;

    double em = 0.0;                          // Space
};

struct ParseResult
{
    bool ok = false;
    Node root;      // Row; valid when ok
    QString error;  // human-readable reason when !ok (diagnostics only)
};

ParseResult parse(const QString &source);

} // namespace Math

#endif // MATH_PARSER_H
