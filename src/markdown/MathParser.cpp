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

#include "MathParser.h"

#include <QHash>
#include <QSet>
#include <QStringView>

#include <algorithm>
#include <utility>

namespace Math {

namespace {

struct Sym
{
    const char16_t *glyph;
    AtomClass cls;
    FontStyle style;
};

const QHash<QString, Sym> &symbolTable()
{
    static const QHash<QString, Sym> table = [] {
        QHash<QString, Sym> t;
        auto add = [&t](const char *name, const char16_t *glyph, AtomClass cls, FontStyle st) {
            t.insert(QLatin1String(name), Sym{glyph, cls, st});
        };
        constexpr AtomClass O = AtomClass::Ord;
        constexpr AtomClass B = AtomClass::Bin;
        constexpr AtomClass R = AtomClass::Rel;
        constexpr AtomClass P = AtomClass::Punct;
        constexpr FontStyle I = FontStyle::Italic;
        constexpr FontStyle N = FontStyle::Roman;

        // Greek (lowercase italic, uppercase upright)
        add("alpha", u"α", O, I); add("beta", u"β", O, I);
        add("gamma", u"γ", O, I); add("delta", u"δ", O, I);
        add("epsilon", u"ϵ", O, I); add("varepsilon", u"ε", O, I);
        add("zeta", u"ζ", O, I); add("eta", u"η", O, I);
        add("theta", u"θ", O, I); add("vartheta", u"ϑ", O, I);
        add("iota", u"ι", O, I); add("kappa", u"κ", O, I);
        add("lambda", u"λ", O, I); add("mu", u"μ", O, I);
        add("nu", u"ν", O, I); add("xi", u"ξ", O, I);
        add("pi", u"π", O, I); add("varpi", u"ϖ", O, I);
        add("rho", u"ρ", O, I); add("varrho", u"ϱ", O, I);
        add("sigma", u"σ", O, I); add("varsigma", u"ς", O, I);
        add("tau", u"τ", O, I); add("upsilon", u"υ", O, I);
        add("phi", u"ϕ", O, I); add("varphi", u"φ", O, I);
        add("chi", u"χ", O, I); add("psi", u"ψ", O, I);
        add("omega", u"ω", O, I);
        add("Gamma", u"Γ", O, N); add("Delta", u"Δ", O, N);
        add("Theta", u"Θ", O, N); add("Lambda", u"Λ", O, N);
        add("Xi", u"Ξ", O, N); add("Pi", u"Π", O, N);
        add("Sigma", u"Σ", O, N); add("Upsilon", u"Υ", O, N);
        add("Phi", u"Φ", O, N); add("Psi", u"Ψ", O, N);
        add("Omega", u"Ω", O, N);

        // Ordinary symbols
        add("infty", u"∞", O, N); add("partial", u"∂", O, N);
        add("nabla", u"∇", O, N); add("forall", u"∀", O, N);
        add("exists", u"∃", O, N); add("emptyset", u"∅", O, N);
        add("varnothing", u"∅", O, N); add("neg", u"¬", O, N);
        add("lnot", u"¬", O, N); add("angle", u"∠", O, N);
        add("prime", u"′", O, N); add("hbar", u"ℏ", O, N);
        add("ell", u"ℓ", O, I); add("Re", u"ℜ", O, N);
        add("Im", u"ℑ", O, N); add("aleph", u"ℵ", O, N);
        add("top", u"⊤", O, N); add("bot", u"⊥", O, N);
        add("mid", u"∣", R, N); add("parallel", u"∥", R, N);
        add("perp", u"⊥", R, N); add("backslash", u"\\", O, N);
        add("ldots", u"…", O, N); add("dots", u"…", O, N);
        add("cdots", u"⋯", O, N); add("vdots", u"⋮", O, N);
        add("ddots", u"⋱", O, N);
        add("therefore", u"∴", R, N); add("because", u"∵", R, N);

        // Binary operators
        add("pm", u"±", B, N); add("mp", u"∓", B, N);
        add("times", u"×", B, N); add("div", u"÷", B, N);
        add("cdot", u"⋅", B, N); add("ast", u"∗", B, N);
        add("star", u"⋆", B, N); add("circ", u"∘", B, N);
        add("bullet", u"∙", B, N); add("cup", u"∪", B, N);
        add("cap", u"∩", B, N); add("land", u"∧", B, N);
        add("wedge", u"∧", B, N); add("lor", u"∨", B, N);
        add("vee", u"∨", B, N); add("oplus", u"⊕", B, N);
        add("otimes", u"⊗", B, N); add("setminus", u"∖", B, N);

        // Relations
        add("leq", u"≤", R, N); add("le", u"≤", R, N);
        add("geq", u"≥", R, N); add("ge", u"≥", R, N);
        add("neq", u"≠", R, N); add("ne", u"≠", R, N);
        add("approx", u"≈", R, N); add("equiv", u"≡", R, N);
        add("sim", u"∼", R, N); add("simeq", u"≃", R, N);
        add("cong", u"≅", R, N); add("propto", u"∝", R, N);
        add("ll", u"≪", R, N); add("gg", u"≫", R, N);
        add("in", u"∈", R, N); add("notin", u"∉", R, N);
        add("ni", u"∋", R, N); add("subset", u"⊂", R, N);
        add("subseteq", u"⊆", R, N); add("supset", u"⊃", R, N);
        add("supseteq", u"⊇", R, N);
        add("to", u"→", R, N); add("rightarrow", u"→", R, N);
        add("leftarrow", u"←", R, N); add("gets", u"←", R, N);
        add("leftrightarrow", u"↔", R, N);
        add("Rightarrow", u"⇒", R, N); add("Leftarrow", u"⇐", R, N);
        add("Leftrightarrow", u"⇔", R, N); add("implies", u"⟹", R, N);
        add("iff", u"⟺", R, N); add("mapsto", u"↦", R, N);
        add("uparrow", u"↑", R, N); add("downarrow", u"↓", R, N);

        add("colon", u":", P, N);

        // Bars, brackets and fences usable as plain atoms (outside \left...\right),
        // e.g. norms \lVert v \rVert and inner products \langle u, v \rangle.
        constexpr AtomClass OP = AtomClass::Open;
        constexpr AtomClass CL = AtomClass::Close;
        add("lVert", u"\u2016", OP, N); add("rVert", u"\u2016", CL, N);
        add("Vert", u"\u2016", O, N);   add("vert", u"|", O, N);
        add("lvert", u"|", OP, N);      add("rvert", u"|", CL, N);
        add("langle", u"\u27E8", OP, N); add("rangle", u"\u27E9", CL, N);
        add("lfloor", u"\u230A", OP, N); add("rfloor", u"\u230B", CL, N);
        add("lceil", u"\u2308", OP, N);  add("rceil", u"\u2309", CL, N);
        add("lbrace", u"{", OP, N);     add("rbrace", u"}", CL, N);
        // dotless i / j, for \hat{\imath} and friends
        add("imath", u"\u0131", O, I);  add("jmath", u"\u0237", O, I);
        return t;
    }();
    return table;
}

// Delimiter token -> glyph. Returns false when the token is not a delimiter.
bool delimGlyph(const QString &token, QString &glyph)
{
    static const QHash<QString, QString> map = {
        {QStringLiteral("("), QStringLiteral("(")}, {QStringLiteral(")"), QStringLiteral(")")},
        {QStringLiteral("["), QStringLiteral("[")}, {QStringLiteral("]"), QStringLiteral("]")},
        {QStringLiteral("|"), QStringLiteral("|")}, {QStringLiteral("/"), QStringLiteral("/")},
        {QStringLiteral("<"), QStringLiteral("⟨")}, {QStringLiteral(">"), QStringLiteral("⟩")},
        {QStringLiteral("\\{"), QStringLiteral("{")}, {QStringLiteral("\\}"), QStringLiteral("}")},
        {QStringLiteral("\\lbrace"), QStringLiteral("{")}, {QStringLiteral("\\rbrace"), QStringLiteral("}")},
        {QStringLiteral("\\|"), QStringLiteral("‖")}, {QStringLiteral("\\vert"), QStringLiteral("|")},
        {QStringLiteral("\\lvert"), QStringLiteral("|")}, {QStringLiteral("\\rvert"), QStringLiteral("|")},
        {QStringLiteral("\\lVert"), QStringLiteral("\u2016")}, {QStringLiteral("\\rVert"), QStringLiteral("\u2016")},
        {QStringLiteral("\\Vert"), QStringLiteral("‖")},
        {QStringLiteral("\\langle"), QStringLiteral("⟨")}, {QStringLiteral("\\rangle"), QStringLiteral("⟩")},
        {QStringLiteral("\\lfloor"), QStringLiteral("⌊")}, {QStringLiteral("\\rfloor"), QStringLiteral("⌋")},
        {QStringLiteral("\\lceil"), QStringLiteral("⌈")}, {QStringLiteral("\\rceil"), QStringLiteral("⌉")},
        {QStringLiteral("\\backslash"), QStringLiteral("\\")},
    };
    const auto it = map.constFind(token);
    if (it == map.constEnd()) return false;
    glyph = it.value();
    return true;
}

const QHash<QString, QString> &largeOpTable()
{
    static const QHash<QString, QString> t = {
        {QStringLiteral("sum"), QStringLiteral("∑")}, {QStringLiteral("prod"), QStringLiteral("∏")},
        {QStringLiteral("coprod"), QStringLiteral("∐")}, {QStringLiteral("int"), QStringLiteral("∫")},
        {QStringLiteral("iint"), QStringLiteral("∬")}, {QStringLiteral("iiint"), QStringLiteral("∭")},
        {QStringLiteral("oint"), QStringLiteral("∮")}, {QStringLiteral("bigcup"), QStringLiteral("⋃")},
        {QStringLiteral("bigcap"), QStringLiteral("⋂")}, {QStringLiteral("bigoplus"), QStringLiteral("⨁")},
        {QStringLiteral("bigotimes"), QStringLiteral("⨂")}, {QStringLiteral("bigvee"), QStringLiteral("⋁")},
        {QStringLiteral("bigwedge"), QStringLiteral("⋀")},
    };
    return t;
}

const QHash<QString, QString> &namedOpTable()
{
    static const QHash<QString, QString> t = [] {
        QHash<QString, QString> m;
        for (const char *n : {"sin", "cos", "tan", "cot", "sec", "csc", "arcsin", "arccos",
                              "arctan", "sinh", "cosh", "tanh", "coth", "log", "ln", "lg", "exp",
                              "deg", "dim", "ker", "hom", "arg", "lim", "max", "min", "sup",
                              "inf", "det", "gcd", "Pr"})
            m.insert(QLatin1String(n), QLatin1String(n));
        m.insert(QStringLiteral("limsup"), QStringLiteral("lim sup"));
        m.insert(QStringLiteral("liminf"), QStringLiteral("lim inf"));
        return m;
    }();
    return t;
}

bool isMatrixEnv(const QString &env)
{
    static const QSet<QString> envs = {
        QStringLiteral("matrix"), QStringLiteral("pmatrix"), QStringLiteral("bmatrix"),
        QStringLiteral("Bmatrix"), QStringLiteral("vmatrix"), QStringLiteral("Vmatrix"),
        QStringLiteral("cases"), QStringLiteral("aligned"), QStringLiteral("align"),
        QStringLiteral("align*"), QStringLiteral("split"), QStringLiteral("gathered"),
        QStringLiteral("array")};
    return envs.contains(env);
}

QString codePoint(char32_t cp)
{
    return QString::fromUcs4(&cp, 1);
}

// \mathbb / \mathcal alphabets: Mathematical Alphanumeric Symbols block, with
// the holes filled by the pre-existing letterlike code points.
bool blackboard(QChar c, QString &out)
{
    static const QHash<char16_t, char16_t> special = {
        {u'C', u'ℂ'}, {u'H', u'ℍ'}, {u'N', u'ℕ'}, {u'P', u'ℙ'},
        {u'Q', u'ℚ'}, {u'R', u'ℝ'}, {u'Z', u'ℤ'}};
    const char16_t u = c.unicode();
    const auto it = special.constFind(u);
    if (it != special.constEnd()) { out = QString(QChar(it.value())); return true; }
    if (u >= u'A' && u <= u'Z') { out = codePoint(0x1D538u + (u - u'A')); return true; }
    if (u >= u'a' && u <= u'z') { out = codePoint(0x1D552u + (u - u'a')); return true; }
    if (u >= u'0' && u <= u'9') { out = codePoint(0x1D7D8u + (u - u'0')); return true; }
    return false;
}

bool calligraphic(QChar c, QString &out)
{
    static const QHash<char16_t, char16_t> special = {
        {u'B', u'ℬ'}, {u'E', u'ℰ'}, {u'F', u'ℱ'}, {u'H', u'ℋ'},
        {u'I', u'ℐ'}, {u'L', u'ℒ'}, {u'M', u'ℳ'}, {u'R', u'ℛ'},
        {u'e', u'ℯ'}, {u'g', u'ℊ'}, {u'o', u'ℴ'}};
    const char16_t u = c.unicode();
    const auto it = special.constFind(u);
    if (it != special.constEnd()) { out = QString(QChar(it.value())); return true; }
    if (u >= u'A' && u <= u'Z') { out = codePoint(0x1D49Cu + (u - u'A')); return true; }
    if (u >= u'a' && u <= u'z') { out = codePoint(0x1D4B6u + (u - u'a')); return true; }
    return false;
}

class Parser
{
public:
    Parser(QStringView s, int depth) : s_(s), depth_(depth) {}

