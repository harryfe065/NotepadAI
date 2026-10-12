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

#include "TerminalTabTitle.h"

// Tab title is the shell cwd, never the process image (Windows ConPTY reports
// pwsh.exe / cmd.exe as the window title). Drive roots stay "d:\" not empty
// basename; empty cwd falls back to "Terminal".
class TestTerminalTabTitle : public QObject
{
    Q_OBJECT

private slots:
    void emptyCwd_fallsBackToTerminal();
    void driveRoot_keepsNativeRoot();
    void nestedPath_isFullNativeCwd();

    void cwdLabel_isFolderName();
    void shellImageTitles_areIgnored();
    void programTitle_isShown();
    void customName_winsAndIsElided();
};

void TestTerminalTabTitle::cwdLabel_isFolderName()
{
    QCOMPARE(terminalCwdLabel(QStringLiteral("/home/foo/proj")), QStringLiteral("proj"));
    QCOMPARE(terminalCwdLabel(QString()), QStringLiteral("Terminal"));
#ifdef Q_OS_WIN
    QCOMPARE(terminalCwdLabel(QStringLiteral("D:/")), QStringLiteral("D:\\"));
#else
    QCOMPARE(terminalCwdLabel(QStringLiteral("/")), QStringLiteral("/"));
#endif
}

void TestTerminalTabTitle::shellImageTitles_areIgnored()
{
    const QString pwsh = QStringLiteral("C:/Program Files/PowerShell/7/pwsh.exe");
    QVERIFY(isShellImageTitle(QStringLiteral("C:\\Program Files\\PowerShell\\7\\pwsh.exe"), pwsh));
    QVERIFY(isShellImageTitle(QStringLiteral("Administrator: C:\\WINDOWS\\system32\\cmd.exe"), pwsh));
    QVERIFY(isShellImageTitle(QStringLiteral("pwsh"), pwsh));
    QVERIFY(isShellImageTitle(QStringLiteral("  "), pwsh));
    QVERIFY(isShellImageTitle(QStringLiteral("bash"), QStringLiteral("/bin/bash")));
    QVERIFY(!isShellImageTitle(QStringLiteral("vim main.cpp"), pwsh));
    QVERIFY(!isShellImageTitle(QStringLiteral("htop"), QStringLiteral("/bin/bash")));
}

void TestTerminalTabTitle::programTitle_isShown()
{
    const QString shell = QStringLiteral("/bin/zsh");
    QCOMPARE(terminalDisplayTitle(QString(), QStringLiteral("vim notes.md"), shell, QStringLiteral("/home/a/proj")),
             QStringLiteral("vim notes.md"));
    QCOMPARE(terminalDisplayTitle(QString(), QStringLiteral("zsh"), shell, QStringLiteral("/home/a/proj")),
             QStringLiteral("proj"));
}

void TestTerminalTabTitle::customName_winsAndIsElided()
{
    const QString shell = QStringLiteral("/bin/zsh");
    QCOMPARE(terminalDisplayTitle(QStringLiteral(" build "), QStringLiteral("vim"), shell, QStringLiteral("/x")),
             QStringLiteral("build"));
    const QString title = terminalDisplayTitle(QString(), QStringLiteral("a very long program title that keeps going"),
                                               shell, QStringLiteral("/x"));
    QCOMPARE(title.size(), 25);
    QVERIFY(title.endsWith(QChar(0x2026)));
}

void TestTerminalTabTitle::emptyCwd_fallsBackToTerminal()
{
    QCOMPARE(terminalTabTitle(QString()), QStringLiteral("Terminal"));
}

void TestTerminalTabTitle::driveRoot_keepsNativeRoot()
{
#ifdef Q_OS_WIN
    QCOMPARE(terminalTabTitle(QStringLiteral("d:/")), QStringLiteral("d:\\"));
    QCOMPARE(terminalTabTitle(QStringLiteral("d:\\")), QStringLiteral("d:\\"));
#else
    QCOMPARE(terminalTabTitle(QStringLiteral("/")), QStringLiteral("/"));
#endif
}

void TestTerminalTabTitle::nestedPath_isFullNativeCwd()
{
#ifdef Q_OS_WIN
    QCOMPARE(terminalTabTitle(QStringLiteral("C:/Users/foo/proj")),
             QStringLiteral("C:\\Users\\foo\\proj"));
#else
    QCOMPARE(terminalTabTitle(QStringLiteral("/home/foo/proj")),
             QStringLiteral("/home/foo/proj"));
#endif
}

QTEST_MAIN(TestTerminalTabTitle)
#include "test_terminal_tab_title.moc"
