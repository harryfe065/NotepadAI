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

#include <QPersistentModelIndex>
#include <QSignalSpy>

#include "Pm2ProcessModel.h"

namespace {

constexpr qint64 kNow = 1'761'300'000'000;

Pm2ProcessInfo proc(int id, const QString &name, Pm2Status status = Pm2Status::Online)
{
    Pm2ProcessInfo p;
    p.pmId = id;
    p.name = name;
    p.status = status;
    p.statusText = status == Pm2Status::Online ? QStringLiteral("online") : QStringLiteral("stopped");
    p.pid = status == Pm2Status::Online ? 1000 + id : 0;
    p.startedAtMs = kNow - 3 * 3'600'000 - 12 * 60'000;
    p.restarts = id;
    p.memoryBytes = 50 * 1024 * 1024;
    p.cpuPercent = 1.5;
    p.cwd = QStringLiteral("/srv/") + name;
    return p;
}

QString text(const Pm2ProcessModel &m, int row, Pm2ProcessModel::Column col)
{
    return m.data(m.index(row, int(col)), Qt::DisplayRole).toString();
}

} // namespace

class TestPm2ProcessModel : public QObject
{
    Q_OBJECT

private slots:
    void firstList_showsEveryColumn();
    void updatedList_editsRowsInPlace();
    void removedProcess_removesOnlyItsRow();
    void newProcess_isAppended();
    void clock_refreshesUptimeOfRunningRowsOnly();
};

void TestPm2ProcessModel::firstList_showsEveryColumn()
{
    Pm2ProcessModel m;
    m.setNow(kNow);
    m.setProcesses({proc(0, QStringLiteral("api")), proc(1, QStringLiteral("job"), Pm2Status::Stopped)});

    QCOMPARE(m.rowCount(), 2);
    QCOMPARE(m.columnCount(), int(Pm2ProcessModel::Column::Count));
    QCOMPARE(text(m, 0, Pm2ProcessModel::Column::Name), QStringLiteral("api"));
    QCOMPARE(text(m, 0, Pm2ProcessModel::Column::Status), QStringLiteral("online"));
    QCOMPARE(text(m, 0, Pm2ProcessModel::Column::Uptime), QStringLiteral("3h 12m"));
    QCOMPARE(text(m, 1, Pm2ProcessModel::Column::Restarts), QStringLiteral("1"));
    QCOMPARE(text(m, 0, Pm2ProcessModel::Column::Cpu), QStringLiteral("1.5%"));
    QCOMPARE(text(m, 0, Pm2ProcessModel::Column::Pid), QStringLiteral("1000"));
    QCOMPARE(text(m, 0, Pm2ProcessModel::Column::Cwd), QStringLiteral("/srv/api"));
    QVERIFY(text(m, 0, Pm2ProcessModel::Column::Memory).startsWith(QStringLiteral("50")));

    // A process that is not running has no uptime, pid, cpu or memory.
    QVERIFY(text(m, 1, Pm2ProcessModel::Column::Uptime).isEmpty());
    QVERIFY(text(m, 1, Pm2ProcessModel::Column::Pid).isEmpty());
    QVERIFY(text(m, 1, Pm2ProcessModel::Column::Cpu).isEmpty());
    QVERIFY(text(m, 1, Pm2ProcessModel::Column::Memory).isEmpty());

    QCOMPARE(m.processAt(1).name, QStringLiteral("job"));
    QCOMPARE(m.rowForId(1), 1);
    QCOMPARE(m.rowForId(42), -1);
}

