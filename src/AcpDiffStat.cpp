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

#include "AcpDiffStat.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QStringView>

#include "git/BufferDiffEngine.h"

namespace AcpDiffStat {

namespace {

constexpr int kMaxBlocks = 256;

// Calls fn(line) for every line of text, without its '\r\n' / '\n'. A final
// newline does not start an extra empty line.
template <typename Fn>
void forEachLine(QStringView text, Fn &&fn)
{
    qsizetype start = 0;
    const qsizetype n = text.size();
    while (start < n) {
        qsizetype end = text.indexOf(QLatin1Char('\n'), start);
        if (end < 0)
            end = n;
        QStringView line = text.sliced(start, end - start);
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        fn(line);
        start = end + 1;
    }
}

int lineCount(QStringView text)
{
    if (text.isEmpty())
        return 0;
    const int newlines = static_cast<int>(text.count(QLatin1Char('\n')));
    return text.endsWith(QLatin1Char('\n')) ? newlines : newlines + 1;
}

enum class Mark : quint8 { None, Context, Add, Remove };

// omp's numbered row, as AcpToolCallCard parses it: "[ ]*[+-]?N[ ]*(|or│)text".
Mark numberedMark(QStringView line)
{
    qsizetype i = 0;
    const qsizetype n = line.size();
    while (i < n && line.at(i) == QLatin1Char(' '))
        ++i;
    if (i >= n)
        return Mark::None;
    Mark mark = Mark::Context;
    if (line.at(i) == QLatin1Char('+')) {
        mark = Mark::Add;
        ++i;
    } else if (line.at(i) == QLatin1Char('-')) {
        mark = Mark::Remove;
        ++i;
    }
    const qsizetype digits = i;
    bool nonZero = false;
    while (i < n && line.at(i) >= QLatin1Char('0') && line.at(i) <= QLatin1Char('9')) {
        nonZero = nonZero || line.at(i) != QLatin1Char('0');
        ++i;
    }
    if (i == digits || !nonZero)
        return Mark::None;
    while (i < n && line.at(i) == QLatin1Char(' '))
        ++i;
    if (i >= n || (line.at(i) != QLatin1Char('|') && line.at(i) != QChar(0x2502)))
        return Mark::None;
    return mark;
}

// The compact `diff` string: omp numbered rows when the first non-empty line
// is one, else unified. False when it holds no diff rows (the card then falls
// back to oldText/newText, and so does the count).
bool countCompact(QStringView diff, Lines *out)
{
    bool decided = false;
    bool numbered = false;
    bool anyRow = false;
    Lines lines;
    forEachLine(diff, [&](QStringView line) {
        if (line.isEmpty())
            return;
        if (!decided) {
            decided = true;
            numbered = numberedMark(line) != Mark::None;
        }
        if (numbered) {
            switch (numberedMark(line)) {
            case Mark::Add: ++lines.added; anyRow = true; break;
            case Mark::Remove: ++lines.removed; anyRow = true; break;
            case Mark::Context: anyRow = true; break;
            case Mark::None: break;
            }
            return;
        }
        if (line.startsWith(QLatin1String("+++")) || line.startsWith(QLatin1String("---")))
            return;
        switch (line.at(0).unicode()) {
        case '+': ++lines.added; anyRow = true; break;
        case '-': ++lines.removed; anyRow = true; break;
        case ' ':
        case '\t': anyRow = true; break;
        default: break;
        }
    });
    if (anyRow)
        *out = lines;
    return anyRow;
}

QList<QStringView> splitLines(QStringView text)
{
    QList<QStringView> out;
    out.reserve(lineCount(text));
    forEachLine(text, [&out](QStringView line) { out.append(line); });
    return out;
}

// Linear fallback for huge texts: common prefix and suffix are unchanged, the
// middle counts as one replace (what the card renders for them too).
Lines prefixSuffix(QStringView oldText, QStringView newText)
{
    const QList<QStringView> a = splitLines(oldText);
    const QList<QStringView> b = splitLines(newText);
    qsizetype pre = 0;
    while (pre < a.size() && pre < b.size() && a.at(pre) == b.at(pre))
        ++pre;
    qsizetype oldEnd = a.size();
    qsizetype newEnd = b.size();
    while (oldEnd > pre && newEnd > pre && a.at(oldEnd - 1) == b.at(newEnd - 1)) {
        --oldEnd;
        --newEnd;
    }
    return {static_cast<int>(newEnd - pre), static_cast<int>(oldEnd - pre)};
}

Lines countText(const QString &oldText, const QString &newText)
{
    if (oldText.size() > kMaxExactDiffChars || newText.size() > kMaxExactDiffChars)
        return prefixSuffix(oldText, newText);
    QByteArray a = oldText.toUtf8();
    QByteArray b = newText.toUtf8();
    // Edit snippets usually lack a final newline; without one on both sides
    // xdiff would see the old last line as changed when a line is appended.
    if (!a.endsWith('\n'))
        a.append('\n');
    if (!b.endsWith('\n'))
        b.append('\n');
    Lines lines;
    for (const BufferDiffEngine::Hunk &h : BufferDiffEngine::diff(a, b)) {
        lines.removed += h.oldCount;
        lines.added += h.newCount;
    }
    return lines;
}

} // namespace

Lines forBlock(const QJsonObject &block)
{
    const QJsonValue compact = block.value(QLatin1String("diff"));
    if (compact.isString()) {
        Lines lines;
        if (countCompact(compact.toString(), &lines))
            return lines;
    }
    const QJsonValue oldVal = block.value(QLatin1String("oldText"));
    const QJsonValue newVal = block.value(QLatin1String("newText"));
    const QString oldText = oldVal.isString() ? oldVal.toString() : QString();
    const QString newText = newVal.isString() ? newVal.toString() : QString();
    if (oldText.isEmpty())
        return {lineCount(newText), 0};
    if (newText.isEmpty())
        return {0, lineCount(oldText)};
    return countText(oldText, newText);
}

Lines forContent(const QJsonArray &content)
{
    Lines total;
    const qsizetype n = qMin<qsizetype>(content.size(), kMaxBlocks);
    for (qsizetype i = 0; i < n; ++i) {
        const QJsonValue item = content.at(i);
        if (!item.isObject())
            continue;
        const QJsonObject block = item.toObject();
        if (block.value(QLatin1String("type")).toString() == QLatin1String("diff"))
            total += forBlock(block);
    }
    return total;
}

Lines forToolCall(const AcpProtocol::AcpToolCall &tc)
{
    if (tc.status != QLatin1String("completed"))
        return {};
    return forContent(tc.content);
}

QString label(const Lines &lines)
{
    if (lines.isEmpty())
        return {};
    return QStringLiteral("+%1 -%2").arg(lines.added).arg(lines.removed);
}

} // namespace AcpDiffStat
