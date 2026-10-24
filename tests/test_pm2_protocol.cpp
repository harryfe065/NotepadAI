/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <QtTest>

#include "DotEnv.h"
#include "Pm2Protocol.h"
#include "TerminalTaskLauncher.h"

class TestPm2Protocol : public QObject
{
    Q_OBJECT

private slots:
    void readyMessage();
    void listMessage_mapsEveryColumn();
    void listMessage_unknownStatusAndMissingFields();
    void resultAndFatalMessages();
    void garbageLinesAreUnknown();

    void uptimeFormatting();
    void missingFromDaemon_namesTheDaemonLacks();
    void nameValidation();
    void entrypointOwnership();
    void logsCommand_perShell();
    void dotEnv();
};

void TestPm2Protocol::readyMessage()
{
    const Pm2HelperMessage m = Pm2Protocol::parseLine(
        R"({"t":"ready","pm2Dir":"C:\\npm\\node_modules\\pm2","pm2Version":"5.4.2","node":"C:\\nodejs\\node.exe","dumpPath":"C:\\Users\\me\\.pm2\\dump.pm2"})");
    QCOMPARE(m.kind, Pm2HelperMessage::Kind::Ready);
    QCOMPARE(m.pm2Dir, QStringLiteral("C:\\npm\\node_modules\\pm2"));
    QCOMPARE(m.pm2Version, QStringLiteral("5.4.2"));
    QCOMPARE(m.nodePath, QStringLiteral("C:\\nodejs\\node.exe"));
    QCOMPARE(m.dumpPath, QStringLiteral("C:\\Users\\me\\.pm2\\dump.pm2"));
}

void TestPm2Protocol::listMessage_mapsEveryColumn()
{
    const Pm2HelperMessage m = Pm2Protocol::parseLine(
        R"({"t":"list","procs":[)"
        R"({"id":3,"name":"api","status":"online","pid":4242,"startedAt":1761300000000,"restarts":5,"unstable":1,"cpu":12.5,"mem":52428800,"cwd":"D:\\proj","script":"C:\\data\\pm2\\api.cjs"},)"
        R"({"id":4,"name":"worker","status":"errored","pid":0,"startedAt":1761300001000,"restarts":15,"unstable":15,"cpu":0,"mem":0,"cwd":"/srv","script":"/srv/w.js"})"
        R"(],"dump":["api","old-job"]})");
    QCOMPARE(m.kind, Pm2HelperMessage::Kind::List);
    QCOMPARE(m.processes.size(), 2);

    const Pm2ProcessInfo &a = m.processes.at(0);
    QCOMPARE(a.pmId, 3);
    QCOMPARE(a.name, QStringLiteral("api"));
    QCOMPARE(a.status, Pm2Status::Online);
    QCOMPARE(a.pid, qint64(4242));
    QCOMPARE(a.startedAtMs, qint64(1761300000000));
    QCOMPARE(a.restarts, 5);
    QCOMPARE(a.unstableRestarts, 1);
    QCOMPARE(a.cpuPercent, 12.5);
    QCOMPARE(a.memoryBytes, qint64(52428800));
    QCOMPARE(a.cwd, QStringLiteral("D:\\proj"));
    QCOMPARE(a.script, QStringLiteral("C:\\data\\pm2\\api.cjs"));

    QCOMPARE(m.processes.at(1).status, Pm2Status::Errored);
    QCOMPARE(m.dumpNames, (QStringList{QStringLiteral("api"), QStringLiteral("old-job")}));
    QVERIFY(m.hasDump);
}

void TestPm2Protocol::listMessage_unknownStatusAndMissingFields()
{
    const Pm2HelperMessage m = Pm2Protocol::parseLine(
        R"({"t":"list","procs":[{"id":0,"name":"x","status":"waiting restart"},{"name":"no-id"}]})");
    QCOMPARE(m.kind, Pm2HelperMessage::Kind::List);
    // A row without an id cannot be acted on and is dropped.
    QCOMPARE(m.processes.size(), 1);
    QCOMPARE(m.processes.at(0).status, Pm2Status::Other);
    QCOMPARE(m.processes.at(0).statusText, QStringLiteral("waiting restart"));
    QCOMPARE(m.processes.at(0).restarts, 0);
    QVERIFY(!m.hasDump);

    QCOMPARE(Pm2Protocol::parseLine(R"({"t":"list","procs":[{"id":1,"name":"a","status":"stopped"}]})")
                 .processes.at(0).status,
             Pm2Status::Stopped);
    QCOMPARE(Pm2Protocol::parseLine(R"({"t":"list","procs":[{"id":1,"name":"a","status":"launching"}]})")
                 .processes.at(0).status,
             Pm2Status::Launching);
    QCOMPARE(Pm2Protocol::parseLine(R"({"t":"list","procs":[{"id":1,"name":"a","status":"stopping"}]})")
                 .processes.at(0).status,
             Pm2Status::Stopping);
}

