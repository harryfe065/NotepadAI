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

#include "TerminalTaskLauncher.h"

class TestTerminalTaskLauncher : public QObject
{
    Q_OBJECT

private slots:
    void shellKinds();
    void powerShell_isEncodedAndStaysOpen();
    void cmd_reportsAfterCommandVerbatim();
    void posix_reportsAndExecsInteractiveShell();
    void posix_quotesForWindowsCommandLine();
    void unknownShell_fallsBackToTyping();
    void windowsQuoting();
    void dropQuoting();
    void dropQuoting_powerShellSmartQuotes();
    void dropQuoting_posixBackslashIsQuoted();
    void paste_stripsControlSequences();
    void paste_normalisesLineBreaks();
};

void TestTerminalTaskLauncher::shellKinds()
{
    QCOMPARE(terminalShellKind(QStringLiteral("C:/Program Files/PowerShell/7/pwsh.exe")), TerminalShellKind::PowerShell);
    QCOMPARE(terminalShellKind(QStringLiteral("C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe")), TerminalShellKind::PowerShell);
    QCOMPARE(terminalShellKind(QStringLiteral("C:/Windows/System32/CMD.EXE")), TerminalShellKind::Cmd);
    QCOMPARE(terminalShellKind(QStringLiteral("/bin/bash")), TerminalShellKind::Posix);
    QCOMPARE(terminalShellKind(QStringLiteral("/usr/bin/zsh")), TerminalShellKind::Posix);
    QCOMPARE(terminalShellKind(QStringLiteral("/usr/local/bin/fish")), TerminalShellKind::Fish);
    QCOMPARE(terminalShellKind(QStringLiteral("/usr/bin/nu")), TerminalShellKind::Other);
}

void TestTerminalTaskLauncher::powerShell_isEncodedAndStaysOpen()
{
    const TerminalTaskLaunch l = terminalTaskLaunch(QStringLiteral("C:/x/pwsh.exe"), QStringLiteral("npm test"),
                                                    QStringLiteral("Task finished (exit code %1)"), true);
    QVERIFY(l.reportsExit);
    QCOMPARE(l.arguments.size(), 4);
    QCOMPARE(l.arguments.at(1), QStringLiteral("-NoExit"));
    QCOMPARE(l.arguments.at(2), QStringLiteral("-EncodedCommand"));
    const QByteArray bytes = QByteArray::fromBase64(l.arguments.at(3).toLatin1());
    const QString script = QString::fromUtf16(reinterpret_cast<const char16_t *>(bytes.constData()), bytes.size() / 2);
    QVERIFY(script.startsWith(QStringLiteral("npm test\n$__npOk = $?")));
    QVERIFY(script.contains(QStringLiteral("]%1;").arg(kTerminalTaskExitOsc)));
    QVERIFY(script.contains(QStringLiteral("'Task finished (exit code '")));
}

void TestTerminalTaskLauncher::cmd_reportsAfterCommandVerbatim()
{
    const TerminalTaskLaunch l = terminalTaskLaunch(QStringLiteral("C:/Windows/System32/cmd.exe"),
                                                    QStringLiteral("make \"a b\" && echo ok"),
                                                    QStringLiteral("done %1"), true);
    QVERIFY(l.reportsExit);
    QCOMPARE(l.arguments.at(0), QStringLiteral("/s"));
    QCOMPARE(l.arguments.at(1), QStringLiteral("/k"));
    const QString line = l.arguments.at(2);
    QVERIFY(line.startsWith(QStringLiteral("\"make \"a b\" && echo ok & call echo ")));
    QVERIFY(line.contains(QStringLiteral("%^errorlevel%")));
    QVERIFY(line.endsWith(QLatin1Char('"')));
    // Not offered where cmd's verbatim command line does not exist.
    QVERIFY(terminalTaskLaunch(QStringLiteral("/x/cmd"), QStringLiteral("x"), QStringLiteral("%1"), false).arguments.isEmpty());
}

void TestTerminalTaskLauncher::posix_reportsAndExecsInteractiveShell()
{
    const TerminalTaskLaunch l = terminalTaskLaunch(QStringLiteral("/bin/bash"), QStringLiteral("make it's"),
                                                    QStringLiteral("Task finished (exit code %1)"), false);
    QVERIFY(l.reportsExit);
    QCOMPARE(l.arguments.size(), 2);
    QCOMPARE(l.arguments.at(0), QStringLiteral("-c"));
    const QString script = l.arguments.at(1);
    QVERIFY(script.startsWith(QStringLiteral("make it's\n__np_status=$?\n")));
    QVERIFY(script.contains(QStringLiteral("'Task finished (exit code '")));
    QVERIFY(script.endsWith(QStringLiteral("exec '/bin/bash' -i")));

    const TerminalTaskLaunch fish = terminalTaskLaunch(QStringLiteral("/usr/bin/fish"), QStringLiteral("make"),
                                                       QStringLiteral("x %1"), false);
    QVERIFY(fish.arguments.at(1).contains(QStringLiteral("set __np_status $status")));
}

