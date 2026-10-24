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

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "Pm2Entrypoint.h"

namespace {

Pm2Definition trickyDefinition()
{
    Pm2Definition def;
    def.name = QStringLiteral("api-server.v2");
    def.command = QStringLiteral("node -e \"console.log('a\\\\b \\\"q\\\"')\" && echo done\n"
                                 "echo second line \u2028 \u00e9\u4e2d");
    def.shell = QStringLiteral("pwsh");
    def.cwd = QStringLiteral("C:\\Users\\me\\proj with space");
    def.envText = QStringLiteral("# comment\nPORT=3000\nTOKEN=\"a b\"\n\nEMPTY=\n");
    def.options.autorestart = false;
    def.options.watch = true;
    def.options.maxMemoryRestart = QStringLiteral("512M");
    def.options.restartDelayMs = 2500;
    return def;
}

// Runs `def`'s entrypoint with node; nullopt when node is not installed.
struct RunResult
{
    QByteArray out;
    int exitCode = -1;
};

std::optional<RunResult> runEntrypoint(const Pm2Definition &def, const QString &dir)
{
    const QString node = QStandardPaths::findExecutable(QStringLiteral("node"));
    if (node.isEmpty())
        return std::nullopt;
    const QString path = dir + QStringLiteral("/entry.cjs");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return RunResult{};
    f.write(Pm2Entrypoint::generate(def));
    f.close();

    QProcess p;
    p.setProgram(node);
    p.setArguments({path});
    p.start();
    if (!p.waitForFinished(30000))
        return RunResult{};
    return RunResult{p.readAllStandardOutput(), p.exitCode()};
}

} // namespace

class TestPm2Entrypoint : public QObject
{
    Q_OBJECT

private slots:
    void roundTrip_keepsEveryField();
    void roundTrip_defaults();
    void parse_rejectsScriptsNotWrittenByNotepadAI();
    void run_appliesEnvAndCwd();
    void run_exitsWithTheCommandsExitCode();
};

void TestPm2Entrypoint::roundTrip_keepsEveryField()
{
    const Pm2Definition def = trickyDefinition();
    const std::optional<Pm2Definition> back = Pm2Entrypoint::parse(Pm2Entrypoint::generate(def));
    QVERIFY(back.has_value());
    QCOMPARE(back->name, def.name);
    QCOMPARE(back->command, def.command);
    QCOMPARE(back->shell, def.shell);
    QCOMPARE(back->cwd, def.cwd);
    QCOMPARE(back->envText, def.envText);
    QVERIFY(back->options == def.options);
}

void TestPm2Entrypoint::roundTrip_defaults()
{
    Pm2Definition def;
    def.name = QStringLiteral("web");
    def.command = QStringLiteral("npm run dev");
    def.cwd = QStringLiteral("/home/me/web");
    const std::optional<Pm2Definition> back = Pm2Entrypoint::parse(Pm2Entrypoint::generate(def));
    QVERIFY(back.has_value());
    QVERIFY(*back == def);
    QVERIFY(back->options.autorestart);
    QVERIFY(!back->options.watch);
}

void TestPm2Entrypoint::parse_rejectsScriptsNotWrittenByNotepadAI()
{
    QVERIFY(!Pm2Entrypoint::parse(QByteArray()).has_value());
    QVERIFY(!Pm2Entrypoint::parse("console.log('hi')\n").has_value());
    QVERIFY(!Pm2Entrypoint::parse("const http = require('http');\nhttp.createServer();\n").has_value());

    // Our header with a damaged definition line.
    QByteArray generated = Pm2Entrypoint::generate(trickyDefinition());
    const int secondLine = generated.indexOf('\n') + 1;
    QVERIFY(secondLine > 0);
    QByteArray broken = generated;
    broken.insert(secondLine + 20, "}}}");
    QVERIFY(!Pm2Entrypoint::parse(broken).has_value());

    // A definition without a name or a command is not usable.
    Pm2Definition noCommand;
    noCommand.name = QStringLiteral("x");
    QVERIFY(!Pm2Entrypoint::parse(Pm2Entrypoint::generate(noCommand)).has_value());
}

void TestPm2Entrypoint::run_appliesEnvAndCwd()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QTemporaryDir cwd;
    QVERIFY(cwd.isValid());

    Pm2Definition def;
    def.name = QStringLiteral("env-check");
    def.command = QStringLiteral(
        "node -e \"process.stdout.write(process.env.PM2_TEST_A + '|' + process.env.PM2_TEST_B + '|' + process.cwd())\"");
    def.cwd = QDir::toNativeSeparators(cwd.path());
    def.envText = QStringLiteral("# ignored\nPM2_TEST_A=\"hello world\"\n  PM2_TEST_B = two\n");

    const std::optional<RunResult> r = runEntrypoint(def, dir.path());
    if (!r)
        QSKIP("node is not installed");
    QCOMPARE(r->exitCode, 0);
    const QStringList parts = QString::fromUtf8(r->out).split(QLatin1Char('|'));
    QCOMPARE(parts.size(), 3);
    QCOMPARE(parts.at(0), QStringLiteral("hello world"));
    QCOMPARE(parts.at(1), QStringLiteral("two"));
    QCOMPARE(QFileInfo(parts.at(2)).canonicalFilePath(), QFileInfo(cwd.path()).canonicalFilePath());
}

void TestPm2Entrypoint::run_exitsWithTheCommandsExitCode()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Pm2Definition def;
    def.name = QStringLiteral("exit-check");
    def.command = QStringLiteral("node -e \"process.exit(7)\"");
    def.cwd = QDir::toNativeSeparators(dir.path());

    const std::optional<RunResult> r = runEntrypoint(def, dir.path());
    if (!r)
        QSKIP("node is not installed");
    QCOMPARE(r->exitCode, 7);
}

QTEST_GUILESS_MAIN(TestPm2Entrypoint)
#include "test_pm2_entrypoint.moc"
