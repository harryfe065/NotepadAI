/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "Pm2Protocol.h"

#include "TerminalTaskLauncher.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QObject>
#include <QSet>

namespace {

Pm2Status statusFromText(const QString &s)
{
    if (s == QLatin1String("online")) return Pm2Status::Online;
    if (s == QLatin1String("launching")) return Pm2Status::Launching;
    if (s == QLatin1String("stopping")) return Pm2Status::Stopping;
    if (s == QLatin1String("stopped")) return Pm2Status::Stopped;
    if (s == QLatin1String("errored")) return Pm2Status::Errored;
    return Pm2Status::Other;
}

Pm2ProcessInfo processFromJson(const QJsonObject &o)
{
    Pm2ProcessInfo p;
    p.pmId = o.value(QLatin1String("id")).toInt(-1);
    p.name = o.value(QLatin1String("name")).toString();
    p.statusText = o.value(QLatin1String("status")).toString();
    p.status = statusFromText(p.statusText);
    p.pid = qint64(o.value(QLatin1String("pid")).toDouble());
    p.startedAtMs = qint64(o.value(QLatin1String("startedAt")).toDouble());
    p.restarts = o.value(QLatin1String("restarts")).toInt();
    p.unstableRestarts = o.value(QLatin1String("unstable")).toInt();
    p.cpuPercent = o.value(QLatin1String("cpu")).toDouble();
    p.memoryBytes = qint64(o.value(QLatin1String("mem")).toDouble());
    p.cwd = o.value(QLatin1String("cwd")).toString();
    p.script = o.value(QLatin1String("script")).toString();
    return p;
}

QStringList stringList(const QJsonArray &a)
{
    QStringList out;
    out.reserve(a.size());
    for (const auto &v : a)
        out.append(v.toString());
    return out;
}

QByteArray line(const QJsonObject &o)
{
    QByteArray out = QJsonDocument(o).toJson(QJsonDocument::Compact);
    out.append('\n');
    return out;
}

QJsonObject appToJson(const Pm2App &app)
{
    QJsonObject o = pm2OptionsToJson(app.options);
    o.insert(QStringLiteral("name"), app.name);
    o.insert(QStringLiteral("script"), app.script);
    o.insert(QStringLiteral("cwd"), app.cwd);
    return o;
}

bool isWindowsDeviceName(const QString &name)
{
    static const QSet<QString> kDevices{
        QStringLiteral("con"),  QStringLiteral("prn"),  QStringLiteral("aux"),  QStringLiteral("nul"),
        QStringLiteral("com1"), QStringLiteral("com2"), QStringLiteral("com3"), QStringLiteral("com4"),
        QStringLiteral("com5"), QStringLiteral("com6"), QStringLiteral("com7"), QStringLiteral("com8"),
        QStringLiteral("com9"), QStringLiteral("lpt1"), QStringLiteral("lpt2"), QStringLiteral("lpt3"),
        QStringLiteral("lpt4"), QStringLiteral("lpt5"), QStringLiteral("lpt6"), QStringLiteral("lpt7"),
        QStringLiteral("lpt8"), QStringLiteral("lpt9"),
    };
    // "con.x" is a device too.
    return kDevices.contains(name.section(QLatin1Char('.'), 0, 0).toLower());
}

} // namespace

