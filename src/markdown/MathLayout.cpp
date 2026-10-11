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

#include "MathLayout.h"

#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QPainter>
#include <QPen>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace Math {

namespace {

// ------------------------------------------------------------------ font

QString g_family;
bool g_familyResolved = false;
QString g_extraFontFile;

QString resolveFamily()
{
    auto fromFile = [](const QString &path) -> QString {
        const int id = QFontDatabase::addApplicationFont(path);
        if (id < 0) return QString();
        const QStringList fams = QFontDatabase::applicationFontFamilies(id);
        return fams.isEmpty() ? QString() : fams.first();
    };
    if (!g_extraFontFile.isEmpty()) {
        const QString f = fromFile(g_extraFontFile);
        if (!f.isEmpty()) return f;
    }
    QString f = fromFile(QStringLiteral(":/fonts/STIXTwoMath-Regular.ttf"));
    if (!f.isEmpty()) return f;
    const QStringList families = QFontDatabase::families();
    for (const char *candidate : {"STIX Two Math", "Cambria Math", "Latin Modern Math",
                                  "Noto Sans Math", "Segoe UI Symbol"}) {
        if (families.contains(QLatin1String(candidate)))
            return QLatin1String(candidate);
    }
    return QGuiApplication::font().family();
}

QFont makeFont(double px, FontStyle style)
{
    QFont f(mathFontFamily());
    f.setPixelSize(std::max(1, int(std::lround(px))));
    f.setHintingPreference(QFont::PreferNoHinting);
    f.setStyleStrategy(QFont::PreferAntialias);
    if (style == FontStyle::Bold) f.setBold(true);
    if (style == FontStyle::Italic) f.setItalic(true);
    return f;
}

// ------------------------------------------------------------------ boxes

// y-down coordinates, origin at the left end of the baseline.
struct Box
{
    double w = 0.0;
    double asc = 0.0;
    double desc = 0.0;
    std::vector<Prim> prims;
};

void append(Box &dst, const Box &src, double dx, double dyUp)
{
    dst.prims.reserve(dst.prims.size() + src.prims.size());
    for (const Prim &p : src.prims) {
        Prim q = p;
        q.pos += QPointF(dx, -dyUp);
        dst.prims.push_back(std::move(q));
    }
}

Prim ruleAt(double x, double yUpTop, double w, double h)
{
    Prim p;
    p.kind = PrimKind::Rule;
    p.pos = QPointF(x, -yUpTop);
    p.size = QSizeF(w, h);
    return p;
}

Prim pathPrim(const QPainterPath &path, double stroke, bool fill = false)
{
    Prim p;
    p.kind = PrimKind::Path;
    p.path = path;
    p.strokeWidth = stroke;
    p.fill = fill;
    return p;
}

// Metrics shared by every layout (fractions of an em).
constexpr double kAxis = 0.25;
constexpr double kRule = 0.055;

// TeX style levels: 0 display, 1 text, 2 script, 3 scriptscript.
double sizeFactor(int level)
{
    switch (level) {
    case 0: case 1: return 1.0;
    case 2: return 0.7;
    default: return 0.5;
    }
}

// ----------------------------------------------------- unicode alphabets

QString codePoint(char32_t cp)
{
    return QString::fromUcs4(&cp, 1);
}

QString italicGlyphs(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (QChar c : text) {
        const char16_t u = c.unicode();
        if (u >= u'a' && u <= u'z') {
            out += u == u'h' ? QString(QChar(0x210E)) : codePoint(0x1D44Eu + (u - u'a'));
        } else if (u >= u'A' && u <= u'Z') {
            out += codePoint(0x1D434u + (u - u'A'));
        } else if (u >= 0x3B1 && u <= 0x3C9) {
            out += codePoint(0x1D6FCu + (u - 0x3B1));
        } else if (u == 0x3F5) {
            out += codePoint(0x1D716);
        } else if (u == 0x3D1) {
            out += codePoint(0x1D717);
        } else if (u == 0x3D5) {
            out += codePoint(0x1D719);
        } else if (u == 0x3F1) {
            out += codePoint(0x1D71A);
        } else if (u == 0x3D6) {
            out += codePoint(0x1D71B);
        } else {
            out += c;
        }
    }
    return out;
}

QString boldGlyphs(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (QChar c : text) {
        const char16_t u = c.unicode();
        if (u >= u'a' && u <= u'z') out += codePoint(0x1D41Au + (u - u'a'));
        else if (u >= u'A' && u <= u'Z') out += codePoint(0x1D400u + (u - u'A'));
        else if (u >= u'0' && u <= u'9') out += codePoint(0x1D7CEu + (u - u'0'));
        else if (u >= 0x3B1 && u <= 0x3C9) out += codePoint(0x1D6C2u + (u - 0x3B1)); // bold small Greek
        else if (u >= 0x391 && u <= 0x3A9) out += codePoint(0x1D6A8u + (u - 0x391)); // bold capital Greek
        else out += c;
    }
    return out;
}

// ------------------------------------------------------------- layouter

class Layouter
{
public:
    explicit Layouter(double basePx) : base_(basePx) {}