    bool parseTop(Node &out)
    {
        if (!parseRow(out)) return false;
        skipWs();
        if (pos_ < s_.size()) {
            if (s_[pos_] == QLatin1Char('}')) return fail("unmatched }");
            if (s_[pos_] == QLatin1Char('&')) return fail("alignment tab outside an environment");
            if (s_.mid(pos_).startsWith(QLatin1String("\\\\"))) return fail("line break outside an environment");
            if (peekCommand() == QLatin1String("right")) return fail("\\right without \\left");
            if (peekCommand() == QLatin1String("end")) return fail("\\end without \\begin");
            return fail("unexpected input");
        }
        return true;
    }

    QString error;

private:
    QStringView s_;
    int pos_ = 0;
    int depth_;

    bool fail(const char *msg)
    {
        if (error.isEmpty()) error = QString::fromLatin1(msg);
        return false;
    }
    bool failCmd(const char *prefix, const QString &name)
    {
        if (error.isEmpty()) error = QString::fromLatin1(prefix) + name;
        return false;
    }

    bool atEnd() const { return pos_ >= s_.size(); }
    void skipWs()
    {
        while (pos_ < s_.size() && s_[pos_].isSpace()) ++pos_;
    }

    // Name of the command at pos_ without consuming it ("" when not a command).
    QString peekCommand() const
    {
        if (pos_ >= s_.size() || s_[pos_] != QLatin1Char('\\')) return QString();
        int j = pos_ + 1;
        if (j < s_.size() && s_[j].isLetter()) {
            const int start = j;
            while (j < s_.size() && s_[j].isLetter()) ++j;
            return s_.mid(start, j - start).toString();
        }
        return QString();
    }

