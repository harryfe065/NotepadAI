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
#include <QLabel>
#include <QPushButton>

#include "widgets/TerminalEmptyPanel.h"

// Seam: TerminalEmptyPanel — the widget alone (no TerminalManager, no ADS).
// Covers what the policy test cannot: rebuilding the widget tree
// (double-free regression) and row geometry.
class TestTerminalEmptyPanel : public QObject
{
    Q_OBJECT

private slots:
    void rebuildRepeatedly_noCrashNoLeak();
    void rowWithCommand_tallerThanNameOnlyRow();
    void remote_noTaskRows();

private:
    static QList<QPushButton *> taskRows(const QWidget &panel);
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

    const QList<TerminalTask> configured{{QStringLiteral("build"), QStringLiteral("just build")},
                                         {QStringLiteral("test"), QStringLiteral("just test")}};

    // The startup crash: a second setContent freed nested layouts twice.
    panel.setContent(QStringLiteral("/ws"), false, configured);
    panel.setContent(QStringLiteral("/ws"), false, configured);
    panel.setSchemeColors(Qt::black, Qt::white, Qt::blue);
    settle();

    QCOMPARE(taskRows(panel).size(), 2);
}

void TestTerminalEmptyPanel::rowWithCommand_tallerThanNameOnlyRow()
{
    TerminalEmptyPanel panel;
    panel.resize(800, 500);
    // Name == command shows one line; a distinct command adds a second.
    const QList<TerminalTask> configured{{QStringLiteral("make"), QStringLiteral("make")},
                                         {QStringLiteral("dev"), QStringLiteral("npm run dev")}};
    panel.setContent(QStringLiteral("/ws"), false, configured);
    panel.show();
    settle();

    const QList<QPushButton *> rows = taskRows(panel);
    QCOMPARE(rows.size(), 2);
    // Labels per row: play icon + name (+ command).
    const QPushButton *oneLine =
        rows.at(0)->findChildren<QLabel *>().size() == 2 ? rows.at(0) : rows.at(1);
    const QPushButton *twoLine = oneLine == rows.at(0) ? rows.at(1) : rows.at(0);
    QVERIFY(twoLine->height() > oneLine->height());
    // Both labels fit: the row is at least as tall as its layout needs.
    QVERIFY(twoLine->height() >= twoLine->layout()->minimumSize().height());
}

void TestTerminalEmptyPanel::remote_noTaskRows()
{
    TerminalEmptyPanel panel;
    panel.setContent(QStringLiteral("/home/u/ws"), true,
                     {{QStringLiteral("build"), QStringLiteral("just build")}});
    settle();
    QCOMPARE(taskRows(panel).size(), 0);
}

QTEST_MAIN(TestTerminalEmptyPanel)
#include "test_terminal_empty_panel.moc"