    Box layoutRoot(const Node &root, bool display) { return row(root, display ? 0 : 1); }

private:
    double base_;

    double px(int level) const { return base_ * sizeFactor(level); }

    // ----- glyph runs
    Box glyphRun(const QString &text, double sizePx, FontStyle style, bool setStyleOnPrim = false)
    {
        Box b;
        if (text.isEmpty()) return b;
        const QFont f = makeFont(sizePx, setStyleOnPrim ? style : FontStyle::Roman);
        const QFontMetricsF fm(f);
        b.w = fm.horizontalAdvance(text);
        const QRectF ink = fm.tightBoundingRect(text);
        b.asc = std::max(0.0, -ink.top());
        b.desc = std::max(0.0, ink.bottom());
        Prim p;
        p.kind = PrimKind::Text;
        p.pos = QPointF(0, 0);
        p.text = text;
        p.px = sizePx;
        p.style = setStyleOnPrim ? style : FontStyle::Roman;
        b.prims.push_back(std::move(p));
        return b;
    }

    // Right overhang of an italic glyph past its advance (for script placement).
    double italicCorrection(const QString &text, double sizePx) const
    {
        const QFontMetricsF fm(makeFont(sizePx, FontStyle::Roman));
        const double over = fm.tightBoundingRect(text).right() - fm.horizontalAdvance(text);
        return std::max(0.0, over);
    }

    // ----- rows with TeX inter-atom spacing
    static AtomClass effectiveClass(const Node &n)
    {
        switch (n.kind) {
        case Node::Kind::Atom: return n.cls;
        case Node::Kind::LargeOp: return AtomClass::Op;
        case Node::Kind::Script: return n.kids.empty() ? AtomClass::Ord : effectiveClass(n.kids.front());
        default: return AtomClass::Ord;
        }
    }

    static double muBetween(AtomClass l, AtomClass r)
    {
        // rows: left; cols: right — Ord Op Bin Rel Open Close Punct
        static const int table[7][7] = {
            /*Ord  */ {0, 3, 4, 5, 0, 0, 0},
            /*Op   */ {3, 3, 0, 5, 0, 0, 0},
            /*Bin  */ {4, 4, 0, 0, 4, 0, 0},
            /*Rel  */ {5, 5, 0, 0, 5, 0, 0},
            /*Open */ {0, 0, 0, 0, 0, 0, 0},
            /*Close*/ {0, 3, 4, 5, 0, 0, 0},
            /*Punct*/ {3, 3, 0, 3, 3, 3, 3},
        };
        return table[int(l)][int(r)];
    }

    Box row(const Node &n, int level)
    {
        Box out;
        const size_t count = n.kids.size();
        std::vector<Box> boxes;
        std::vector<AtomClass> cls;
        std::vector<bool> isSpace;
        boxes.reserve(count);
        cls.reserve(count);
        isSpace.reserve(count);
        for (const Node &k : n.kids) {
            boxes.push_back(item(k, level));
            cls.push_back(effectiveClass(k));
            isSpace.push_back(k.kind == Node::Kind::Space);
        }
        // A binary operator needs a left operand and a right operand; otherwise
        // it is unary (ordinary), e.g. the minus in "-a" or "(-a)".
        for (size_t i = 0; i < count; ++i) {
            if (cls[i] != AtomClass::Bin) continue;
            const bool noLeft = i == 0 || cls[i - 1] == AtomClass::Bin || cls[i - 1] == AtomClass::Rel
                                || cls[i - 1] == AtomClass::Open || cls[i - 1] == AtomClass::Punct
                                || cls[i - 1] == AtomClass::Op;
            const bool noRight = i + 1 == count || cls[i + 1] == AtomClass::Rel
                                 || cls[i + 1] == AtomClass::Close || cls[i + 1] == AtomClass::Punct;
            if (noLeft || noRight) cls[i] = AtomClass::Ord;
        }

        const bool spaced = level < 2;
        double x = 0.0;
        for (size_t i = 0; i < count; ++i) {
            if (spaced && i > 0 && !isSpace[i] && !isSpace[i - 1])
                x += muBetween(cls[i - 1], cls[i]) / 18.0 * px(level);
            append(out, boxes[i], x, 0.0);
            x += boxes[i].w;
            out.asc = std::max(out.asc, boxes[i].asc);
            out.desc = std::max(out.desc, boxes[i].desc);
        }
        out.w = x;
        return out;
    }