    bool enter()
    {
        if (++depth_ > kMaxDepth) return fail("nesting too deep");
        return true;
    }
    void leave() { --depth_; }

    static Node makeAtom(const QString &text, AtomClass cls, FontStyle st)
    {
        Node n;
        n.kind = Node::Kind::Atom;
        n.text = text;
        n.cls = cls;
        n.style = st;
        return n;
    }

    // Row of items up to a terminator ('}', '&', "\\\\", \right, \end, end of input).
    bool parseRow(Node &out)
    {
        out = Node();
        out.kind = Node::Kind::Row;
        while (true) {
            skipWs();
            if (atEnd()) return true;
            const QChar c = s_[pos_];
            if (c == QLatin1Char('}') || c == QLatin1Char('&')) return true;
            if (c == QLatin1Char('\\')) {
                if (s_.mid(pos_).startsWith(QLatin1String("\\\\"))) return true;
                const QString name = peekCommand();
                if (name == QLatin1String("right") || name == QLatin1String("end")) return true;
            }
            Node item;
            if (c == QLatin1Char('^') || c == QLatin1Char('_')) {
                item.kind = Node::Kind::Row; // script with no base
            } else if (!parseItem(item, false)) {
                return false;
            }
            if (!parseScripts(item)) return false;
            out.kids.push_back(std::move(item));
        }
    }

