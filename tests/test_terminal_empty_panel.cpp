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

#include <QtTest>
#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QPushButton>

#include "widgets/TerminalEmptyPanel.h"

// Seam: TerminalEmptyPanel — the widget alone (no TerminalManager, no ADS).
// Covers what the policy/scanner tests cannot: rebuilding the widget tree
// (double-free regression), row geometry and the private-recipe filter.
class TestTerminalEmptyPanel : public QObject
{
    Q_OBJECT

private slots:
    void rebuildRepeatedly_noCrashNoLeak();
    void rowWithCommand_tallerThanNameOnlyRow();
    void privateRecipes_notListed();
    void remote_noTaskRows();
    void rebuild200Rows_timing();

private:
    static QList<QPushButton *> taskRows(const QWidget &panel);
    static bool hasLabel(const QWidget &panel, const QString &text);
    static void settle();
};

QList<QPushButton *> TestTerminalEmptyPanel::taskRows(const QWidget &panel)
{
    QList<QPushButton *> rows;
    for (QPushButton *b : panel.findChildren<QPushButton *>()) {
        if (b->property("nnTaskRow").toBool())
            rows.append(b);
    }
    return rows;
}

bool TestTerminalEmptyPanel::hasLabel(const QWidget &panel, const QString &text)
{
    for (const QLabel *l : panel.findChildren<QLabel *>()) {
        if (l->text() == text)
            return true;
    }
    return false;
}

void TestTerminalEmptyPanel::settle()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

void TestTerminalEmptyPanel::rebuildRepeatedly_noCrashNoLeak()
{
    TerminalEmptyPanel panel;
    panel.resize(800, 500);
    panel.show();

    const QList<TerminalTask> configured{{QStringLiteral("build"), QStringLiteral("just build")}};
    const QList<WorkspaceTaskGroup> groups{
        {QStringLiteral("Makefile"),
         {{QStringLiteral("all"), QStringLiteral("make all")},
          {QStringLiteral("test"), QStringLiteral("make test")}}}};

    // The startup crash: a second setContent freed nested layouts twice.
    panel.setContent(QStringLiteral("/ws"), false, configured, groups);
    panel.setContent(QStringLiteral("/ws"), false, configured, groups);
    panel.setSchemeColors(Qt::black, Qt::white, Qt::blue);
    panel.setDiscoveredGroups({});
    panel.setDiscoveredGroups(groups);
    settle();

    QCOMPARE(taskRows(panel).size(), 3);
}

void TestTerminalEmptyPanel::rowWithCommand_tallerThanNameOnlyRow()
{
    TerminalEmptyPanel panel;
    panel.resize(800, 500);
    // Name == command shows one line; a distinct command adds a second.
    const QList<TerminalTask> configured{{QStringLiteral("make"), QStringLiteral("make")},
                                         {QStringLiteral("dev"), QStringLiteral("npm run dev")}};
    panel.setContent(QStringLiteral("/ws"), false, configured, {});
    panel.show();
    settle();

    const QList<QPushButton *> rows = taskRows(panel);
    QCOMPARE(rows.size(), 2);
    const QPushButton *oneLine = rows.at(0)->findChildren<QLabel *>().size() == 3 ? rows.at(0) : rows.at(1);
    const QPushButton *twoLine = oneLine == rows.at(0) ? rows.at(1) : rows.at(0);
    QVERIFY(twoLine->height() > oneLine->height());
    // Both labels fit: the row is at least as tall as its layout needs.
    QVERIFY(twoLine->height() >= twoLine->layout()->minimumSize().height());
}

void TestTerminalEmptyPanel::privateRecipes_notListed()
{
    TerminalEmptyPanel panel;
    WorkspaceTask helper{QStringLiteral("_helper"), QStringLiteral("just _helper")};
    helper.isPrivate = true;
    const WorkspaceTask build{QStringLiteral("build"), QStringLiteral("just build")};
    WorkspaceTask hidden{QStringLiteral("hidden"), QStringLiteral("make hidden")};
    hidden.isPrivate = true;

    panel.setContent(QStringLiteral("/ws"), false, {},
                     {{QStringLiteral("Justfile"), {helper, build}},
                      {QStringLiteral("Makefile"), {hidden}}});
    settle();

    QCOMPARE(taskRows(panel).size(), 1);
    QVERIFY(hasLabel(panel, QStringLiteral("Justfile")));
    // A group holding only private recipes gets no header either.
    QVERIFY(!hasLabel(panel, QStringLiteral("Makefile")));
}

void TestTerminalEmptyPanel::remote_noTaskRows()
{
    TerminalEmptyPanel panel;
    panel.setContent(QStringLiteral("/home/u/ws"), true,
                     {{QStringLiteral("build"), QStringLiteral("just build")}},
                     {{QStringLiteral("Justfile"), {{QStringLiteral("b"), QStringLiteral("just b")}}}});
    settle();
    QCOMPARE(taskRows(panel).size(), 0);
}

void TestTerminalEmptyPanel::rebuild200Rows_timing()
{
    // Measurement for the row-cap decision (cap groups at 50 if a rebuild
    // of 200 rows costs more than 8 ms): prints, never fails.
    TerminalEmptyPanel panel;
    panel.resize(900, 600);
    panel.setSchemeColors(Qt::black, Qt::white, Qt::blue);
    panel.show();

    WorkspaceTaskGroup group{QStringLiteral("package.json"), {}};
    for (int i = 0; i < 200; ++i) {
        group.tasks.append({QStringLiteral("script-%1").arg(i),
                            QStringLiteral("npm run script-%1").arg(i)});
    }
    const QList<WorkspaceTaskGroup> groups{group};
    panel.setContent(QStringLiteral("/ws"), false, {}, groups);
    settle();

    constexpr int kRuns = 10;
    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < kRuns; ++i) {
        panel.setContent(QStringLiteral("/ws"), false, {}, groups);
        settle();
    }
    const double ms = double(timer.nsecsElapsed()) / 1e6 / kRuns;
    qInfo("TerminalEmptyPanel rebuild, 200 rows: %.2f ms (incl. teardown + layout)", ms);
    QCOMPARE(taskRows(panel).size(), 200);
}

QTEST_MAIN(TestTerminalEmptyPanel)
#include "test_terminal_empty_panel.moc"