    // ----- items
    Box item(const Node &n, int level)
    {
        switch (n.kind) {
        case Node::Kind::Row: return row(n, level);
        case Node::Kind::Atom: return atom(n, level);
        case Node::Kind::LargeOp: return largeOp(n, level);
        case Node::Kind::Script: return script(n, level);
        case Node::Kind::Frac: return frac(n, level);
        case Node::Kind::Sqrt: return sqrtBox(n, level);
        case Node::Kind::Delimited: return delimited(n, level);
        case Node::Kind::Matrix: return matrix(n, level);
        case Node::Kind::Text: return textRun(n, level);
        case Node::Kind::Accent: return accent(n, level);
        case Node::Kind::Space: {
            Box b;
            b.w = n.em * px(level);
            return b;
        }
        }
        return Box();
    }

    Box atom(const Node &n, int level)
    {
        QString text = n.text;
        if (n.style == FontStyle::Italic) text = italicGlyphs(text);
        else if (n.style == FontStyle::Bold) text = boldGlyphs(text);
        return glyphRun(text, px(level), FontStyle::Roman);
    }

    Box textRun(const Node &n, int level)
    {
        return glyphRun(n.text, px(level), n.style, true);
    }

    // ----- large operators and scripts
    static bool isIntegral(const QString &t)
    {
        return t == QStringLiteral("∫") || t == QStringLiteral("∬")
               || t == QStringLiteral("∭") || t == QStringLiteral("∮");
    }

    static bool wantsLimitsAbove(const Node &op, int level)
    {
        if (op.limits == LimitsMode::Limits) return true;
        if (op.limits == LimitsMode::NoLimits) return false;
        if (level != 0) return false;
        if (op.named) {
            static const QStringList limitsNames = {
                QStringLiteral("lim"), QStringLiteral("max"), QStringLiteral("min"),
                QStringLiteral("sup"), QStringLiteral("inf"), QStringLiteral("det"),
                QStringLiteral("gcd"), QStringLiteral("Pr"), QStringLiteral("lim sup"),
                QStringLiteral("lim inf")};
            return limitsNames.contains(op.text);
        }
        return !isIntegral(op.text);
    }

    Box largeOp(const Node &n, int level)
    {
        if (n.named) {
            return glyphRun(n.text, px(level), FontStyle::Roman);
        }
        const double scale = level == 0 ? (isIntegral(n.text) ? 1.7 : 1.45) : 1.0;
        Box g = glyphRun(n.text, px(level) * scale, FontStyle::Roman);
        // centre the glyph on the math axis
        const double shiftUp = px(level) * kAxis - (g.asc - g.desc) / 2.0;
        Box out;
        out.w = g.w;
        append(out, g, 0.0, shiftUp);
        out.asc = g.asc + shiftUp;
        out.desc = std::max(0.0, g.desc - shiftUp);
        return out;
    }

    Box script(const Node &n, int level)
    {
        const Node &baseNode = n.kids.front();
        const int sl = std::min(3, level + 1 < 2 ? 2 : level + 1);
        Box sub, sup;
        if (n.hasSub) sub = row(n.kids.at(1), sl);
        if (n.hasSup) sup = row(n.kids.at(n.hasSub ? 2 : 1), sl);
        const Box base = item(baseNode, level);
        const double em = px(level);

        Box out;
        if (baseNode.kind == Node::Kind::LargeOp && wantsLimitsAbove(baseNode, level)) {
            const double w = std::max({base.w, sub.w, sup.w});
            const double gap = 0.12 * em;
            append(out, base, (w - base.w) / 2.0, 0.0);
            out.asc = base.asc;
            out.desc = base.desc;
            if (n.hasSup) {
                const double up = base.asc + gap + sup.desc;
                append(out, sup, (w - sup.w) / 2.0, up);
                out.asc = up + sup.asc;
            }
            if (n.hasSub) {
                const double down = base.desc + gap + sub.asc;
                append(out, sub, (w - sub.w) / 2.0, -down);
                out.desc = down + sub.desc;
            }
            out.w = w;
            return out;
        }

        double supShift = 0.0, subShift = 0.0;
        const double scriptEm = px(sl);
        if (n.hasSup) {
            supShift = std::max({0.40 * em, base.asc - 0.38 * scriptEm, sup.desc + 0.25 * em});
        }
        if (n.hasSub) {
            subShift = std::max({0.15 * em, base.desc + 0.15 * scriptEm, sub.asc - 0.80 * em});
        }
        if (n.hasSup && n.hasSub) {
            subShift = std::max(subShift, 0.24 * em);
            // keep the two scripts at least four rule widths apart
            const double gap = (supShift - sup.desc) - (sub.asc - subShift);
            const double minGap = 4.0 * kRule * em;
            if (gap < minGap) {
                const double need = minGap - gap;
                subShift += need;
            }
        }
        const bool italicBase = baseNode.kind == Node::Kind::Atom && baseNode.style == FontStyle::Italic;
        const double corr = italicBase ? italicCorrection(atomText(baseNode), px(level)) : 0.0;

        append(out, base, 0.0, 0.0);
        const double sx = base.w;
        double w = base.w;
        out.asc = base.asc;
        out.desc = base.desc;
        constexpr double kScriptPad = 0.04;
        if (n.hasSup) {
            append(out, sup, sx + corr, supShift);
            out.asc = std::max(out.asc, supShift + sup.asc);
            w = std::max(w, sx + corr + sup.w);
        }
        if (n.hasSub) {
            append(out, sub, sx, -subShift);
            out.desc = std::max(out.desc, subShift + sub.desc);
            w = std::max(w, sx + sub.w);
        }
        out.w = w + kScriptPad * em;
        return out;
    }