    // Wraps `item` in a Script node when ^ _ ' follow.
    bool parseScripts(Node &item)
    {
        bool hasSub = false, hasSup = false, supFromPrime = false;
        Node sub, sup;
        while (true) {
            skipWs();
            if (atEnd()) break;
            const QChar c = s_[pos_];
            if (c == QLatin1Char('\'')) {
                if (hasSup && !supFromPrime) return fail("double superscript");
                ++pos_;
                if (!hasSup) {
                    sup = Node();
                    sup.kind = Node::Kind::Row;
                    sup.kids.push_back(makeAtom(QStringLiteral("′"), AtomClass::Ord, FontStyle::Roman));
                    hasSup = true;
                    supFromPrime = true;
                } else {
                    sup.kids.front().text += QChar(0x2032);
                }
            } else if (c == QLatin1Char('^')) {
                if (hasSup) return fail("double superscript");
                ++pos_;
                if (!parseArg(sup)) return false;
                hasSup = true;
            } else if (c == QLatin1Char('_')) {
                if (hasSub) return fail("double subscript");
                ++pos_;
                if (!parseArg(sub)) return false;
                hasSub = true;
            } else {
                break;
            }
        }
        if (!hasSub && !hasSup) return true;
        Node script;
        script.kind = Node::Kind::Script;
        script.hasSub = hasSub;
        script.hasSup = hasSup;
        script.kids.push_back(std::move(item));
        if (hasSub) script.kids.push_back(std::move(sub));
        if (hasSup) script.kids.push_back(std::move(sup));
        item = std::move(script);
        return true;
    }

