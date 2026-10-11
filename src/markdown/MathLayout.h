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

#ifndef MATH_LAYOUT_H
#define MATH_LAYOUT_H

#include <QColor>
#include <QImage>
#include <QPainterPath>
#include <QPointF>
#include <QSizeF>
#include <QString>

#include <optional>
#include <vector>

#include "MathParser.h"

// TeX-style layout for the Math::Node tree, drawn with QPainter. Layout is a
// flat list of positioned primitives (glyph runs, rules, stroked paths), so
// painting is one pass and a Layout can be measured without a paint device.
//
// Coordinates: x right, y DOWN, origin at the left end of the baseline.
//
// Complexity: O(nodes) layout (child primitive lists are translated once per
// nesting level, so O(nodes · depth) worst case with depth <= Math::kMaxDepth),
// one painter pass to draw.
namespace Math {

enum class PrimKind { Text, Rule, Path };

struct Prim
{
    PrimKind kind = PrimKind::Text;
    QPointF pos;              // Text: baseline-left; Rule: top-left; Path: path offset
    QString text;
    double px = 0.0;          // Text: font pixel size
    FontStyle style = FontStyle::Roman;
    QSizeF size;              // Rule
    QPainterPath path;        // Path, in local coordinates
    double strokeWidth = 0.0; // Path: pen width (stroked) — ignored when fill
    bool fill = false;        // Path: fill instead of stroke
};

struct Layout
{
    bool ok = false;
    QString error;
    double width = 0.0;
    double ascent = 0.0;   // above the baseline
    double descent = 0.0;  // below the baseline
    double axis = 0.0;     // math axis height above the baseline
    std::vector<Prim> prims;

    double height() const { return ascent + descent; }
};

// Parse + lay out `source` at `px` pixels per em. `display` selects display
// style (big operators, full-size fractions) vs inline text style.
Layout layoutFormula(const QString &source, bool display, double px);

struct Rendered
{
    QImage image;               // premultiplied ARGB, device pixel ratio set
    double baselineFromTop = 0; // in device-independent pixels
    double axisFromTop = 0;     // == height / 2: padded symmetric about the axis
};

// layoutFormula + paint. nullopt when the formula is outside the subset.
std::optional<Rendered> renderFormula(const QString &source, bool display, double px,
                                      const QColor &color, double dpr);

// Math font: the bundled STIX Two Math (":/fonts/…") when present, else a
// platform math font, else the application font. Registered once, lazily.
QString mathFontFamily();

// Test/embedding hook: register a font file before the first layout.
void registerMathFontFile(const QString &path);

} // namespace Math

#endif // MATH_LAYOUT_H