void TestPm2Protocol::resultAndFatalMessages()
{
    Pm2HelperMessage m = Pm2Protocol::parseLine(R"({"t":"result","id":17,"ok":true})");
    QCOMPARE(m.kind, Pm2HelperMessage::Kind::Result);
    QCOMPARE(m.requestId, 17);
    QVERIFY(m.ok);

    m = Pm2Protocol::parseLine(R"({"t":"result","id":18,"ok":false,"error":"process or namespace not found"})");
    QCOMPARE(m.kind, Pm2HelperMessage::Kind::Result);
    QCOMPARE(m.requestId, 18);
    QVERIFY(!m.ok);
    QCOMPARE(m.error, QStringLiteral("process or namespace not found"));

    m = Pm2Protocol::parseLine(R"({"t":"fatal","code":"no-pm2","error":"pm2 was not found"})");
    QCOMPARE(m.kind, Pm2HelperMessage::Kind::Fatal);
    QCOMPARE(m.code, QStringLiteral("no-pm2"));
    QCOMPARE(m.error, QStringLiteral("pm2 was not found"));
}

void TestPm2Protocol::garbageLinesAreUnknown()
{
    QCOMPARE(Pm2Protocol::parseLine("").kind, Pm2HelperMessage::Kind::Unknown);
    QCOMPARE(Pm2Protocol::parseLine("npm WARN exec The following package was not found").kind,
             Pm2HelperMessage::Kind::Unknown);
    QCOMPARE(Pm2Protocol::parseLine(R"({"t":"something-new"})").kind, Pm2HelperMessage::Kind::Unknown);
    QCOMPARE(Pm2Protocol::parseLine("[1,2]").kind, Pm2HelperMessage::Kind::Unknown);
}

