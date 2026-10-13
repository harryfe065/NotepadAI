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

#include "TaskRunnerGutter.h"

#include "NotepadNextApplication.h"
#include "ScintillaNext.h"
#include "Scintilla.h"
#include "TerminalManager.h"
#include "TerminalTaskRegistry.h"
#include "WorkspaceTaskScanner.h"
#include "remote/ExecutionContext.h"
#include "remote/ExecutionContextRegistry.h"
#include "remote/RemoteExecutionContext.h"
#include "remote/SshProfile.h"

#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QTimer>
#include <QToolTip>

namespace {
constexpr int kMargin = 3;
constexpr int kMarker = 20;
constexpr int kMarginWidth = 16;
constexpr int kDebounceMs = 300;
constexpr int kMaxPmWalkLevels = 5;
// Green play icon color — same on both light and dark themes.
const QColor kIconColor(0x4C, 0xAF, 0x50);
} // namespace

QCache<QString, QString> TaskRunnerGutter::s_pmCache(32);

TaskRunnerGutter::TaskRunnerGutter(ScintillaNext *editor, TerminalManager *termMgr)
    : QObject(editor)
    , m_editor(editor)
    , m_termMgr(termMgr)
    , m_debounce(new QTimer(this))
{
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(kDebounceMs);
    connect(m_debounce, &QTimer::timeout, this, &TaskRunnerGutter::reparseAndUpdateMarkers);

    // Setup margin 3: symbol type, mask for marker 20, sensitive, arrow cursor
    m_editor->setMarginTypeN(kMargin, SC_MARGIN_SYMBOL);
    m_editor->setMarginMaskN(kMargin, 1 << kMarker);
    m_editor->setMarginSensitiveN(kMargin, true);
    m_editor->setMarginCursorN(kMargin, SC_CURSORARROW);
    m_editor->setMarginWidthN(kMargin, 0); // hidden until tasks found

    defineMarker();

    // Install eventFilter on the editor widget for tooltip handling
    m_editor->installEventFilter(this);

    // Connect to Scintilla notifications for margin click and text modification
    connect(m_editor, &ScintillaNext::notify, this, &TaskRunnerGutter::onNotify);

    // Wire cache invalidation on git dirty
    if (auto *app = qobject_cast<NotepadNextApplication *>(qApp)) {
        connect(app, &NotepadNextApplication::gitWorkingTreeDirtied,
                this, [](const QString &) { s_pmCache.clear(); });
        connect(app, &NotepadNextApplication::effectiveThemeChanged,
                this, &TaskRunnerGutter::defineMarker);
    }

    // Initial parse
    reparseAndUpdateMarkers();
}

TaskRunnerGutter::~TaskRunnerGutter() = default;

QImage TaskRunnerGutter::generateRunIcon(int size, const QColor &color)
{
    QImage img(size, size, QImage::Format_RGBA8888);
    img.fill(Qt::transparent);

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(color);

    // Play triangle: pointing right, centered in the square
    const qreal margin = size * 0.2;
    const qreal left = margin + size * 0.05;
    const qreal top = margin;
    const qreal right = size - margin;
    const qreal mid = size / 2.0;

    QPolygonF tri;
    tri << QPointF(left, top)
        << QPointF(right, mid)
        << QPointF(left, size - margin);
    p.drawPolygon(tri);
    p.end();

    return img;
}

void TaskRunnerGutter::defineMarker()
{
    const qreal dpr = m_editor->devicePixelRatioF();
    const int pxSize = static_cast<int>(kMarginWidth * dpr);

    QImage icon = generateRunIcon(pxSize, kIconColor);

    m_editor->rGBAImageSetWidth(pxSize);
    m_editor->rGBAImageSetHeight(pxSize);
    m_editor->rGBAImageSetScale(static_cast<int>(dpr * 100));
    m_editor->markerDefineRGBAImage(kMarker, reinterpret_cast<const char *>(icon.constBits()));
}

void TaskRunnerGutter::recalcMarginBounds()
{
    // Sum widths of margins 0, 1, 2 to get left edge of margin 3
    int left = static_cast<int>(m_editor->marginLeft()); // text area left padding
    left += static_cast<int>(m_editor->marginWidthN(0));
    left += static_cast<int>(m_editor->marginWidthN(1));
    left += static_cast<int>(m_editor->marginWidthN(2));
    m_marginLeft = left;
    m_marginRight = left + static_cast<int>(m_editor->marginWidthN(kMargin));
}

bool TaskRunnerGutter::isTaskFile(const QString &filename)
{
    if (filename.isEmpty()) return false;
    const QString base = QFileInfo(filename).fileName();
    if (base.compare(QLatin1String("package.json"), Qt::CaseInsensitive) == 0) return true;
    if (base.compare(QLatin1String("deno.json"), Qt::CaseInsensitive) == 0) return true;
    if (base == QLatin1String("justfile") || base == QLatin1String("Justfile") ||
        base == QLatin1String(".justfile")) return true;
    if (base.endsWith(QLatin1String(".just"), Qt::CaseInsensitive)) return true;
    if (base == QLatin1String("makefile") || base == QLatin1String("Makefile") ||
        base == QLatin1String("GNUmakefile")) return true;
    return false;
}

