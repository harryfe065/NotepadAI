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
#include <QCoreApplication>
#include <QFile>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>

#include "AcpAgentDefinition.h"
#include "AcpAgentManager.h"
#include "AcpConnection.h"
#include "AcpOpenSessions.h"
#include "AcpAgentRegistry.h"
#include "AcpHistoryStore.h"
#include "AiAgentDock.h"
#include "ApplicationSettings.h"
#include "DataPaths.h"

class TestAcpAgentManager : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void construct_exposesRegistry_andSeedsBuiltin();
    void historyStore_runsOnWorkerThread();
    void shutdown_joinsWorkerThread();
    void registry_pointerIsStableAcrossCalls();
    void deleteSessionHistory_removesFile();
    void runHeadlessPrompt_tearsDownWhenProcessFails();
    void runHeadlessPrompt_doesNotOpenDock_closeSessionFreesImmediately();
    void openAgent_groupsByCwd_andRaiseFlag();
    void openSessions_persistAndRestoreDormant();

private:
    QTemporaryDir tempDir;
};

void TestAcpAgentManager::initTestCase()
{
    QVERIFY(tempDir.isValid());
    QCoreApplication::setOrganizationName("NotepadNextTest");
    QCoreApplication::setApplicationName("NotepadNextTest_AcpAgentManager");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tempDir.path());
    // Session history lives under the data dir: keep it in the temp dir too.
    DataPaths::init(tempDir.path(), DataPaths::Source::Default);
}

void TestAcpAgentManager::init()
{
    // Reset persistent Ai/* keys between tests.
    QSettings s;
    s.remove(QStringLiteral("Ai"));
    s.sync();
}

void TestAcpAgentManager::construct_exposesRegistry_andSeedsBuiltin()
{
    ApplicationSettings settings;
    AcpAgentManager manager(&settings);
    AcpAgentRegistry *reg = manager.registry();
    QVERIFY(reg != nullptr);
    QVERIFY(reg->contains(AcpAgentRegistry::builtinClaudeCodeId()));
}

void TestAcpAgentManager::historyStore_runsOnWorkerThread()
{
    ApplicationSettings settings;
    AcpAgentManager manager(&settings);
    QThread *historyThread = manager.historyStoreThread();
    QVERIFY(historyThread != nullptr);
    QVERIFY(historyThread->isRunning());
    QVERIFY(historyThread != QThread::currentThread());

    QVERIFY(manager.historyStore() != nullptr);
    QCOMPARE(manager.historyStore()->thread(), historyThread);
}

void TestAcpAgentManager::shutdown_joinsWorkerThread()
{
    ApplicationSettings settings;
    AcpAgentManager manager(&settings);
    QThread *historyThread = manager.historyStoreThread();
    QVERIFY(historyThread->isRunning());

    manager.shutdown();

    // After shutdown the worker thread has been quit() + wait()ed.
    QVERIFY(!historyThread->isRunning());
}

void TestAcpAgentManager::registry_pointerIsStableAcrossCalls()
{
    ApplicationSettings settings;
    AcpAgentManager manager(&settings);
    QVERIFY(manager.registry() == manager.registry());
}