void TestPm2Protocol::uptimeFormatting()
{
    QCOMPARE(Pm2Protocol::formatUptime(0), QStringLiteral("0s"));
    QCOMPARE(Pm2Protocol::formatUptime(42'999), QStringLiteral("42s"));
    QCOMPARE(Pm2Protocol::formatUptime(12 * 60'000 + 5'000), QStringLiteral("12m 5s"));
    QCOMPARE(Pm2Protocol::formatUptime(3 * 3'600'000 + 12 * 60'000 + 59'000), QStringLiteral("3h 12m"));
    QCOMPARE(Pm2Protocol::formatUptime(2 * 86'400'000LL + 4 * 3'600'000 + 1), QStringLiteral("2d 4h"));
    // Clock skew between the daemon's start stamp and now.
    QCOMPARE(Pm2Protocol::formatUptime(-5'000), QStringLiteral("0s"));
}

void TestPm2Protocol::missingFromDaemon_namesTheDaemonLacks()
{
    Pm2ProcessInfo api;
    api.pmId = 0;
    api.name = QStringLiteral("api");
    const QList<Pm2ProcessInfo> live{api};

    QCOMPARE(Pm2Protocol::missingFromDaemon({QStringLiteral("api"), QStringLiteral("job"), QStringLiteral("job"),
                                             QStringLiteral("web")},
                                            live),
             (QStringList{QStringLiteral("job"), QStringLiteral("web")}));
    QVERIFY(Pm2Protocol::missingFromDaemon({QStringLiteral("api")}, live).isEmpty());
    QVERIFY(Pm2Protocol::missingFromDaemon({}, {}).isEmpty());
}

void TestPm2Protocol::nameValidation()
{
    const QStringList taken{QStringLiteral("api")};
    QVERIFY(Pm2Protocol::validateName(QStringLiteral("web-2.dev_x"), taken).isEmpty());

    QVERIFY(!Pm2Protocol::validateName(QString(), taken).isEmpty());
    QVERIFY(!Pm2Protocol::validateName(QStringLiteral("api"), taken).isEmpty());
    QVERIFY(!Pm2Protocol::validateName(QStringLiteral("has space"), taken).isEmpty());
    QVERIFY(!Pm2Protocol::validateName(QStringLiteral("a/b"), taken).isEmpty());
    QVERIFY(!Pm2Protocol::validateName(QStringLiteral(".hidden"), taken).isEmpty());
    QVERIFY(!Pm2Protocol::validateName(QStringLiteral("-flag"), taken).isEmpty());
    // pm2 reads an all-digit name as a process id.
    QVERIFY(!Pm2Protocol::validateName(QStringLiteral("123"), taken).isEmpty());
    // Windows device names cannot be file names.
    QVERIFY(!Pm2Protocol::validateName(QStringLiteral("con"), taken).isEmpty());
    QVERIFY(!Pm2Protocol::validateName(QStringLiteral("LPT1"), taken).isEmpty());
    QVERIFY(!Pm2Protocol::validateName(QString(65, QLatin1Char('a')), taken).isEmpty());
    QVERIFY(Pm2Protocol::validateName(QString(64, QLatin1Char('a')), taken).isEmpty());
}

void TestPm2Protocol::entrypointOwnership()
{
#ifdef Q_OS_WIN
    const QString dir = QStringLiteral("C:/Users/me/AppData/Roaming/NotepadAI/pm2");
    QVERIFY(Pm2Protocol::isOwnEntrypoint(QStringLiteral("C:\\Users\\me\\AppData\\Roaming\\NotepadAI\\pm2\\api.cjs"), dir));
    QVERIFY(Pm2Protocol::isOwnEntrypoint(QStringLiteral("c:/users/ME/appdata/roaming/notepadai/pm2/api.cjs"), dir));
    QVERIFY(!Pm2Protocol::isOwnEntrypoint(QStringLiteral("C:\\Users\\me\\AppData\\Roaming\\NotepadAI\\pm2\\sub\\api.cjs"), dir));
    QVERIFY(!Pm2Protocol::isOwnEntrypoint(QStringLiteral("C:\\Users\\me\\AppData\\Roaming\\NotepadAI\\pm2\\api.js"), dir));
    QVERIFY(!Pm2Protocol::isOwnEntrypoint(QStringLiteral("D:\\proj\\server.js"), dir));
#else
    const QString dir = QStringLiteral("/home/me/.local/share/NotepadAI/pm2");
    QVERIFY(Pm2Protocol::isOwnEntrypoint(QStringLiteral("/home/me/.local/share/NotepadAI/pm2/api.cjs"), dir));
#ifndef Q_OS_MACOS
    QVERIFY(!Pm2Protocol::isOwnEntrypoint(QStringLiteral("/home/me/.local/share/NotepadAI/PM2/api.cjs"), dir));
#endif
    QVERIFY(!Pm2Protocol::isOwnEntrypoint(QStringLiteral("/srv/server.js"), dir));
#endif
    QVERIFY(!Pm2Protocol::isOwnEntrypoint(QString(), dir));
}

void TestPm2Protocol::logsCommand_perShell()
{
    const QString node = QStringLiteral("C:\\Program Files\\nodejs\\node.exe");
    const QString pm2Dir = QStringLiteral("C:\\Users\\me\\AppData\\Roaming\\npm\\node_modules\\pm2");

    QCOMPARE(Pm2Protocol::logsCommand(TerminalShellKind::PowerShell, node, pm2Dir, 7),
             QStringLiteral("& 'C:\\Program Files\\nodejs\\node.exe' "
                            "C:\\Users\\me\\AppData\\Roaming\\npm\\node_modules\\pm2\\bin\\pm2 logs 7 --lines 200"));
    QCOMPARE(Pm2Protocol::logsCommand(TerminalShellKind::Cmd, node, pm2Dir, 7),
             QStringLiteral("\"C:\\Program Files\\nodejs\\node.exe\" "
                            "C:\\Users\\me\\AppData\\Roaming\\npm\\node_modules\\pm2\\bin\\pm2 logs 7 --lines 200"));
    QCOMPARE(Pm2Protocol::logsCommand(TerminalShellKind::Posix, QStringLiteral("/usr/bin/node"),
                                      QStringLiteral("/home/me/.npm/_npx/ab12/node_modules/pm2"), 0),
             QStringLiteral("/usr/bin/node /home/me/.npm/_npx/ab12/node_modules/pm2/bin/pm2 logs 0 --lines 200"));
}

void TestPm2Protocol::dotEnv()
{
    const QList<QPair<QString, QString>> pairs = parseDotEnv(QStringLiteral(
        "# comment\r\n\r\nA=1\r\n  B = two words  \nC=\"quoted value\"\nD='single'\nE=\"unbalanced'\n=novalue\nnoequals\nF=\n"));
    const QList<QPair<QString, QString>> expected{
        {QStringLiteral("A"), QStringLiteral("1")},
        {QStringLiteral("B"), QStringLiteral("two words")},
        {QStringLiteral("C"), QStringLiteral("quoted value")},
        {QStringLiteral("D"), QStringLiteral("single")},
        {QStringLiteral("E"), QStringLiteral("\"unbalanced'")},
        {QStringLiteral("F"), QString()},
    };
    QCOMPARE(pairs, expected);
}

QTEST_GUILESS_MAIN(TestPm2Protocol)
#include "test_pm2_protocol.moc"
