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

// Offline coverage of ACP-over-SSH (D8), all driven by FakeSshTransport — NO
// network, no sshd, deterministic:
//
//   1. SshAcpProcessChannel::buildRemoteCommand / shellQuote — the pure
//      `cd <cwd> && exec <command> <args...>` construction with POSIX quoting.
//   2. The exec-channel IO adapter end-to-end: write(frame) → bytes captured on
//      the channel (chWritten); scripted stdout → readyReadStdout; channel exit →
//      finished. Driven through a real SshSessionWorker exec engine wrapped in a
//      WorkerExecHost (the worker is the production data path; SshAcpProcessChannel
//      is a thin reqId→signal adapter over IAcpExecHost, which SshConnectionExecHost
//      forwards to the worker 1:1 — so this exercises the full path without the
//      libssh2 / mbedTLS / CredentialStore stack a live SshConnection needs).
//   3. The remote binary probe: a probe-MISS (`command -v` exits non-zero) emits
//      a clear error through AcpConnection's error path and spawns NOTHING — no
//      local fallback; a probe-HIT consults the injected channel builder and
//      starts the channel.

#include <QtTest>
#include <QByteArray>
#include <QSignalSpy>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

#include "AcpAgentDefinition.h"
#include "AcpConnection.h"
#include "AcpErrorClassifier.h"
#include "IAcpExecHost.h"
#include "IAcpProcessChannel.h"
#include "SshAcpProcessChannel.h"

#include "remote/ExecutionContext.h"
#include "remote/ISshTransport.h"
#include "remote/SshSessionWorker.h"

#include "FakeSshTransport.h"

using namespace remote;

// PLACEHOLDER_HELPERS

static SshSessionWorker::ConnectParams agentParams()
{
    SshSessionWorker::ConnectParams p;
    p.host = QStringLiteral("host.example");
    p.port = 22;
    p.username = QStringLiteral("alice");
    p.authMethod = SshProfile::AuthMethod::Agent;
    return p;
}

static ISshTransport::ReadResult dataChunk(const char *s)
{
    ISshTransport::ReadResult r;
    r.data = QByteArray(s);
    return r;
}

static ISshTransport::ReadResult eofResult()
{
    ISshTransport::ReadResult r;
    r.eof = true;
    return r;
}

// An IAcpExecHost backed by a real SshSessionWorker + FakeSshTransport, driven
// directly on the test thread (no QThread / no live SshConnection). This is the
// production exec data path; SshConnectionExecHost is the same forward in
// production, so wrapping the worker here exercises SshAcpProcessChannel's full
// IO without the libssh2 stack — same isolation rationale as
// test_remote_git_runner. reqId is minted host-side (mirroring SshConnection).
class WorkerExecHost : public IAcpExecHost
{
public:
    explicit WorkerExecHost(QObject *parent = nullptr)
        : IAcpExecHost(parent)
    {
        auto fake = std::make_unique<FakeSshTransport>();
        m_fake = fake.get();
        m_worker = new SshSessionWorker(std::move(fake), /*channelCap=*/10, this);
        connect(m_worker, &SshSessionWorker::execStdoutChunk, this, &IAcpExecHost::execStdout);
        connect(m_worker, &SshSessionWorker::execStderrChunk, this, &IAcpExecHost::execStderr);
        connect(m_worker, &SshSessionWorker::execDone, this, &IAcpExecHost::execDone);
        // Drive the connect/auth state machine to Ready (host-key gate accepted).
        m_worker->startConnect(agentParams());
        m_worker->acceptHostKey();
    }

    FakeSshTransport *fake() const { return m_fake; }
    SshSessionWorker *worker() const { return m_worker; }

    quint64 execStart(const QString &command, const QByteArray &stdinPayload) override
    {
        const quint64 reqId = ++m_nextReqId;
        m_worker->requestExec(reqId, command, stdinPayload); // services synchronously
        return reqId;
    }
    void execWrite(quint64 reqId, const QByteArray &bytes) override
    {
        m_worker->requestExecWrite(reqId, bytes);
    }
    void execCancel(quint64 reqId) override { m_worker->requestExecCancel(reqId); }

private:
    FakeSshTransport *m_fake = nullptr;     // owned by m_worker
    SshSessionWorker *m_worker = nullptr;   // child of this
    quint64 m_nextReqId = 0;
};

