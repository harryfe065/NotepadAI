/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Integration test for the load-on-construct contract from task 3.4: a session
 * survives a "restart" round-trip via the default history directory (no
 * historyDirOverride), under DataPaths::appDataLocation(). The data dir is
 * pointed at a temp dir so the scratch never touches the real one.
 */

#include <QtTest>
#include <QFile>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>

#include "AcpHistoryStore.h"
#include "AcpProtocol.h"
#include "AcpSessionModel.h"
#include "DataPaths.h"

class TestAcpSessionModelPersistenceIntegration : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void roundTripViaDefaultAppDataLocation();

private:
    QTemporaryDir m_dataDir;
};

void TestAcpSessionModelPersistenceIntegration::initTestCase()
{
    QVERIFY(m_dataDir.isValid());
    DataPaths::init(m_dataDir.path(), DataPaths::Source::Default);
}

void TestAcpSessionModelPersistenceIntegration::roundTripViaDefaultAppDataLocation()
{
    const QString sessionId = QStringLiteral("integ-session");

    // Pre-clean any leftover from a prior run.
    const QString defaultDir = DataPaths::appDataLocation() + QStringLiteral("/acp-history");
    QFile::remove(defaultDir + QStringLiteral("/") + sessionId + QStringLiteral(".json"));

    AcpHistoryStore store;
    // Do NOT setHistoryDir — exercise the default-path branch.

    {
        AcpSessionModel a(sessionId, QStringLiteral("integProj"));
        a.setHistoryStore(&store);
        a.onPromptStarted();
        a.onMessageChunk(QStringLiteral("ping"));
        a.onPromptEnded();

        QSignalSpy spy(&store, &AcpHistoryStore::flushed);
        QVERIFY(spy.wait(2000));
    }

    AcpSessionModel b(sessionId, QStringLiteral("ignored"));
    QCOMPARE(b.messages().size(), 1);
    QCOMPARE(b.messages().first().content.first().text, QStringLiteral("ping"));
    QCOMPARE(b.projectId(), QStringLiteral("integProj"));

    // Cleanup the on-disk file so subsequent runs start fresh.
    store.deleteHistory(sessionId);
}

QTEST_GUILESS_MAIN(TestAcpSessionModelPersistenceIntegration)
#include "test_acp_session_model_persistence_integration.moc"
