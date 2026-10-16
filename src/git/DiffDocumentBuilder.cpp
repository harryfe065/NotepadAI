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

#include "DiffDocumentBuilder.h"

#include "GitCommitDetail.h"
#include "GitDiffSyntaxMapper.h"
#include "GitRelativeTime.h"

#include <QByteArrayView>
#include <QCoreApplication>

#include <algorithm>

using namespace DiffDoc;
using Reason = DiffFileClassifier::Reason;
using LineKind = GitDiffParser::LineKind;

namespace {

constexpr QByteArrayView kMinusSign("\xE2\x88\x92");   // −
constexpr QByteArrayView kSquare("\xE2\x96\xA0");      // ■
constexpr QByteArrayView kArrow(" \xE2\x86\x92 ");     // " → "
constexpr QByteArrayView kDot(" \xC2\xB7 ");           // " · "

QByteArray tr(const char *text)
{
    return QCoreApplication::translate("DiffDocumentBuilder", text).toUtf8();
}

// Appends styled bytes and whole rows to a Rows. Positions recorded in links
// and ranges are offsets into `rows.text`.
struct Writer {
    Rows &rows;

    qint32 pos() const { return static_cast<qint32>(rows.text.size()); }
    qint32 row() const { return rows.rowCount(); }

    void put(QByteArrayView s, Style style)
    {
        rows.text.append(s);
        rows.styles.append(s.size(), static_cast<char>(style));
    }

    void link(QByteArrayView s, Style style, LinkKind kind, qint32 file, QByteArray payload = {})
    {
        rows.links.append(Link{pos(), static_cast<qint32>(s.size()), kind, file, std::move(payload)});
        put(s, style);
    }

    void endRow(RowKind kind, Style style = Style::Default, qint32 oldLn = -1, qint32 newLn = -1)
    {
        rows.text.append('\n');
        rows.styles.append(static_cast<char>(style));
        rows.kinds.append(kind);
        rows.oldLn.append(oldLn);
        rows.newLn.append(newLn);
    }

    void squares(qint64 added, qint64 deleted)
    {
        const auto s = DiffDocumentBuilder::squares(added, deleted);
        for (int i = 0; i < s.green; ++i) put(kSquare, Style::SqAdd);
        for (int i = 0; i < s.red; ++i) put(kSquare, Style::SqDel);
        for (int i = 0; i < s.grey; ++i) put(kSquare, Style::SqNeutral);
    }

