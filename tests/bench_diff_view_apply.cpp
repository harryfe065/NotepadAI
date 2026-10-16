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

// Manual timing of the diff view's UI-thread work (no ctest): build (the pool
// part), apply, first paint, the sliced decoration finishing (wall time,
// events in between), page scroll and expanding a collapsed file, on a
// real ScintillaNext under the offscreen platform. Prints the median of runs.
//
//   bench_diff_view_apply [runs]

#include "DiffLanguagesLua.h"
#include "DiffView.h"
#include "DiffViewApp.h"
#include "ProfileScope.h"
#include "ScintillaNext.h"

#include "Scintilla.h"

#include <QApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QTextStream>

#include <algorithm>
#include <cstdio>
#include <vector>

// Outside the application: no settings, no Lua catalog (no syntax colours).
namespace DiffViewApp {
ApplicationSettings *settings() { return nullptr; }
} // namespace DiffViewApp
namespace DiffLanguages {
std::shared_ptr<const Catalog> catalog() { return nullptr; }
} // namespace DiffLanguages

namespace {

QByteArray pathOf(int f)
{
    const QByteArray n = QByteArray::number(f);
    // Some lockfiles and generated files, which start collapsed.
    if (f % 50 == 49) return "pkg" + n + "/package-lock.json";
    if (f % 50 == 24) return "dist/bundle" + n + ".min.js";
    return "src/module" + QByteArray::number(f % 37) + "/file" + n + ".cpp";
}

// `files` files of about `rowsPerFile` rows each: hunks of 3 context, 4
// deleted, 4 added (word-level pairs), 3 context. `bigFile` >= 0 gets
// `bigRows` instead (counting hunk headers, which the parser's bodyRows
// does not), enough to start collapsed.
QByteArray makePatch(int files, int rowsPerFile, int bigFile = -1, int bigRows = 0)
{
    QByteArray out;
    for (int f = 0; f < files; ++f) {
        const QByteArray path = pathOf(f);
        out += "diff --git a/" + path + " b/" + path + "\n";
        out += "index 1234567..89abcde 100644\n";
        out += "--- a/" + path + "\n+++ b/" + path + "\n";
        const int rows = f == bigFile ? bigRows : rowsPerFile;
        int line = 1;
        for (int written = 0; written < rows;) {
            out += "@@ -" + QByteArray::number(line) + ",10 +" + QByteArray::number(line) + ",10 @@ void function"
                   + QByteArray::number(line) + "()\n";
            ++written;
            for (int i = 0; i < 3; ++i, ++written) out += "     int context = value + " + QByteArray::number(i) + ";\n";
            for (int i = 0; i < 4; ++i, ++written) out += "-    result = compute(alpha, beta, " + QByteArray::number(i) + ");\n";
            for (int i = 0; i < 4; ++i, ++written) out += "+    result = compute(alpha, gamma, " + QByteArray::number(i) + ");\n";
            for (int i = 0; i < 3; ++i, ++written) out += "     return context * " + QByteArray::number(i) + ";\n";
            line += 40;
        }
    }
    return out;
}

double median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[v.size() / 2];
}

double ms(const QElapsedTimer &t) { return t.nsecsElapsed() / 1e6; }

void settle()
{
    for (int i = 0; i < 4; ++i) QCoreApplication::processEvents();
}

struct Result {
    std::vector<double> build, apply, paint, decorate, scroll, expand;
};

void runCase(const char *name, const QByteArray &patch, DiffView::Mode mode, int runs, bool expand)
{
    Result r;
    qint64 lines = 0;
    for (int run = 0; run < runs; ++run) {
        ScintillaNext editor(QStringLiteral("bench"));
        editor.resize(1400, 900);
        editor.show();
        auto *view = new DiffView(&editor, mode);
        settle();

        QElapsedTimer t;
        t.start();
        DiffView::Source source;
        source.patch = patch;
        DiffView::Content content = DiffView::build(source, mode, nullptr, QDateTime::currentSecsSinceEpoch());
        r.build.push_back(ms(t));

        t.restart();
        view->showContent(content);
        r.apply.push_back(ms(t));

        t.restart();
        editor.viewport()->repaint();
        r.paint.push_back(ms(t));
        lines = editor.send(SCI_GETLINECOUNT);

        // Off-screen markers and word highlights land in slices; done when
        // the last rows (a body's last change or a collapsed file's header)
        // carry a marker.
        const qint64 last = lines - 1;
        const int allMarks = (1 << 13) | (1 << 14) | (1 << 15) | (1 << 16) | (1 << 17);
        t.restart();
        while (editor.send(SCI_MARKERPREVIOUS, last, allMarks) < last - 20 && t.elapsed() < 10000)
            QCoreApplication::processEvents();
        r.decorate.push_back(ms(t));

        // Page down 40 times: margin fill, sticky header, repaint.
        const int page = static_cast<int>(editor.send(SCI_LINESONSCREEN));
        t.restart();
        for (int i = 0; i < 40; ++i) {
            editor.send(SCI_LINESCROLL, 0, page);
            QCoreApplication::processEvents();
            editor.viewport()->repaint();
        }
        r.scroll.push_back(ms(t) / 40);

        if (expand) {
            const auto &files = view->document()->files;
            int file = -1;
            for (int i = 0; i < files.size() && file < 0; ++i)
                if (files[i].collapsed && files[i].expandable) file = i;
            if (file >= 0) {
                const qint64 before = editor.send(SCI_GETLINECOUNT);
                t.restart();
                view->setFileCollapsed(file, false);
                // The body is built on the pool; wait for it to land.
                while (editor.send(SCI_GETLINECOUNT) <= before + 1 && t.elapsed() < 10000)
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
                editor.viewport()->repaint();
                r.expand.push_back(ms(t));
            }
        }
    }
    std::printf("%-34s lines %7lld | build %8.2f | apply %8.2f | paint %6.2f | decorated %7.2f"
                " | scroll/page %6.2f",
                name, static_cast<long long>(lines), median(r.build), median(r.apply), median(r.paint),
                median(r.decorate), median(r.scroll));
    if (expand) std::printf(" | expand %7.2f", median(r.expand));
    std::printf("  (ms, median of %d)\n", runs);
#ifndef NDEBUG
    // Per-phase means (debug builds only): onUpdateUi is the per-scroll-step
    // work without Scintilla's paint.
    for (const ProfileScope::Sample &s : ProfileScope::snapshot()) {
        if (!s.tag.startsWith(QLatin1String("DiffView::")) || s.callCount == 0) continue;
        std::printf("    %-30s mean %8.3f ms  max %8.3f ms  calls %llu\n", qPrintable(s.tag),
                    s.totalNs / 1e6 / double(s.callCount), s.maxNs / 1e6,
                    static_cast<unsigned long long>(s.callCount));
    }
    ProfileScope::resetForTesting();
#endif
    std::fflush(stdout);
}

} // namespace

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    const int runs = argc > 1 ? qMax(1, QByteArray(argv[1]).toInt()) : 5;

    runCase("preview 1 file / 3k rows", makePatch(1, 3000), DiffView::Mode::Preview, runs, false);
    runCase("commit 50 files / 3k rows", makePatch(50, 60), DiffView::Mode::Commit, runs, false);
    runCase("commit 1000 files / 100k rows", makePatch(1000, 100), DiffView::Mode::Commit, runs, false);
    runCase("expand 1 file of 1700 rows", makePatch(2, 30, 0, 1700), DiffView::Mode::Commit, runs, true);
    return 0;
}
