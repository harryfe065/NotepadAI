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

#include "MarkdownPreprocessor.h"

#include <QStringView>

namespace MarkdownPreprocessor {

namespace {

struct Ctx
{
    const QSet<int> &flipped;
    const MathAccept &accept;
    Result &res;
    int detailsOrdinal = 0;
};

bool isWs(QChar c)
{
    return c == QLatin1Char(' ') || c == QLatin1Char('\t')
           || c == QLatin1Char('\n') || c == QLatin1Char('\r');
}

// ---------------------------------------------------------------- tags

struct Attr
{
    QString name; // lowercased
    QString value;
};

struct Tag
{
    QString name; // lowercased
    bool closing = false;
    QVector<Attr> attrs;
    bool attrsOk = true;  // attribute text parsed cleanly
    int end = 0;          // index one past '>'
};

// Parses "name=value" pairs; any malformed input clears `ok`.
QVector<Attr> parseAttrs(QStringView s, bool &ok)
{
    QVector<Attr> out;
    ok = true;
    int i = 0;
    const int n = int(s.size());
    while (i < n) {
        while (i < n && isWs(s[i])) ++i;
        if (i >= n) break;
        if (s[i] == QLatin1Char('/')) { // self-closing slash, only at the very end
            ++i;
            while (i < n && isWs(s[i])) ++i;
            if (i < n) ok = false;
            break;
        }
        const int nameStart = i;
        while (i < n && (s[i].isLetterOrNumber() || s[i] == QLatin1Char('-'))) ++i;
        if (i == nameStart) { ok = false; break; }
        Attr a;
        a.name = s.mid(nameStart, i - nameStart).toString().toLower();
        while (i < n && isWs(s[i])) ++i;
        if (i < n && s[i] == QLatin1Char('=')) {
            ++i;
            while (i < n && isWs(s[i])) ++i;
            if (i >= n) { ok = false; break; }
            if (s[i] == QLatin1Char('"') || s[i] == QLatin1Char('\'')) {
                const QChar q = s[i++];
                const int vs = i;
                while (i < n && s[i] != q) ++i;
                if (i >= n) { ok = false; break; }
                a.value = s.mid(vs, i - vs).toString();
                ++i;
            } else {
                const int vs = i;
                while (i < n && !isWs(s[i]) && s[i] != QLatin1Char('/')) ++i;
                a.value = s.mid(vs, i - vs).toString();
            }
        }
        out.append(a);
    }
    return out;
}

// Parses a tag starting at s[i] == '<'. Returns false when it is not a
// syntactically complete tag on one line.
bool parseTag(QStringView s, int i, Tag &t)
{
    const int n = int(s.size());
    int j = i + 1;
    if (j < n && s[j] == QLatin1Char('/')) {
        t.closing = true;
        ++j;
    }
    const int nameStart = j;
    while (j < n && s[j].isLetter()) ++j;
    if (j == nameStart) return false;
    t.name = s.mid(nameStart, j - nameStart).toString().toLower();
    // The name must end at whitespace, '/', or '>' — not run into more letters
    // or digits (<details2>).
    if (j < n && (s[j].isDigit() || s[j] == QLatin1Char('-'))) return false;
    const int attrStart = j;
    QChar quote;
    while (j < n) {
        const QChar c = s[j];
        if (c == QLatin1Char('\n')) return false;
        if (!quote.isNull()) {
            if (c == quote) quote = QChar();
        } else if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            quote = c;
        } else if (c == QLatin1Char('<')) {
            return false;
        } else if (c == QLatin1Char('>')) {
            break;
        }
        ++j;
    }
    if (j >= n) return false;
    t.end = j + 1;
    if (t.closing) {
        // closing tags carry nothing
        for (int k = attrStart; k < j; ++k)
            if (!isWs(s[k])) return false;
        return true;
    }
    t.attrs = parseAttrs(s.mid(attrStart, j - attrStart), t.attrsOk);
    return t.attrsOk;
}

bool onlyAttrs(const Tag &t, std::initializer_list<const char *> allowed)
{
    for (const Attr &a : t.attrs) {
        bool found = false;
        for (const char *name : allowed) {
            if (a.name == QLatin1String(name)) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

QString attrValue(const Tag &t, const char *name)
{
    for (const Attr &a : t.attrs)
        if (a.name == QLatin1String(name)) return a.value;
    return QString();
}

bool hasAttr(const Tag &t, const char *name)
{
    for (const Attr &a : t.attrs)
        if (a.name == QLatin1String(name)) return true;
    return false;
}

// Position of the close tag matching an already-consumed open tag `name`,
// counting nested same-name tags. -1 when unbalanced (streaming partial).
bool findMatchingClose(QStringView s, int from, const QString &name, int &closeStart, int &closeEnd)
{
    int depth = 1;
    int i = from;
    const int n = int(s.size());
    while (i < n) {
        const int lt = int(s.indexOf(QLatin1Char('<'), i));
        if (lt < 0) return false;
        Tag t;
        if (parseTag(s, lt, t) && t.name == name) {
            if (t.closing) {
                if (--depth == 0) {
                    closeStart = lt;
                    closeEnd = t.end;
                    return true;
                }
            } else {
                ++depth;
            }
            i = t.end;
        } else {
            i = lt + 1;
        }
    }
    return false;
}

bool onlyIndentBefore(QStringView s, int i)
{
    int spaces = 0;
    for (int k = i - 1; k >= 0; --k) {
        if (s[k] == QLatin1Char('\n')) return true;
        if (s[k] != QLatin1Char(' ')) return false;
        if (++spaces > 3) return false;
    }
    return true;
}

QChar subSupMap(QChar c, bool sup)
{
    static const char16_t kSub[] = u"₀₁₂₃₄₅₆₇₈₉₊₋₌₍₎";
    static const char16_t kSup[] = u"⁰¹²³⁴⁵⁶⁷⁸⁹⁺⁻⁼⁽⁾";
    const QString digitsOps = QStringLiteral("0123456789+-=()");
    const int idx = int(digitsOps.indexOf(c));
    if (idx >= 0) return QChar(sup ? kSup[idx] : kSub[idx]);
    if (sup) {
        if (c == QLatin1Char('n')) return QChar(0x207F);
        if (c == QLatin1Char('i')) return QChar(0x2071);
    } else {
        switch (c.unicode()) {
        case 'a': return QChar(0x2090);
        case 'e': return QChar(0x2091);
        case 'o': return QChar(0x2092);
        case 'x': return QChar(0x2093);
        case 'i': return QChar(0x1D62);
        case 'j': return QChar(0x2C7C);
        case 'n': return QChar(0x2099);
        case 'm': return QChar(0x2098);
        default: break;
        }
    }
    return QChar();
}

// ---------------------------------------------------------------- scanner

void scan(QStringView s, Ctx &c, QString &out);

// Occurrences of an ASCII tag prefix, ASCII case-insensitive.
int countOpenTags(QStringView s, QLatin1String prefix)
{
    int count = 0;
    qsizetype from = 0;
    while (true) {
        const qsizetype at = s.indexOf(prefix, from, Qt::CaseInsensitive);
        if (at < 0) return count;
        ++count;
        from = at + prefix.size();
    }
}

bool emitMath(Ctx &c, QStringView raw, bool display, QString &out)
{
    const QString src = raw.trimmed().toString();
    if (src.isEmpty() || src.size() > kMaxFormulaChars
        || c.res.math.size() >= kMaxFormulas) {
        return false;
    }
    if (c.accept && !c.accept(src, display))
        return false;
    const int index = c.res.math.size();
    c.res.math.append({src, display});
    // Qt's markdown importer drops an image whose alt text is empty, so the
    // alt must be non-empty for the <img> to survive.
    const QString img = QStringLiteral("![formula](math:%1)").arg(index);
    if (display)
        out += QStringLiteral("\n\n") + img + QStringLiteral("\n\n");
    else
        out += img;
    return true;
}

// Returns the new scan index when a formula starting at s[i] was consumed, else 0.\n// When the delimiters matched but the formula was refused (unsupported, over a\n// limit), `literalEnd` is set past the closer so the scanner keeps that span as\n// text instead of re-reading its inner `$` as a new opener.
int tryMath(QStringView s, int i, Ctx &c, QString &out, int &literalEnd)
{
    const int n = int(s.size());
    const QChar ch = s[i];

    auto escapedAt = [&](int pos) {
        int bs = 0;
        for (int k = pos - 1; k >= 0 && s[k] == QLatin1Char('\\'); --k) ++bs;
        return (bs & 1) != 0;
    };

    if (ch == QLatin1Char('$')) {
        if (i + 1 < n && s[i + 1] == QLatin1Char('$')) { // $$ … $$
            for (int j = i + 2; j + 1 < n; ++j) {
                if (s[j] == QLatin1Char('$') && s[j + 1] == QLatin1Char('$') && !escapedAt(j)) {
                    if (!emitMath(c, s.mid(i + 2, j - (i + 2)), true, out)) {
                        literalEnd = j + 2;
                        return 0;
                    }
                    return j + 2;
                }
            }
            return 0;
        }
        // inline $ … $ : opener not followed by whitespace; closer not preceded
        // by whitespace and not followed by a digit; same line.
        if (i + 1 >= n || isWs(s[i + 1])) return 0;
        for (int j = i + 1; j < n; ++j) {
            const QChar d = s[j];
            if (d == QLatin1Char('\\')) { ++j; continue; }
            if (d == QLatin1Char('\n')) return 0;
            if (d != QLatin1Char('$')) continue;
            if (isWs(s[j - 1])) return 0;
            if (j + 1 < n && s[j + 1].isDigit()) return 0;
            if (!emitMath(c, s.mid(i + 1, j - (i + 1)), false, out)) {
                literalEnd = j + 1;
                return 0;
            }
            return j + 1;
        }
        return 0;
    }

    if (ch == QLatin1Char('\\') && i + 1 < n) {
        if (s[i + 1] == QLatin1Char('(')) { // \( … \)
            for (int j = i + 2; j + 1 < n; ++j) {
                if (s[j] == QLatin1Char('\n')) return 0;
                if (s[j] == QLatin1Char('\\') && s[j + 1] == QLatin1Char(')')) {
                    if (!emitMath(c, s.mid(i + 2, j - (i + 2)), false, out)) {
                        literalEnd = j + 2;
                        return 0;
                    }
                    return j + 2;
                }
            }
            return 0;
        }
        if (s[i + 1] == QLatin1Char('[')) { // \[ … \]
            const int j = int(s.indexOf(QLatin1String("\\]"), i + 2));
            if (j < 0) return 0;
            if (!emitMath(c, s.mid(i + 2, j - (i + 2)), true, out)) {
                literalEnd = j + 2;
                return 0;
            }
            return j + 2;
        }
    }
    return 0;
}

// Emits `open + inner + close`, keeping edge whitespace outside the markers
// (CommonMark emphasis does not open next to whitespace).
bool emitWrapped(QStringView inner, const QString &marker, Ctx &c, QString &out)
{
    if (inner.trimmed().isEmpty()) return false;
    int a = 0;
    while (a < inner.size() && isWs(inner[a])) ++a;
    int b = int(inner.size());
    while (b > a && isWs(inner[b - 1])) --b;
    out += inner.left(a);
    out += marker;
    scan(inner.mid(a, b - a), c, out);
    out += marker;
    out += inner.mid(b);
    return true;
}

// Returns the new scan index when an allowlisted HTML construct starting at
// s[i] == '<' was consumed, else 0 (the '<' stays literal text).
int tryHtml(QStringView s, int i, Ctx &c, QString &out)
{
    const int n = int(s.size());

    if (s.mid(i, 4) == QLatin1String("<!--")) {
        const int e = int(s.indexOf(QLatin1String("-->"), i + 4));
        return e < 0 ? 0 : e + 3;
    }

    Tag t;
    if (!parseTag(s, i, t) || t.closing)
        return 0;
    const QString &name = t.name;

    // <br> / <br/> / <br />
    if (name == QLatin1String("br")) {
        if (!t.attrs.isEmpty()) return 0;
        out += QStringLiteral("  \n");
        return t.end;
    }

    const bool blockStart = onlyIndentBefore(s, i);

    // <details [open]><summary>…</summary> body </details>
    if (name == QLatin1String("details")) {
        if (!blockStart || !onlyAttrs(t, {"open"})) return 0;
        int closeStart = 0, closeEnd = 0;
        if (!findMatchingClose(s, t.end, name, closeStart, closeEnd)) return 0;
        QStringView body = s.mid(t.end, closeStart - t.end);

        QString title = QStringLiteral("Details");
        Tag st;
        if (body.trimmed().startsWith(QLatin1Char('<'))) {
            int lead = 0;
            while (lead < body.size() && isWs(body[lead])) ++lead;
            if (parseTag(body, lead, st) && !st.closing && st.name == QLatin1String("summary")
                && st.attrs.isEmpty()) {
                int sc = 0, se = 0;
                if (findMatchingClose(body, st.end, st.name, sc, se)) {
                    QString raw = body.mid(st.end, sc - st.end).toString();
                    raw.replace(QLatin1Char('\n'), QLatin1Char(' '));
                    raw.replace(QLatin1Char('\r'), QLatin1Char(' '));
                    // Entities, not backslashes: "\[" would read as a math opener.
                    raw.replace(QLatin1Char('['), QStringLiteral("&#91;"));
                    raw.replace(QLatin1Char(']'), QStringLiteral("&#93;"));
                    QString processed;
                    Ctx titleCtx{c.flipped, c.accept, c.res, c.detailsOrdinal};
                    scan(raw.trimmed(), titleCtx, processed);
                    c.detailsOrdinal = titleCtx.detailsOrdinal;
                    processed.replace(QLatin1Char('\n'), QLatin1Char(' '));
                    title = processed.trimmed();
                    if (title.isEmpty()) title = QStringLiteral("Details");
                    body = body.mid(se);
                }
            }
        }

        c.res.hasDetails = true;
        const int ordinal = c.detailsOrdinal++;
        const bool open = hasAttr(t, "open") != c.flipped.contains(ordinal);
        out += QStringLiteral("\n\n[") + QChar(open ? 0x25BE : 0x25B8) + QLatin1Char(' ')
               + title + QStringLiteral("](") + QLatin1String(kDetailsScheme)
               + QString::number(ordinal) + QStringLiteral(")\n\n");
        if (open) {
            scan(body.trimmed(), c, out);
            out += QStringLiteral("\n\n");
        } else {
            // Keep ordinals stable: collapsed nested <details> still count.
            c.detailsOrdinal += countOpenTags(body, QLatin1String("<details"));
        }
        return closeEnd;
    }

    // <div align=…> / <p align=…> / <center>
    if (name == QLatin1String("div") || name == QLatin1String("p")
        || name == QLatin1String("center")) {
        if (!blockStart) return 0;
        QChar kind;
        if (name == QLatin1String("center")) {
            if (!t.attrs.isEmpty()) return 0;
            kind = QLatin1Char('c');
        } else {
            if (t.attrs.size() != 1 || !onlyAttrs(t, {"align"})) return 0;
            const QString v = attrValue(t, "align").toLower();
            if (v == QLatin1String("left")) kind = QLatin1Char('l');
            else if (v == QLatin1String("center")) kind = QLatin1Char('c');
            else if (v == QLatin1String("right")) kind = QLatin1Char('r');
            else if (v == QLatin1String("justify")) kind = QLatin1Char('j');
            else return 0;
        }
        int closeStart = 0, closeEnd = 0;
        if (!findMatchingClose(s, t.end, name, closeStart, closeEnd)) return 0;
        c.res.hasAlign = true;
        out += QStringLiteral("\n\n") + kAlignBegin + kind + QStringLiteral("\n\n");
        scan(s.mid(t.end, closeStart - t.end).trimmed(), c, out);
        out += QStringLiteral("\n\n") + kAlignEnd + QStringLiteral("\n\n");
        return closeEnd;
    }

    // Inline pairs.
    enum class Pair { None, Bold, Italic, Strike, Code, Link, Sub, Sup };
    Pair pair = Pair::None;
    if (name == QLatin1String("b") || name == QLatin1String("strong")) pair = Pair::Bold;
    else if (name == QLatin1String("i") || name == QLatin1String("em")) pair = Pair::Italic;
    else if (name == QLatin1String("s") || name == QLatin1String("del")) pair = Pair::Strike;
    else if (name == QLatin1String("code") || name == QLatin1String("kbd")) pair = Pair::Code;
    else if (name == QLatin1String("a")) pair = Pair::Link;
    else if (name == QLatin1String("sub")) pair = Pair::Sub;
    else if (name == QLatin1String("sup")) pair = Pair::Sup;
    if (pair == Pair::None) return 0;

    if (pair == Pair::Link ? !onlyAttrs(t, {"href"}) : !t.attrs.isEmpty())
        return 0;

    int closeStart = 0, closeEnd = 0;
    if (!findMatchingClose(s, t.end, name, closeStart, closeEnd)) return 0;
    const QStringView inner = s.mid(t.end, closeStart - t.end);
    if (inner.contains(QLatin1String("\n\n"))) return 0; // never across paragraphs

    switch (pair) {
    case Pair::Bold:
        return emitWrapped(inner, QStringLiteral("**"), c, out) ? closeEnd : 0;
    case Pair::Italic:
        return emitWrapped(inner, QStringLiteral("*"), c, out) ? closeEnd : 0;
    case Pair::Strike:
        return emitWrapped(inner, QStringLiteral("~~"), c, out) ? closeEnd : 0;
    case Pair::Code:
        if (inner.isEmpty() || inner.contains(QLatin1Char('`')) || inner.contains(QLatin1Char('\n')))
            return 0;
        out += QLatin1Char('`');
        out += inner;
        out += QLatin1Char('`');
        return closeEnd;
    case Pair::Link: {
        const QString href = attrValue(t, "href");
        const QString lower = href.toLower();
        if (!(lower.startsWith(QLatin1String("http://")) || lower.startsWith(QLatin1String("https://"))
              || lower.startsWith(QLatin1String("mailto:"))))
            return 0;
        for (QChar ch : href) {
            if (isWs(ch) || ch == QLatin1Char('(') || ch == QLatin1Char(')')
                || ch == QLatin1Char('<') || ch == QLatin1Char('>') || ch == QLatin1Char('"'))
                return 0;
        }
        if (inner.trimmed().isEmpty() || inner.contains(QLatin1Char('[')) || inner.contains(QLatin1Char(']')))
            return 0;
        out += QLatin1Char('[');
        scan(inner.trimmed(), c, out);
        out += QStringLiteral("](") + href + QLatin1Char(')');
        return closeEnd;
    }
    case Pair::Sub:
    case Pair::Sup: {
        if (inner.isEmpty()) return 0;
        QString mapped;
        for (QChar ch : inner) {
            const QChar m = subSupMap(ch, pair == Pair::Sup);
            if (m.isNull()) return 0;
            mapped += m;
        }
        out += mapped;
        return closeEnd;
    }
    case Pair::None:
        break;
    }
    return 0;
}

struct Fence
{
    int end = 0;          // one past the closing fence line (or end of input)
    bool closed = false;  // a closing fence was found (false while streaming)
    QStringView info;     // text after the opening run
    QStringView body;     // lines between the fences, without the final newline
};

// Fence opener at s[i] (line start, ≤3 spaces indent). False when not a fence.
bool parseFence(QStringView s, int i, Fence &fence)
{
    const int n = int(s.size());
    int j = i;
    int indent = 0;
    while (j < n && s[j] == QLatin1Char(' ') && indent < 4) { ++j; ++indent; }
    if (indent > 3 || j >= n) return false;
    const QChar f = s[j];
    if (f != QLatin1Char('`') && f != QLatin1Char('~')) return false;
    int run = 0;
    while (j < n && s[j] == f) { ++j; ++run; }
    if (run < 3) return false;
    // backtick fences must not contain a backtick in the info string
    int eol = int(s.indexOf(QLatin1Char('\n'), j));
    if (eol < 0) eol = n;
    if (f == QLatin1Char('`') && s.mid(j, eol - j).contains(QLatin1Char('`'))) return false;

    fence = Fence();
    fence.info = s.mid(j, eol - j);
    fence.end = n;
    fence.body = eol < n ? s.mid(eol + 1) : QStringView();

    int line = eol;
    while (line < n) {
        const int ls = line + 1;
        int le = int(s.indexOf(QLatin1Char('\n'), ls));
        if (le < 0) le = n;
        int k = ls, ind = 0;
        while (k < le && s[k] == QLatin1Char(' ') && ind < 4) { ++k; ++ind; }
        if (ind <= 3) {
            int r = 0;
            while (k < le && s[k] == f) { ++k; ++r; }
            if (r >= run) {
                while (k < le && isWs(s[k])) ++k;
                if (k == le) {
                    fence.closed = true;
                    fence.end = le;
                    fence.body = s.mid(eol + 1, std::max(0, ls - 1 - (eol + 1)));
                    return true;
                }
            }
        }
        line = le;
        if (le >= n) break;
    }
    return true;
}

// True when, after only blank space, another fence opens at a line start.
bool fenceFollows(QStringView s, int from)
{
    const int n = int(s.size());
    int lineStart = from;
    int q = from;
    while (q < n && isWs(s[q])) {
        if (s[q] == QLatin1Char('\n')) lineStart = q + 1;
        ++q;
    }
    if (q >= n) return false;
    Fence next;
    return parseFence(s, lineStart, next);
}

// A fence holding one display formula: ```math / latex / tex / katex, or an
// unlabelled fence whose whole content is a single $$…$$ or \[…\] block (the
// shape agents use to show LaTeX they believe will not be rendered). A
// $$…$$ / \[…\] wrapper inside a labelled fence is optional.
bool mathFenceSource(const Fence &f, QStringView &source)
{
    const QString info = f.info.trimmed().toString().toLower();
    const bool labelled = info == QLatin1String("math") || info == QLatin1String("latex")
                          || info == QLatin1String("tex") || info == QLatin1String("katex");
    if (!labelled && !info.isEmpty()) return false;

    const QStringView body = f.body.trimmed();
    const bool dollar = body.size() >= 4 && body.startsWith(QLatin1String("$$"))
                        && body.endsWith(QLatin1String("$$"));
    const bool bracket = body.size() >= 4 && body.startsWith(QLatin1String("\\["))
                         && body.endsWith(QLatin1String("\\]"));
    if (dollar || bracket) {
        const QStringView inner = body.mid(2, body.size() - 4);
        // two formulas in one fence is not "a single formula"
        if (inner.contains(QLatin1String(dollar ? "$$" : "\\]"))) return false;
        source = inner;
        return true;
    }
    if (!labelled) return false;
    source = body;
    return !body.isEmpty();
}

// Inline code spans end where their block does (CommonMark): an ATX heading line
// ends at its newline, a paragraph at its first blank line. Without this a
// literal ``` in a heading pairs with the next fence's opener and swallows the
// fence (and the math inside it).
//
// End of the heading line containing `from`, or -1 when it is not a heading.
int headingLineEnd(QStringView s, int from)
{
    const int n = int(s.size());
    int eol = int(s.indexOf(QLatin1Char('\n'), from));
    if (eol < 0) eol = n;

    int ls = from;
    while (ls > 0 && s[ls - 1] != QLatin1Char('\n')) --ls;
    int k = ls, spaces = 0;
    while (k < n && s[k] == QLatin1Char(' ') && spaces < 4) { ++k; ++spaces; }
    if (spaces <= 3) {
        int hashes = 0;
        while (k < n && s[k] == QLatin1Char('#') && hashes < 7) { ++k; ++hashes; }
        if (hashes >= 1 && hashes <= 6
            && (k >= n || s[k] == QLatin1Char(' ') || s[k] == QLatin1Char('\t')
                || s[k] == QLatin1Char('\n')))
            return eol; // heading: the span cannot leave its own line
    }
    return -1;
}

// Index of the newline that precedes the first blank line at or after `from`
// (end of input when none). The caller memoises it per paragraph, so a run of
// code spans costs one scan of the paragraph, not one per span.
int paragraphEnd(QStringView s, int from)
{
    const int n = int(s.size());
    int eol = int(s.indexOf(QLatin1Char('\n'), from));
    if (eol < 0) eol = n;
    int line = eol;
    while (line < n) {
        const int next = line + 1;
        int le = int(s.indexOf(QLatin1Char('\n'), next));
        if (le < 0) le = n;
        bool blank = true;
        for (int q = next; q < le; ++q) {
            if (!isWs(s[q])) { blank = false; break; }
        }
        if (blank) return line; // blank line: paragraph (and any span) ends here
        line = le;
        if (le >= n) break;
    }
    return n;
}

void scan(QStringView s, Ctx &c, QString &out)
{
    const int n = int(s.size());
    int i = 0;
    int runStart = 0;
    bool lineStart = true;
    int paraEnd = -1; // memo for paragraphEnd(); valid while i <= paraEnd
    int lineMemoEnd = -1;     // memo for the heading check: newline ending the
    int lineMemoHeading = -1; // line it was computed for / heading end or -1

    auto flushRun = [&](int upTo) {
        if (upTo > runStart) out += s.mid(runStart, upTo - runStart);
    };

    while (i < n) {
        const QChar ch = s[i];

        if (lineStart) {
            Fence fence;
            if (parseFence(s, i, fence)) {
                QStringView source;
                QString piece;
                if (fence.closed && mathFenceSource(fence, source)
                    && emitMath(c, source, true, piece)) {
                    flushRun(i);
                    out += piece;
                    runStart = fence.end;
                } else if (fence.closed && fenceFollows(s, fence.end)) {
                    // Qt lays back-to-back fences out as one run of code lines.
                    // A zero-width paragraph between them keeps them two blocks.
                    flushRun(fence.end);
                    out += QStringLiteral("\n\n\u200B\n");
                    runStart = fence.end;
                } // else fenced code is verbatim: it stays inside the pending run
                i = fence.end;
                lineStart = false;
                continue;
            }
        }

        if (ch == QLatin1Char('\n')) {
            lineStart = true;
            ++i;
            continue;
        }
        if (ch != QLatin1Char(' ')) lineStart = false;

        if (ch == QLatin1Char('`')) {
            int run = 0;
            while (i + run < n && s[i + run] == QLatin1Char('`')) ++run;
            int j = i + run;
            bool closed = false;
            if (i > lineMemoEnd) { // once per line: O(n) total even on one huge line
                const int e = int(s.indexOf(QLatin1Char('\n'), i));
                lineMemoEnd = e < 0 ? n : e;
                lineMemoHeading = headingLineEnd(s, i);
            }
            int limit = lineMemoHeading;
            if (limit < 0) {
                if (i > paraEnd) paraEnd = paragraphEnd(s, i);
                limit = paraEnd;
            }
            while (j < limit) {
                const int k = int(s.indexOf(QLatin1Char('`'), j));
                if (k < 0 || k >= limit) break;
                int r = 0;
                while (k + r < n && s[k + r] == QLatin1Char('`')) ++r;
                // a fence can interrupt a paragraph: a run that opens a line is
                // a fence, not the closer of a span from an earlier line
                if (r >= 3 && onlyIndentBefore(s, k)) break;
                if (r == run) { j = k + r; closed = true; break; }
                j = k + r;
            }
            i = closed ? j : i + run;
            continue;
        }

        if (ch == QLatin1Char('\\')) {
            if (i + 1 < n && (s[i + 1] == QLatin1Char('(') || s[i + 1] == QLatin1Char('['))) {
                QString piece;
                int literalEnd = 0;
                const int next = tryMath(s, i, c, piece, literalEnd);
                if (next > 0) {
                    flushRun(i);
                    out += piece;
                    i = next;
                    runStart = i;
                    continue;
                }
                if (literalEnd > i) {
                    i = literalEnd;
                    continue;
                }
            }
            i += (i + 1 < n) ? 2 : 1; // an escaped char is never a delimiter
            continue;
        }

        if (ch == QLatin1Char('$')) {
            QString piece;
            int literalEnd = 0;
            const int next = tryMath(s, i, c, piece, literalEnd);
            if (next > 0) {
                flushRun(i);
                out += piece;
                i = next;
                runStart = i;
                continue;
            }
            i = literalEnd > i ? literalEnd : i + 1;
            continue;
        }

        if (ch == QLatin1Char('<')) {
            QString piece;
            const int next = tryHtml(s, i, c, piece);
            if (next > 0) {
                flushRun(i);
                out += piece;
                i = next;
                runStart = i;
                lineStart = false;
                continue;
            }
            ++i;
            continue;
        }

        if (ch == QLatin1Char(']') && i + 1 < n && s[i + 1] == QLatin1Char('(')) {
            // Link destination: verbatim, `$a$` inside a URL is not math.
            int depth = 1;
            int j = i + 2;
            while (j < n && depth > 0 && s[j] != QLatin1Char('\n')) {
                if (s[j] == QLatin1Char('(')) ++depth;
                else if (s[j] == QLatin1Char(')')) --depth;
                ++j;
            }
            i = depth == 0 ? j : i + 2;
            continue;
        }

        ++i;
    }
    flushRun(n);
}

} // namespace

Result run(const QString &input, const QSet<int> &flippedDetails, const MathAccept &accept)
{
    Result r;

    // Message text must never be able to forge an alignment sentinel.
    QString text = input;
    if (text.contains(kAlignBegin) || text.contains(kAlignEnd)) {
        text.remove(kAlignBegin);
        text.remove(kAlignEnd);
    }

    // Fences are candidates too: ```math / ```latex hold display formulas.
    const bool candidate = text.contains(QLatin1Char('$')) || text.contains(QLatin1Char('<'))
                           || text.contains(QLatin1String("\\("))
                           || text.contains(QLatin1String("\\["))
                           || text.contains(QLatin1String("```"))
                           || text.contains(QLatin1String("~~~"));
    if (!candidate) {
        r.markdown = text;
        r.changed = text != input;
        return r;
    }

    r.markdown.reserve(text.size() + 32);
    Ctx ctx{flippedDetails, accept, r};
    scan(text, ctx, r.markdown);
    r.changed = r.markdown != input;
    return r;
}

} // namespace MarkdownPreprocessor