// A remote ExecutionContext whose exec() resolves synchronously with a scripted
// exit code (so the probe path is deterministic, no event loop). isRemote()==true
// so AcpConnection takes the remote branch. createPty/createGitRunner/fsBackend
// are unused by the probe path.
class FakeRemoteContext : public ExecutionContext
{
public:
    int probeExitCode = 127;          // default: binary absent (command -v miss)
    QByteArray probeStdout;
    QStringList lastExecArgv;
    int execCalls = 0;

    bool isRemote() const override { return true; }
    QString displayName() const override { return QStringLiteral("alice@host.example"); }
    State state() const override { return State::Connected; }
    IPtyProcess *createPty(QObject *) override { return nullptr; }
    IGitProcessRunner *createGitRunner(QObject *) override { return nullptr; }
    IFileSystemBackend *fsBackend() override { return nullptr; }
    QString resolveCwd(const QString &requested) const override { return requested; }

    void exec(const QString &, const QStringList &argv, const QByteArray &,
              int, ExecCallback cb) override
    {
        ++execCalls;
        lastExecArgv = argv;
        if (cb) {
            cb(probeExitCode, probeStdout, QByteArray());
        }
    }
};

class TestAcpRemoteTransport : public QObject
{
    Q_OBJECT

private slots:
    // command construction (pure)
    void command_cdPrefixExecAndQuoting();
    void command_emptyCwdSkipsCd();
    void command_embeddedSingleQuoteEscaped();

    // exec-channel IO adapter
    void channel_writeFrameReachesChannel();
    void channel_scriptedStdoutSurfacesReadyRead();
    void channel_exitSurfacesFinished();

    // binary probe
    void probe_missEmitsErrorAndSpawnsNothing();
    void probe_hitBuildsChannelAndStarts();
};

// --- command construction -----------------------------------------------------

namespace {

// The remote command tries the default PATH, then bash -lc, then bash -ic
// (SshAcpProcessChannel::buildRemoteCommand). The first tier is the plain
// `cd ... && exec ...` that runs when the binary is on the default PATH.
QString directTier(const QString &cmd)
{
    const QString then = QStringLiteral(" >/dev/null 2>&1; then ");
    const int from = cmd.indexOf(then);
    const int to = cmd.indexOf(QStringLiteral("; elif bash -lc "));
    if (!cmd.startsWith(QStringLiteral("if command -v ")) || from < 0 || to < from
        || !cmd.endsWith(QStringLiteral("; else exit 127; fi"))) {
        return QStringLiteral("<unexpected shape: %1>").arg(cmd);
    }
    return cmd.mid(from + then.size(), to - from - then.size());
}

} // namespace

void TestAcpRemoteTransport::command_cdPrefixExecAndQuoting()
{
    const QString cmd = SshAcpProcessChannel::buildRemoteCommand(
        QStringLiteral("/home/alice/my project"),
        QStringLiteral("claude-code-acp"),
        { QStringLiteral("--acp"), QStringLiteral("--flag=v 2") });
    QCOMPARE(directTier(cmd),
             QStringLiteral("cd '/home/alice/my project' && exec 'claude-code-acp' "
                            "'--acp' '--flag=v 2'"));
    // The shell fallbacks run the same thing, quoted once more.
    QVERIFY(cmd.contains(QStringLiteral("exec bash -lc 'exec 0<&4 4<&- 1>&3 3>&-; "
                                        "cd '\\''/home/alice/my project'\\'' && ")));
    QVERIFY(cmd.contains(QStringLiteral("elif bash -ic 'command -v \"$0\"' 'claude-code-acp' ")));
}

void TestAcpRemoteTransport::command_emptyCwdSkipsCd()
{
    const QString cmd = SshAcpProcessChannel::buildRemoteCommand(
        QString(), QStringLiteral("agent"), {});
    QCOMPARE(directTier(cmd), QStringLiteral("exec 'agent'"));
}

