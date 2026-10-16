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

#include "ClaudeSessionFiles.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

namespace {

void writeFile(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(bytes);
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

qint64 isoMs(const char *iso)
{
    return QDateTime::fromString(QString::fromLatin1(iso), Qt::ISODateWithMs).toMSecsSinceEpoch();
}

} // namespace

class TestClaudeSessionFiles : public QObject
{
    Q_OBJECT

private slots:
    void configDir_agentEnvWins();
    void configDir_fallsBackToProcessEnvThenHome();
    void encodeProjectDir_replacesNonAlphanumerics();
    void locate_findsCwdFolderFirst();
    void locate_scansOtherProjectFolders();
    void locate_missingIsEmpty();
    void newestRecord_skipsUntimedTrailingRecords();
    void newestRecord_findsTimestampBeyondTailWindow();
    void newestRecord_noTimestampIsMinusOne();
    void copy_writesIntoSameFolderNameAndSidecar();
    void copy_overwritesOlderCopy();
};

void TestClaudeSessionFiles::configDir_agentEnvWins()
{
    QProcessEnvironment proc;
    proc.insert(QStringLiteral("CLAUDE_CONFIG_DIR"), QStringLiteral("C:/proc"));
    QMap<QString, QString> env;
    env.insert(QStringLiteral("CLAUDE_CONFIG_DIR"), QStringLiteral("  C:\\Users\\me\\.claude-sub  "));
    QCOMPARE(ClaudeSessionFiles::configDir(env, proc, QStringLiteral("C:/Users/me")),
             QStringLiteral("C:/Users/me/.claude-sub"));
}

void TestClaudeSessionFiles::configDir_fallsBackToProcessEnvThenHome()
{
    QProcessEnvironment proc;
    proc.insert(QStringLiteral("CLAUDE_CONFIG_DIR"), QStringLiteral("/srv/claude"));
    QCOMPARE(ClaudeSessionFiles::configDir({}, proc, QStringLiteral("/home/me")),
             QStringLiteral("/srv/claude"));
    QCOMPARE(ClaudeSessionFiles::configDir({}, QProcessEnvironment(), QStringLiteral("/home/me")),
             QStringLiteral("/home/me/.claude"));
}

void TestClaudeSessionFiles::encodeProjectDir_replacesNonAlphanumerics()
{
    // Observed on disk: D:\projects\Cpp\notepad-ade -> D--projects-Cpp-notepad-ade.
    QCOMPARE(ClaudeSessionFiles::encodeProjectDir(QStringLiteral("D:\\projects\\Cpp\\notepad-ade")),
             QStringLiteral("D--projects-Cpp-notepad-ade"));
    QCOMPARE(ClaudeSessionFiles::encodeProjectDir(QStringLiteral("/home/me/my app.v2")),
             QStringLiteral("-home-me-my-app-v2"));
}

void TestClaudeSessionFiles::locate_findsCwdFolderFirst()
{
    QTemporaryDir cfg;
    const QString id = QStringLiteral("ef4daa04-5edc-4ef7-bca9-560c4f9e1cc5");
    const QString own = cfg.path() + QStringLiteral("/projects/D--tmp-probe/") + id + QStringLiteral(".jsonl");
    writeFile(own, "{}\n");
    writeFile(cfg.path() + QStringLiteral("/projects/aaa-other/") + id + QStringLiteral(".jsonl"), "{}\n");
    QCOMPARE(ClaudeSessionFiles::locateTranscript(cfg.path(), QStringLiteral("D:\\tmp\\probe"), id), own);
}

void TestClaudeSessionFiles::locate_scansOtherProjectFolders()
{
    // Worktrees and over-long cwds end up in folders we cannot derive.
    QTemporaryDir cfg;
    const QString id = QStringLiteral("11111111-2222-3333-4444-555555555555");
    const QString path = cfg.path() + QStringLiteral("/projects/D--tmp-probe-abc123/") + id + QStringLiteral(".jsonl");
    writeFile(path, "{}\n");
    QCOMPARE(ClaudeSessionFiles::locateTranscript(cfg.path(), QStringLiteral("D:\\tmp\\probe"), id), path);
}

void TestClaudeSessionFiles::locate_missingIsEmpty()
{
    QTemporaryDir cfg;
    QVERIFY(ClaudeSessionFiles::locateTranscript(cfg.path(), QStringLiteral("D:\\x"), QStringLiteral("nope")).isEmpty());
    QVERIFY(ClaudeSessionFiles::locateTranscript(cfg.path() + QStringLiteral("/absent"), QStringLiteral("D:\\x"),
                                                 QStringLiteral("nope")).isEmpty());
}

void TestClaudeSessionFiles::newestRecord_skipsUntimedTrailingRecords()
{
    QTemporaryDir dir;
    const QString path = dir.path() + QStringLiteral("/s.jsonl");
    writeFile(path,
              "{\"type\":\"user\",\"timestamp\":\"2026-10-16T09:48:10.000Z\"}\n"
              "{\"type\":\"assistant\",\"message\":{\"content\":\"say \\\"timestamp\\\":\\\"1999\\\"\"},"
              "\"timestamp\":\"2026-10-16T09:48:12.345Z\"}\n"
              "{\"type\":\"summary\",\"summary\":\"probe\"}\n");
    QCOMPARE(ClaudeSessionFiles::newestRecordMs(path), isoMs("2026-10-16T09:48:12.345Z"));
}

void TestClaudeSessionFiles::newestRecord_findsTimestampBeyondTailWindow()
{
    QTemporaryDir dir;
    const QString path = dir.path() + QStringLiteral("/s.jsonl");
    QByteArray big = "{\"type\":\"assistant\",\"message\":\"";
    big += QByteArray(300 * 1024, 'x');
    big += "\",\"timestamp\":\"2026-10-16T10:00:00.000Z\"}\n";
    writeFile(path, "{\"timestamp\":\"2026-10-16T09:00:00.000Z\"}\n" + big
                        + "{\"type\":\"summary\",\"leaf\":\"" + QByteArray(200 * 1024, 'y') + "\"}\n");
    QCOMPARE(ClaudeSessionFiles::newestRecordMs(path), isoMs("2026-10-16T10:00:00.000Z"));
}

void TestClaudeSessionFiles::newestRecord_noTimestampIsMinusOne()
{
    QTemporaryDir dir;
    const QString path = dir.path() + QStringLiteral("/s.jsonl");
    writeFile(path, "{\"type\":\"summary\"}\n");
    QCOMPARE(ClaudeSessionFiles::newestRecordMs(path), qint64(-1));
    QCOMPARE(ClaudeSessionFiles::newestRecordMs(dir.path() + QStringLiteral("/absent.jsonl")), qint64(-1));
}

void TestClaudeSessionFiles::copy_writesIntoSameFolderNameAndSidecar()
{
    QTemporaryDir src;
    QTemporaryDir dst;
    const QString folder = src.path() + QStringLiteral("/projects/D--tmp-probe-hashed");
    writeFile(folder + QStringLiteral("/abc.jsonl"), "line1\nline2\n");
    writeFile(folder + QStringLiteral("/abc/subagents/agent-1.jsonl"), "sub\n");
    writeFile(folder + QStringLiteral("/abc/tool-results/r.txt"), "out\n");

    const auto outcome = ClaudeSessionFiles::copySession(folder + QStringLiteral("/abc.jsonl"), dst.path());
    QVERIFY2(outcome.ok, qPrintable(outcome.error));
    const QString dstFolder = dst.path() + QStringLiteral("/projects/D--tmp-probe-hashed");
    QCOMPARE(outcome.destPath, dstFolder + QStringLiteral("/abc.jsonl"));
    QCOMPARE(readFile(dstFolder + QStringLiteral("/abc.jsonl")), QByteArray("line1\nline2\n"));
    QCOMPARE(readFile(dstFolder + QStringLiteral("/abc/subagents/agent-1.jsonl")), QByteArray("sub\n"));
    QCOMPARE(readFile(dstFolder + QStringLiteral("/abc/tool-results/r.txt")), QByteArray("out\n"));
}

void TestClaudeSessionFiles::copy_overwritesOlderCopy()
{
    // Switching A -> B -> A: B's copy is the live one, A's is stale.
    QTemporaryDir src;
    QTemporaryDir dst;
    const QString rel = QStringLiteral("/projects/p/s.jsonl");
    writeFile(src.path() + rel, "new conversation, longer than before\n");
    writeFile(dst.path() + rel, "old conversation, much much longer stale content here\n");
    const auto outcome = ClaudeSessionFiles::copySession(src.path() + rel, dst.path());
    QVERIFY2(outcome.ok, qPrintable(outcome.error));
    QCOMPARE(readFile(dst.path() + rel), QByteArray("new conversation, longer than before\n"));
}

QTEST_GUILESS_MAIN(TestClaudeSessionFiles)
#include "test_claude_session_files.moc"