void TestAcpAgentManager::deleteSessionHistory_removesFile()
{
    ApplicationSettings settings;
    AcpAgentManager manager(&settings);

    AcpHistoryStore *store = manager.historyStore();
    QVERIFY(store != nullptr);

    // Point the store at a fresh tmp dir so we don't collide with real data.
    QTemporaryDir histDir;
    QVERIFY(histDir.isValid());
    QMetaObject::invokeMethod(store, "setHistoryDir", Qt::BlockingQueuedConnection,
                              Q_ARG(QString, histDir.path()));

    const QString sid = QStringLiteral("session-to-delete");
    QSignalSpy flushedSpy(store, &AcpHistoryStore::flushed);
    QSignalSpy deletedSpy(store, &AcpHistoryStore::historyDeleted);

    // Schedule a write and flush it so a real file lands on disk.
    QJsonObject payload;
    payload.insert(QStringLiteral("sessionId"), sid);
    QMetaObject::invokeMethod(store, "scheduleWrite", Qt::QueuedConnection,
                              Q_ARG(QString, sid),
                              Q_ARG(QJsonObject, payload));
    QMetaObject::invokeMethod(store, "flushAll", Qt::BlockingQueuedConnection);
    QVERIFY(flushedSpy.wait(2000) || flushedSpy.count() >= 1);

    const QString filePath = histDir.path() + QStringLiteral("/") + sid + QStringLiteral(".json");
    QVERIFY(QFile::exists(filePath));

    // Drive the manager API under test — must route to the store on its
    // worker thread and ultimately delete the file.
    manager.deleteSessionHistory(sid);

    QVERIFY(deletedSpy.wait(2000));
    QVERIFY(!QFile::exists(filePath));
}

void TestAcpAgentManager::runHeadlessPrompt_tearsDownWhenProcessFails()
{
    ApplicationSettings settings;
    AcpAgentManager manager(&settings);

    AcpAgentDefinition def;
    def.id = QStringLiteral("test-headless");
    def.name = QStringLiteral("Headless");
    def.command = QStringLiteral("definitely-not-a-real-binary-xyz");
    QVERIFY(manager.registry()->addAgent(def));

    QTemporaryDir workDir;
    QVERIFY(workDir.isValid());

    QSignalSpy finishedSpy(&manager, &AcpAgentManager::headlessSessionFinished);
    const QString sid = manager.runHeadlessPrompt(def.id, workDir.path(), QStringLiteral("commit"));
    QVERIFY(!sid.isEmpty());

    QTRY_VERIFY_WITH_TIMEOUT(finishedSpy.count() >= 1, 5000);
    QCOMPARE(finishedSpy.first().at(0).toString(), sid);
    QCOMPARE(finishedSpy.first().at(1).toBool(), false);
    QVERIFY(manager.connectionFor(sid) == nullptr);
}

void TestAcpAgentManager::runHeadlessPrompt_doesNotOpenDock_closeSessionFreesImmediately()
{
    ApplicationSettings settings;
    AcpAgentManager manager(&settings);

    AcpAgentDefinition def;
    def.id = QStringLiteral("test-headless-close");
    def.name = QStringLiteral("Headless");
    def.command = QStringLiteral("definitely-not-a-real-binary-xyz");
    QVERIFY(manager.registry()->addAgent(def));

    QTemporaryDir workDir;
    QVERIFY(workDir.isValid());

    QSignalSpy openedSpy(&manager, &AcpAgentManager::agentOpened);
    const QString sid = manager.runHeadlessPrompt(def.id, workDir.path(), QStringLiteral("commit"));
    QVERIFY(!sid.isEmpty());
    QCOMPARE(openedSpy.count(), 0);

    manager.closeSession(sid);
    QVERIFY(manager.connectionFor(sid) == nullptr);
}

void TestAcpAgentManager::openAgent_groupsByCwd_andRaiseFlag()
{
    ApplicationSettings settings;
    AcpAgentManager manager(&settings);

    AcpAgentDefinition def;
    def.id = QStringLiteral("test-group");
    def.name = QStringLiteral("Grouped");
    def.command = QStringLiteral("definitely-not-a-real-binary-xyz");
    QVERIFY(manager.registry()->addAgent(def));

    QTemporaryDir workDir;
    QVERIFY(workDir.isValid());
    QTemporaryDir otherDir;
    QVERIFY(otherDir.isValid());

    AiAgentDock *first = manager.openAgent(def.id, workDir.path());
    QVERIFY(first != nullptr);
    QCOMPARE(first->slotCount(), 1);
    const QString firstId = first->sessionId();

    AiAgentDock *background = manager.openAgent(def.id, workDir.path(), false, nullptr, false);
    QCOMPARE(background, first);
    QCOMPARE(first->slotCount(), 2);
    QCOMPARE(first->sessionId(), firstId);
    QCOMPARE(first->currentIndex(), 0);

    AiAgentDock *other = manager.openAgent(def.id, otherDir.path());
    QVERIFY(other != first);
    QCOMPARE(other->slotCount(), 1);

    QPointer<AiAgentDock> firstPtr(first);
    manager.closeSession(first->sessionIdAt(1));
    QVERIFY(!firstPtr.isNull());
    QCOMPARE(first->slotCount(), 1);
    manager.closeSession(first->sessionIdAt(0));
    QTRY_VERIFY_WITH_TIMEOUT(firstPtr.isNull(), 2000);

    QPointer<AiAgentDock> otherPtr(other);
    other->close();
    QTRY_VERIFY_WITH_TIMEOUT(otherPtr.isNull(), 2000);
}

