/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef PM2_PROTOCOL_H
#define PM2_PROTOCOL_H

#include <QByteArray>
#include <QByteArrayView>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <cstdint>

#include "Pm2Entrypoint.h"

enum class TerminalShellKind : uint8_t;

enum class Pm2Status : uint8_t { Online, Launching, Stopping, Stopped, Errored, Other };

// Has a live process (pid, uptime, CPU, memory; can be stopped).
inline bool pm2IsRunning(Pm2Status s)
{
    return s == Pm2Status::Online || s == Pm2Status::Launching || s == Pm2Status::Stopping;
}

// What pm2 is asked to start: an entrypoint and pm2's options for it.
struct Pm2App
{
    QString name;
    QString script;
    QString cwd;
    Pm2Options options;
};

// One row of `pm2 list`, as the helper reports it.
struct Pm2ProcessInfo
{
    int pmId = -1;
    QString name;
    Pm2Status status = Pm2Status::Other;
    QString statusText; // pm2's own word, shown for Other
    qint64 pid = 0;
    qint64 startedAtMs = 0; // pm2_env.pm_uptime: when it last started
    int restarts = 0;
    int unstableRestarts = 0;
    double cpuPercent = 0;
    qint64 memoryBytes = 0;
    QString cwd;
    QString script; // pm_exec_path

    bool operator==(const Pm2ProcessInfo &) const = default;
};

// One line from the helper (newline-delimited JSON on its stdout).
struct Pm2HelperMessage
{
    enum class Kind : uint8_t { Unknown, Ready, List, Result, Fatal };
    Kind kind = Kind::Unknown;

    // Ready
    QString pm2Dir;
    QString pm2Version;
    QString nodePath;
    QString dumpPath;

    // List
    QList<Pm2ProcessInfo> processes;
    QStringList dumpNames; // names in pm2's saved list (dump.pm2)
    bool hasDump = false;

    // Result
    int requestId = 0;
    bool ok = false;

    // Result / Fatal
    QString error;
    // Fatal: "no-pm2" when pm2 is not on PATH (try npx next), else empty.
    QString code;
};

namespace Pm2Protocol {

Pm2HelperMessage parseLine(QByteArrayView line);

// Commands for the helper, one JSON line each (trailing '\n' included).
// `pmIds` address processes by id: names may repeat in pm2.
enum class IdsOp : uint8_t { Stop, Restart, Delete };
enum class SimpleOp : uint8_t { Save, Resurrect };

QByteArray startCommand(int requestId, const Pm2App &app);
QByteArray idsCommand(int requestId, IdsOp op, const QList<int> &pmIds);
// Delete `pmId`, then start `app` (rename, cwd or option change).
QByteArray recreateCommand(int requestId, int pmId, const Pm2App &app);
QByteArray simpleCommand(int requestId, SimpleOp op);

// "42s", "12m 5s", "3h 12m", "2d 4h".
QString formatUptime(qint64 ms);

// Names in pm2's saved list that no live process has, in dump order, once
// each: the processes a Resurrect would bring back.
QStringList missingFromDaemon(const QStringList &dumpNames, const QList<Pm2ProcessInfo> &live);

// Empty when `name` can name a new PM2 process (and its entrypoint file),
// else a one-line reason.
QString validateName(const QString &name, const QStringList &takenNames);

// True when `script` is an entrypoint NotepadAI wrote into `entrypointDir`.
bool isOwnEntrypoint(const QString &script, const QString &entrypointDir);

// `pm2 logs <id>` typed into a terminal of `kind`, run through the same pm2
// the helper uses.
QString logsCommand(TerminalShellKind kind, const QString &nodePath, const QString &pm2Dir, int pmId);

} // namespace Pm2Protocol

#endif // PM2_PROTOCOL_H
