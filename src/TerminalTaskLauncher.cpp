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

#include "TerminalTaskLauncher.h"

#include <QFileInfo>

namespace {

const QChar kEsc(0x1B);
const QChar kBel(0x07);

QString shellBaseName(const QString &shellPath)
{
    // completeBaseName drops ".exe"; Windows names compare case-insensitively.
    return QFileInfo(shellPath).completeBaseName().toLower();
}

// POSIX single quotes: everything literal except the quote itself.
QString singleQuote(const QString &s)
{
    QString out = s;
    out.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + out + QLatin1Char('\'');
}

// The finished text split around its %1 placeholder.
void splitFinished(const QString &finishedText, QString *prefix, QString *suffix)
{
    const int at = finishedText.indexOf(QStringLiteral("%1"));
    if (at < 0) {
        *prefix = finishedText + QLatin1Char(' ');
        suffix->clear();
        return;
    }
    *prefix = finishedText.left(at);
    *suffix = finishedText.mid(at + 2);
}

QString powerShellScript(const QString &command, const QString &finishedText)
{
    QString prefix;
    QString suffix;
    splitFinished(finishedText, &prefix, &suffix);
    auto psQuote = [](QString s) {
        s.replace(QLatin1Char('\''), QStringLiteral("''"));
        return QLatin1Char('\'') + s + QLatin1Char('\'');
    };
    // $? must be read on the line right after the command.
    return command + QStringLiteral(
               "\n$__npOk = $?; $__npCode = $LASTEXITCODE\n"
               "$__npExit = if ($__npOk) { 0 } elseif ($__npCode) { $__npCode } else { 1 }\n"
               "Write-Host (\"$([char]27)]%1;$__npExit$([char]7)$([char]27)[2m\" + %2 + $__npExit + %3 + \"$([char]27)[22m\")\n"
               "Remove-Variable __npOk, __npCode, __npExit -ErrorAction SilentlyContinue")
        .arg(kTerminalTaskExitOsc)
        .arg(psQuote(prefix), psQuote(suffix));
}

QString posixScript(const QString &command, const QString &shellPath, const QString &finishedText, bool fish)
{
    QString prefix;
    QString suffix;
    splitFinished(finishedText, &prefix, &suffix);
    const QString status = fish ? QStringLiteral("set __np_status $status")
                                : QStringLiteral("__np_status=$?");
    const QString value = fish ? QStringLiteral("$__np_status") : QStringLiteral("\"$__np_status\"");
    return command + QLatin1Char('\n') + status + QLatin1Char('\n')
        + QStringLiteral("printf '\\033]%1;%d\\007\\033[2m%s%d%s\\033[22m\\n' %2 %3 %2 %4\n")
              .arg(kTerminalTaskExitOsc)
              .arg(value, singleQuote(prefix), singleQuote(suffix))
        + QStringLiteral("exec ") + singleQuote(shellPath) + QStringLiteral(" -i");
}

} // namespace

TerminalShellKind terminalShellKind(const QString &shellPath)
{
    const QString base = shellBaseName(shellPath);
    if (base == QLatin1String("pwsh") || base == QLatin1String("powershell"))
        return TerminalShellKind::PowerShell;
    if (base == QLatin1String("cmd"))
        return TerminalShellKind::Cmd;
    if (base == QLatin1String("bash") || base == QLatin1String("zsh") || base == QLatin1String("sh")
        || base == QLatin1String("dash") || base == QLatin1String("ksh") || base == QLatin1String("ash"))
        return TerminalShellKind::Posix;
    if (base == QLatin1String("fish"))
        return TerminalShellKind::Fish;
    return TerminalShellKind::Other;
}

QString quoteWindowsArgument(const QString &arg)
{
    if (!arg.isEmpty() && !arg.contains(QLatin1Char(' ')) && !arg.contains(QLatin1Char('\t'))
        && !arg.contains(QLatin1Char('"')) && !arg.contains(QLatin1Char('\n'))) {
        return arg;
    }
    QString out = QStringLiteral("\"");
    int backslashes = 0;
    for (const QChar c : arg) {
        if (c == QLatin1Char('\\')) {
            ++backslashes;
            continue;
        }
        if (c == QLatin1Char('"')) {
            out += QString(backslashes * 2 + 1, QLatin1Char('\\'));
            out += c;
        } else {
            out += QString(backslashes, QLatin1Char('\\'));
            out += c;
        }
        backslashes = 0;
    }
    out += QString(backslashes * 2, QLatin1Char('\\'));
    out += QLatin1Char('"');
    return out;
}