    static QString atomText(const Node &n)
    {
        return n.style == FontStyle::Italic ? italicGlyphs(n.text)
               : n.style == FontStyle::Bold ? boldGlyphs(n.text)
                                            : n.text;
    }

    // ----- fractions
    Box frac(const Node &n, int level)
    {
        int styleLevel = level;
        if (n.styleOverride == 0) styleLevel = std::max(level, 1);
        if (n.styleOverride == 1) styleLevel = 0;
        const bool display = styleLevel == 0;
        const int inner = std::min(3, styleLevel + 1 < 1 ? 1 : (styleLevel == 0 ? 1 : styleLevel + 1));
        const double em = px(styleLevel);
        const Box num = row(n.kids.at(0), inner);
        const Box den = row(n.kids.at(1), inner);

        const double t = n.bar ? kRule * em : 0.0;
        const double axis = kAxis * em;
        const double clearance = display ? 3.0 * (kRule * em) : (kRule * em);
        double up = display ? 0.677 * em : 0.394 * em;
        double down = display ? 0.686 * em : 0.345 * em;
        if (n.bar) {
            up = std::max(up, axis + t / 2.0 + clearance + num.desc);
            down = std::max(down, den.asc - (axis - t / 2.0 - clearance));
        } else {
            // binomial: no rule, just keep the two rows apart
            const double minGap = display ? 3.0 * kRule * em : kRule * em * 1.5;
            const double gap = (up - num.desc) - (-down + den.asc);
            if (gap < minGap) {
                up += (minGap - gap) / 2.0;
                down += (minGap - gap) / 2.0;
            }
        }
        const double pad = 0.1 * em;
        const double w = std::max(num.w, den.w) + 2.0 * pad;

        Box out;
        out.w = w;
        append(out, num, pad + (w - 2.0 * pad - num.w) / 2.0, up);
        append(out, den, pad + (w - 2.0 * pad - den.w) / 2.0, -down);
        if (n.bar)
            out.prims.push_back(ruleAt(pad, axis + t / 2.0, w - 2.0 * pad, t));
        out.asc = up + num.asc;
        out.desc = down + den.desc;
        return out;
    }

    // ----- square roots
    Box sqrtBox(const Node &n, int level)
    {
        const double em = px(level);
        const Box body = row(n.kids.at(0), level);
        const double t = kRule * em * 1.1;
        const double gap = (level == 0 ? 0.17 : 0.12) * em;
        const double rw = 0.62 * em;
        const double yTopUp = body.asc + gap + t / 2.0;
        const double yBotUp = -(body.desc + 0.03 * em);
        const double H = yTopUp - yBotUp; // total radical height

        double indexShift = 0.0;
        Box index;
        const bool hasIndex = n.kids.size() > 1;
        if (hasIndex) {
            index = row(n.kids.at(1), 3);
            indexShift = std::max(0.0, index.w - 0.42 * rw);
        }

        // radical sign as one stroked polyline (y-down)
        QPainterPath path;
        const double x0 = indexShift;
        path.moveTo(x0, -(yBotUp + 0.40 * H));
        path.lineTo(x0 + 0.14 * rw, -(yBotUp + 0.46 * H));
        path.lineTo(x0 + 0.42 * rw, -yBotUp);
        path.lineTo(x0 + rw, -yTopUp);
        path.lineTo(x0 + rw + body.w + 0.08 * em, -yTopUp);
        Prim p = pathPrim(path, t);
        Box out;
        out.prims.push_back(std::move(p));
        append(out, body, x0 + rw + 0.04 * em, 0.0);
        out.w = x0 + rw + body.w + 0.12 * em;
        out.asc = yTopUp + t / 2.0;
        out.desc = -yBotUp;
        if (hasIndex) {
            const double idxUp = yBotUp + 0.55 * H;
            append(out, index, 0.0, idxUp);
            out.asc = std::max(out.asc, idxUp + index.asc);
        }
        return out;
    }