void TestAcpRemoteTransport::command_embeddedSingleQuoteEscaped()
{
    QCOMPARE(SshAcpProcessChannel::shellQuote(QStringLiteral("a'b")),
             QStringLiteral("'a'\\''b'"));
}

// --- exec-channel IO adapter --------------------------------------------------

void TestAcpRemoteTransport::channel_writeFrameReachesChannel()
{
    WorkerExecHost host;
    // No stdout EOF scripted → the op stays in Streaming after start, so a frame
    // written via the channel can be appended + drained on the next service pass.
    SshAcpProcessChannel channel(&host, QStringLiteral("agent"),
                                 { QStringLiteral("--acp") }, QStringLiteral("/repo"));
    QSignalSpy startedSpy(&channel, &IAcpProcessChannel::started);
    channel.start();
    QCOMPARE(startedSpy.count(), 1);
    QVERIFY(channel.isRunning());

    // The exact `cd ... && exec ...` string reached the transport's exec.
    QCOMPARE(directTier(host.fake()->lastExecCommand),
             QStringLiteral("cd '/repo' && exec 'agent' '--acp'"));

    // First exec channel gets transport id 1 (fake's nextId starts at 1).
    const QByteArray frame = QByteArrayLiteral("{\"jsonrpc\":\"2.0\",\"id\":1}\n");
    channel.write(frame); // → execWrite → buffered + serviceExec drains it
    QCOMPARE(host.fake()->chWritten.value(1), frame);

    // A second frame appends to the same channel stdin (session-lifetime stdin).
    const QByteArray frame2 = QByteArrayLiteral("{\"jsonrpc\":\"2.0\",\"id\":2}\n");
    channel.write(frame2);
    QCOMPARE(host.fake()->chWritten.value(1), frame + frame2);
}

void TestAcpRemoteTransport::channel_scriptedStdoutSurfacesReadyRead()
{
    WorkerExecHost host;
    SshAcpProcessChannel channel(&host, QStringLiteral("agent"), {}, QStringLiteral("/repo"));
    QSignalSpy outSpy(&channel, &IAcpProcessChannel::readyReadStdout);

    // Start with NO scripted stdout so the op stays Streaming and start()
    // returns with the channel's reqId latched (production delivers exec results
    // queued, i.e. after start(); driving the worker synchronously, we mirror that
    // by scripting + servicing only AFTER start()).
    channel.start();
    QVERIFY(channel.isRunning());

    host.fake()->readScript[1] = { dataChunk("{\"jsonrpc\":\"2.0\"}\n") };
    host.worker()->serviceExecForTest(); // drains the scripted chunk now

    QByteArray out;
    for (const auto &call : outSpy) {
        out += call.at(0).toByteArray();
    }
    QCOMPARE(out, QByteArrayLiteral("{\"jsonrpc\":\"2.0\"}\n"));
    QVERIFY(channel.isRunning()); // no EOF yet → still streaming
}

void TestAcpRemoteTransport::channel_exitSurfacesFinished()
{
    WorkerExecHost host;
    SshAcpProcessChannel channel(&host, QStringLiteral("agent"), {}, QStringLiteral("/repo"));
    QSignalSpy finishedSpy(&channel, &IAcpProcessChannel::finished);

    // Stay Streaming through start(); then script stdout EOF + exit status and
    // service one pass so execDone routes to the (now-latched) channel reqId.
    channel.start();
    QVERIFY(channel.isRunning());

    host.fake()->readScript[1] = { dataChunk("bye\n"), eofResult() };
    host.fake()->exitStatus[1] = 3;
    host.worker()->serviceExecForTest();

    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(finishedSpy.first().at(0).toInt(), 3);
    QCOMPARE(finishedSpy.first().at(1).value<QProcess::ExitStatus>(), QProcess::NormalExit);
    QVERIFY(!channel.isRunning());
}

// --- binary probe -------------------------------------------------------------

