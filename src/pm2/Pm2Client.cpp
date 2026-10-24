/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "Pm2Client.h"

#include "AcpProtocol.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>

namespace {

// pm2 found by the first helper that reached "ready" this session.
QString s_nodePath;
QString s_pm2Dir;

constexpr qsizetype kStderrTailBytes = 4096;
constexpr int kStopGraceMs = 3000;
// POSIX login shell: command not found.
constexpr int kShellNotFound = 127;

} // namespace

Pm2Client::Pm2Client(QObject *parent)
    : QObject(parent)
{
}

Pm2Client::~Pm2Client()
{
    stop();
}

QString Pm2Client::entrypointDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/pm2");
}

QString Pm2Client::sessionNodePath()
{
    return s_nodePath;
}

QString Pm2Client::sessionPm2Dir()
{
    return s_pm2Dir;
}

QString Pm2Client::helperScriptPath(QString *error)
{
    QFile resource(QStringLiteral(":/pm2/pm2-helper.cjs"));
    if (!resource.open(QIODevice::ReadOnly)) {
        *error = tr("The pm2 helper is missing from this build.");
        return {};
    }
    const QByteArray script = resource.readAll();

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/pm2-helper");
    QString path = dir + QStringLiteral("/pm2-helper.cjs");
    QFile current(path);
    if (current.open(QIODevice::ReadOnly) && current.size() == script.size() && current.readAll() == script)
        return path;
    current.close();

    QSaveFile out(path);
    if (!QDir().mkpath(dir) || !out.open(QIODevice::WriteOnly) || out.write(script) != script.size()
        || !out.commit()) {
        *error = tr("Could not write %1: %2").arg(QDir::toNativeSeparators(path), out.errorString());
        return {};
    }
    return path;
}

void Pm2Client::start()
{
    if (m_process)
        return;
    m_error.clear();
    launch(false);
}

void Pm2Client::launch(bool throughNpx)
{
    m_throughNpx = throughNpx;
    m_stdout.clear();
    m_stderr.clear();

    QString error;
    const QString helper = helperScriptPath(&error);
    if (helper.isEmpty()) {
        setState(State::Failed, error);
        return;
    }

#ifdef Q_OS_WIN
    constexpr bool isPosix = false;
#else
    constexpr bool isPosix = true;
#endif

    QString command;
    QStringList args;
    QString resolved;
    if (throughNpx) {
        command = QStringLiteral("npx");
        args = {QStringLiteral("-y"), QStringLiteral("-p"), QStringLiteral("pm2"), QStringLiteral("node"),
                QDir::toNativeSeparators(helper)};
        resolved = QStandardPaths::findExecutable(command);
        if (!isPosix && resolved.isEmpty()) {
            setState(State::Failed, tr("pm2 is not installed and npx was not found. Install Node.js, "
                                       "or run npm install -g pm2."));
            return;
        }
    } else {
        command = QStringLiteral("node");
        args = {QDir::toNativeSeparators(helper)};
        if (!s_pm2Dir.isEmpty())
            args.append(s_pm2Dir);
        resolved = s_nodePath.isEmpty() ? QStandardPaths::findExecutable(command) : s_nodePath;
        if (!isPosix && resolved.isEmpty()) {
            setState(State::NodeMissing);
            return;
        }
    }

    const AcpProtocol::SpawnArgv spawn = AcpProtocol::buildSpawnArgv(command, args, isPosix, resolved);
    auto *process = new QProcess(this);
    process->setProgram(spawn.program);
#ifdef Q_OS_WIN
    if (!spawn.nativeArgumentsLine.isEmpty())
        process->setNativeArguments(spawn.nativeArgumentsLine);
    else
#endif
        process->setArguments(spawn.arguments);
    process->setWorkingDirectory(QDir::homePath());

    connect(process, &QProcess::readyReadStandardOutput, this, &Pm2Client::onReadyRead);
    connect(process, &QProcess::readyReadStandardError, this, [this, process]() {
        m_stderr.append(process->readAllStandardError());
        if (m_stderr.size() > kStderrTailBytes)
            m_stderr.remove(0, m_stderr.size() - kStderrTailBytes);
    });
    connect(process, &QProcess::finished, this, [this](int exitCode) { onFinished(exitCode); });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart || process != m_process)
            return;
        m_process = nullptr;
        process->deleteLater();
        if (m_throughNpx)
            setState(State::Failed, tr("Could not start npx: %1").arg(process->errorString()));
        else
            setState(State::NodeMissing);
    });

    m_process = process;
    setState(throughNpx ? State::Installing : State::Connecting);
    process->start();
}