    // ----- delimiters
    static QPainterPath delimPath(const QString &g, double w, double h, bool &filled)
    {
        // Local coordinates: x in [0,w], y in [0,h] downward.
        filled = false;
        QPainterPath p;
        const QChar c = g.isEmpty() ? QChar() : g.at(0);
        switch (c.unicode()) {
        case '(':
            p.moveTo(w * 0.85, 0);
            p.cubicTo(w * 0.15, h * 0.22, w * 0.15, h * 0.78, w * 0.85, h);
            break;
        case ')':
            p.moveTo(w * 0.15, 0);
            p.cubicTo(w * 0.85, h * 0.22, w * 0.85, h * 0.78, w * 0.15, h);
            break;
        case '[':
            p.moveTo(w * 0.8, 0); p.lineTo(w * 0.25, 0); p.lineTo(w * 0.25, h); p.lineTo(w * 0.8, h);
            break;
        case ']':
            p.moveTo(w * 0.2, 0); p.lineTo(w * 0.75, 0); p.lineTo(w * 0.75, h); p.lineTo(w * 0.2, h);
            break;
        case '{': {
            // two outer hooks, a straight stem each side, and a pointed middle
            const double stem = w * 0.58, tip = w * 0.08;
            p.moveTo(w * 0.9, 0);
            p.cubicTo(w * 0.7, 0, stem, h * 0.04, stem, h * 0.16);
            p.lineTo(stem, h * 0.40);
            p.cubicTo(stem, h * 0.46, w * 0.36, h * 0.5, tip, h * 0.5);
            p.cubicTo(w * 0.36, h * 0.5, stem, h * 0.54, stem, h * 0.60);
            p.lineTo(stem, h * 0.84);
            p.cubicTo(stem, h * 0.96, w * 0.7, h, w * 0.9, h);
            break;
        }
        case '}': {
            const double stem = w * 0.42, tip = w * 0.92;
            p.moveTo(w * 0.1, 0);
            p.cubicTo(w * 0.3, 0, stem, h * 0.04, stem, h * 0.16);
            p.lineTo(stem, h * 0.40);
            p.cubicTo(stem, h * 0.46, w * 0.64, h * 0.5, tip, h * 0.5);
            p.cubicTo(w * 0.64, h * 0.5, stem, h * 0.54, stem, h * 0.60);
            p.lineTo(stem, h * 0.84);
            p.cubicTo(stem, h * 0.96, w * 0.3, h, w * 0.1, h);
            break;
        }
        case '|':
            p.moveTo(w * 0.5, 0); p.lineTo(w * 0.5, h);
            break;
        case 0x2016:
            p.moveTo(w * 0.34, 0); p.lineTo(w * 0.34, h);
            p.moveTo(w * 0.66, 0); p.lineTo(w * 0.66, h);
            break;
        case 0x27E8:
            p.moveTo(w * 0.8, 0); p.lineTo(w * 0.2, h * 0.5); p.lineTo(w * 0.8, h);
            break;
        case 0x27E9:
            p.moveTo(w * 0.2, 0); p.lineTo(w * 0.8, h * 0.5); p.lineTo(w * 0.2, h);
            break;
        case 0x230A: // ⌊
            p.moveTo(w * 0.25, 0); p.lineTo(w * 0.25, h); p.lineTo(w * 0.8, h);
            break;
        case 0x230B: // ⌋
            p.moveTo(w * 0.75, 0); p.lineTo(w * 0.75, h); p.lineTo(w * 0.2, h);
            break;
        case 0x2308: // ⌈
            p.moveTo(w * 0.25, h); p.lineTo(w * 0.25, 0); p.lineTo(w * 0.8, 0);
            break;
        case 0x2309: // ⌉
            p.moveTo(w * 0.75, h); p.lineTo(w * 0.75, 0); p.lineTo(w * 0.2, 0);
            break;
        case '/':
            p.moveTo(w * 0.9, 0); p.lineTo(w * 0.1, h);
            break;
        case '\\':
            p.moveTo(w * 0.1, 0); p.lineTo(w * 0.9, h);
            break;
        default:
            break;
        }
        return p;
    }

