/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

// Regression tests for the Git tab refresh path (GitStatusSync +
// GitStatusModel). The Changes tab used to rebuild everything on every
// refresh: parse on the UI thread, modelReset + expandAll, all numstat rows
// re-emitted. With an agent touching files that meant O(N) stalls per refresh
// and lost selection. These lock in: identical output is a no-op, parsing is
// off the UI thread, small changes are incremental (persistent indexes and
// numstat survive), and repeated numstat merges emit nothing.

#include <QAbstractItemModelTester>
#include <QtTest>

#include "GitStatusModel.h"
#include "GitStatusSync.h"

namespace {

const QByteArray kSha(40, '0');

QByteArray header(int ahead = 0, int behind = 0)
{
    QByteArray b;
    b += "# branch.oid " + kSha + '\0';
    b += "# branch.head master";
    b += '\0';
    b += "# branch.upstream origin/master";
    b += '\0';
    b += "# branch.ab +" + QByteArray::number(ahead) + " -" + QByteArray::number(behind) + '\0';
    return b;
}

// `1 <XY> N... 100644 100644 100644 <hH> <hI> <path>`
QByteArray changed(const char *xy, const QByteArray &path)
{
    return QByteArray("1 ") + xy + " N... 100644 100644 100644 " + kSha + ' ' + kSha + ' ' + path + '\0';
}

QByteArray conflict(const QByteArray &path)
{
    return "u UU N... 100644 100644 100644 100644 " + kSha + ' ' + kSha + ' ' + kSha + ' ' + path + '\0';
}

QByteArray untracked(const QByteArray &path) { return "? " + path + '\0'; }

QByteArray fileName(int i) { return "src/f" + QByteArray::number(i).rightJustified(5, '0') + ".cpp"; }

// Unstaged modifications of f00000 .. f<n-1>, skipping `skip(i)`.
template <typename Skip>
QByteArray trackedRange(int n, Skip skip)
{
    QByteArray b = header();
    for (int i = 0; i < n; ++i)
        if (!skip(i)) b += changed(".M", fileName(i));
    return b;
}
QByteArray trackedRange(int n) { return trackedRange(n, [](int) { return false; }); }

QByteArray numstatFor(int n, int added)
{
    QByteArray b;
    for (int i = 0; i < n; ++i)
        b += QByteArray::number(added) + "\t1\t" + fileName(i) + '\0';
    return b;
}

QModelIndex sectionIdx(const GitStatusModel &m, GitStatusEntry::Section s)
{
    for (int r = 0; r < m.rowCount(); ++r) {
        const QModelIndex i = m.index(r, 0);
        if (m.sectionOf(i) == s) return i;
    }
    return {};
}

QModelIndex childByPath(const GitStatusModel &m, GitStatusEntry::Section s, const QString &path)
{
    const QModelIndex sec = sectionIdx(m, s);
    for (int r = 0; r < m.rowCount(sec); ++r) {
        const QModelIndex i = m.index(r, 0, sec);
        if (i.data(GitStatusModel::RelPathRole).toString() == path) return i;
    }
    return {};
}

// Counts every signal a view reacts to with relayout / repaint.
struct ModelSpies {
    QSignalSpy reset, inserted, removed, changed, layout, moved;
    explicit ModelSpies(GitStatusModel *m)
        : reset(m, &QAbstractItemModel::modelReset)
        , inserted(m, &QAbstractItemModel::rowsInserted)
        , removed(m, &QAbstractItemModel::rowsRemoved)
        , changed(m, &QAbstractItemModel::dataChanged)
        , layout(m, &QAbstractItemModel::layoutChanged)
        , moved(m, &QAbstractItemModel::rowsMoved)
    {}
    int total() const
    {
        return int(reset.size() + inserted.size() + removed.size() + changed.size() + layout.size() + moved.size());
    }
};

} // namespace