void Pm2Client::stop()
{
    QProcess *process = m_process;
    m_process = nullptr;
    if (process) {
        disconnect(process, nullptr, this, nullptr);
        if (process->state() == QProcess::NotRunning) {
            // Never started (or already gone): no finished() will come.
            process->deleteLater();
        } else {
            // Outlives this client: it only has to exit.
            process->setParent(QCoreApplication::instance());
            connect(process, &QProcess::finished, process, &QObject::deleteLater);
            QTimer::singleShot(kStopGraceMs, process, [process]() { process->kill(); });
            process->closeWriteChannel();
        }
    }
    if (m_state != State::Idle)
        setState(State::Idle);
}

void Pm2Client::setState(State state, const QString &error)
{
    m_error = error;
    if (m_state == state && error.isEmpty())
        return;
    m_state = state;
    emit stateChanged(state);
}

void Pm2Client::onReadyRead()
{
    if (!m_process)
        return;
    m_stdout.append(m_process->readAllStandardOutput());
    qsizetype start = 0;
    for (qsizetype nl = m_stdout.indexOf('\n'); nl >= 0; nl = m_stdout.indexOf('\n', start)) {
        QByteArrayView line(m_stdout.constData() + start, nl - start);
        start = nl + 1;
        if (line.endsWith('\r'))
            line.chop(1);
        handleLine(line);
        // A line may have stopped the client.
        if (!m_process)
            return;
    }
    m_stdout.remove(0, start);
}

void Pm2Client::handleLine(QByteArrayView line)
{
    Pm2HelperMessage m = Pm2Protocol::parseLine(line);
    switch (m.kind) {
    case Pm2HelperMessage::Kind::List:
        emit listReceived(m.processes, m.dumpNames, m.hasDump);
        break;
    case Pm2HelperMessage::Kind::Result:
        emit commandFinished(m.requestId, m.ok, m.error);
        break;
    case Pm2HelperMessage::Kind::Ready:
        s_nodePath = m.nodePath;
        s_pm2Dir = m.pm2Dir;
        m_pm2Version = m.pm2Version;
        setState(State::Ready);
        break;
    case Pm2HelperMessage::Kind::Fatal:
        // Not on PATH: the helper exits and onFinished() retries through npx.
        if (m.code == QLatin1String("no-pm2") && !m_throughNpx)
            m_retryThroughNpx = true;
        else
            setState(State::Failed, m.error);
        break;
    case Pm2HelperMessage::Kind::Unknown:
        break;
    }
}

void Pm2Client::onFinished(int exitCode)
{
    QProcess *process = m_process;
    if (!process)
        return;
    m_process = nullptr;
    process->deleteLater();

    if (m_retryThroughNpx) {
        m_retryThroughNpx = false;
        launch(true);
        return;
    }
    switch (m_state) {
    case State::Ready:
        setState(State::Failed, tr("The pm2 helper stopped unexpectedly.") + stderrTail());
        break;
    case State::Connecting:
    case State::Installing:
        if (exitCode == kShellNotFound && !m_throughNpx)
            setState(State::NodeMissing);
        else
            setState(State::Failed, tr("Could not connect to pm2.") + stderrTail());
        break;
    default:
        break;
    }
}

QString Pm2Client::stderrTail() const
{
    const QString text = QString::fromLocal8Bit(m_stderr).trimmed();
    if (text.isEmpty())
        return {};
    // The last few lines carry the reason (npm and node print it last).
    QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (lines.size() > 3)
        lines = lines.mid(lines.size() - 3);
    return QLatin1Char('\n') + lines.join(QLatin1Char('\n')).trimmed();
}

int Pm2Client::send(int requestId, const QByteArray &line)
{
    if (m_state != State::Ready || !m_process)
        return 0;
    m_process->write(line);
    return requestId;
}

int Pm2Client::startProcess(const Pm2App &app)
{
    const int id = nextId();
    return send(id, Pm2Protocol::startCommand(id, app));
}

int Pm2Client::stopProcesses(const QList<int> &pmIds)
{
    const int id = nextId();
    return send(id, Pm2Protocol::idsCommand(id, Pm2Protocol::IdsOp::Stop, pmIds));
}

int Pm2Client::restartProcesses(const QList<int> &pmIds)
{
    const int id = nextId();
    return send(id, Pm2Protocol::idsCommand(id, Pm2Protocol::IdsOp::Restart, pmIds));
}

int Pm2Client::deleteProcesses(const QList<int> &pmIds)
{
    const int id = nextId();
    return send(id, Pm2Protocol::idsCommand(id, Pm2Protocol::IdsOp::Delete, pmIds));
}

int Pm2Client::recreateProcess(int pmId, const Pm2App &app)
{
    const int id = nextId();
    return send(id, Pm2Protocol::recreateCommand(id, pmId, app));
}

int Pm2Client::save()
{
    const int id = nextId();
    return send(id, Pm2Protocol::simpleCommand(id, Pm2Protocol::SimpleOp::Save));
}

int Pm2Client::resurrect()
{
    const int id = nextId();
    return send(id, Pm2Protocol::simpleCommand(id, Pm2Protocol::SimpleOp::Resurrect));
}