// The parsers live in WorkspaceTaskScanner (shared with the terminal empty
// panel's root scan); these wrappers only adapt the shared result to the
// gutter's TaskEntry (dropping entries with no line to mark) and, for
// package.json, supply the package manager detected for the editor file's
// folder.

QVector<TaskRunnerGutter::TaskEntry> TaskRunnerGutter::parsePackageJson(const QByteArray &text)
{
    const QString dir = QFileInfo(m_editor->getFilePath()).absolutePath();
    const QList<WorkspaceTask> tasks =
        WorkspaceTaskScanner::parsePackageJson(text, detectPackageManager(dir));
    QVector<TaskEntry> entries;
    entries.reserve(tasks.size());
    for (const WorkspaceTask &t : tasks) {
        if (t.line >= 0)
            entries.append({t.line, t.name, t.command, t.hasRequiredParams});
    }
    return entries;
}

QVector<TaskRunnerGutter::TaskEntry> TaskRunnerGutter::parseJustfile(const QByteArray &text)
{
    const QList<WorkspaceTask> tasks = WorkspaceTaskScanner::parseJustfile(text);
    QVector<TaskEntry> entries;
    entries.reserve(tasks.size());
    for (const WorkspaceTask &t : tasks) {
        if (t.line >= 0)
            entries.append({t.line, t.name, t.command, t.hasRequiredParams});
    }
    return entries;
}

QVector<TaskRunnerGutter::TaskEntry> TaskRunnerGutter::parseMakefile(const QByteArray &text)
{
    const QList<WorkspaceTask> tasks = WorkspaceTaskScanner::parseMakefile(text);
    QVector<TaskEntry> entries;
    entries.reserve(tasks.size());
    for (const WorkspaceTask &t : tasks) {
        if (t.line >= 0)
            entries.append({t.line, t.name, t.command, t.hasRequiredParams});
    }
    return entries;
}

QVector<TaskRunnerGutter::TaskEntry> TaskRunnerGutter::parseDenoJson(const QByteArray &text)
{
    const QList<WorkspaceTask> tasks = WorkspaceTaskScanner::parseDenoJson(text);
    QVector<TaskEntry> entries;
    entries.reserve(tasks.size());
    for (const WorkspaceTask &t : tasks) {
        if (t.line >= 0)
            entries.append({t.line, t.name, t.command, t.hasRequiredParams});
    }
    return entries;
}

void TaskRunnerGutter::reparseAndUpdateMarkers()
{
    if (!m_editor) return;
    if (!m_editor->isFile()) return;

    const QString filePath = m_editor->getFilePath();
    const QString filename = QFileInfo(filePath).fileName();

    // Clear existing markers
    m_editor->markerDeleteAll(kMarker);
    m_tasks.clear();

    if (!isTaskFile(filename)) {
        m_editor->setMarginWidthN(kMargin, 0);
        return;
    }

    // Get editor content
    const int len = static_cast<int>(m_editor->length());
    const QByteArray text = m_editor->get_text_range(0, len);

    // Dispatch to correct parser
    if (filename.compare(QLatin1String("package.json"), Qt::CaseInsensitive) == 0) {
        m_tasks = parsePackageJson(text);
    } else if (filename == QLatin1String("justfile") || filename == QLatin1String("Justfile") ||
               filename == QLatin1String(".justfile") ||
               filename.endsWith(QLatin1String(".just"), Qt::CaseInsensitive)) {
        m_tasks = parseJustfile(text);
    } else if (filename == QLatin1String("makefile") || filename == QLatin1String("Makefile") ||
               filename == QLatin1String("GNUmakefile")) {
        m_tasks = parseMakefile(text);
    } else if (filename.compare(QLatin1String("deno.json"), Qt::CaseInsensitive) == 0) {
        m_tasks = parseDenoJson(text);
    }

    // Update margin width: show only if tasks exist
    const int newWidth = m_tasks.isEmpty() ? 0 : kMarginWidth;
    if (m_editor->marginWidthN(kMargin) != newWidth) {
        m_editor->setMarginWidthN(kMargin, newWidth);
    }

    // Place markers
    for (const TaskEntry &t : m_tasks) {
        m_editor->markerAdd(t.line, kMarker);
    }

    recalcMarginBounds();
}