void TestTerminalTaskLauncher::posix_quotesForWindowsCommandLine()
{
    const TerminalTaskLaunch l = terminalTaskLaunch(QStringLiteral("C:/Git/bin/bash.exe"), QStringLiteral("echo \"hi\""),
                                                    QStringLiteral("%1"), true);
    QCOMPARE(l.arguments.at(0), QStringLiteral("-c"));
    QVERIFY(l.arguments.at(1).startsWith(QLatin1Char('"')));
    QVERIFY(l.arguments.at(1).contains(QStringLiteral("echo \\\"hi\\\"")));
}

void TestTerminalTaskLauncher::unknownShell_fallsBackToTyping()
{
    const TerminalTaskLaunch l = terminalTaskLaunch(QStringLiteral("/usr/bin/nu"), QStringLiteral("ls"),
                                                    QStringLiteral("%1"), false);
    QVERIFY(l.arguments.isEmpty());
    QVERIFY(!l.reportsExit);
}

void TestTerminalTaskLauncher::windowsQuoting()
{
    QCOMPARE(quoteWindowsArgument(QStringLiteral("plain")), QStringLiteral("plain"));
    QCOMPARE(quoteWindowsArgument(QString()), QStringLiteral("\"\""));
    QCOMPARE(quoteWindowsArgument(QStringLiteral("a b")), QStringLiteral("\"a b\""));
    QCOMPARE(quoteWindowsArgument(QStringLiteral("say \"x\"")), QStringLiteral("\"say \\\"x\\\"\""));
    // Trailing backslashes double before the closing quote.
    QCOMPARE(quoteWindowsArgument(QStringLiteral("C:\\a b\\")), QStringLiteral("\"C:\\a b\\\\\""));
}

void TestTerminalTaskLauncher::dropQuoting()
{
    QCOMPARE(quotePathForShell(QStringLiteral("/tmp/a.txt"), TerminalShellKind::Posix), QStringLiteral("/tmp/a.txt"));
    QCOMPARE(quotePathForShell(QStringLiteral("/tmp/my file"), TerminalShellKind::Posix), QStringLiteral("'/tmp/my file'"));
    QCOMPARE(quotePathForShell(QStringLiteral("/tmp/it's"), TerminalShellKind::Fish), QStringLiteral("'/tmp/it'\\''s'"));
    QCOMPARE(quotePathForShell(QStringLiteral("C:/a b/c.txt"), TerminalShellKind::PowerShell), QStringLiteral("'C:/a b/c.txt'"));
    QCOMPARE(quotePathForShell(QStringLiteral("C:/o'k"), TerminalShellKind::PowerShell), QStringLiteral("'C:/o''k'"));
    QCOMPARE(quotePathForShell(QStringLiteral("C:/a b"), TerminalShellKind::Cmd), QStringLiteral("\"C:/a b\""));
    QCOMPARE(quotePathForShell(QStringLiteral("C:/plain"), TerminalShellKind::Cmd), QStringLiteral("C:/plain"));
}

void TestTerminalTaskLauncher::dropQuoting_powerShellSmartQuotes()
{
    // PowerShell ends a single-quoted string on U+2018..U+201B as well as on
    // ', so each one is doubled.
    const QString path = QStringLiteral("C:/a") + QChar(0x2019) + QStringLiteral("b");
    const QString expected = QStringLiteral("'C:/a") + QChar(0x2019) + QChar(0x2019) + QStringLiteral("b'");
    QCOMPARE(quotePathForShell(path, TerminalShellKind::PowerShell), expected);
}

void TestTerminalTaskLauncher::dropQuoting_posixBackslashIsQuoted()
{
    // A bare backslash escapes the next character in POSIX shells.
    QCOMPARE(quotePathForShell(QStringLiteral("a\\b"), TerminalShellKind::Posix), QStringLiteral("'a\\b'"));
    QCOMPARE(quotePathForShell(QStringLiteral("C:\\x"), TerminalShellKind::Cmd), QStringLiteral("C:\\x"));
}

void TestTerminalTaskLauncher::paste_stripsControlSequences()
{
    // ESC [201~ would end bracketed paste and run the rest as typed input.
    const QString evil = QStringLiteral("ls\x1b[201~rm -rf /\x07\x7f") + QChar(0x9B) + QStringLiteral("x");
    QCOMPARE(sanitizeTerminalPaste(evil), QStringLiteral("ls[201~rm -rf /x"));
    QCOMPARE(sanitizeTerminalPaste(QStringLiteral("a\tb")), QStringLiteral("a\tb"));
    QCOMPARE(sanitizeTerminalPaste(QStringLiteral("caf") + QChar(0xE9)), QStringLiteral("caf") + QChar(0xE9));
}

void TestTerminalTaskLauncher::paste_normalisesLineBreaks()
{
    QCOMPARE(sanitizeTerminalPaste(QStringLiteral("a\r\nb\nc\rd")), QStringLiteral("a\rb\rc\rd"));
}

QTEST_APPLESS_MAIN(TestTerminalTaskLauncher)

#include "test_terminal_task_launcher.moc"
