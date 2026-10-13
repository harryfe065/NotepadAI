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

#include "WorkspaceTaskScanner.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>
#include <functional>
#include <limits>

namespace {

// 0-based line numbers for a sequence of ascending character offsets in
// one forward pass over the content (matches arrive in document order, so
// this stays linear instead of rescanning from 0 per match).
class LineCounter
{
public:
    explicit LineCounter(const QString &content) : m_content(content) {}

    int lineAt(int offset)
    {
        const int end = qMin(offset, m_content.size());
        for (; m_pos < end; ++m_pos) {
            if (m_content.at(m_pos) == QLatin1Char('\n'))
                ++m_line;
        }
        return m_line;
    }

private:
    const QString &m_content;
    int m_pos = 0;
    int m_line = 0;
};

// Positions of the keys directly inside the JSON object whose '{' is at
// `open`, first occurrence wins. One linear pass that tracks strings and
// nesting, so a value equal to a key name ("lint": "test") or a key of a
// nested object is never taken for the key. Keys are compared raw: one
// written with escapes is simply not found (line -1).
QHash<QString, qsizetype> objectKeyPositions(const QString &s, qsizetype open)
{
    QHash<QString, qsizetype> keys;
    if (open < 0)
        return keys;
    const qsizetype n = s.size();
    int depth = 0;
    for (qsizetype i = open; i < n; ++i) {
        const QChar c = s.at(i);
        if (c == u'"') {
            const qsizetype start = i;
            for (++i; i < n && s.at(i) != u'"'; ++i) {
                if (s.at(i) == u'\\')
                    ++i;
            }
            if (depth != 1 || i >= n)
                continue;
            qsizetype j = i + 1;
            while (j < n && s.at(j).isSpace())
                ++j;
            if (j < n && s.at(j) == u':') {
                const QString key = s.mid(start + 1, i - start - 1);
                if (!keys.contains(key))
                    keys.insert(key, start);
            }
        } else if (c == u'{' || c == u'[') {
            ++depth;
        } else if (c == u'}' || c == u']') {
            if (--depth <= 0)
                break;
        }
    }
    return keys;
}

// Tasks of a JSON `"block": { "name": "…" }` object (package.json "scripts",
// deno.json "tasks") in declaration order. QJsonObject loses that order, so
// entries are placed by the position of their key inside the block — which
// is also their line for gutter markers. A key that cannot be placed sorts
// last with line -1.
QList<WorkspaceTask> parseJsonTaskBlock(const QByteArray &text, const char *blockKey,
                                        const std::function<QString(const QString &)> &commandFor)
{
    QList<WorkspaceTask> entries;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(text, &err);
    if (doc.isNull() || !doc.isObject())
        return entries;

    const QString blockName = QLatin1String(blockKey);
    const QJsonObject block = doc.object().value(blockName).toObject();
    if (block.isEmpty())
        return entries;

    const QString content = QString::fromUtf8(text);
    QHash<QString, qsizetype> taskKeys;
    const QHash<QString, qsizetype> rootKeys =
        objectKeyPositions(content, content.indexOf(QLatin1Char('{')));
    if (const auto it = rootKeys.constFind(blockName); it != rootKeys.cend()) {
        // Past `"block"`, then `:`, to the block's own '{'.
        qsizetype j = *it + blockName.size() + 2;
        while (j < content.size() && content.at(j) != u'{')
            ++j;
        taskKeys = objectKeyPositions(content, j < content.size() ? j : -1);
    }

    struct Placed {
        qsizetype pos;
        WorkspaceTask task;
    };
    QList<Placed> placed;
    placed.reserve(block.size());
    for (auto it = block.begin(); it != block.end(); ++it) {
        const QString key = it.key();
        WorkspaceTask task;
        task.name = key;
        task.command = commandFor(key);
        placed.append({taskKeys.value(key, -1), task});
    }
    const auto order = [](qsizetype pos) {
        return pos < 0 ? std::numeric_limits<qsizetype>::max() : pos;
    };
    std::stable_sort(placed.begin(), placed.end(), [&order](const Placed &a, const Placed &b) {
        return order(a.pos) < order(b.pos);
    });
    LineCounter lines(content);
    entries.reserve(placed.size());
    for (Placed &pl : placed) {
        pl.task.line = pl.pos >= 0 ? lines.lineAt(int(pl.pos)) : -1;
        entries.append(pl.task);
    }
    return entries;
}

// True when the `[...]` attribute lines right above the recipe header that
// starts at `lineStart` include `private` (`[private]`, `[private, no-cd]`,
// stacked lines). Comment lines among them are skipped; anything else ends
// the run.
bool hasPrivateAttribute(const QString &s, qsizetype lineStart)
{
    qsizetype end = lineStart;
    while (end > 0) {
        const qsizetype nl = end - 1; // the '\n' ending the previous line
        const qsizetype begin = nl > 0 ? s.lastIndexOf(QLatin1Char('\n'), nl - 1) + 1 : 0;
        const QStringView line = QStringView(s).mid(begin, nl - begin).trimmed();
        end = begin;
        if (line.startsWith(u'#'))
            continue;
        if (!line.startsWith(u'[') || !line.endsWith(u']'))
            return false;
        for (const QStringView attr : line.mid(1, line.size() - 2).split(u','))
            if (attr.trimmed() == u"private")
                return true;
    }
    return false;
}

QByteArray readFile(const QDir &dir, const QString &name)
{
    QFile f(dir.filePath(name));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

// First existing of `names` wins; an existing-but-taskless file still wins
// (a Justfile next to a justfile is one project, not two).
QByteArray readFirstExisting(const QDir &dir, const QStringList &names, bool *found)
{
    for (const QString &name : names) {
        if (QFileInfo::exists(dir.filePath(name))) {
            *found = true;
            return readFile(dir, name);
        }
    }
    *found = false;
    return {};
}

} // namespace

QList<WorkspaceTask> WorkspaceTaskScanner::parsePackageJson(const QByteArray &text,
                                                            const QString &packageManager)
{
    const QString pm = packageManager.isEmpty() ? QStringLiteral("npm") : packageManager;
    return parseJsonTaskBlock(text, "scripts", [pm](const QString &key) {
        // yarn uses `yarn <script>`, the others `<pm> run <script>`.
        return pm == QLatin1String("yarn") ? QStringLiteral("yarn %1").arg(key)
                                           : QStringLiteral("%1 run %2").arg(pm, key);
    });
}

QList<WorkspaceTask> WorkspaceTaskScanner::parseDenoJson(const QByteArray &text)
{
    return parseJsonTaskBlock(text, "tasks", [](const QString &key) {
        return QStringLiteral("deno task %1").arg(key);
    });
}

QList<WorkspaceTask> WorkspaceTaskScanner::parseJustfile(const QByteArray &text)
{
    QList<WorkspaceTask> entries;
    static const QRegularExpression re(
        QStringLiteral(R"(^@?([A-Za-z_][A-Za-z0-9_-]*)(?:[\t ]+([^:\r\n]*))?[\t ]*:(?!=))"),
        QRegularExpression::MultilineOption);
    // [*|+][$]name[=default]: a param without a default must be supplied by
    // the caller, except `*` variadics, which may be empty.
    static const QRegularExpression paramRe(
        QStringLiteral(R"(([+*]?)\$?[A-Za-z_][A-Za-z0-9_-]*(\s*=\s*(?:"[^"]*"|'[^']*'|\S+))?)"));

    const QString content = QString::fromUtf8(text);
    LineCounter lines(content);
    QRegularExpressionMatchIterator it = re.globalMatch(content);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        WorkspaceTask task;
        task.name = m.captured(1);
        task.command = QStringLiteral("just %1").arg(task.name);
        task.line = lines.lineAt(m.capturedStart(0));
        task.isPrivate = task.name.startsWith(QLatin1Char('_'))
                         || hasPrivateAttribute(content, m.capturedStart(0));

        const QString params = m.captured(2).trimmed();
        if (!params.isEmpty()) {
            QRegularExpressionMatchIterator pit = paramRe.globalMatch(params);
            while (pit.hasNext()) {
                const QRegularExpressionMatch p = pit.next();
                if (!p.hasCaptured(2) && p.capturedView(1) != u"*") {
                    task.hasRequiredParams = true;
                    break;
                }
            }
        }
        entries.append(task);
    }
    return entries;
}

QList<WorkspaceTask> WorkspaceTaskScanner::parseMakefile(const QByteArray &text)
{
    QList<WorkspaceTask> entries;
    static const QRegularExpression re(
        QStringLiteral(R"(^([A-Za-z_][A-Za-z0-9_-]*)[ \t]*:(?!=))"),
        QRegularExpression::MultilineOption);

    const QString content = QString::fromUtf8(text);
    LineCounter lines(content);
    QRegularExpressionMatchIterator it = re.globalMatch(content);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        WorkspaceTask task;
        task.name = m.captured(1);
        task.command = QStringLiteral("make %1").arg(task.name);
        task.line = lines.lineAt(m.capturedStart(0));
        entries.append(task);
    }
    return entries;
}

QString WorkspaceTaskScanner::detectPackageManager(const QString &dir)
{
    if (dir.isEmpty())
        return QStringLiteral("npm");
    const QDir d(dir);
    if (QFileInfo::exists(d.filePath(QStringLiteral("yarn.lock"))))
        return QStringLiteral("yarn");
    if (QFileInfo::exists(d.filePath(QStringLiteral("pnpm-lock.yaml"))))
        return QStringLiteral("pnpm");
    if (QFileInfo::exists(d.filePath(QStringLiteral("bun.lockb")))
        || QFileInfo::exists(d.filePath(QStringLiteral("bun.lock"))))
        return QStringLiteral("bun");
    return QStringLiteral("npm");
}

QList<WorkspaceTaskGroup> WorkspaceTaskScanner::scanRoot(const QString &workspaceRoot)
{
    QList<WorkspaceTaskGroup> groups;
    if (workspaceRoot.isEmpty())
        return groups;
    const QDir dir(workspaceRoot);
    if (!dir.exists())
        return groups;

    bool found = false;

    const QByteArray just = readFirstExisting(
        dir, {QStringLiteral("justfile"), QStringLiteral("Justfile"), QStringLiteral(".justfile")},
        &found);
    if (found) {
        if (auto tasks = parseJustfile(just); !tasks.isEmpty())
            groups.append({QStringLiteral("Justfile"), tasks});
    }

    const QByteArray make = readFirstExisting(
        dir, {QStringLiteral("Makefile"), QStringLiteral("makefile"), QStringLiteral("GNUmakefile")},
        &found);
    if (found) {
        if (auto tasks = parseMakefile(make); !tasks.isEmpty())
            groups.append({QStringLiteral("Makefile"), tasks});
    }

    if (QFileInfo::exists(dir.filePath(QStringLiteral("package.json")))) {
        const QByteArray text = readFile(dir, QStringLiteral("package.json"));
        if (auto tasks = parsePackageJson(text, detectPackageManager(workspaceRoot));
            !tasks.isEmpty())
            groups.append({QStringLiteral("package.json"), tasks});
    }

    if (QFileInfo::exists(dir.filePath(QStringLiteral("deno.json")))) {
        const QByteArray text = readFile(dir, QStringLiteral("deno.json"));
        if (auto tasks = parseDenoJson(text); !tasks.isEmpty())
            groups.append({QStringLiteral("deno.json"), tasks});
    }

    return groups;
}