    // One argument: a {group} or a single token, always returned as a Row.
    bool parseArg(Node &out)
    {
        skipWs();
        if (atEnd()) return fail("missing argument");
        const QChar c = s_[pos_];
        if (c == QLatin1Char('}') || c == QLatin1Char('&') || c == QLatin1Char('^')
            || c == QLatin1Char('_'))
            return fail("missing argument");
        if (c == QLatin1Char('{')) {
            ++pos_;
            if (!enter()) return false;
            if (!parseRow(out)) return false;
            leave();
            if (atEnd() || s_[pos_] != QLatin1Char('}')) return fail("unclosed {");
            ++pos_;
            return true;
        }
        Node item;
        if (!parseItem(item, true)) return false;
        out = Node();
        out.kind = Node::Kind::Row;
        out.kids.push_back(std::move(item));
        return true;
    }

    // Raw text of a {group} (nested braces balanced, no macro expansion).
    bool parseRawGroup(QString &out)
    {
        skipWs();
        if (atEnd() || s_[pos_] != QLatin1Char('{')) return fail("expected {");
        ++pos_;
        int depth = 1;
        const int start = pos_;
        while (pos_ < s_.size()) {
            const QChar c = s_[pos_];
            if (c == QLatin1Char('{')) ++depth;
            else if (c == QLatin1Char('}') && --depth == 0) break;
            ++pos_;
        }
        if (pos_ >= s_.size()) return fail("unclosed {");
        out = s_.mid(start, pos_ - start).toString();
        ++pos_;
        return true;
    }

    bool parseItem(Node &out, bool singleToken)
    {
        if (atEnd()) return fail("unexpected end");
        const QChar c = s_[pos_];

        if (c == QLatin1Char('{')) {
            ++pos_;
            if (!enter()) return false;
            if (!parseRow(out)) return false;
            leave();
            if (atEnd() || s_[pos_] != QLatin1Char('}')) return fail("unclosed {");
            ++pos_;
            return true;
        }
        if (c == QLatin1Char('\\'))
            return parseCommand(out);

        if (c.isDigit() && c.unicode() < 0x80) {
            const int start = pos_;
            ++pos_;
            if (!singleToken) {
                while (pos_ < s_.size()
                       && ((s_[pos_].isDigit() && s_[pos_].unicode() < 0x80)
                           || (s_[pos_] == QLatin1Char('.') && pos_ + 1 < s_.size()
                               && s_[pos_ + 1].isDigit()))) {
                    ++pos_;
                }
            }
            out = makeAtom(s_.mid(start, pos_ - start).toString(), AtomClass::Ord, FontStyle::Roman);
            return true;
        }
        ++pos_;
        if (c.isLetter()) {
            out = makeAtom(QString(c), AtomClass::Ord, FontStyle::Italic);
            return true;
        }
        switch (c.unicode()) {
        case '+': out = makeAtom(QStringLiteral("+"), AtomClass::Bin, FontStyle::Roman); return true;
        case '-': out = makeAtom(QStringLiteral("−"), AtomClass::Bin, FontStyle::Roman); return true;
        case '*': out = makeAtom(QStringLiteral("∗"), AtomClass::Bin, FontStyle::Roman); return true;
        case '=': case '<': case '>': case ':':
            out = makeAtom(QString(c), AtomClass::Rel, FontStyle::Roman); return true;
        case '(': case '[':
            out = makeAtom(QString(c), AtomClass::Open, FontStyle::Roman); return true;
        case ')': case ']':
            out = makeAtom(QString(c), AtomClass::Close, FontStyle::Roman); return true;
        case ',': case ';':
            out = makeAtom(QString(c), AtomClass::Punct, FontStyle::Roman); return true;
        case '/': case '|': case '!': case '.': case '?': case '@':
            out = makeAtom(QString(c), AtomClass::Ord, FontStyle::Roman); return true;
        case '\'':
            out = makeAtom(QStringLiteral("′"), AtomClass::Ord, FontStyle::Roman); return true;
        case '~':
            out = Node(); out.kind = Node::Kind::Space; out.em = 1.0 / 3.0; return true;
        default: break;
        }
        if (c.unicode() >= 0x80) { // pasted unicode symbol (≤, ∑, …)
            out = makeAtom(QString(c), AtomClass::Ord, FontStyle::Roman);
            return true;
        }
        --pos_;
        return fail("unsupported character");
    }

    bool parseDelimToken(QString &glyph)
    {
        skipWs();
        if (atEnd()) return fail("missing delimiter");
        QString token;
        if (s_[pos_] == QLatin1Char('\\')) {
            int j = pos_ + 1;
            if (j >= s_.size()) return fail("missing delimiter");
            if (s_[j].isLetter()) {
                while (j < s_.size() && s_[j].isLetter()) ++j;
            } else {
                ++j;
            }
            token = s_.mid(pos_, j - pos_).toString();
            pos_ = j;
        } else {
            token = QString(s_[pos_]);
            ++pos_;
        }
        if (token == QLatin1String(".")) { glyph.clear(); return true; }
        if (!delimGlyph(token, glyph)) return failCmd("unsupported delimiter ", token);
        return true;
    }