namespace Pm2Protocol {

Pm2HelperMessage parseLine(QByteArrayView text)
{
    Pm2HelperMessage m;
    if (text.isEmpty() || text.front() != '{')
        return m;
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(text.toByteArray(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return m;
    const QJsonObject o = doc.object();
    const QString type = o.value(QLatin1String("t")).toString();

    if (type == QLatin1String("list")) {
        m.kind = Pm2HelperMessage::Kind::List;
        const QJsonArray procs = o.value(QLatin1String("procs")).toArray();
        m.processes.reserve(procs.size());
        for (const auto &v : procs) {
            Pm2ProcessInfo p = processFromJson(v.toObject());
            if (p.pmId >= 0)
                m.processes.append(std::move(p));
        }
        const QJsonValue dump = o.value(QLatin1String("dump"));
        m.hasDump = dump.isArray();
        m.dumpNames = stringList(dump.toArray());
    } else if (type == QLatin1String("result")) {
        m.kind = Pm2HelperMessage::Kind::Result;
        m.requestId = o.value(QLatin1String("id")).toInt();
        m.ok = o.value(QLatin1String("ok")).toBool();
        m.error = o.value(QLatin1String("error")).toString();
    } else if (type == QLatin1String("ready")) {
        m.kind = Pm2HelperMessage::Kind::Ready;
        m.pm2Dir = o.value(QLatin1String("pm2Dir")).toString();
        m.pm2Version = o.value(QLatin1String("pm2Version")).toString();
        m.nodePath = o.value(QLatin1String("node")).toString();
        m.dumpPath = o.value(QLatin1String("dumpPath")).toString();
    } else if (type == QLatin1String("fatal")) {
        m.kind = Pm2HelperMessage::Kind::Fatal;
        m.error = o.value(QLatin1String("error")).toString();
        m.code = o.value(QLatin1String("code")).toString();
    }
    return m;
}

QByteArray startCommand(int requestId, const Pm2App &app)
{
    return line(QJsonObject{
        {QStringLiteral("id"), requestId},
        {QStringLiteral("op"), QStringLiteral("start")},
        {QStringLiteral("app"), appToJson(app)},
    });
}

QByteArray idsCommand(int requestId, IdsOp op, const QList<int> &pmIds)
{
    QJsonArray ids;
    for (const int id : pmIds)
        ids.append(id);
    QString name;
    switch (op) {
    case IdsOp::Stop: name = QStringLiteral("stop"); break;
    case IdsOp::Restart: name = QStringLiteral("restart"); break;
    case IdsOp::Delete: name = QStringLiteral("delete"); break;
    }
    return line(QJsonObject{
        {QStringLiteral("id"), requestId},
        {QStringLiteral("op"), name},
        {QStringLiteral("ids"), ids},
    });
}

QByteArray recreateCommand(int requestId, int pmId, const Pm2App &app)
{
    return line(QJsonObject{
        {QStringLiteral("id"), requestId},
        {QStringLiteral("op"), QStringLiteral("recreate")},
        {QStringLiteral("pmId"), pmId},
        {QStringLiteral("app"), appToJson(app)},
    });
}

QByteArray simpleCommand(int requestId, SimpleOp op)
{
    return line(QJsonObject{
        {QStringLiteral("id"), requestId},
        {QStringLiteral("op"), op == SimpleOp::Save ? QStringLiteral("save") : QStringLiteral("resurrect")},
    });
}

QString formatUptime(qint64 ms)
{
    const qint64 s = qMax<qint64>(0, ms) / 1000;
    const qint64 m = s / 60;
    const qint64 h = m / 60;
    const qint64 d = h / 24;
    if (d > 0)
        return QStringLiteral("%1d %2h").arg(d).arg(h % 24);
    if (h > 0)
        return QStringLiteral("%1h %2m").arg(h).arg(m % 60);
    if (m > 0)
        return QStringLiteral("%1m %2s").arg(m).arg(s % 60);
    return QStringLiteral("%1s").arg(s);
}

QStringList missingFromDaemon(const QStringList &dumpNames, const QList<Pm2ProcessInfo> &live)
{
    QSet<QString> seen;
    seen.reserve(live.size() + dumpNames.size());
    for (const Pm2ProcessInfo &p : live)
        seen.insert(p.name);
    QStringList out;
    for (const QString &name : dumpNames) {
        if (!seen.contains(name)) {
            seen.insert(name);
            out.append(name);
        }
    }
    return out;
}

QString validateName(const QString &name, const QStringList &takenNames)
{
    if (name.isEmpty())
        return QObject::tr("Enter a name.");
    if (name.size() > 64)
        return QObject::tr("Use at most 64 characters.");
    bool allDigits = true;
    for (const QChar c : name) {
        const char16_t u = c.unicode();
        const bool digit = u >= u'0' && u <= u'9';
        const bool ok = digit || (u >= u'a' && u <= u'z') || (u >= u'A' && u <= u'Z') || u == u'.'
            || u == u'_' || u == u'-';
        if (!ok)
            return QObject::tr("Use only letters, digits, '.', '_' and '-'.");
        allDigits = allDigits && digit;
    }
    if (name.front() == QLatin1Char('.') || name.front() == QLatin1Char('-'))
        return QObject::tr("Start the name with a letter, digit or '_'.");
    if (allDigits)
        return QObject::tr("Add a letter: pm2 reads a number as a process id.");
    if (isWindowsDeviceName(name))
        return QObject::tr("'%1' is reserved by Windows.").arg(name);
    // Entrypoint file names collide case-insensitively on Windows and macOS.
    for (const QString &taken : takenNames) {
        if (taken.compare(name, Qt::CaseInsensitive) == 0)
            return QObject::tr("A process named '%1' already exists.").arg(taken);
    }
    return {};
}

bool isOwnEntrypoint(const QString &script, const QString &entrypointDir)
{
    if (script.isEmpty() || entrypointDir.isEmpty())
        return false;
    const QString path = QDir::cleanPath(QDir::fromNativeSeparators(script));
    if (!path.endsWith(QLatin1String(".cjs")))
        return false;
    const QString dir = QDir::cleanPath(QDir::fromNativeSeparators(entrypointDir));
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    constexpr Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
    return QFileInfo(path).path().compare(dir, cs) == 0;
}

QString logsCommand(TerminalShellKind kind, const QString &nodePath, const QString &pm2Dir, int pmId)
{
    const QChar sep = pm2Dir.contains(QLatin1Char('\\')) ? QLatin1Char('\\') : QLatin1Char('/');
    const QString bin = pm2Dir + sep + QStringLiteral("bin") + sep + QStringLiteral("pm2");
    const QString node = quotePathForShell(nodePath, kind);
    QString out;
    // PowerShell reads a quoted first word as a string, not a command.
    if (kind == TerminalShellKind::PowerShell && node.startsWith(QLatin1Char('\'')))
        out = QStringLiteral("& ");
    out += node + QLatin1Char(' ') + quotePathForShell(bin, kind)
        + QStringLiteral(" logs %1 --lines 200").arg(pmId);
    return out;
}

} // namespace Pm2Protocol
