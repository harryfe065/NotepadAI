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

#include "TerminalEmptyPanelPolicy.h"

using Policy = TerminalEmptyPanelPolicy;
using Input = TerminalEmptyPanelPolicy::Input;

// Seam: TerminalEmptyPanelPolicy::shouldShow() — the whole show/hide decision
// for the terminal empty panel, as a pure function of the five inputs
// TerminalManager can observe.
class TestTerminalEmptyPanelPolicy : public QObject
{
    Q_OBJECT

private slots:
    void allClear_shows();
    void settingOff_hides();
    void noActiveWorkspace_hides();
    void suppressed_hides();
    void terminalVisible_hides();
    void shuttingDown_hides();

    void workspaceKey_local_normalizesCaseAndSlash();
    void workspaceKey_ssh_keepsUriAndCase();
    void workspaceKey_empty_isEmpty();
};

static Input allClear()
{
    Input in;
    in.settingEnabled = true;
    in.hasActiveWorkspace = true;
    in.suppressedForActiveWorkspace = false;
    in.anyTerminalVisible = false;
    in.shuttingDown = false;
    return in;
}

void TestTerminalEmptyPanelPolicy::allClear_shows()
{
    QCOMPARE(Policy::shouldShow(allClear()), true);
}

void TestTerminalEmptyPanelPolicy::settingOff_hides()
{
    Input in = allClear();
    in.settingEnabled = false;
    QCOMPARE(Policy::shouldShow(in), false);
}

void TestTerminalEmptyPanelPolicy::noActiveWorkspace_hides()
{
    Input in = allClear();
    in.hasActiveWorkspace = false;
    QCOMPARE(Policy::shouldShow(in), false);
}

void TestTerminalEmptyPanelPolicy::suppressed_hides()
{
    Input in = allClear();
    in.suppressedForActiveWorkspace = true;
    QCOMPARE(Policy::shouldShow(in), false);
}

void TestTerminalEmptyPanelPolicy::terminalVisible_hides()
{
    Input in = allClear();
    in.anyTerminalVisible = true;
    QCOMPARE(Policy::shouldShow(in), false);
}

void TestTerminalEmptyPanelPolicy::shuttingDown_hides()
{
    Input in = allClear();
    in.shuttingDown = true;
    QCOMPARE(Policy::shouldShow(in), false);
}

void TestTerminalEmptyPanelPolicy::workspaceKey_local_normalizesCaseAndSlash()
{
#ifdef Q_OS_WIN
    QCOMPARE(Policy::workspaceKey(QStringLiteral("D:/Ws/App/")),
             QStringLiteral("d:/ws/app"));
#else
    QCOMPARE(Policy::workspaceKey(QStringLiteral("/Ws/App/")),
             QStringLiteral("/Ws/App"));
#endif
}

void TestTerminalEmptyPanelPolicy::workspaceKey_ssh_keepsUriAndCase()
{
    QCOMPARE(Policy::workspaceKey(QStringLiteral("ssh://Prod/opt/App/")),
             QStringLiteral("ssh://Prod/opt/App"));
}

void TestTerminalEmptyPanelPolicy::workspaceKey_empty_isEmpty()
{
    QCOMPARE(Policy::workspaceKey(QString()), QString());
}

QTEST_APPLESS_MAIN(TestTerminalEmptyPanelPolicy)
#include "test_terminal_empty_panel_policy.moc"