QString sanitizeTerminalPaste(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        const char16_t c = text.at(i).unicode();
        if (c == u'\r') {
            out.append(QLatin1Char('\r'));
            if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('\n')) ++i;
            continue;
        }
        if (c == u'\n') {
            out.append(QLatin1Char('\r'));
            continue;
        }
        if (c == u'\t' || (c >= 0x20 && c != 0x7F && (c < 0x80 || c > 0x9F))) {
            out.append(text.at(i));
        }
    }
    return out;
}

QString quotePathForShell(const QString &path, TerminalShellKind kind)
{
    static const QString kPlain = QStringLiteral(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/\\:._-+@");
    bool plain = !path.isEmpty();
    for (const QChar c : path) {
        if (!kPlain.contains(c)) {
            plain = false;
            break;
        }
    }
    // A bare backslash is an escape for POSIX shells, so only Windows shells
    // may leave it unquoted.
    if (plain && (kind == TerminalShellKind::PowerShell || kind == TerminalShellKind::Cmd
                  || !path.contains(QLatin1Char('\\')))) {
        return path;
    }
    switch (kind) {
    case TerminalShellKind::PowerShell: {
        // PowerShell also closes single-quoted strings on the typographic
        // quotes U+2018-U+201B; double every one of them.
        QString s;
        s.reserve(path.size() + 2);
        for (const QChar c : path) {
            s.append(c);
            if (c == QLatin1Char('\'') || (c.unicode() >= 0x2018 && c.unicode() <= 0x201B)) {
                s.append(c);
            }
        }
        return QLatin1Char('\'') + s + QLatin1Char('\'');
    }
    case TerminalShellKind::Cmd:
        // cmd has no escape inside quotes; a double quote cannot occur in a
        // Windows file name anyway.
        return QLatin1Char('"') + path + QLatin1Char('"');
    case TerminalShellKind::Posix:
    case TerminalShellKind::Fish:
    case TerminalShellKind::Other:
        break;
    }
    return singleQuote(path);
}

TerminalTaskLaunch terminalTaskLaunch(const QString &shellPath, const QString &command,
                                      const QString &finishedText, bool windowsCommandLine)
{
    TerminalTaskLaunch launch;
    switch (terminalShellKind(shellPath)) {
    case TerminalShellKind::PowerShell: {
        // -EncodedCommand sidesteps every quoting layer: UTF-16LE, base64.
        const QString script = powerShellScript(command, finishedText);
        const QByteArray utf16(reinterpret_cast<const char *>(script.utf16()),
                               script.size() * 2);
        launch.arguments = {QStringLiteral("-NoLogo"), QStringLiteral("-NoExit"),
                            QStringLiteral("-EncodedCommand"), QString::fromLatin1(utf16.toBase64())};
        launch.reportsExit = true;
        break;
    }
    case TerminalShellKind::Cmd: {
        if (!windowsCommandLine) break;
        QString prefix;
        QString suffix;
        splitFinished(finishedText, &prefix, &suffix);
        // /s strips only the outer quotes and runs the rest verbatim. `call`
        // re-expands %^errorlevel% after the command has run.
        const QString code = QStringLiteral("%^errorlevel%");
        const QString report = QStringLiteral(" & call echo ") + kEsc + QStringLiteral("]%1;").arg(kTerminalTaskExitOsc)
            + code + kBel + kEsc + QStringLiteral("[2m") + prefix + code + suffix + kEsc + QStringLiteral("[22m");
        launch.arguments = {QStringLiteral("/s"), QStringLiteral("/k"),
                            QLatin1Char('"') + command + report + QLatin1Char('"')};
        launch.reportsExit = true;
        break;
    }
    case TerminalShellKind::Posix:
    case TerminalShellKind::Fish: {
        const bool fish = terminalShellKind(shellPath) == TerminalShellKind::Fish;
        const QString script = posixScript(command, shellPath, finishedText, fish);
        launch.arguments = {QStringLiteral("-c"), script};
        if (windowsCommandLine) {
            for (QString &arg : launch.arguments) {
                arg = quoteWindowsArgument(arg);
            }
        }
        launch.reportsExit = true;
        break;
    }
    case TerminalShellKind::Other:
        break;
    }
    return launch;
}
