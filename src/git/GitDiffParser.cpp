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

#include "GitDiffParser.h"

#include <cstring>

namespace {

// Parse decimal integer from [p, q). Returns -1 on empty or non-digit.
inline qint32 parseDec(const char *p, const char *q)
{
    if (p == q) return -1;
    qint32 v = 0;
    while (p < q) {
        const char c = *p++;
        if (c < '0' || c > '9') return -1;
        v = v * 10 + (c - '0');
    }
    return v;
}

// Parse one "start[,len]" range of a hunk header starting at p. A missing
// length means 1 (git omits ",1").
bool parseRange(const char *&p, const char *end, qint32 &start, qint32 &len)
{
    const char *q = p;
    while (q < end && *q >= '0' && *q <= '9') ++q;
    start = parseDec(p, q);
    if (start < 0) return false;
    len = 1;
    if (q < end && *q == ',') {
        const char *r = ++q;
        while (r < end && *r >= '0' && *r <= '9') ++r;
        len = parseDec(q, r);
        if (len < 0) return false;
        q = r;
    }
    p = q;
    return true;
}

// Parse hunk header "@@ -O[,L] +N[,L] @@ ...".
bool parseHunkHeader(const char *p, const char *end, qint32 &oldStart, qint32 &oldLen,
                     qint32 &newStart, qint32 &newLen)
{
    if (end - p < 8) return false;
    if (p[0] != '@' || p[1] != '@' || p[2] != ' ') return false;
    p += 3;
    if (*p != '-') return false;
    ++p;
    if (!parseRange(p, end, oldStart, oldLen)) return false;
    if (end - p < 2 || p[0] != ' ' || p[1] != '+') return false;
    p += 2;
    return parseRange(p, end, newStart, newLen);
}

bool startsWith(const char *p, qsizetype len, const char *lit)
{
    const qsizetype n = static_cast<qsizetype>(std::strlen(lit));
    return len >= n && std::memcmp(p, lit, static_cast<size_t>(n)) == 0;
}

quint32 parseOctal(const char *p, const char *end)
{
    quint32 v = 0;
    while (p < end && *p >= '0' && *p <= '7') v = v * 8 + quint32(*p++ - '0');
    return v;
}

// Undo git's C-style path quoting ("a/f\303\266o\tbar"). Unquoted input is
// returned as is.
QByteArray unquotePath(const char *p, const char *end)
{
    if (end - p < 2 || *p != '"') return QByteArray(p, end - p);
    QByteArray out;
    out.reserve(end - p);
    ++p;
    while (p < end && *p != '"') {
        char c = *p++;
        if (c == '\\' && p < end) {
            c = *p++;
            switch (c) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'a': c = '\a'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'v': c = '\v'; break;
                default:
                    if (c >= '0' && c <= '7') {
                        int v = c - '0';
                        for (int k = 0; k < 2 && p < end && *p >= '0' && *p <= '7'; ++k)
                            v = v * 8 + (*p++ - '0');
                        c = static_cast<char>(v);
                    }
                    break;  // '\\' and '"' map to themselves
            }
        }
        out.append(c);
    }
    return out;
}

// Path from a "---"/"+++" line body: "a/x", "b/x", "/dev/null" or quoted.
// Returns false for /dev/null. Trailing tab metadata is dropped.
bool sidePath(const char *p, const char *end, QByteArray &out)
{
    if (end - p >= 9 && std::memcmp(p, "/dev/null", 9) == 0) return false;
    QByteArray path = unquotePath(p, end);
    if (*p != '"') {
        const qsizetype tab = path.indexOf('\t');
        if (tab >= 0) path.truncate(tab);
    }
    if (path.size() >= 2 && (path.startsWith("a/") || path.startsWith("b/"))) path.remove(0, 2);
    out = path;
    return true;
}