    void applyStyle(Node &n, FontStyle st) const
    {
        if (n.kind == Node::Kind::Atom) {
            if (n.text.size() >= 1 && n.text.at(0).isLetter() && n.text.at(0).unicode() < 0x80)
                n.style = st;
            else if (st == FontStyle::Bold)
                n.style = st;
        }
        for (Node &k : n.kids) applyStyle(k, st);
    }

    // Substitute Unicode alphabet characters (\mathbb, \mathcal) over a Row.
    bool mapAlphabet(Node &n, bool blackboardStyle) const
    {
        if (n.kind == Node::Kind::Atom && n.text.size() == 1) {
            QString mapped;
            const bool found = blackboardStyle ? blackboard(n.text.at(0), mapped)
                                               : calligraphic(n.text.at(0), mapped);
            if (found) {
                n.text = mapped;
                n.style = FontStyle::Roman;
            }
        }
        for (Node &k : n.kids) mapAlphabet(k, blackboardStyle);
        return true;
    }

    bool parseCommand(Node &out)
    {
        ++pos_; // backslash
        if (atEnd()) return fail("dangling backslash");

        // Single-character commands.
        if (!s_[pos_].isLetter()) {
            const QChar c = s_[pos_++];
            switch (c.unicode()) {
            case ',': out = space(3.0 / 18.0); return true;
            case ':': out = space(4.0 / 18.0); return true;
            case ';': out = space(5.0 / 18.0); return true;
            case '!': out = space(-3.0 / 18.0); return true;
            case ' ': out = space(1.0 / 3.0); return true;
            case '{': out = makeAtom(QStringLiteral("{"), AtomClass::Open, FontStyle::Roman); return true;
            case '}': out = makeAtom(QStringLiteral("}"), AtomClass::Close, FontStyle::Roman); return true;
            case '|': out = makeAtom(QStringLiteral("‖"), AtomClass::Ord, FontStyle::Roman); return true;
            case '$': case '%': case '&': case '#': case '_':
                out = makeAtom(QString(c), AtomClass::Ord, FontStyle::Roman); return true;
            default:
                return failCmd("unsupported command \\", QString(c));
            }
        }

        const int start = pos_;
        while (pos_ < s_.size() && s_[pos_].isLetter()) ++pos_;
        const QString name = s_.mid(start, pos_ - start).toString();

        const auto sym = symbolTable().constFind(name);
        if (sym != symbolTable().constEnd()) {
            out = makeAtom(QString::fromUtf16(sym->glyph), sym->cls, sym->style);
            return true;
        }

        if (name == QLatin1String("quad")) { out = space(1.0); return true; }
        if (name == QLatin1String("qquad")) { out = space(2.0); return true; }

        const auto big = largeOpTable().constFind(name);
        if (big != largeOpTable().constEnd()) {
            out = Node();
            out.kind = Node::Kind::LargeOp;
            out.text = big.value();
            return parseLimitsModifier(out);
        }
        const auto fn = namedOpTable().constFind(name);
        if (fn != namedOpTable().constEnd()) {
            out = Node();
            out.kind = Node::Kind::LargeOp;
            out.text = fn.value();
            out.named = true;
            return parseLimitsModifier(out);
        }

        if (name == QLatin1String("frac") || name == QLatin1String("dfrac")
            || name == QLatin1String("tfrac") || name == QLatin1String("binom")) {
            Node frac;
            frac.kind = Node::Kind::Frac;
            Node a, b;
            if (!enter() || !parseArg(a) || !parseArg(b)) return false;
            leave();
            frac.kids.push_back(std::move(a));
            frac.kids.push_back(std::move(b));
            if (name == QLatin1String("dfrac")) frac.styleOverride = 1;
            if (name == QLatin1String("tfrac")) frac.styleOverride = 0;
            if (name == QLatin1String("binom")) {
                frac.bar = false;
                Node wrap;
                wrap.kind = Node::Kind::Delimited;
                wrap.left = QStringLiteral("(");
                wrap.right = QStringLiteral(")");
                Node content;
                content.kind = Node::Kind::Row;
                content.kids.push_back(std::move(frac));
                wrap.kids.push_back(std::move(content));
                out = std::move(wrap);
            } else {
                out = std::move(frac);
            }
            return true;
        }

        if (name == QLatin1String("sqrt")) return parseSqrt(out);

        if (name == QLatin1String("left")) return parseLeftRight(out);
        if (name == QLatin1String("begin")) return parseEnvironment(out);

        static const QSet<QString> bigDelims = {
            QStringLiteral("big"), QStringLiteral("bigl"), QStringLiteral("bigr"), QStringLiteral("bigm"),
            QStringLiteral("Big"), QStringLiteral("Bigl"), QStringLiteral("Bigr"), QStringLiteral("Bigm"),
            QStringLiteral("bigg"), QStringLiteral("biggl"), QStringLiteral("biggr"), QStringLiteral("biggm"),
            QStringLiteral("Bigg"), QStringLiteral("Biggl"), QStringLiteral("Biggr"), QStringLiteral("Biggm")};
        if (bigDelims.contains(name)) {
            QString glyph;
            if (!parseDelimToken(glyph)) return false;
            const bool close = name.endsWith(QLatin1Char('r'));
            const bool mid = name.endsWith(QLatin1Char('m'));
            out = makeAtom(glyph, mid ? AtomClass::Rel : (close ? AtomClass::Close : AtomClass::Open),
                           FontStyle::Roman);
            return true;
        }

        // Text-mode runs.
        if (name == QLatin1String("text") || name == QLatin1String("mbox")
            || name == QLatin1String("textrm") || name == QLatin1String("textbf")
            || name == QLatin1String("textit")) {
            QString raw;
            if (!parseRawGroup(raw)) return false;
            // text mode: TeX's escaped specials lose their backslash
            static const QString specials = QStringLiteral("#$%&_{}");
            QString plain;
            plain.reserve(raw.size());
            for (int k = 0; k < raw.size(); ++k) {
                if (raw[k] == QLatin1Char('\\') && k + 1 < raw.size()
                    && specials.contains(raw[k + 1])) {
                    continue;
                }
                plain += raw[k];
            }
            out = Node();
            out.kind = Node::Kind::Text;
            out.text = plain;
            out.style = name == QLatin1String("textbf") ? FontStyle::Bold
                        : name == QLatin1String("textit") ? FontStyle::Italic
                                                          : FontStyle::Roman;
            return true;
        }

        if (name == QLatin1String("operatorname")) {
            QString raw;
            if (!parseRawGroup(raw)) return false;
            raw = raw.trimmed();
            if (raw.isEmpty()) return fail("empty operatorname");
            out = Node();
            out.kind = Node::Kind::LargeOp;
            out.text = raw;
            out.named = true;
            return parseLimitsModifier(out);
        }

        // Font commands.
        if (name == QLatin1String("mathrm") || name == QLatin1String("mathbf")
            || name == QLatin1String("mathit") || name == QLatin1String("boldsymbol")
            || name == QLatin1String("mathbb") || name == QLatin1String("mathcal")
            || name == QLatin1String("mathsf") || name == QLatin1String("mathtt")) {
            Node arg;
            if (!enter() || !parseArg(arg)) return false;
            leave();
            if (name == QLatin1String("mathbb")) mapAlphabet(arg, true);
            else if (name == QLatin1String("mathcal")) mapAlphabet(arg, false);
            else applyStyle(arg, name == QLatin1String("mathrm") || name == QLatin1String("mathsf")
                                         || name == QLatin1String("mathtt")
                                      ? FontStyle::Roman
                                  : name == QLatin1String("mathit") ? FontStyle::Italic
                                                                    : FontStyle::Bold);
            if (arg.kids.size() == 1) out = std::move(arg.kids.front());
            else out = std::move(arg);
            return true;
        }

        // Accents.
        static const QHash<QString, QString> accents = {
            {QStringLiteral("hat"), QStringLiteral("hat")}, {QStringLiteral("widehat"), QStringLiteral("hat")},
            {QStringLiteral("bar"), QStringLiteral("bar")}, {QStringLiteral("vec"), QStringLiteral("vec")},
            {QStringLiteral("dot"), QStringLiteral("dot")}, {QStringLiteral("ddot"), QStringLiteral("ddot")},
            {QStringLiteral("tilde"), QStringLiteral("tilde")}, {QStringLiteral("widetilde"), QStringLiteral("tilde")},
            {QStringLiteral("overline"), QStringLiteral("overline")},
            {QStringLiteral("overrightarrow"), QStringLiteral("overrightarrow")},
            {QStringLiteral("overleftarrow"), QStringLiteral("overleftarrow")},
            {QStringLiteral("underline"), QStringLiteral("underline")}};
        const auto acc = accents.constFind(name);
        if (acc != accents.constEnd()) {
            Node arg;
            if (!enter() || !parseArg(arg)) return false;
            leave();
            out = Node();
            out.kind = Node::Kind::Accent;
            out.text = acc.value();
            out.kids.push_back(std::move(arg));
            return true;
        }

        return failCmd("unsupported command \\", name);
    }