    // One delimiter sized to `targetH` (centred on the axis).
    Box delimiter(const QString &glyph, double targetH, int level)
    {
        Box b;
        if (glyph.isEmpty()) return b;
        const double em = px(level);
        if (targetH <= 1.25 * em) {
            Box g = glyphRun(glyph, em, FontStyle::Roman);
            if (glyph.at(0) == QLatin1Char('{') || glyph.at(0) == QLatin1Char('}')
                || glyph.at(0) == QLatin1Char('(') || glyph.at(0) == QLatin1Char(')')
                || glyph.at(0) == QLatin1Char('[') || glyph.at(0) == QLatin1Char(']'))
                return g;
            return g;
        }
        const double w = std::clamp(0.16 * em + 0.045 * targetH, 0.32 * em, 0.62 * em);
        bool filled = false;
        QPainterPath path = delimPath(glyph, w, targetH, filled);
        if (path.isEmpty()) return glyphRun(glyph, em, FontStyle::Roman);
        const double axis = kAxis * em;
        // path origin: top-left at (0, -(axis + targetH/2)) in y-down coordinates
        Prim p = pathPrim(path, kRule * em * 1.25);
        p.pos = QPointF(0.0, -(axis + targetH / 2.0));
        b.prims.push_back(std::move(p));
        b.w = w;
        b.asc = axis + targetH / 2.0;
        b.desc = targetH / 2.0 - axis;
        return b;
    }

    Box wrapDelims(const Box &content, const QString &left, const QString &right, int level)
    {
        const double em = px(level);
        const double axis = kAxis * em;
        const double half = std::max(content.asc - axis, content.desc + axis);
        const double target = std::max(2.0 * half * 0.96, em);
        const Box l = delimiter(left, target, level);
        const Box r = delimiter(right, target, level);
        // drawn (stretched) delimiters need more air than font glyphs
        auto gapFor = [em](const Box &d) {
            const bool drawn = !d.prims.empty() && d.prims.front().kind == PrimKind::Path;
            return (drawn ? 0.14 : 0.06) * em;
        };
        Box out;
        double x = 0.0;
        append(out, l, 0.0, 0.0);
        x += l.w + (l.w > 0 ? gapFor(l) : 0.0);
        append(out, content, x, 0.0);
        x += content.w + (r.w > 0 ? gapFor(r) : 0.0);
        append(out, r, x, 0.0);
        x += r.w;
        out.w = x;
        out.asc = std::max({content.asc, l.asc, r.asc});
        out.desc = std::max({content.desc, l.desc, r.desc});
        return out;
    }

    Box delimited(const Node &n, int level)
    {
        const Box content = row(n.kids.at(0), level);
        return wrapDelims(content, n.left, n.right, level);
    }

    // ----- matrices and aligned environments
    Box matrix(const Node &n, int level)
    {
        const bool alignedLike = n.env == QLatin1String("aligned") || n.env == QLatin1String("align")
                                 || n.env == QLatin1String("align*") || n.env == QLatin1String("split");
        const bool isCases = n.env == QLatin1String("cases");
        const int cellLevel = (n.env.endsWith(QLatin1String("matrix")) && level == 0) ? 1 : level;
        const double em = px(cellLevel);
        const int rows = n.rows, cols = n.cols;

        std::vector<Box> cells;
        cells.reserve(n.kids.size());
        for (const Node &k : n.kids) cells.push_back(row(k, cellLevel));

        std::vector<double> colW(size_t(cols), 0.0);
        std::vector<double> rowAsc(size_t(rows), 0.7 * em), rowDesc(size_t(rows), 0.3 * em);
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                const Box &b = cells[size_t(r * cols + c)];
                colW[size_t(c)] = std::max(colW[size_t(c)], b.w);
                rowAsc[size_t(r)] = std::max(rowAsc[size_t(r)], b.asc);
                rowDesc[size_t(r)] = std::max(rowDesc[size_t(r)], b.desc);
            }
        }
        // x origin of each column
        std::vector<double> colX(size_t(cols), 0.0);
        double x = 0.0;
        for (int c = 0; c < cols; ++c) {
            colX[size_t(c)] = x;
            double sep = isCases ? 1.0 * em : 1.0 * em;
            if (alignedLike) sep = (c % 2 == 0) ? 0.0 : 2.0 * em;
            if (n.env == QLatin1String("matrix") || n.env.endsWith(QLatin1String("matrix")))
                sep = 1.0 * em;
            x += colW[size_t(c)] + (c + 1 < cols ? sep : 0.0);
        }
        const double totalW = x;