// "diff --git a/x b/y": the paths are exact only when quoted or when both
// sides are equal; renames are corrected later by "rename from/to" and the
// ---/+++ lines.
void pathsFromDiffGit(const char *p, const char *end, GitDiffParser::FileSpan &f)
{
    p += 11;  // "diff --git "
    if (p >= end) return;
    if (*p == '"') {
        const char *q = p + 1;
        while (q < end && !(*q == '"' && q[-1] != '\\')) ++q;
        if (q >= end) return;
        sidePath(p, q + 1, f.oldPath);
        p = q + 1;
        if (p < end && *p == ' ') ++p;
        sidePath(p, end, f.newPath);
        return;
    }
    // Unquoted "a/P b/Q": with equal sides the split point is the middle.
    const qsizetype n = end - p;
    if (n >= 7 && (n - 1) % 2 == 0) {
        const qsizetype half = (n - 1) / 2;
        if (p[half] == ' ' && std::memcmp(p + 2, p + half + 3, static_cast<size_t>(half - 2)) == 0) {
            f.oldPath = QByteArray(p + 2, half - 2);
            f.newPath = f.oldPath;
            return;
        }
    }
    const char *b = end;
    while (b > p + 2 && !(b[-1] == ' ' && b[0] == 'b' && b[1] == '/')) --b;
    if (b > p + 2) {
        sidePath(p, b - 1, f.oldPath);
        sidePath(b, end, f.newPath);
    }
}

// Fold one extended header line into the file's summary.
void applyHeaderLine(const char *p, qsizetype len, GitDiffParser::FileSpan &f)
{
    const char *end = p + len;
    if (startsWith(p, len, "--- ")) {
        if (!sidePath(p + 4, end, f.oldPath)) { f.oldPath.clear(); f.status = 'A'; }
    } else if (startsWith(p, len, "+++ ")) {
        if (!sidePath(p + 4, end, f.newPath)) { f.newPath.clear(); f.status = 'D'; }
    } else if (startsWith(p, len, "index ")) {
        const char *sp = static_cast<const char *>(std::memchr(p + 6, ' ', static_cast<size_t>(len - 6)));
        if (sp) {
            const quint32 mode = parseOctal(sp + 1, end);
            if (!f.oldMode && f.status != 'A') f.oldMode = mode;
            if (!f.newMode && f.status != 'D') f.newMode = mode;
        }
    } else if (startsWith(p, len, "new file mode ")) {
        f.status = 'A';
        f.newMode = parseOctal(p + 14, end);
        f.oldPath.clear();
    } else if (startsWith(p, len, "deleted file mode ")) {
        f.status = 'D';
        f.oldMode = parseOctal(p + 18, end);
        f.newPath.clear();
    } else if (startsWith(p, len, "old mode ")) {
        f.oldMode = parseOctal(p + 9, end);
    } else if (startsWith(p, len, "new mode ")) {
        f.newMode = parseOctal(p + 9, end);
    } else if (startsWith(p, len, "rename from ")) {
        f.status = 'R';
        f.oldPath = unquotePath(p + 12, end);
    } else if (startsWith(p, len, "rename to ")) {
        f.status = 'R';
        f.newPath = unquotePath(p + 10, end);
    } else if (startsWith(p, len, "copy from ")) {
        f.status = 'C';
        f.oldPath = unquotePath(p + 10, end);
    } else if (startsWith(p, len, "copy to ")) {
        f.status = 'C';
        f.newPath = unquotePath(p + 8, end);
    } else if (startsWith(p, len, "similarity index ")) {
        f.similarity = static_cast<quint8>(qBound(0, parseDec(p + 17, end - 1), 100));
    } else if (startsWith(p, len, "Binary files ") || startsWith(p, len, "GIT binary patch")) {
        f.binary = true;
    }
}

} // namespace