    static Node space(double em)
    {
        Node n;
        n.kind = Node::Kind::Space;
        n.em = em;
        return n;
    }

    bool parseLimitsModifier(Node &op)
    {
        skipWs();
        const QString m = peekCommand();
        if (m == QLatin1String("limits")) {
            op.limits = LimitsMode::Limits;
            pos_ += 7;
        } else if (m == QLatin1String("nolimits")) {
            op.limits = LimitsMode::NoLimits;
            pos_ += 9;
        }
        return true;
    }

    bool parseSqrt(Node &out)
    {
        Node index;
        bool hasIndex = false;
        skipWs();
        if (!atEnd() && s_[pos_] == QLatin1Char('[')) {
            ++pos_;
            int depth = 1;
            const int start = pos_;
            while (pos_ < s_.size()) {
                const QChar c = s_[pos_];
                if (c == QLatin1Char('[')) ++depth;
                else if (c == QLatin1Char(']') && --depth == 0) break;
                ++pos_;
            }
            if (pos_ >= s_.size()) return fail("unclosed [");
            Parser sub(s_.mid(start, pos_ - start), depth_ + 1);
            if (sub.depth_ > kMaxDepth) return fail("nesting too deep");
            if (!sub.parseTop(index)) {
                if (error.isEmpty()) error = sub.error;
                return false;
            }
            ++pos_; // ]
            hasIndex = true;
        }
        Node body;
        if (!enter() || !parseArg(body)) return false;
        leave();
        out = Node();
        out.kind = Node::Kind::Sqrt;
        out.kids.push_back(std::move(body));
        if (hasIndex) out.kids.push_back(std::move(index));
        return true;
    }