    void counts(qint64 added, qint64 deleted)
    {
        put(QByteArray('+' + QByteArray::number(added)), Style::Plus);
        put(" ", Style::HeaderPlain);
        put(kMinusSign.toByteArray() + QByteArray::number(deleted), Style::Minus);
        put(" ", Style::HeaderPlain);
        squares(added, deleted);
    }
};

QByteArrayView dirOf(QByteArrayView path)
{
    return path.first(path.lastIndexOf('/') + 1);
}

Style statusStyle(char status)
{
    switch (status) {
        case 'A': return Style::StatusAdded;
        case 'D': return Style::StatusDeleted;
        case 'R':
        case 'C': return Style::StatusRenamed;
        default: return Style::StatusModified;
    }
}

// Per-file facts the builder needs beyond the FileSpan.
struct FileFacts {
    qint32 hunks = 0;
};

QVector<FileFacts> factsFor(const GitDiffParser::Result &p)
{
    QVector<FileFacts> facts(p.files.size());
    for (const auto &h : p.hunks)
        if (h.fileIndex >= 0 && h.fileIndex < facts.size()) ++facts[h.fileIndex].hunks;
    return facts;
}

// One file's facts: hunks are ordered by file, so two binary searches.
FileFacts factsOf(const GitDiffParser::Result &p, qint32 fileIndex)
{
    auto byFile = [](const GitDiffParser::HunkSpan &h, qint32 i) { return h.fileIndex < i; };
    const auto first = std::lower_bound(p.hunks.begin(), p.hunks.end(), fileIndex, byFile);
    const auto last = std::lower_bound(first, p.hunks.end(), fileIndex + 1, byFile);
    return FileFacts{static_cast<qint32>(last - first)};
}

bool isExpandable(const GitDiffParser::FileSpan &f, const FileFacts &facts)
{
    return !f.binary && facts.hunks > 0;
}

Reason reasonOf(const Input &in, qint32 i)
{
    return i < in.reasons.size() ? in.reasons[i] : Reason::None;
}

bool startsCollapsed(const Input &in, qint32 i, bool expandable)
{
    if (!expandable) return false;
    if (i < in.collapsed.size()) return in.collapsed[i];
    return DiffFileClassifier::startsCollapsed(reasonOf(in, i), in.parsed->files.size());
}

const Language *languageAt(const Input &in, const QVector<qint16> &table, qint32 i)
{
    const qint16 index = i < table.size() ? table[i] : qint16(-1);
    return index >= 0 && index < in.languages.size() ? &in.languages[index] : nullptr;
}

// Message shown in place of a body that cannot be expanded.
QByteArray staticMessage(const GitDiffParser::FileSpan &f, Reason reason)
{
    if (f.binary) return tr("Binary file not shown.");
    if (reason == Reason::RenamedOnly) return tr("File renamed without changes.");
    if (reason == Reason::ModeOnly) return tr("File mode changed.");
    if (f.status == 'A' || f.status == 'D') return tr("Empty file.");
    return tr("No content changes.");
}

QByteArray collapsedMessage(Reason reason)
{
    switch (reason) {
        case Reason::Lockfile: return tr("Lockfile not shown.");
        case Reason::Generated: return tr("Generated file not shown.");
        case Reason::Deleted: return tr("This file was deleted.");
        case Reason::Large: return tr("Large diff not shown.");
        default: return tr("Diff collapsed.");
    }
}

void writeMessageRow(Writer &w, const QByteArray &message)
{
    w.put("  ", Style::Placeholder);
    w.put(message, Style::Placeholder);
    w.endRow(RowKind::Placeholder, Style::Placeholder);
}

void writePlaceholder(Writer &w, Reason reason, qint32 fileIndex)
{
    w.put("  ", Style::Placeholder);
    w.put(collapsedMessage(reason), Style::Placeholder);
    w.put(" ", Style::Placeholder);
    w.link(tr("Load diff"), Style::Link, LinkKind::LoadDiff, fileIndex);
    w.endRow(RowKind::Placeholder, Style::Placeholder);
}

void writeHeader(Writer &w, const GitDiffParser::FileSpan &f, qint32 fileIndex, Reason reason,
                 bool collapsed, bool expandable, QVector<Range> &strikes)
{
    // Nothing to fold (binary, rename or mode only): a plain muted glyph, not a link.
    if (expandable)
        w.link(collapsed ? kChevronClosed : kChevronOpen, Style::Chevron, LinkKind::Chevron, fileIndex);
    else
        w.put(kChevronOpen, Style::Chevron);
    w.put(" ", Style::HeaderPlain);
    w.put(QByteArrayView(&f.status, 1), statusStyle(f.status));
    w.put("  ", Style::HeaderPlain);

    const qint32 pathStart = w.pos();
    const QByteArray &path = f.path();
    const bool renamed = (f.status == 'R' || f.status == 'C') && !f.oldPath.isEmpty()
                         && f.oldPath != f.newPath;
    if (renamed) {
        const QByteArrayView oldDir = dirOf(f.oldPath);
        const QByteArrayView newDir = dirOf(f.newPath);
        if (oldDir == newDir) {
            w.put(oldDir, Style::Dir);
            w.put(QByteArrayView(f.oldPath).sliced(oldDir.size()), Style::Dir);
            w.put(kArrow, Style::HeaderPlain);
        } else {
            w.put(f.oldPath, Style::Dir);
            w.put(kArrow, Style::HeaderPlain);
            w.put(newDir, Style::Dir);
        }
        w.put(QByteArrayView(f.newPath).sliced(newDir.size()), Style::Name);
    } else {
        const QByteArrayView dir = dirOf(path);
        w.put(dir, Style::Dir);
        const bool struck = f.status == 'D';
        w.put(QByteArrayView(path).sliced(dir.size()), struck ? Style::NameStruck : Style::Name);
        if (struck) strikes.append(Range{pathStart, w.pos() - pathStart});
    }
    w.rows.links.append(Link{pathStart, w.pos() - pathStart, LinkKind::Path, fileIndex, path});

    const bool noBody = f.binary || reason == Reason::RenamedOnly || reason == Reason::ModeOnly;
    if (!noBody) {
        w.put("  ", Style::HeaderPlain);
        w.counts(f.added, f.deleted);
    }
    if (f.oldMode && f.newMode && f.oldMode != f.newMode && !f.submodule) {
        w.put("  ", Style::HeaderPlain);
        w.put(QByteArray::number(f.oldMode, 8) + kArrow.toByteArray() + QByteArray::number(f.newMode, 8),
              Style::Badge);
    }
    if (f.binary) {
        w.put("  ", Style::HeaderPlain);
        w.put(tr("binary"), Style::Badge);
    }
    if (f.submodule) {
        w.put("  ", Style::HeaderPlain);
        w.put(tr("submodule"), Style::Badge);
    }
}

Style baseStyle(LineKind kind)
{
    switch (kind) {
        case LineKind::Added: return Style::Added;
        case LineKind::Deleted: return Style::Deleted;
        case LineKind::NoNewline: return Style::NoNewline;
        default: return Style::Context;
    }
}

// Writes the hunks of file `i` into `w`: hunk headers, body rows with their
// +/-/space prefix, lexer styles through the language slot tables, and word
// spans as ranges.
void writeHunks(Writer &w, const Input &in, qint32 i)
{
    const GitDiffParser::Result &p = *in.parsed;
    const GitDiffParser::FileSpan &f = p.files[i];
    const qint32 first = f.firstRow;
    const qint32 count = f.endRow - f.firstRow;

    GitDiffParser::Result sub;
    sub.texts = p.texts.mid(first, count);
    sub.kinds = p.kinds.mid(first, count);
    const Language *oldLang = languageAt(in, in.oldLanguage, i);
    const Language *newLang = languageAt(in, in.newLanguage, i);
    GitDiffSyntaxMapper::Input mi;
    mi.oldLexerName = oldLang ? oldLang->lexer : QString();
    mi.newLexerName = newLang ? newLang->lexer : QString();
    mi.oldSetup = oldLang ? &oldLang->setup : nullptr;
    mi.newSetup = newLang ? &newLang->setup : nullptr;
    mi.parsed = &sub;
    static thread_local GitDiffSyntaxMapper::State state;
    const GitDiffSyntaxMapper::Overlay ov = GitDiffSyntaxMapper::map(mi, state);

    QVector<qint32> contentPos(count, -1);
    for (qint32 k = 0; k < count; ++k) {
        const qint32 r = first + k;
        const LineKind kind = p.kinds[r];
        const QByteArray &text = p.texts[r];
        switch (kind) {
            case LineKind::HunkHeader: {
                w.rows.hunkRows.append(w.row());
                const qsizetype close = text.indexOf("@@", 2);
                const qsizetype split = close < 0 ? text.size() : close + 2;
                w.put(QByteArrayView(text).first(split), Style::HunkRange);
                w.put(QByteArrayView(text).sliced(split), Style::HunkContext);
                w.endRow(RowKind::HunkHeader, Style::HunkContext);
                break;
            }
            case LineKind::Context:
            case LineKind::Added:
            case LineKind::Deleted: {
                const Style base = baseStyle(kind);
                const char prefix = kind == LineKind::Added ? '+' : kind == LineKind::Deleted ? '-' : ' ';
                w.rows.text.append(prefix);
                w.rows.styles.append(static_cast<char>(base));
                contentPos[k] = w.pos();
                w.rows.text.append(text);

                const Language *lang = kind == LineKind::Deleted ? oldLang : newLang;
                const QByteArray *lexed = ov.ok && lang && k < ov.styleBytesPerRow.size()
                                              ? &ov.styleBytesPerRow[k] : nullptr;
                if (lexed && lexed->size() == text.size()) {
                    const qsizetype at = w.rows.styles.size();
                    w.rows.styles.resize(at + text.size());
                    char *dst = w.rows.styles.data() + at;
                    const char *src = lexed->constData();
                    for (qsizetype b = 0; b < text.size(); ++b) {
                        const quint8 slot = lang->styleSlots[(static_cast<quint8>(src[b])
                                                         - kFirstLexerSlot) & 0x7F];
                        dst[b] = static_cast<char>(slot ? slot : static_cast<quint8>(base));
                    }
                } else {
                    w.rows.styles.append(text.size(), static_cast<char>(base));
                }
                const RowKind rk = kind == LineKind::Added ? RowKind::Added
                                   : kind == LineKind::Deleted ? RowKind::Deleted : RowKind::Context;
                w.endRow(rk, base, p.oldLn[r], p.newLn[r]);
                break;
            }
            case LineKind::NoNewline:
                w.put(text, Style::NoNewline);
                w.endRow(RowKind::NoNewline, Style::NoNewline);
                break;
            case LineKind::FileHeader:
            case LineKind::Empty:
                break;   // replaced by the file header row
        }
    }

    auto addSpans = [&](const QVector<GitDiffSyntaxMapper::WordSpan> &spans, QVector<Range> &out) {
        const qsizetype before = out.size();
        for (const auto &s : spans) {
            if (s.row < 0 || s.row >= count || contentPos[s.row] < 0 || s.colEnd <= s.colStart) continue;
            out.append(Range{contentPos[s.row] + s.colStart, s.colEnd - s.colStart});
        }
        auto byPos = [](const Range &a, const Range &b) { return a.pos < b.pos; };
        if (!std::is_sorted(out.begin() + before, out.end(), byPos))
            std::sort(out.begin() + before, out.end(), byPos);
    };
    addSpans(ov.addWordSpans, w.rows.addWords);
    addSpans(ov.delWordSpans, w.rows.delWords);
}

void writeBody(Writer &w, const Input &in, qint32 i, const FileFacts &facts)
{
    const GitDiffParser::FileSpan &f = in.parsed->files[i];
    if (isExpandable(f, facts))
        writeHunks(w, in, i);
    else
        writeMessageRow(w, staticMessage(f, reasonOf(in, i)));
}

bool isUrlTerminator(char c)
{
    return c == ' ' || c == '\t' || c == '<' || c == '>' || c == '"' || c == '\'';
}

bool isTrailingPunct(char c)
{
    return c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?' || c == ')'
           || c == ']' || c == '}';
}