class TestGitStatusSync : public QObject
{
    Q_OBJECT

private slots:
    void submit_parsesOffThreadAndAppliesLater();
    void identicalOutput_emitsUnchangedAndTouchesNothing();
    void identicalOutputWhileParsing_appliesOnce();
    void latestSubmitWins();
    void header_aheadBehindReported();
    void smallChange_isIncremental_keepsIndexesAndNumstat();
    void statusChangeInPlace_emitsDataChangedOnly();
    void sectionAppearsAbove_childIndexesStayValid();
    void sectionVanishes_removedAsTopLevelRow();
    void fragmentedChange_resetsButKeepsNumstat();
    void mergeNumstat_repeatIsSilent_changesAreTargeted();
    void mergeNumstat_missingRowGoesBackToUnknown();

private:
    // Submit and wait for the parse to land.
    static void apply(GitStatusSync &sync, const QByteArray &out)
    {
        QSignalSpy applied(&sync, &GitStatusSync::applied);
        sync.submit(out);
        QVERIFY(sync.pending());
        QVERIFY(applied.wait(5000));
        QVERIFY(!sync.pending());
    }
};

void TestGitStatusSync::submit_parsesOffThreadAndAppliesLater()
{
    GitStatusModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    GitStatusSync sync(&model);
    QSignalSpy applied(&sync, &GitStatusSync::applied);

    sync.submit(trackedRange(50));
    // Nothing is parsed on the caller's stack: the model is untouched until
    // the event loop delivers the worker's result.
    QVERIFY(sync.pending());
    QCOMPARE(model.totalEntries(), 0);
    QCOMPARE(applied.size(), 0);

    QVERIFY(applied.wait(5000));
    QCOMPARE(model.totalEntries(), 50);
    QCOMPARE(model.entriesInSection(GitStatusEntry::Tracked), 50);
}

void TestGitStatusSync::identicalOutput_emitsUnchangedAndTouchesNothing()
{
    GitStatusModel model;
    GitStatusSync sync(&model);
    const QByteArray out = trackedRange(200);
    apply(sync, out);
    const auto serial = model.changeSerial();

    ModelSpies spies(&model);
    QSignalSpy unchanged(&sync, &GitStatusSync::unchanged);
    QSignalSpy applied(&sync, &GitStatusSync::applied);
    sync.submit(QByteArray(out.constData(), out.size())); // equal bytes, distinct buffer

    QCOMPARE(unchanged.size(), 1); // synchronous, no parse
    QVERIFY(!sync.pending());
    QTest::qWait(50);
    QCOMPARE(applied.size(), 0);
    QCOMPARE(spies.total(), 0);
    QCOMPARE(model.changeSerial(), serial);
}

void TestGitStatusSync::identicalOutputWhileParsing_appliesOnce()
{
    GitStatusModel model;
    GitStatusSync sync(&model);
    QSignalSpy unchanged(&sync, &GitStatusSync::unchanged);
    QSignalSpy applied(&sync, &GitStatusSync::applied);
    const QByteArray out = trackedRange(100);

    sync.submit(out);
    sync.submit(out); // the parse in flight already covers these bytes
    QCOMPARE(unchanged.size(), 0);
    QVERIFY(applied.wait(5000));
    QTest::qWait(50);
    QCOMPARE(applied.size(), 1);
    QCOMPARE(model.totalEntries(), 100);
}

void TestGitStatusSync::latestSubmitWins()
{
    GitStatusModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    GitStatusSync sync(&model);
    QSignalSpy applied(&sync, &GitStatusSync::applied);

    sync.submit(trackedRange(5000)); // slower parse, superseded
    sync.submit(trackedRange(7));
    QTRY_VERIFY_WITH_TIMEOUT(!sync.pending(), 5000);
    QTest::qWait(100); // let the stale worker finish and be dropped
    QCOMPARE(applied.size(), 1);
    QCOMPARE(model.totalEntries(), 7);
}

void TestGitStatusSync::header_aheadBehindReported()
{
    GitStatusModel model;
    GitStatusSync sync(&model);
    GitStatusParser::Header h;
    connect(&sync, &GitStatusSync::applied, this, [&h](const GitStatusParser::Header &got) { h = got; });
    apply(sync, header(3, 7) + changed(".M", "a.txt"));
    QCOMPARE(h.ahead, 3);
    QCOMPARE(h.behind, 7);
    QVERIFY(h.hasUpstream);
}

