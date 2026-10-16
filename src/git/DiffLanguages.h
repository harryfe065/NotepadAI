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

#ifndef DIFF_LANGUAGES_H
#define DIFF_LANGUAGES_H

#include "DiffDocument.h"

#include <QHash>
#include <QRegularExpression>
#include <QStringList>

#include <memory>

// Language detection and lexer style slots for diff views, off the UI thread.
//
// The languages live in Lua (src/languages/*.lua). DiffLanguagesLua takes one
// snapshot of them on the UI thread (Catalog::fromSerialized); after that,
// detecting a file's language and assigning style slots is pure and runs in
// the pool job that builds the document.
namespace DiffLanguages {

struct StyleDef {
    quint8 id = 0;
    quint32 fg = 0;         // 0xRRGGBB, as authored for a light background
    quint8 fontStyle = 0;   // 1 bold, 2 italic, 4 underline
};

struct LanguageDef {
    QString     name;
    QString     lexer;
    QStringList filenames;
    QStringList patterns;   // Lua patterns on the file name
    QStringList extensions;
    QVector<StyleDef> styles;
    GitDiffSyntaxMapper::LexerSetup setup;
};

class Catalog {
public:
    explicit Catalog(QVector<LanguageDef> languages);

    // Parses the snapshot produced by the Lua script in DiffLanguagesLua.
    // Fields: \x1f, list items: \x1d, languages: \x1e.
    static std::shared_ptr<const Catalog> fromSerialized(const QByteArray &data);

    // Index of the language for `path` (exact file name, then name pattern,
    // then extension, as detectLanguageFromExtension does), or -1 when it has
    // no lexer to run.
    int detect(QByteArrayView path) const;

    const QVector<LanguageDef> &languages() const { return m_languages; }

private:
    QVector<LanguageDef> m_languages;
    QHash<QString, int>  m_byFilename;
    QHash<QString, int>  m_byExtension;
    QVector<std::pair<QRegularExpression, int>> m_patterns;
};

// Lua pattern -> regular expression (the subset language files use:
// anchors, %-classes and escapes, sets, . * + - ?). Invalid on failure.
QRegularExpression regexFromLuaPattern(const QString &pattern);

// Fills `out` with `def`'s style slots starting at `base`: one slot per
// style the language defines, except the default style (0), Scintilla's
// predefined ids (32-39) and styles that look like the default. LexCPP's
// inactive styles (id | 0x40) share their active style's slot. Returns the
// number of slots used, or -1 when they do not fit below 256.
int compact(const LanguageDef &def, int base, DiffDoc::Language &out);

struct Assignment {
    QVector<DiffDoc::Language> languages;
    QVector<qint16> oldLanguage;   // per file
    QVector<qint16> newLanguage;
};

// Detects each file's old- and new-side language and hands out contiguous
// slot ranges from 40 in file order. A language that does not fit gets no
// entry (its files render without syntax colours).
Assignment assign(const Catalog &catalog, const GitDiffParser::Result &parsed);

} // namespace DiffLanguages

#endif // DIFF_LANGUAGES_H