bool isAlnum(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

// One commit-message line with http(s) URLs and, for a known forge, #N
// references as links.
void writeBodyLine(Writer &w, QByteArrayView line, const QByteArray &issueUrlBase)
{
    qsizetype plain = 0;
    qsizetype k = 0;
    const qsizetype n = line.size();
    auto flushPlain = [&](qsizetype upto) {
        if (upto > plain) w.put(line.sliced(plain, upto - plain), Style::CardBody);
    };
    while (k < n) {
        const QByteArrayView rest = line.sliced(k);
        const bool boundary = k == 0 || !isAlnum(line[k - 1]);
        if (boundary && (rest.startsWith("https://") || rest.startsWith("http://"))) {
            qsizetype end = k;
            while (end < n && !isUrlTerminator(line[end])) ++end;
            while (end > k && isTrailingPunct(line[end - 1])) --end;
            flushPlain(k);
            const QByteArrayView url = line.sliced(k, end - k);
            w.link(url, Style::Link, LinkKind::Url, -1, url.toByteArray());
            plain = k = end;
            continue;
        }
        if (line[k] == '#' && !issueUrlBase.isEmpty() && boundary && k + 1 < n
            && line[k + 1] >= '0' && line[k + 1] <= '9') {
            qsizetype end = k + 1;
            while (end < n && line[end] >= '0' && line[end] <= '9') ++end;
            if (end == n || !isAlnum(line[end])) {
                flushPlain(k);
                const QByteArrayView ref = line.sliced(k, end - k);
                w.link(ref, Style::Link, LinkKind::Url, -1, issueUrlBase + ref.sliced(1).toByteArray());
                plain = k = end;
                continue;
            }
        }
        ++k;
    }
    flushPlain(n);
    w.endRow(RowKind::CardBody, Style::CardBody);
}

// The message without its subject paragraph, trailing blank lines dropped.
QByteArrayView messageBody(const GitCommitDetail &c)
{
    QByteArrayView body(c.body);
    const qsizetype blank = body.indexOf("\n\n");
    body = blank < 0 ? QByteArrayView() : body.sliced(blank + 2);
    while (!body.isEmpty() && (body.back() == '\n' || body.back() == '\r' || body.back() == ' '))
        body.chop(1);
    while (body.startsWith('\n')) body = body.sliced(1);
    return body;
}

void writeCard(Writer &w, Document &doc, const Input &in)
{
    const GitCommitDetail &c = *in.commit;
    w.put(c.subject, Style::CardTitle);
    w.endRow(RowKind::CardTitle, Style::CardTitle);

    const QByteArrayView body = messageBody(c);
    if (!body.isEmpty()) {
        w.endRow(RowKind::Blank);
        qsizetype start = 0;
        while (start <= body.size()) {
            qsizetype nl = body.indexOf('\n', start);
            if (nl < 0) nl = body.size();
            QByteArrayView line = body.sliced(start, nl - start);
            if (line.endsWith('\r')) line.chop(1);
            writeBodyLine(w, line, in.issueUrlBase);
            start = nl + 1;
        }
    }
    w.endRow(RowKind::Blank);

    w.put(c.authorName, Style::CardMeta);
    w.put(kDot, Style::CardMeta);
    w.put(tr("authored "), Style::CardMeta);
    const QByteArray when = GitRelativeTime::format(c.authorTime, in.nowSecs).toUtf8();
    doc.tips.append(Tip{w.pos(), static_cast<qint32>(when.size()), GitRelativeTime::localTime(c.authorTime)});
    w.put(when, Style::CardMeta);
    if (c.committerName != c.authorName || c.committerEmail != c.authorEmail) {
        w.put(kDot, Style::CardMeta);
        w.put(tr("committed by "), Style::CardMeta);
        w.put(c.committerName, Style::CardMeta);
    }
    w.endRow(RowKind::CardMeta, Style::CardMeta);

    w.put("commit ", Style::CardMeta);
    w.link(c.sha.left(7), Style::Link, LinkKind::CopySha, -1, c.sha);
    w.put(" ", Style::CardMeta);
    w.link(tr("Copy"), Style::Link, LinkKind::CopySha, -1, c.sha);
    for (const QByteArray &parent : c.parents.split(' ')) {
        if (parent.isEmpty()) continue;
        w.put(kDot, Style::CardMeta);
        w.put(tr("parent "), Style::CardMeta);
        w.link(parent.left(7), Style::Link, LinkKind::Parent, -1, parent);
    }
    w.endRow(RowKind::CardMeta, Style::CardMeta);

    const GitDiffParser::Result &p = *in.parsed;
    if (!p.files.isEmpty()) {
        const qsizetype n = p.files.size();
        w.put(n == 1 ? tr("1 file changed")
                     : QCoreApplication::translate("DiffDocumentBuilder", "%1 files changed").arg(n).toUtf8(),
              Style::CardMeta);
        w.put("  ", Style::CardMeta);
        w.counts(p.addedCount, p.deletedCount);
        w.endRow(RowKind::CardMeta, Style::CardMeta);
    }
    if (c.truncated) {
        w.put(tr("Diff truncated: the commit is larger than the 5 MB display limit"), Style::CardMeta);
        w.endRow(RowKind::CardMeta, Style::CardMeta);
    }
}

qsizetype estimateBytes(const Input &in, const QVector<FileFacts> &facts)
{
    const GitDiffParser::Result &p = *in.parsed;
    qsizetype bytes = 256;
    if (in.commit) bytes += in.commit->body.size() + 512;
    for (qint32 i = 0; i < p.files.size(); ++i) {
        const auto &f = p.files[i];
        bytes += 2 * f.path().size() + 96;
        if (!startsCollapsed(in, i, isExpandable(f, facts[i])))
            bytes += f.bodyBytes + 2 * qsizetype(f.bodyRows) + 48 * facts[i].hunks;
    }
    return bytes;
}

} // namespace