        const double rowGap = 0.2 * em;
        Box grid;
        double yUp = 0.0; // baseline of current row, measured down from the top as negative
        double top = 0.0;
        std::vector<double> rowBase(size_t(rows), 0.0);
        {
            double y = 0.0; // y-down from top of first row
            for (int r = 0; r < rows; ++r) {
                y += rowAsc[size_t(r)];
                rowBase[size_t(r)] = y;
                y += rowDesc[size_t(r)] + (r + 1 < rows ? rowGap : 0.0);
            }
            top = y; // total height
        }
        // centre the grid on the math axis
        yUp = top / 2.0 - px(level) * kAxis;
        for (int r = 0; r < rows; ++r) {
            const double baselineUp = yUp - rowBase[size_t(r)];
            for (int c = 0; c < cols; ++c) {
                const Box &b = cells[size_t(r * cols + c)];
                double dx = colX[size_t(c)];
                if (alignedLike) {
                    if (c % 2 == 0) {
                        dx += colW[size_t(c)] - b.w;
                    } else {
                        // "a &= b": the relation after the alignment point is
                        // spaced as if it had a left operand
                        const Node &cell = n.kids[size_t(r * cols + c)];
                        if (!cell.kids.empty()) {
                            const AtomClass first = effectiveClass(cell.kids.front());
                            if (first == AtomClass::Rel) dx += 5.0 / 18.0 * em;
                            else if (first == AtomClass::Bin) dx += 4.0 / 18.0 * em;
                        }
                    }
                } else if (!isCases) {
                    dx += (colW[size_t(c)] - b.w) / 2.0;
                }
                append(grid, b, dx, baselineUp);
            }
        }
        grid.w = totalW;
        grid.asc = yUp;
        grid.desc = top - yUp;

        QString left, right;
        if (n.env == QLatin1String("pmatrix")) { left = QStringLiteral("("); right = QStringLiteral(")"); }
        else if (n.env == QLatin1String("bmatrix")) { left = QStringLiteral("["); right = QStringLiteral("]"); }
        else if (n.env == QLatin1String("Bmatrix")) { left = QStringLiteral("{"); right = QStringLiteral("}"); }
        else if (n.env == QLatin1String("vmatrix")) { left = QStringLiteral("|"); right = QStringLiteral("|"); }
        else if (n.env == QLatin1String("Vmatrix")) { left = QStringLiteral("‖"); right = QStringLiteral("‖"); }
        else if (isCases) { left = QStringLiteral("{"); }
        if (left.isEmpty() && right.isEmpty()) return grid;
        return wrapDelims(grid, left, right, cellLevel);
    }

    // ----- accents
    Box accent(const Node &n, int level)
    {
        const double em = px(level);
        const Box c = row(n.kids.at(0), level);
        Box out;
        append(out, c, 0.0, 0.0);
        out.w = c.w;
        out.asc = c.asc;
        out.desc = c.desc;
        const double t = kRule * em;
        const QString &k = n.text;
        const double cx = c.w / 2.0;

        if (k == QLatin1String("underline")) {
            const double y = c.desc + 0.1 * em;
            out.prims.push_back(ruleAt(0.0, -y, c.w, t));
            out.desc = y + t;
            return out;
        }
        if (k == QLatin1String("overline")) {
            const double y = c.asc + 0.1 * em;
            out.prims.push_back(ruleAt(0.0, y + t, c.w, t));
            out.asc = y + t;
            return out;
        }

        const double base = std::max(c.asc, 0.45 * em); // floats above x-height at least
        const double y0 = base + 0.12 * em;             // accent baseline, y up
        QPainterPath path;
        double stroke = t;
        bool fill = false;
        const double hw = std::min(0.28 * em, std::max(0.12 * em, c.w * 0.45));
        if (k == QLatin1String("hat")) {
            path.moveTo(cx - hw, -y0);
            path.lineTo(cx, -(y0 + 0.13 * em));
            path.lineTo(cx + hw, -y0);
        } else if (k == QLatin1String("tilde")) {
            path.moveTo(cx - hw, -y0);
            path.cubicTo(cx - hw * 0.4, -(y0 + 0.16 * em), cx + hw * 0.1, -(y0 - 0.02 * em),
                         cx + hw, -(y0 + 0.12 * em));
        } else if (k == QLatin1String("bar")) {
            path.moveTo(cx - hw, -y0);
            path.lineTo(cx + hw, -y0);
        } else if (k == QLatin1String("overrightarrow") || k == QLatin1String("overleftarrow")) {
            // a long arrow as wide as the content (vec is a short one)
            const double ah = 0.07 * em;
            const double x0 = 0.04 * em, x1 = std::max(c.w - 0.04 * em, x0 + 0.3 * em);
            path.moveTo(x0, -y0);
            path.lineTo(x1, -y0);
            if (k == QLatin1String("overrightarrow")) {
                path.moveTo(x1 - ah, -(y0 + ah));
                path.lineTo(x1, -y0);
                path.lineTo(x1 - ah, -(y0 - ah));
            } else {
                path.moveTo(x0 + ah, -(y0 + ah));
                path.lineTo(x0, -y0);
                path.lineTo(x0 + ah, -(y0 - ah));
            }
            out.w = std::max(out.w, x1 + 0.04 * em);
        } else if (k == QLatin1String("vec")) {
            const double ah = 0.07 * em;
            path.moveTo(cx - hw, -y0);
            path.lineTo(cx + hw, -y0);
            path.moveTo(cx + hw - ah, -(y0 + ah));
            path.lineTo(cx + hw, -y0);
            path.lineTo(cx + hw - ah, -(y0 - ah));
        } else if (k == QLatin1String("dot") || k == QLatin1String("ddot")) {
            const double r = 0.045 * em;
            fill = true;
            stroke = 0.0;
            if (k == QLatin1String("dot")) {
                path.addEllipse(QPointF(cx, -y0), r, r);
            } else {
                path.addEllipse(QPointF(cx - 0.1 * em, -y0), r, r);
                path.addEllipse(QPointF(cx + 0.1 * em, -y0), r, r);
            }
        }
        if (!path.isEmpty()) out.prims.push_back(pathPrim(path, stroke, fill));
        out.asc = std::max(c.asc, y0 + 0.18 * em);
        return out;
    }
};

} // namespace

