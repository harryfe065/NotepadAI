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

#ifndef SSH_ACP_PROCESS_CHANNEL_H
#define SSH_ACP_PROCESS_CHANNEL_H

#include <QByteArray>
#include <QPointer>
#include <QString>
#include <QStringList>

#include "IAcpExecHost.h"
#include "IAcpProcessChannel.h"

// IAcpProcessChannel over an SSH exec channel (D8). It runs the agent on the
// remote host by exec'ing
//
//     cd <cwd> && exec <command> <args...>
//
// (every token POSIX single-quoted) over one exec channel of the multiplexed SSH
// connection, then speaks JSON-RPC over that channel's stdin/stdout exactly like
// the local QProcess path. start() opens the channel via IAcpExecHost::execStart;
// write() appends a frame via execWrite (stdin stays open for the whole session —
// the agent reads frames continuously, so no EOF is needed); kill() / destruction
// tears the channel down via execCancel. stdout/stderr/exit are surfaced through
// the host's reqId-keyed relay signals and re-emitted on the IAcpProcessChannel
// surface, so AcpConnection's framing/dispatch is unchanged.
//
// The host is NOT owned (it is the SshConnection facade, owned by the registry).
// All libssh2 work happens on the connection's worker thread; this object lives
// on the UI thread and only posts queued requests / receives queued results.
class SshAcpProcessChannel : public IAcpProcessChannel
{
    Q_OBJECT

public:
    SshAcpProcessChannel(IAcpExecHost *host,
                         QString command,
                         QStringList args,
                         QString workingDirectory,
                         QObject *parent = nullptr);
    ~SshAcpProcessChannel() override;

    void start() override;
    void write(const QByteArray &bytes) override;
    void kill() override;
    bool isRunning() const override { return m_running; }

    // POSIX single-quote one token (close/escape/reopen for embedded quotes) —
    // identical rule to RemoteGitProcessRunner::shellQuote. Defined inline so
    // tests can assert it without linking the SSH stack.
    static QString shellQuote(const QString &token)
    {
        QString out;
        out.reserve(token.size() + 2);
        out.append(QLatin1Char('\''));
        for (const QChar c : token) {
            if (c == QLatin1Char('\'')) {
                out.append(QLatin1String("'\\''"));
            } else {
                out.append(c);
            }
        }
        out.append(QLatin1Char('\''));
        return out;
    }

    // Build a remote launch command that probes for the binary in the default
    // PATH, then bash's login PATH, then bash's interactive (.bashrc) PATH,
    // and exec's through whichever shell found it. The structure is:
    //   if command -v <cmd> >/dev/null 2>&1; then
    //     cd <cwd> && exec <cmd> <args>
    //   elif bash -lc 'command -v "$0"' <cmd> </dev/null >/dev/null 2>&1; then
    //     exec bash -lc 'exec 0<&4 4<&- 1>&3 3>&-; cd <cwd> && exec <cmd> <args>'     //       4<&0 3>&1 0</dev/null 1>/dev/null
    //   elif bash -ic 'command -v "$0"' <cmd> </dev/null >/dev/null 2>&1; then
    //     exec bash -ic 'exec 0<&4 4<&- 1>&3 3>&-; cd <cwd> && exec <cmd> <args>' \
    //       4<&0 3>&1 0</dev/null 1>/dev/null
    //   else
    //     exit 127
    //   fi
    // Three tiers:
    //  1. Default PATH — no bash needed, no startup files, clean stdio.
    //  2. bash -lc — login, non-interactive. Sources /etc/profile +
    //     ~/.bash_profile (or ~/.profile). Covers profile-based nvm installs.
    //     Stdout and stdin protected during startup like tier 3 (profiles can
    //     echo or read); restored (0<&4 4<&- 1>&3 3>&-) before exec.
    //  3. bash -ic — interactive, non-login. Sources ~/.bashrc directly.
    //     Covers the common case where nvm wrote to ~/.bashrc and .bashrc has
    //     a non-interactive early-return guard that blocks -lc from reaching
    //     the nvm init. Both stdout AND stdin are protected: stdout via fd3,
    //     stdin via fd4 (interactive bash readline init can consume bytes).
    //     The condition probe also gets </dev/null to prevent stdin consumption
    //     before the branch is chosen. Restored (0<&4 4<&- 1>&3 3>&-) before
    //     exec so the agent owns clean stdio. Uses POSIX fd-dup (>&N, <&N)
    //     instead of /dev/fd/N for portability (absent on minimal containers).
    // Once a branch is chosen, `exec` replaces the shell — no fallback can
    // fire after the agent starts.
    static QString buildRemoteCommand(const QString &cwd, const QString &command,
                                      const QStringList &args)
    {
        const QString qcmd = shellQuote(command);
        QString inner;
        if (!cwd.isEmpty()) {
            inner += QStringLiteral("cd ") + shellQuote(cwd) + QStringLiteral(" && ");
        }
        inner += QStringLiteral("exec ") + qcmd;
        for (const QString &arg : args) {
            inner += QLatin1Char(' ') + shellQuote(arg);
        }
        QString cmd;
        cmd += QStringLiteral("if command -v ") + qcmd + QStringLiteral(" >/dev/null 2>&1; then ");
        cmd += inner + QStringLiteral("; ");
        cmd += QStringLiteral("elif bash -lc 'command -v \"$0\"' ") + qcmd + QStringLiteral(" </dev/null >/dev/null 2>&1; then ");
        cmd += QStringLiteral("exec bash -lc ")
             + shellQuote(QStringLiteral("exec 0<&4 4<&- 1>&3 3>&-; ") + inner)
             + QStringLiteral(" 4<&0 3>&1 0</dev/null 1>/dev/null; ");
        cmd += QStringLiteral("elif bash -ic 'command -v \"$0\"' ") + qcmd + QStringLiteral(" </dev/null >/dev/null 2>&1; then ");
        cmd += QStringLiteral("exec bash -ic ")
             + shellQuote(QStringLiteral("exec 0<&4 4<&- 1>&3 3>&-; ") + inner)
             + QStringLiteral(" 4<&0 3>&1 0</dev/null 1>/dev/null; ");
        cmd += QStringLiteral("else exit 127; fi");
        return cmd;
    }

private:
    void onExecStdout(quint64 reqId, const QByteArray &chunk);
    void onExecStderr(quint64 reqId, const QByteArray &chunk);
    void onExecDone(quint64 reqId, int exitStatus);

    QPointer<IAcpExecHost> m_host; // not owned
    QString m_command;
    QStringList m_args;
    QString m_workingDirectory;
    quint64 m_reqId = 0; // 0 = not started / finished
    bool m_running = false;
};

#endif // SSH_ACP_PROCESS_CHANNEL_H
