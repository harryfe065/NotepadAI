/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef PM2_CLIENT_H
#define PM2_CLIENT_H

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

#include "Pm2Protocol.h"

class QProcess;

// The Node helper (pm2-helper.cjs) behind Terminal > PM2 Processes: one
// process, alive between start() and stop(), that holds a pm2 API connection
// and pushes the process list. pm2 is found once per session: `pm2` on PATH
// first, else `npx -y -p pm2`; the package directory the helper reports is
// reused for every later start (and for `pm2 logs`). See ADR 0005.
class Pm2Client : public QObject
{
    Q_OBJECT

public:
    enum class State : uint8_t {
        Idle,
        Connecting, // helper started, no "ready" yet
        Installing, // same, through npx (may download pm2)
        Ready,
        NodeMissing,
        Failed, // errorText() says why
    };

    explicit Pm2Client(QObject *parent = nullptr);
    ~Pm2Client() override;

    void start();
    // Closes the helper's stdin; it disconnects from pm2 and exits. Never
    // waits: a helper that does not exit within 3 s is killed.
    void stop();

    State state() const { return m_state; }
    QString errorText() const { return m_error; }
    QString pm2Version() const { return m_pm2Version; }

    // Each returns the request id its commandFinished() will carry, or 0 when
    // the helper is not ready.
    int startProcess(const Pm2App &app);
    int stopProcesses(const QList<int> &pmIds);
    int restartProcesses(const QList<int> &pmIds);
    int deleteProcesses(const QList<int> &pmIds);
    int recreateProcess(int pmId, const Pm2App &app);
    int save();
    int resurrect();

    // Where NotepadAI writes entrypoints: <AppDataLocation>/pm2.
    static QString entrypointDir();
    // The node and pm2 package the helper used this session (empty before
    // the first "ready").
    static QString sessionNodePath();
    static QString sessionPm2Dir();

signals:
    void stateChanged(Pm2Client::State state);
    void listReceived(const QList<Pm2ProcessInfo> &processes, const QStringList &dumpNames, bool hasDump);
    void commandFinished(int requestId, bool ok, const QString &error);

private:
    void launch(bool throughNpx);
    void setState(State state, const QString &error = QString());
    void onReadyRead();
    void onFinished(int exitCode);
    void handleLine(QByteArrayView line);
    // `requestId`, or 0 when the helper is not ready.
    int send(int requestId, const QByteArray &line);
    int nextId() { return ++m_lastRequestId; }
    QString stderrTail() const;
    // Writes the bundled helper to disk when the copy there differs.
    static QString helperScriptPath(QString *error);

    QPointer<QProcess> m_process;
    State m_state = State::Idle;
    QString m_error;
    QString m_pm2Version;
    QByteArray m_stdout;
    QByteArray m_stderr;
    bool m_throughNpx = false;
    bool m_retryThroughNpx = false;
    int m_lastRequestId = 0;
};

#endif // PM2_CLIENT_H