// ------------------------------------------------------------------ public

QString mathFontFamily()
{
    if (!g_familyResolved) {
        g_family = resolveFamily();
        g_familyResolved = true;
    }
    return g_family;
}

void registerMathFontFile(const QString &path)
{
    g_extraFontFile = path;
    g_familyResolved = false;
}

Layout layoutFormula(const QString &source, bool display, double px)
{
    Layout out;
    const ParseResult parsed = parse(source);
    if (!parsed.ok) {
        out.error = parsed.error;
        return out;
    }
    Layouter layouter(px);
    Box box = layouter.layoutRoot(parsed.root, display);
    out.ok = true;
    out.width = box.w;
    out.ascent = box.asc;
    out.descent = box.desc;
    out.axis = kAxis * px;
    out.prims = std::move(box.prims);
    return out;
}

std::optional<Rendered> renderFormula(const QString &source, bool display, double px,
                                      const QColor &color, double dpr)
{
    const Layout l = layoutFormula(source, display, px);
    if (!l.ok) return std::nullopt;

    const double pad = 0.1 * px;
    const double half = std::max(l.ascent - l.axis, l.descent + l.axis) + pad;
    const double wDip = std::ceil(l.width + 2.0 * pad);
    const double hDip = std::ceil(2.0 * half);
    const double axisFromTop = hDip / 2.0;
    const double baselineFromTop = axisFromTop + l.axis;
    if (wDip > 8192 || hDip > 8192 || wDip < 1 || hDip < 1) return std::nullopt;

    Rendered r;
    r.image = QImage(int(std::ceil(wDip * dpr)), int(std::ceil(hDip * dpr)),
                     QImage::Format_ARGB32_Premultiplied);
    r.image.setDevicePixelRatio(dpr);
    r.image.fill(Qt::transparent);
    r.axisFromTop = axisFromTop;
    r.baselineFromTop = baselineFromTop;

    QPainter p(&r.image);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing
                     | QPainter::SmoothPixmapTransform);
    p.translate(pad, baselineFromTop);
    p.setPen(color);
    p.setBrush(Qt::NoBrush);
    for (const Prim &prim : l.prims) {
        switch (prim.kind) {
        case PrimKind::Text:
            p.setFont(makeFont(prim.px, prim.style));
            p.setPen(color);
            p.drawText(prim.pos, prim.text);
            break;
        case PrimKind::Rule:
            p.fillRect(QRectF(prim.pos, prim.size), color);
            break;
        case PrimKind::Path:
            p.save();
            p.translate(prim.pos);
            if (prim.fill) {
                p.setPen(Qt::NoPen);
                p.setBrush(color);
            } else {
                QPen pen(color, prim.strokeWidth);
                pen.setCapStyle(Qt::FlatCap);
                pen.setJoinStyle(Qt::MiterJoin);
                p.setPen(pen);
                p.setBrush(Qt::NoBrush);
            }
            p.drawPath(prim.path);
            p.restore();
            break;
        }
    }
    p.end();
    return r;
}

} // namespace Math
