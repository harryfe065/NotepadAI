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

#include "DiffLanguages.h"

#include <algorithm>

namespace DiffLanguages {

namespace {

constexpr char kField = '\x1f';
constexpr char kItem = '\x1d';
constexpr char kRecord = '\x1e';

QStringList items(const QByteArray &field)
{
    QStringList out;
    if (field.isEmpty()) return out;
    for (const QByteArray &item : field.split(kItem)) out.append(QString::fromUtf8(item));
    return out;
}

bool hasLexer(const LanguageDef &def)
{
    return !def.lexer.isEmpty() && def.lexer != QLatin1String("null");
}

// Ranges for a Lua %-class, without brackets. Empty for a non-class letter.
QString classRanges(QChar c, bool *negated)
{
    *negated = c.isUpper();
    switch (c.toLower().toLatin1()) {
        case 'a': return QStringLiteral("A-Za-z");
        case 'd': return QStringLiteral("0-9");
        case 'l': return QStringLiteral("a-z");
        case 's': return QStringLiteral("\\t\\n\\v\\f\\r ");
        case 'u': return QStringLiteral("A-Z");
        case 'w': return QStringLiteral("A-Za-z0-9");
        case 'x': return QStringLiteral("0-9A-Fa-f");
        case 'p': return QStringLiteral("!-/:-@\\[-`{-~");
        case 'c': return QStringLiteral("\\x00-\\x1f\\x7f");
        default: return {};
    }
}

QString escapeInSet(QChar c)
{
    if (c == '\\' || c == ']' || c == '[' || c == '^' || c == '-') return QStringLiteral("\\") + c;
    return QString(c);
}

} // namespace

QRegularExpression regexFromLuaPattern(const QString &pattern)
{
    static const QRegularExpression kInvalid(QStringLiteral("("));
    QString re;
    re.reserve(pattern.size() * 2);
    const qsizetype n = pattern.size();
    bool haveItem = false;   // a quantifier may follow
    for (qsizetype i = 0; i < n; ++i) {
        const QChar c = pattern[i];
        if (c == '^' && i == 0) {
            re += QStringLiteral("\\A");
            continue;
        }
        if (c == '$' && i == n - 1) {
            re += QStringLiteral("\\z");
            continue;
        }
        if (c == '%') {
            if (++i >= n) return kInvalid;
            const QChar e = pattern[i];
            bool negated = false;
            const QString ranges = classRanges(e, &negated);
            if (!ranges.isEmpty()) {
                re += (negated ? QStringLiteral("[^") : QStringLiteral("[")) + ranges + ']';
            } else if (e.isLetterOrNumber()) {
                return kInvalid;   // %b, %f, back-references: not supported
            } else {
                re += QRegularExpression::escape(QString(e));
            }
            haveItem = true;
            continue;
        }
        if (c == '[') {
            QString set = QStringLiteral("[");
            qsizetype j = i + 1;
            if (j < n && pattern[j] == '^') {
                set += '^';
                ++j;
            }
            bool first = true;
            for (; j < n && (first || pattern[j] != ']'); ++j, first = false) {
                const QChar s = pattern[j];
                if (s == '%' && j + 1 < n) {
                    bool negated = false;
                    const QString ranges = classRanges(pattern[++j], &negated);
                    if (negated) return kInvalid;
                    set += ranges.isEmpty() ? escapeInSet(pattern[j]) : ranges;
                } else if (s == '-' && !first && j + 1 < n && pattern[j + 1] != ']') {
                    set += '-';
                } else {
                    set += escapeInSet(s);
                }
            }
            if (j >= n) return kInvalid;
            re += set + ']';
            i = j;
            haveItem = true;
            continue;
        }
        if ((c == '*' || c == '+' || c == '?' || c == '-') && haveItem) {
            re += c == '-' ? QStringLiteral("*?") : QString(c);
            haveItem = false;
            continue;
        }
        if (c == '.') {
            re += '.';
            haveItem = true;
            continue;
        }
        if (c == '(' || c == ')') {
            re += c;
            haveItem = false;
            continue;
        }
        re += QRegularExpression::escape(QString(c));
        haveItem = true;
    }
    QRegularExpression out(re);
    return out.isValid() ? out : kInvalid;
}

Catalog::Catalog(QVector<LanguageDef> languages)
    : m_languages(std::move(languages))
{
    for (int i = 0; i < m_languages.size(); ++i) {
        const LanguageDef &d = m_languages[i];
        for (const QString &f : d.filenames)
            if (!m_byFilename.contains(f)) m_byFilename.insert(f, i);
        for (const QString &e : d.extensions)
            if (!m_byExtension.contains(e)) m_byExtension.insert(e, i);
        for (const QString &p : d.patterns) {
            QRegularExpression re = regexFromLuaPattern(p);
            if (re.isValid()) {
                re.optimize();
                m_patterns.append({std::move(re), i});
            }
        }
    }
}

std::shared_ptr<const Catalog> Catalog::fromSerialized(const QByteArray &data)
{
    QVector<LanguageDef> defs;
    for (const QByteArray &record : data.split(kRecord)) {
        const QList<QByteArray> f = record.split(kField);
        if (f.size() < 8 || f[0].isEmpty()) continue;
        LanguageDef d;
        d.name = QString::fromUtf8(f[0]);
        d.lexer = QString::fromUtf8(f[1]);
        d.filenames = items(f[2]);
        d.patterns = items(f[3]);
        d.extensions = items(f[4]);
        if (!f[5].isEmpty()) {
            for (const QByteArray &s : f[5].split(kItem)) {
                const QList<QByteArray> parts = s.split(',');
                if (parts.size() < 3) continue;
                bool ok = false;
                const int id = parts[0].toInt(&ok);
                if (!ok || id < 0 || id > 255) continue;
                const quint32 bgr = parts[1].toUInt();
                d.styles.append(StyleDef{static_cast<quint8>(id),
                                         ((bgr & 0xFF) << 16) | (bgr & 0xFF00) | ((bgr >> 16) & 0xFF),
                                         static_cast<quint8>(parts[2].toUInt())});
            }
        }
        if (!f[6].isEmpty()) {
            for (const QByteArray &k : f[6].split(kItem)) {
                const qsizetype eq = k.indexOf('=');
                if (eq <= 0) continue;
                bool ok = false;
                const int set = k.left(eq).toInt(&ok);
                if (!ok || set < 0 || set > 8) continue;
                if (d.setup.keywords.size() <= set) d.setup.keywords.resize(set + 1);
                d.setup.keywords[set] = k.mid(eq + 1);
            }
        }
        if (!f[7].isEmpty()) {
            for (const QByteArray &p : f[7].split(kItem)) {
                const qsizetype eq = p.indexOf('=');
                if (eq > 0) d.setup.properties.append({p.left(eq), p.mid(eq + 1)});
            }
        }
        defs.append(std::move(d));
    }
    return std::make_shared<const Catalog>(std::move(defs));
}

int Catalog::detect(QByteArrayView path) const
{
    if (path.isEmpty()) return -1;
    const QString name = QString::fromUtf8(path.sliced(path.lastIndexOf('/') + 1));
    int found = m_byFilename.value(name, -1);
    if (found < 0) {
        for (const auto &[re, index] : m_patterns) {
            if (re.match(name).hasMatch()) {
                found = index;
                break;
            }
        }
    }
    if (found < 0) {
        const qsizetype dot = name.lastIndexOf('.');
        const QString ext = dot >= 0 && dot < name.size() - 1 ? name.mid(dot + 1) : QString();
        found = m_byExtension.value(ext, -1);
    }
    return found >= 0 && hasLexer(m_languages[found]) ? found : -1;
}

int compact(const LanguageDef &def, int base, DiffDoc::Language &out)
{
    out.lexer = def.lexer;
    out.setup = def.setup;
    out.styleSlots.fill(0);
    out.slotStyles.clear();

    StyleDef defaultStyle;
    for (const StyleDef &s : def.styles)
        if (s.id == 0) defaultStyle = s;

    QVector<StyleDef> kept;
    for (const StyleDef &s : def.styles) {
        if (s.id == 0 || s.id >= 128 || (s.id >= 32 && s.id <= 39)) continue;
        if ((s.fg & 0xFFFFFF) == (defaultStyle.fg & 0xFFFFFF) && s.fontStyle == defaultStyle.fontStyle)
            continue;
        if (std::any_of(kept.cbegin(), kept.cend(), [&](const StyleDef &k) { return k.id == s.id; }))
            continue;
        kept.append(s);
    }
    std::sort(kept.begin(), kept.end(), [](const StyleDef &a, const StyleDef &b) { return a.id < b.id; });
    const int used = static_cast<int>(kept.size());
    if (base + used - 1 > 255) {
        out.styleSlots.fill(0);
        return -1;
    }
    out.slotStyles.reserve(used);
    for (int k = 0; k < used; ++k) {
        const auto slot = static_cast<quint8>(base + k);
        out.styleSlots[kept[k].id] = slot;
        out.slotStyles.append(DiffDoc::SlotStyle{slot, kept[k].fg, kept[k].fontStyle});
    }
    // LexCPP marks inactive (preprocessor-disabled) code by OR-ing 0x40 into
    // the style; show it like the active style unless the language says
    // otherwise.
    if (def.lexer.startsWith(QLatin1String("cpp"))) {
        for (int id = 0x40; id < 0x80; ++id)
            if (out.styleSlots[id] == 0) out.styleSlots[id] = out.styleSlots[id - 0x40];
    }
    return used;
}

Assignment assign(const Catalog &catalog, const GitDiffParser::Result &parsed)
{
    Assignment a;
    const qsizetype fileCount = parsed.files.size();
    a.oldLanguage.resize(fileCount);
    a.newLanguage.resize(fileCount);
    QHash<int, qint16> byCatalog;   // catalog index -> assignment index (-1: did not fit)
    int next = DiffDoc::kFirstLexerSlot;
    auto languageFor = [&](const QByteArray &path) -> qint16 {
        const int c = catalog.detect(path);
        if (c < 0) return -1;
        const auto it = byCatalog.constFind(c);
        if (it != byCatalog.cend()) return it.value();
        DiffDoc::Language lang;
        const int used = compact(catalog.languages()[c], next, lang);
        qint16 index = -1;
        if (used >= 0) {
            next += used;
            index = static_cast<qint16>(a.languages.size());
            a.languages.append(std::move(lang));
        }
        byCatalog.insert(c, index);
        return index;
    };
    for (qsizetype i = 0; i < fileCount; ++i) {
        const auto &f = parsed.files[i];
        a.oldLanguage[i] = languageFor(f.oldPath);
        a.newLanguage[i] = f.newPath == f.oldPath ? a.oldLanguage[i] : languageFor(f.newPath);
    }
    return a;
}

} // namespace DiffLanguages
