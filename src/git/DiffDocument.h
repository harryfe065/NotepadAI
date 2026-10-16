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

#ifndef DIFF_DOCUMENT_H
#define DIFF_DOCUMENT_H

#include "DiffFileClassifier.h"
#include "GitDiffParser.h"
#include "GitDiffSyntaxMapper.h"

#include <QByteArray>
#include <QString>
#include <QVector>

#include <array>
#include <cstdint>
#include <memory>

struct GitCommitDetail;

// The finished contents of a diff editor buffer, built on the pool by
// DiffDocumentBuilder and applied by DiffView on the UI thread. Plain data:
// immutable once published as shared_ptr<const Document>.
namespace DiffDoc {

// Fixed Scintilla style ids (0-31). 32-39 are Scintilla's predefined styles;
// lexer styles are remapped into slots 40-255 (DiffLanguages::assign).
enum class Style : std::uint8_t {
    Default = 0,
    Context,
    Added,
    Deleted,
    NoNewline,
    HunkRange,
    HunkContext,
    Chevron,
    StatusAdded,
    StatusModified,
    StatusDeleted,
    StatusRenamed,
    Dir,
    Name,
    NameStruck,
    Badge,
    Plus,
    Minus,
    SqAdd,
    SqDel,
    SqNeutral,
    Placeholder,
    Link,
    CardTitle,
    CardBody,
    CardMeta,
    HeaderPlain,
    // Line-number margin text, set by DiffView per row kind.
    MarginContext,
    MarginAdded,
    MarginDeleted,
    MarginHunk,
    MarginHeader,
    Count
};
static_assert(static_cast<int>(Style::Count) <= 32);

inline constexpr int kFirstLexerSlot = 40;

// What a buffer row is; DiffView derives markers and margin tints from it.
enum class RowKind : std::uint8_t {
    Blank,
    Separator,      // blank row carrying the rule above the next file header
    CardTitle,
    CardBody,
    CardMeta,
    FileHeader,
    Placeholder,    // collapsed / no-content message under a file header
    HunkHeader,
    Context,
    Added,
    Deleted,
    NoNewline,
};

enum class LinkKind : std::uint8_t {
    Chevron,        // toggles the file
    Path,           // opens the file
    LoadDiff,       // expands the file
    CopySha,        // copies the commit sha
    Parent,         // opens the parent commit (payload = sha)
    Url,            // opens payload externally (http/https only)
};

// Byte ranges are relative to the start of the Rows they belong to.
struct Link {
    qint32     pos = 0;
    qint32     len = 0;
    LinkKind   kind = LinkKind::Url;
    qint32     file = -1;
    QByteArray payload;
};

struct Range {
    qint32 pos = 0;
    qint32 len = 0;
};

struct Tip {
    qint32  pos = 0;
    qint32  len = 0;
    QString text;
};

// A run of whole rows, each ending in '\n'. A file's body is built as one
// Rows (inline or as a fragment for Load diff).
struct Rows {
    QByteArray        text;
    QByteArray        styles;       // one style byte per text byte
    QVector<RowKind>  kinds;
    QVector<qint32>   oldLn;        // -1 = blank
    QVector<qint32>   newLn;
    QVector<qint32>   hunkRows;     // rows of hunk headers, ascending
    QVector<Link>     links;
    QVector<Range>    addWords;     // ascending
    QVector<Range>    delWords;     // ascending

    qint32 rowCount() const { return static_cast<qint32>(kinds.size()); }
};

// The header row's chevron: "▾" expanded, "▸" collapsed. Same byte length,
// so DiffView swaps it in place.
inline constexpr char kChevronOpen[] = "\xE2\x96\xBE";
inline constexpr char kChevronClosed[] = "\xE2\x96\xB8";
inline constexpr int kChevronBytes = 3;

struct File {
    qint32     headerRow = 0;       // row in the document
    qint32     bodyRows = 0;        // rows under the header (body or placeholder)
    DiffFileClassifier::Reason reason = DiffFileClassifier::Reason::None;
    bool       collapsed = false;
    bool       expandable = false;  // has hunks that can be shown
    char       status = 'M';
    qint32     added = 0;
    qint32     deleted = 0;
    QByteArray path;                // new path, or old path for a deletion
    QByteArray oldPath;             // set for renames / copies
    QByteArray headerText;          // header row without '\n'
    QByteArray headerStyles;
};

// The look of one style slot, as the language authored it for a light
// background (DiffView applies the dark transform).
struct SlotStyle {
    quint8 slot = 0;
    quint32 fg = 0;         // 0xRRGGBB
    quint8 fontStyle = 0;   // 1 bold, 2 italic, 4 underline
};

// Per-language lexer → style slot table (DiffLanguages::assign).
// styleSlots[lexerStyle] is the Scintilla style id to write, 0 = no syntax
// colour (the row's base style).
struct Language {
    QString                    lexer;
    GitDiffSyntaxMapper::LexerSetup setup;   // keywords + properties
    std::array<quint8, 128>    styleSlots{};
    QVector<SlotStyle>         slotStyles;
};

struct Input {
    std::shared_ptr<const GitDiffParser::Result> parsed;
    QVector<DiffFileClassifier::Reason> reasons;   // per file; empty = all None
    QVector<bool>    collapsed;                    // per file; empty = from reasons
    QVector<qint16>  oldLanguage;                  // per file, index into languages, -1 none
    QVector<qint16>  newLanguage;
    QVector<Language> languages;
    std::shared_ptr<const GitCommitDetail> commit; // the card, commit view only
    QByteArray       issueUrlBase;                 // "<web>/issues/" or empty: #N stays text
    qint64           nowSecs = 0;                  // for relative times
};

struct Document : Rows {
    QVector<File>  files;
    QVector<Range> strikes;          // deleted file names
    QVector<Tip>   tips;             // dwell tooltips (card time)
    qint32         maxOldLn = 0;     // over every file, collapsed ones included
    qint32         maxNewLn = 0;
    std::shared_ptr<const Input> input;   // for fragment builds
};

} // namespace DiffDoc

#endif // DIFF_DOCUMENT_H
