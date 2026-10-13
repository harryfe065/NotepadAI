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
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>
#include <functional>

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

// Tasks of a JSON `"block": { "name": "…" }` object (package.json "scripts",
// deno.json "tasks") in declaration order. QJsonObject loses that order, so
// entries are placed by the byte position of their quoted key after the
// block's key — which is also their line for gutter markers.
QList<WorkspaceTask> parseJsonTaskBlock(const QByteArray &text, const char *blockKey,
                                        const std::function<QString(const QString &)> &commandFor)
{
    QList<WorkspaceTask> entries;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(text, &err);
    if (doc.isNull() || !doc.isObject())
        return entries;

    const QJsonObject block = doc.object().value(QLatin1String(blockKey)).toObject();
    if (block.isEmpty())
        return entries;

    const QString content = QString::fromUtf8(text);
    int searchFrom = content.indexOf(QLatin1Char('"') + QLatin1String(blockKey) + QLatin1Char('"'));
    if (searchFrom < 0)
        searchFrom = 0;

    struct Placed {
        int pos;
        WorkspaceTask task;
    };
    QList<Placed> placed;
    placed.reserve(block.size());
    for (auto it = block.begin(); it != block.end(); ++it) {
        const QString key = it.key();
        const int pos = content.indexOf(QLatin1Char('"') + key + QLatin1Char('"'), searchFrom);
        WorkspaceTask task;
        task.name = key;
        task.command = commandFor(key);
        placed.append({pos, task});
    }
    std::sort(placed.begin(), placed.end(),
              [](const Placed &a, const Placed &b) { return a.pos < b.pos; });
    LineCounter lines(content);
    for (Placed &pl : placed)
        pl.task.line = pl.pos >= 0 ? lines.lineAt(pl.pos) : -1;
    entries.reserve(placed.size());
    for (const Placed &p : placed)
        entries.append(p.task);
    return entries;
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
    static const QRegularExpression paramRe(
        QStringLiteral(R"(([A-Za-z_][A-Za-z0-9_-]*)(?:\s*=\s*(?:"[^"]*"|'[^']*'|\S+))?)"));

    const QString content = QString::fromUtf8(text);
    LineCounter lines(content);
    QRegularExpressionMatchIterator it = re.globalMatch(content);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        WorkspaceTask task;
        task.name = m.captured(1);
        task.command = QStringLiteral("just %1").arg(task.name);
        task.line = lines.lineAt(m.capturedStart(0));

        // A param without an `=` default must be supplied by the caller.
        const QString params = m.captured(2).trimmed();
        if (!params.isEmpty()) {
            QRegularExpressionMatchIterator pit = paramRe.globalMatch(params);
            while (pit.hasNext()) {
                if (!pit.next().captured(0).trimmed().contains(QLatin1Char('='))) {
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