void TestGitStatusSync::smallChange_isIncremental_keepsIndexesAndNumstat()
{
    // The agent scenario: thousands of dirty files, one more appears and one
    // goes clean between refreshes.
    constexpr int n = 4000;
    GitStatusModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    GitStatusSync sync(&model);
    apply(sync, trackedRange(n, [](int i) { return i == 2001; }));
    model.mergeNumstat(GitNumstatParser::parse(numstatFor(n, 9)), false);

    const QString keptPath = QString::fromLatin1(fileName(3000));
    QPersistentModelIndex kept = childByPath(model, GitStatusEntry::Tracked, keptPath);
    QVERIFY(kept.isValid());
    QCOMPARE(kept.data(GitStatusModel::AddedLinesRole).toInt(), 9);

    ModelSpies spies(&model);
    apply(sync, trackedRange(n, [](int i) { return i == 1000; })); // 1000 clean, 2001 dirty

    QCOMPARE(spies.reset.size(), 0);
    QCOMPARE(spies.layout.size(), 0);
    QCOMPARE(spies.removed.size(), 1);
    QCOMPARE(spies.inserted.size(), 1);
    QCOMPARE(spies.removed.at(0).at(1).toInt(), spies.removed.at(0).at(2).toInt()); // one row
    QCOMPARE(spies.inserted.at(0).at(1).toInt(), spies.inserted.at(0).at(2).toInt());
    // Same count: the section header text did not change, kept rows did not either.
    QCOMPARE(spies.changed.size(), 0);

    QVERIFY(kept.isValid());
    QCOMPARE(kept.data(GitStatusModel::RelPathRole).toString(), keptPath);
    QCOMPARE(kept.data(GitStatusModel::AddedLinesRole).toInt(), 9); // carried, not blanked
    QVERIFY(!childByPath(model, GitStatusEntry::Tracked, QString::fromLatin1(fileName(1000))).isValid());
    const QModelIndex added = childByPath(model, GitStatusEntry::Tracked, QString::fromLatin1(fileName(2001)));
    QVERIFY(added.isValid());
    QCOMPARE(added.data(GitStatusModel::AddedLinesRole).toInt(), -1); // unknown until numstat
    QCOMPARE(model.entriesInSection(GitStatusEntry::Tracked), n - 1);
}

void TestGitStatusSync::statusChangeInPlace_emitsDataChangedOnly()
{
    GitStatusModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    GitStatusSync sync(&model);
    apply(sync, header() + changed(".M", "a") + changed(".M", "b") + changed(".M", "c"));
    QPersistentModelIndex b = childByPath(model, GitStatusEntry::Tracked, QStringLiteral("b"));

    ModelSpies spies(&model);
    apply(sync, header() + changed(".M", "a") + changed(".D", "b") + changed(".M", "c"));

    QCOMPARE(spies.reset.size(), 0);
    QCOMPARE(spies.inserted.size(), 0);
    QCOMPARE(spies.removed.size(), 0);
    QCOMPARE(spies.changed.size(), 1);
    QCOMPARE(spies.changed.at(0).at(0).value<QModelIndex>(), QModelIndex(b));
    QCOMPARE(spies.changed.at(0).at(1).value<QModelIndex>(), QModelIndex(b));
    QCOMPARE(b.data(GitStatusModel::ChangeRole).toInt(), int(GitStatusEntry::Deleted));
}

void TestGitStatusSync::sectionAppearsAbove_childIndexesStayValid()
{
    GitStatusModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    GitStatusSync sync(&model);
    apply(sync, header() + changed(".M", "a") + changed(".M", "b"));
    QPersistentModelIndex b = childByPath(model, GitStatusEntry::Tracked, QStringLiteral("b"));
    QCOMPARE(b.parent().row(), 0);

    ModelSpies spies(&model);
    apply(sync, header() + changed(".M", "a") + changed(".M", "b") + conflict("z"));

    QCOMPARE(spies.reset.size(), 0);
    QCOMPARE(spies.inserted.size(), 1);
    QVERIFY(!spies.inserted.at(0).at(0).value<QModelIndex>().isValid()); // top-level
    QVERIFY(b.isValid());
    QCOMPARE(b.data(GitStatusModel::RelPathRole).toString(), QStringLiteral("b"));
    QCOMPARE(b.parent().row(), 1); // Conflicts sits above Tracked
    QCOMPARE(model.sectionOf(b), GitStatusEntry::Tracked);
}