// Restored sessions: the open-session list follows opens and closes, survives
// the shutdown teardown, and a new manager rebuilds it with dormant agents.
void TestAcpAgentManager::openSessions_persistAndRestoreDormant()
{
    ApplicationSettings settings;
    const QString key = QLatin1String(AcpOpenSessions::kSettingsKey);
    AcpAgentDefinition def;
    def.id = QStringLiteral("test-restore");
    def.name = QStringLiteral("CC restore");
    def.command = QStringLiteral("definitely-not-a-real-binary-xyz");
    QTemporaryDir workDir;
    QVERIFY(workDir.isValid());

    QStringList ids;
    {
        AcpAgentManager manager(&settings);
        QVERIFY(manager.registry()->addAgent(def));
        AiAgentDock *dock = manager.openAgent(def.id, workDir.path());
        QVERIFY(dock);
        manager.openAgent(def.id, workDir.path());
        manager.openAgent(def.id, workDir.path());
        QCOMPARE(dock->slotCount(), 3);
        manager.closeSession(dock->sessionIdAt(0));
        ids = {dock->sessionIdAt(0), dock->sessionIdAt(1)};
        QTRY_COMPARE_WITH_TIMEOUT(AcpOpenSessions::fromJson(settings.value(key).toString().toUtf8())
                                      .value(0).sessions.size(),
                                  2, 2000);
        manager.freezeOpenSessions();
        manager.shutdown(); // tears every slot down: must not empty the list
    }

    const QVector<AcpOpenSessions::Group> saved =
        AcpOpenSessions::fromJson(settings.value(key).toString().toUtf8());
    QCOMPARE(saved.size(), 1);
    QCOMPARE(saved.at(0).sessions.size(), 2);
    QCOMPARE(saved.at(0).sessions.at(0).id, ids.at(0));
    QCOMPARE(saved.at(0).sessions.at(1).agentId, def.id);
    QCOMPARE(saved.at(0).sessions.at(1).agentName, def.name);

    AcpAgentManager restored(&settings);
    // The registry persists in the same settings: the agent is already there.
    QVERIFY(restored.registry()->contains(def.id));
    const QList<AiAgentDock *> docks = restored.restoreOpenSessions({});
    QCOMPARE(docks.size(), 1);
    QCOMPARE(docks.at(0)->slotCount(), 2);
    QCOMPARE(docks.at(0)->sessionIdAt(0), ids.at(0));
    QCOMPARE(docks.at(0)->sessionIdAt(1), ids.at(1));
    for (const QString &id : std::as_const(ids)) {
        AcpConnection *conn = restored.connectionFor(id);
        QVERIFY(conn);
        QVERIFY(conn->isDormant()); // no process until the first prompt
        QCOMPARE(conn->definition().id, def.id);
    }
    QList<QPointer<AiAgentDock>> alive;
    for (AiAgentDock *d : docks)
        alive.append(d);
    restored.shutdown();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    for (const QPointer<AiAgentDock> &d : std::as_const(alive))
        delete d.data();
}

QTEST_MAIN(TestAcpAgentManager)
#include "test_acp_agent_manager.moc"