void TestPm2ProcessModel::updatedList_editsRowsInPlace()
{
    Pm2ProcessModel m;
    m.setNow(kNow);
    m.setProcesses({proc(0, QStringLiteral("api")), proc(1, QStringLiteral("job")), proc(2, QStringLiteral("web"))});
    const QPersistentModelIndex webIndex(m.index(2, 0));

    QSignalSpy changed(&m, &QAbstractItemModel::dataChanged);
    QSignalSpy inserted(&m, &QAbstractItemModel::rowsInserted);
    QSignalSpy removed(&m, &QAbstractItemModel::rowsRemoved);
    QSignalSpy reset(&m, &QAbstractItemModel::modelReset);

    QList<Pm2ProcessInfo> next{proc(0, QStringLiteral("api")), proc(1, QStringLiteral("job")), proc(2, QStringLiteral("web"))};
    next[1].cpuPercent = 80;
    next[1].restarts = 9;
    m.setProcesses(next);

    QCOMPARE(inserted.count(), 0);
    QCOMPARE(removed.count(), 0);
    QCOMPARE(reset.count(), 0);
    QCOMPARE(changed.count(), 1);
    const QModelIndex from = changed.at(0).at(0).toModelIndex();
    const QModelIndex to = changed.at(0).at(1).toModelIndex();
    QCOMPARE(from.row(), 1);
    QCOMPARE(to.row(), 1);
    QVERIFY(from.column() <= int(Pm2ProcessModel::Column::Restarts));
    QVERIFY(to.column() >= int(Pm2ProcessModel::Column::Cpu));
    QCOMPARE(text(m, 1, Pm2ProcessModel::Column::Restarts), QStringLiteral("9"));

    // The same list again changes nothing.
    changed.clear();
    m.setProcesses(next);
    QCOMPARE(changed.count(), 0);
    QCOMPARE(webIndex.row(), 2);
}

void TestPm2ProcessModel::removedProcess_removesOnlyItsRow()
{
    Pm2ProcessModel m;
    m.setNow(kNow);
    m.setProcesses({proc(0, QStringLiteral("api")), proc(1, QStringLiteral("job")), proc(2, QStringLiteral("web"))});
    const QPersistentModelIndex webIndex(m.index(2, 0));
    QSignalSpy removed(&m, &QAbstractItemModel::rowsRemoved);
    QSignalSpy reset(&m, &QAbstractItemModel::modelReset);

    m.setProcesses({proc(0, QStringLiteral("api")), proc(2, QStringLiteral("web"))});

    QCOMPARE(reset.count(), 0);
    QCOMPARE(removed.count(), 1);
    QCOMPARE(removed.at(0).at(1).toInt(), 1);
    QCOMPARE(removed.at(0).at(2).toInt(), 1);
    QCOMPARE(m.rowCount(), 2);
    QCOMPARE(webIndex.row(), 1);
    QCOMPARE(text(m, 1, Pm2ProcessModel::Column::Name), QStringLiteral("web"));

    m.setProcesses({});
    QCOMPARE(m.rowCount(), 0);
    QVERIFY(!webIndex.isValid());
}

void TestPm2ProcessModel::newProcess_isAppended()
{
    Pm2ProcessModel m;
    m.setNow(kNow);
    m.setProcesses({proc(0, QStringLiteral("api"))});
    const QPersistentModelIndex apiIndex(m.index(0, 0));
    QSignalSpy inserted(&m, &QAbstractItemModel::rowsInserted);

    m.setProcesses({proc(5, QStringLiteral("new")), proc(0, QStringLiteral("api"))});

    QCOMPARE(inserted.count(), 1);
    QCOMPARE(m.rowCount(), 2);
    QCOMPARE(apiIndex.row(), 0);
    QCOMPARE(text(m, 1, Pm2ProcessModel::Column::Name), QStringLiteral("new"));
}

void TestPm2ProcessModel::clock_refreshesUptimeOfRunningRowsOnly()
{
    Pm2ProcessModel m;
    m.setNow(kNow);
    m.setProcesses({proc(0, QStringLiteral("api")), proc(1, QStringLiteral("job"), Pm2Status::Stopped)});
    QSignalSpy changed(&m, &QAbstractItemModel::dataChanged);

    m.setNow(kNow + 60'000);

    QCOMPARE(changed.count(), 1);
    QCOMPARE(changed.at(0).at(0).toModelIndex().row(), 0);
    QCOMPARE(changed.at(0).at(0).toModelIndex().column(), int(Pm2ProcessModel::Column::Uptime));
    QCOMPARE(changed.at(0).at(1).toModelIndex().column(), int(Pm2ProcessModel::Column::Uptime));
    QCOMPARE(text(m, 0, Pm2ProcessModel::Column::Uptime), QStringLiteral("3h 13m"));
}

QTEST_GUILESS_MAIN(TestPm2ProcessModel)
#include "test_pm2_process_model.moc"