void TestGitStatusSync::sectionVanishes_removedAsTopLevelRow()
{
    GitStatusModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    GitStatusSync sync(&model);
    apply(sync, header() + changed("M.", "s") + changed(".M", "t") + untracked("u"));
    QCOMPARE(model.rowCount(), 3);
    QPersistentModelIndex u = childByPath(model, GitStatusEntry::Untracked, QStringLiteral("u"));

    ModelSpies spies(&model);
    apply(sync, header() + changed(".M", "t") + untracked("u"));

    QCOMPARE(spies.reset.size(), 0);
    QCOMPARE(spies.removed.size(), 1);
    QVERIFY(!spies.removed.at(0).at(0).value<QModelIndex>().isValid());
    QCOMPARE(model.rowCount(), 2);
    QVERIFY(u.isValid());
    QCOMPARE(u.parent().row(), 1);
}

void TestGitStatusSync::fragmentedChange_resetsButKeepsNumstat()
{
    // Half the rows vanish in alternating runs: replaying hundreds of
    // remove-runs costs more than one reset.
    constexpr int n = 1000;
    GitStatusModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    GitStatusSync sync(&model);
    apply(sync, trackedRange(n));
    model.mergeNumstat(GitNumstatParser::parse(numstatFor(n, 4)), false);

    ModelSpies spies(&model);
    apply(sync, trackedRange(n, [](int i) { return i % 2 == 1; }));

    QCOMPARE(spies.reset.size(), 1);
    QCOMPARE(model.entriesInSection(GitStatusEntry::Tracked), n / 2);
    const QModelIndex row = childByPath(model, GitStatusEntry::Tracked, QString::fromLatin1(fileName(500)));
    QVERIFY(row.isValid());
    QCOMPARE(row.data(GitStatusModel::AddedLinesRole).toInt(), 4);
}

void TestGitStatusSync::mergeNumstat_repeatIsSilent_changesAreTargeted()
{
    constexpr int n = 3000;
    GitStatusModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    GitStatusSync sync(&model);
    apply(sync, trackedRange(n));
    const QByteArray stats = numstatFor(n, 2);
    model.mergeNumstat(GitNumstatParser::parse(stats), false);

    // Same numbers again (agent re-saved a file without changing line counts).
    ModelSpies spies(&model);
    model.mergeNumstat(GitNumstatParser::parse(stats), false);
    QCOMPARE(spies.total(), 0);

    // One file's counts move: exactly that row is signalled.
    QByteArray bumped = stats;
    const QByteArray from = "2\t1\t" + fileName(1234) + '\0';
    const QByteArray to = "8\t1\t" + fileName(1234) + '\0';
    bumped.replace(from, to);
    model.mergeNumstat(GitNumstatParser::parse(bumped), false);
    QCOMPARE(spies.changed.size(), 1);
    const QModelIndex row = childByPath(model, GitStatusEntry::Tracked, QString::fromLatin1(fileName(1234)));
    QCOMPARE(spies.changed.at(0).at(0).value<QModelIndex>(), row);
    QCOMPARE(spies.changed.at(0).at(1).value<QModelIndex>(), row);
    QCOMPARE(row.data(GitStatusModel::AddedLinesRole).toInt(), 8);

    // The staged side has no rows here: merging it touches nothing.
    model.mergeNumstat(GitNumstatParser::parse(bumped), true);
    QCOMPARE(spies.changed.size(), 1);
}

void TestGitStatusSync::mergeNumstat_missingRowGoesBackToUnknown()
{
    GitStatusModel model;
    GitStatusSync sync(&model);
    apply(sync, header() + changed(".M", "a") + changed(".M", "b"));
    model.mergeNumstat(GitNumstatParser::parse(QByteArray("1\t1\ta") + '\0' + "5\t0\tb" + '\0'), false);
    const QModelIndex b = childByPath(model, GitStatusEntry::Tracked, QStringLiteral("b"));
    QCOMPARE(b.data(GitStatusModel::AddedLinesRole).toInt(), 5);

    // `b` was reverted to its index content between the two numstat runs.
    model.mergeNumstat(GitNumstatParser::parse(QByteArray("1\t1\ta") + '\0'), false);
    QCOMPARE(b.data(GitStatusModel::AddedLinesRole).toInt(), -1);

    // An empty diff clears every row of that side.
    model.mergeNumstat({}, false);
    const QModelIndex a = childByPath(model, GitStatusEntry::Tracked, QStringLiteral("a"));
    QCOMPARE(a.data(GitStatusModel::AddedLinesRole).toInt(), -1);
}

QTEST_MAIN(TestGitStatusSync)
#include "test_git_status_sync.moc"