namespace DiffDocumentBuilder {

Squares squares(qint64 added, qint64 deleted)
{
    Squares s;
    const qint64 total = added + deleted;
    if (total <= 0) return s;
    if (total > 5) {
        s.green = static_cast<int>(added * 5 / total);
        s.red = static_cast<int>(deleted * 5 / total);
    } else {
        s.green = static_cast<int>(added);
        s.red = static_cast<int>(deleted);
    }
    s.grey = 5 - s.green - s.red;
    return s;
}

QVector<Reason> classifyFiles(const GitDiffParser::Result &parsed, const QVector<bool> &linguistGenerated)
{
    QVector<Reason> reasons;
    reasons.reserve(parsed.files.size());
    for (qint32 i = 0; i < parsed.files.size(); ++i)
        reasons.append(DiffFileClassifier::classify(parsed.files[i], linguistGenerated.value(i, false)));
    return reasons;
}

std::shared_ptr<const Document> build(std::shared_ptr<const Input> input)
{
    auto doc = std::make_shared<Document>();
    if (!input->parsed) {
        auto patched = std::make_shared<Input>(*input);
        patched->parsed = std::make_shared<const GitDiffParser::Result>();
        input = std::move(patched);
    }
    doc->input = input;
    const Input &src = *input;
    const GitDiffParser::Result &p = *src.parsed;

    const QVector<FileFacts> facts = factsFor(p);
    const qsizetype bytes = estimateBytes(src, facts);
    doc->text.reserve(bytes);
    doc->styles.reserve(bytes);

    Writer w{*doc};
    if (src.commit) writeCard(w, *doc, src);

    doc->files.reserve(p.files.size());
    for (qint32 i = 0; i < p.files.size(); ++i) {
        const GitDiffParser::FileSpan &f = p.files[i];
        if (i > 0 || src.commit) w.endRow(RowKind::Separator);
        const bool expandable = isExpandable(f, facts[i]);
        const bool collapsed = startsCollapsed(src, i, expandable);
        const Reason reason = reasonOf(src, i);

        File file;
        file.headerRow = w.row();
        file.reason = reason;
        file.collapsed = collapsed;
        file.expandable = expandable;
        file.status = f.status;
        file.added = f.added;
        file.deleted = f.deleted;
        file.path = f.path();
        if (f.status == 'R' || f.status == 'C') file.oldPath = f.oldPath;

        const qint32 headerPos = w.pos();
        writeHeader(w, f, i, reason, collapsed, expandable, doc->strikes);
        file.headerText = doc->text.sliced(headerPos);
        file.headerStyles = doc->styles.sliced(headerPos);
        w.endRow(RowKind::FileHeader, Style::HeaderPlain);

        if (collapsed)
            writePlaceholder(w, reason, i);
        else
            writeBody(w, src, i, facts[i]);
        file.bodyRows = w.row() - file.headerRow - 1;
        doc->files.append(std::move(file));
    }
    if (p.files.isEmpty()) writeMessageRow(w, tr("No changes to show"));

    for (qint32 v : p.oldLn) doc->maxOldLn = qMax(doc->maxOldLn, v);
    for (qint32 v : p.newLn) doc->maxNewLn = qMax(doc->maxNewLn, v);
    return doc;
}

Rows buildBody(const Input &input, qint32 fileIndex)
{
    Rows rows;
    if (!input.parsed || fileIndex < 0 || fileIndex >= input.parsed->files.size()) return rows;
    const FileFacts facts = factsOf(*input.parsed, fileIndex);
    const auto &f = input.parsed->files[fileIndex];
    const qsizetype bytes = f.bodyBytes + 2 * qsizetype(f.bodyRows) + 48 * facts.hunks;
    rows.text.reserve(bytes);
    rows.styles.reserve(bytes);
    Writer w{rows};
    writeBody(w, input, fileIndex, facts);
    return rows;
}

Rows buildPlaceholder(const Input &input, qint32 fileIndex)
{
    Rows rows;
    Writer w{rows};
    writePlaceholder(w, reasonOf(input, fileIndex), fileIndex);
    return rows;
}

} // namespace DiffDocumentBuilder