    bool parseLeftRight(Node &out)
    {
        QString left;
        if (!parseDelimToken(left)) return false;
        if (!enter()) return false;
        Node content;
        if (!parseRow(content)) return false;
        leave();
        if (peekCommand() != QLatin1String("right")) return fail("\\left without \\right");
        pos_ += 6; // \right
        QString right;
        if (!parseDelimToken(right)) return false;
        out = Node();
        out.kind = Node::Kind::Delimited;
        out.left = left;
        out.right = right;
        out.kids.push_back(std::move(content));
        return true;
    }

    bool parseEnvironment(Node &out)
    {
        QString env;
        if (!parseRawGroup(env)) return false;
        env = env.trimmed();
        if (!isMatrixEnv(env)) return failCmd("unsupported environment ", env);
        if (env == QLatin1String("array")) {
            QString spec;
            if (!parseRawGroup(spec)) return false;
        }
        if (!enter()) return false;

        std::vector<std::vector<Node>> rows(1);
        while (true) {
            Node cell;
            if (!parseRow(cell)) return false;
            rows.back().push_back(std::move(cell));
            skipWs();
            if (atEnd()) return fail("missing \\end");
            if (s_[pos_] == QLatin1Char('&')) {
                ++pos_;
                continue;
            }
            if (s_.mid(pos_).startsWith(QLatin1String("\\\\"))) {
                pos_ += 2;
                rows.emplace_back();
                continue;
            }
            if (peekCommand() == QLatin1String("end")) {
                pos_ += 4;
                QString endEnv;
                if (!parseRawGroup(endEnv)) return false;
                if (endEnv.trimmed() != env) return fail("mismatched \\end");
                break;
            }
            return fail("unexpected input in environment");
        }
        leave();

        // A trailing "\\" before \end leaves one empty row; drop it.
        if (rows.size() > 1 && rows.back().size() == 1 && rows.back().front().kids.empty())
            rows.pop_back();

        size_t cols = 0;
        for (const auto &r : rows) cols = std::max(cols, r.size());
        out = Node();
        out.kind = Node::Kind::Matrix;
        out.env = env;
        out.rows = int(rows.size());
        out.cols = int(cols);
        for (auto &r : rows) {
            for (size_t i = 0; i < cols; ++i) {
                if (i < r.size()) {
                    out.kids.push_back(std::move(r[i]));
                } else {
                    Node empty;
                    empty.kind = Node::Kind::Row;
                    out.kids.push_back(std::move(empty));
                }
            }
        }
        return true;
    }
};

} // namespace

ParseResult parse(const QString &source)
{
    ParseResult r;
    if (source.size() > kMaxSourceChars) {
        r.error = QStringLiteral("formula too long");
        return r;
    }
    if (source.trimmed().isEmpty()) {
        r.error = QStringLiteral("empty formula");
        return r;
    }
    Parser p{QStringView(source), 0};
    if (!p.parseTop(r.root)) {
        r.error = p.error.isEmpty() ? QStringLiteral("parse error") : p.error;
        r.root = Node();
        return r;
    }
    r.ok = true;
    return r;
}

} // namespace Math