GitDiffParser::Result GitDiffParser::parse(const QByteArray &diff)
{
    Result r;
    const char *data = diff.constData();
    const qsizetype n = diff.size();
    if (n == 0) return r;

    // Pre-reserve based on a coarse estimate (assume ~50 bytes/line).
    const qsizetype estLines = n / 50 + 8;
    r.texts.reserve(estLines);
    r.kinds.reserve(estLines);
    r.oldLn.reserve(estLines);
    r.newLn.reserve(estLines);

    qint32 curOld = -1;
    qint32 curNew = -1;
    // Lines still owed to the current hunk, from its header. While either is
    // positive every line is body, so a deleted "-- comment" is never taken
    // for a "--- a/x" header.
    qint32 oldLeft = 0;
    qint32 newLeft = 0;
    bool sawBinary = false;
    FileSpan *file = nullptr;

    qsizetype i = 0;
    while (i < n) {
        // Find end of line (LF). Handle \r\n by trimming trailing \r.
        qsizetype j = i;
        while (j < n && data[j] != '\n') ++j;
        qsizetype lineEnd = j;
        if (lineEnd > i && data[lineEnd - 1] == '\r') --lineEnd;

        const char *p = data + i;
        const qsizetype len = lineEnd - i;
        const char first = (len > 0) ? *p : '\0';
        const auto row = static_cast<qint32>(r.kinds.size());
        const bool inHunk = oldLeft > 0 || newLeft > 0;

        LineKind kind;
        qint32 oLn = -1, nLn = -1;
        qsizetype textStart = i;
        qsizetype textLen = len;

        if (!inHunk && startsWith(p, len, "diff --git ")) {
            if (file) file->endRow = row;
            r.files.append(FileSpan{});
            file = &r.files.last();
            file->firstRow = row;
            pathsFromDiffGit(p, p + len, *file);
            kind = LineKind::FileHeader;
        } else {
            if (!file) {
                // A bare patch without "diff --git": one file, paths unknown
                // until ---/+++ name them.
                r.files.append(FileSpan{});
                file = &r.files.last();
                file->firstRow = row;
            }
            if (inHunk && (first == ' ' || first == '+' || first == '-' || len == 0)) {
                // Body line. An empty line inside a hunk is a context line
                // whose leading space was stripped.
                textStart = len > 0 ? i + 1 : i;
                textLen = len > 0 ? len - 1 : 0;
                if (first == '+') {
                    kind = LineKind::Added;
                    nLn = curNew++;
                    --newLeft;
                    ++r.addedCount;
                    ++file->added;
                } else if (first == '-') {
                    kind = LineKind::Deleted;
                    oLn = curOld++;
                    --oldLeft;
                    ++r.deletedCount;
                    ++file->deleted;
                } else {
                    kind = LineKind::Context;
                    oLn = curOld++;
                    nLn = curNew++;
                    --oldLeft;
                    --newLeft;
                }
                ++file->bodyRows;
                file->bodyBytes += textLen;
                file->maxLineBytes = qMax(file->maxLineBytes, static_cast<qint32>(textLen));
            } else if (first == '\\') {
                kind = LineKind::NoNewline;
            } else if (len >= 4 && p[0] == '@' && p[1] == '@') {
                qint32 os = -1, ol = 0, ns = -1, nl = 0;
                if (parseHunkHeader(p, p + len, os, ol, ns, nl)) {
                    curOld = os;
                    curNew = ns;
                    oldLeft = ol;
                    newLeft = nl;
                }
                r.hunks.append(HunkSpan{row, static_cast<qint32>(r.files.size() - 1)});
                kind = LineKind::HunkHeader;
            } else if (len == 0) {
                kind = LineKind::Empty;
            } else {
                // Extended header: index, mode, rename, ---/+++, Binary files...
                kind = LineKind::FileHeader;
                applyHeaderLine(p, len, *file);
                if (file->binary) sawBinary = true;
            }
        }

        r.texts.append(QByteArray(data + textStart, static_cast<int>(textLen)));
        r.kinds.append(kind);
        r.oldLn.append(oLn);
        r.newLn.append(nLn);

        i = j + 1; // step past the LF (or off end)
    }

    if (file) file->endRow = static_cast<qint32>(r.kinds.size());
    constexpr quint32 kTypeMask = 0170000;
    constexpr quint32 kGitlink = 0160000;
    for (FileSpan &f : r.files) {
        if ((f.oldMode & kTypeMask) == kGitlink || (f.newMode & kTypeMask) == kGitlink)
            f.submodule = true;
        if (f.status == 'M' && f.oldMode && f.newMode
            && (f.oldMode & kTypeMask) != (f.newMode & kTypeMask))
            f.status = 'T';
    }

    r.isBinary = sawBinary;
    return r;
}