void TestAcpRemoteTransport::probe_missEmitsErrorAndSpawnsNothing()
{
    FakeRemoteContext ctx;
    ctx.probeExitCode = 127; // `command -v` → not found

    bool builderCalled = false;
    AcpConnection conn;
    conn.setRemoteSpawn(&ctx, [&builderCalled](ExecutionContext *, const AcpAgentDefinition &,
                                               const QString &, QObject *) -> IAcpProcessChannel * {
        builderCalled = true;
        return nullptr;
    });

    // Capture via a lambda (not QSignalSpy): the signal's first arg is a plain
    // enum class without Q_DECLARE_METATYPE, which QSignalSpy can't marshal.
    int errorCount = 0;
    QString friendly;
    QObject::connect(&conn, &AcpConnection::errorOccurred, &conn,
                     [&](AcpErrorClassifier::AcpErrorKind, const QString &msg) {
                         ++errorCount;
                         friendly = msg;
                     });

    AcpAgentDefinition agent;
    agent.id = QStringLiteral("claude");
    agent.name = QStringLiteral("Claude Code");
    agent.command = QStringLiteral("claude-code-acp");

    conn.spawn(agent, QStringLiteral("/home/alice/project"));

    // The probe ran `command -v <binary>` under sh (then bash -lc / -ic) …
    QCOMPARE(ctx.execCalls, 1);
    QCOMPARE(ctx.lastExecArgv.size(), 3);
    QCOMPARE(ctx.lastExecArgv.at(0), QStringLiteral("sh"));
    QCOMPARE(ctx.lastExecArgv.at(1), QStringLiteral("-c"));
    QVERIFY(ctx.lastExecArgv.at(2).startsWith(
        QStringLiteral("if command -v 'claude-code-acp' >/dev/null 2>&1; then command -v 'claude-code-acp'; ")));
    // … missed → a clear error naming the agent + host, and NO spawn (builder
    // never consulted — no local fallback).
    QCOMPARE(errorCount, 1);
    QVERIFY(!builderCalled);
    QVERIFY(friendly.contains(QStringLiteral("Claude Code")));
    QVERIFY(friendly.contains(QStringLiteral("alice@host.example")));
}

void TestAcpRemoteTransport::probe_hitBuildsChannelAndStarts()
{
    FakeRemoteContext ctx;
    ctx.probeExitCode = 0; // `command -v` → found
    ctx.probeStdout = QByteArrayLiteral("/usr/local/bin/claude-code-acp\n");

    QString builtCwd;
    AcpAgentDefinition builtAgent;
    AcpConnection conn;
    auto host = std::make_unique<WorkerExecHost>();
    conn.setRemoteSpawn(&ctx, [&](ExecutionContext *, const AcpAgentDefinition &agent,
                                  const QString &cwd, QObject *parent) -> IAcpProcessChannel * {
        builtCwd = cwd;
        builtAgent = agent;
        return new SshAcpProcessChannel(host.get(), agent.command, agent.args, cwd, parent);
    });

    int errorCount = 0;
    QObject::connect(&conn, &AcpConnection::errorOccurred, &conn,
                     [&](AcpErrorClassifier::AcpErrorKind, const QString &) { ++errorCount; });

    AcpAgentDefinition agent;
    agent.id = QStringLiteral("claude");
    agent.name = QStringLiteral("Claude Code");
    agent.command = QStringLiteral("claude-code-acp");
    agent.args = { QStringLiteral("--acp") };

    conn.spawn(agent, QStringLiteral("/home/alice/project"));

    // Probe hit → builder consulted with the captured remote cwd, channel started
    // (the exec command reached the worker's transport), and no error emitted.
    QCOMPARE(ctx.execCalls, 1);
    QCOMPARE(errorCount, 0);
    QCOMPARE(builtCwd, QStringLiteral("/home/alice/project"));
    QCOMPARE(builtAgent.command, QStringLiteral("claude-code-acp"));
    QCOMPARE(directTier(host->fake()->lastExecCommand),
             QStringLiteral("cd '/home/alice/project' && exec 'claude-code-acp' '--acp'"));
}

QTEST_MAIN(TestAcpRemoteTransport)

#include "test_acp_remote_transport.moc"