QString TaskRunnerGutter::detectPackageManager(const QString &dir)
{
    if (dir.isEmpty()) return QStringLiteral("npm");

    // Check cache first
    if (QString *cached = s_pmCache.object(dir)) {
        return *cached;
    }

    QDir d(dir);
    for (int i = 0; i < kMaxPmWalkLevels; ++i) {
        if (QFileInfo::exists(d.filePath(QStringLiteral("yarn.lock")))) {
            s_pmCache.insert(dir, new QString(QStringLiteral("yarn")));
            return QStringLiteral("yarn");
        }
        if (QFileInfo::exists(d.filePath(QStringLiteral("pnpm-lock.yaml")))) {
            s_pmCache.insert(dir, new QString(QStringLiteral("pnpm")));
            return QStringLiteral("pnpm");
        }
        if (QFileInfo::exists(d.filePath(QStringLiteral("bun.lockb")))) {
            s_pmCache.insert(dir, new QString(QStringLiteral("bun")));
            return QStringLiteral("bun");
        }
        if (!d.cdUp()) break;
    }

    s_pmCache.insert(dir, new QString(QStringLiteral("npm")));
    return QStringLiteral("npm");
}

void TaskRunnerGutter::onNotify(Scintilla::NotificationData *pscn)
{
    using Notification = Scintilla::Notification;
    using MT = Scintilla::ModificationFlags;

    switch (pscn->nmhdr.code) {
    case Notification::MarginClick:
        if (pscn->margin == kMargin) {
            const int line = static_cast<int>(m_editor->lineFromPosition(pscn->position));
            onMarginClick(kMargin, line);
        }
        break;
    case Notification::Modified: {
        const auto mt = pscn->modificationType;
        if (FlagSet(mt, MT::InsertText) || FlagSet(mt, MT::DeleteText)) {
            m_debounce->start();
        }
        break;
    }
    default:
        break;
    }
}

void TaskRunnerGutter::onMarginClick(int margin, int line)
{
    if (margin != kMargin) return;

    // Find task at this line
    const TaskEntry *found = nullptr;
    for (const TaskEntry &t : m_tasks) {
        if (t.line == line) {
            found = &t;
            break;
        }
    }
    if (!found) return;
    if (!m_termMgr) return;

    // Resolve execution context: if the editor is remote, route through the
    // remote terminal path; otherwise use the local terminal as before.
    remote::ExecutionContext *context = nullptr;
    QString cwd;

    if (m_editor->isRemote()) {
        // Remote file: extract profileId from the editor's URI, look up the
        // remote context from the registry, and use the remote file's parent
        // directory as the working directory.
        const remote::SshUri parsed = remote::parseSshUri(m_editor->remoteUri());
        if (parsed.valid) {
            if (auto *app = qobject_cast<NotepadNextApplication *>(qApp)) {
                if (auto *registry = app->getExecutionContextRegistry()) {
                    context = registry->remoteContext(parsed.profileId);
                }
            }
        }
        // Remote cwd = parent directory of the remote POSIX path
        const QString remotePath = m_editor->remotePath();
        const int lastSlash = remotePath.lastIndexOf(QLatin1Char('/'));
        cwd = (lastSlash > 0) ? remotePath.left(lastSlash) : QStringLiteral("/");
    } else {
        cwd = QFileInfo(m_editor->getFilePath()).absolutePath();
    }

    if (found->hasRequiredParams) {
        // Open a terminal (remote over SSH when the editor is remote) with
        // the command typed and a trailing space, not entered, so the user
        // can complete the arguments.
        m_termMgr->openTerminalWithInput(context, cwd, found->command + QLatin1Char(' '));
        return;
    }

    TerminalTask task;
    task.name = found->name;
    task.command = found->command;
    m_termMgr->runOrRestartTask(cwd, task, context);
}

bool TaskRunnerGutter::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_editor) return false;

    if (event->type() == QEvent::MouseMove) {
        auto *me = static_cast<QMouseEvent *>(event);
        const int x = me->pos().x();

        // Fast integer range check — hot path, must be < 1ns for non-margin moves
        if (x < m_marginLeft || x >= m_marginRight) {
            if (m_lastTooltipLine != -1) {
                QToolTip::hideText();
                m_lastTooltipLine = -1;
            }
            return false;
        }

        // Mouse is over margin 3 — compute line
        const int y = me->pos().y();
        const int pos = static_cast<int>(m_editor->positionFromPoint(x, y));
        const int line = static_cast<int>(m_editor->lineFromPosition(pos));

        // Same-line dedup
        if (line == m_lastTooltipLine) return false;
        m_lastTooltipLine = line;

        // Find task at this line
        for (const TaskEntry &t : m_tasks) {
            if (t.line == line) {
                QString tip;
                if (t.hasRequiredParams) {
                    // Show placeholder for required params
                    tip = QStringLiteral("Run: %1 <args>").arg(t.command);
                } else {
                    tip = QStringLiteral("Run: %1").arg(t.command);
                }
                QToolTip::showText(me->globalPos(), tip, m_editor);
                return false;
            }
        }

        // No task at this line
        QToolTip::hideText();
        m_lastTooltipLine = -1;
        return false;
    }

    if (event->type() == QEvent::Leave) {
        if (m_lastTooltipLine != -1) {
            QToolTip::hideText();
            m_lastTooltipLine = -1;
        }
    }

    return false;
}
