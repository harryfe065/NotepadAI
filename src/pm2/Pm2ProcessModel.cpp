/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "Pm2ProcessModel.h"

#include <QLocale>
#include <QtAlgorithms>

namespace {

bool isRunning(const Pm2ProcessInfo &p)
{
    return pm2IsRunning(p.status);
}

using Column = Pm2ProcessModel::Column;

// Columns whose shown value differs between `a` and `b` (bit per column).
uint changedColumns(const Pm2ProcessInfo &a, const Pm2ProcessInfo &b)
{
    auto bit = [](Column c) { return 1u << int(c); };
    uint mask = 0;
    if (a.name != b.name) mask |= bit(Column::Name);
    if (a.status != b.status || a.statusText != b.statusText)
        mask |= bit(Column::Status) | bit(Column::Uptime) | bit(Column::Cpu) | bit(Column::Memory)
            | bit(Column::Pid);
    if (a.startedAtMs != b.startedAtMs) mask |= bit(Column::Uptime);
    if (a.restarts != b.restarts || a.unstableRestarts != b.unstableRestarts) mask |= bit(Column::Restarts);
    if (a.cpuPercent != b.cpuPercent) mask |= bit(Column::Cpu);
    if (a.memoryBytes != b.memoryBytes) mask |= bit(Column::Memory);
    if (a.pid != b.pid) mask |= bit(Column::Pid);
    if (a.cwd != b.cwd || a.script != b.script) mask |= bit(Column::Cwd);
    return mask;
}

} // namespace

Pm2ProcessModel::Pm2ProcessModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

void Pm2ProcessModel::reindex()
{
    m_rowOfId.clear();
    m_rowOfId.reserve(m_rows.size());
    for (int i = 0; i < m_rows.size(); ++i)
        m_rowOfId.insert(m_rows.at(i).pmId, i);
}

void Pm2ProcessModel::setProcesses(const QList<Pm2ProcessInfo> &processes)
{
    QHash<int, int> incoming;
    incoming.reserve(processes.size());
    for (int i = 0; i < processes.size(); ++i)
        incoming.insert(processes.at(i).pmId, i);

    // Removals, back to front in contiguous runs.
    bool removed = false;
    for (int row = int(m_rows.size()) - 1; row >= 0;) {
        if (incoming.contains(m_rows.at(row).pmId)) {
            --row;
            continue;
        }
        int first = row;
        while (first > 0 && !incoming.contains(m_rows.at(first - 1).pmId))
            --first;
        beginRemoveRows(QModelIndex(), first, row);
        m_rows.remove(first, row - first + 1);
        endRemoveRows();
        removed = true;
        row = first - 1;
    }
    if (removed)
        reindex();

    // In-place edits.
    for (int row = 0; row < m_rows.size(); ++row) {
        const Pm2ProcessInfo &next = processes.at(incoming.value(m_rows.at(row).pmId));
        const uint mask = changedColumns(m_rows.at(row), next);
        if (!mask)
            continue;
        m_rows[row] = next;
        const int firstCol = qCountTrailingZeroBits(mask);
        const int lastCol = 31 - qCountLeadingZeroBits(mask);
        emit dataChanged(index(row, firstCol), index(row, lastCol));
    }

    // Additions, in the order pm2 lists them.
    QList<int> added;
    for (int i = 0; i < processes.size(); ++i) {
        if (!m_rowOfId.contains(processes.at(i).pmId))
            added.append(i);
    }
    if (!added.isEmpty()) {
        const int first = int(m_rows.size());
        beginInsertRows(QModelIndex(), first, first + int(added.size()) - 1);
        for (const int i : added) {
            m_rowOfId.insert(processes.at(i).pmId, int(m_rows.size()));
            m_rows.append(processes.at(i));
        }
        endInsertRows();
    }
}

void Pm2ProcessModel::setNow(qint64 nowMs)
{
    m_nowMs = nowMs;
    int first = -1;
    int last = -1;
    for (int row = 0; row < m_rows.size(); ++row) {
        if (!isRunning(m_rows.at(row)))
            continue;
        if (first < 0)
            first = row;
        last = row;
    }
    if (first >= 0) {
        const int col = int(Column::Uptime);
        emit dataChanged(index(first, col), index(last, col), {Qt::DisplayRole, SortRole});
    }
}

int Pm2ProcessModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

int Pm2ProcessModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(Column::Count);
}

QString Pm2ProcessModel::displayText(const Pm2ProcessInfo &p, Column column) const
{
    const bool running = isRunning(p);
    switch (column) {
    case Column::Name:
        return p.name;
    case Column::Status:
        return p.statusText;
    case Column::Uptime:
        return running ? Pm2Protocol::formatUptime(m_nowMs - p.startedAtMs) : QString();
    case Column::Restarts:
        return QString::number(p.restarts);
    case Column::Cpu:
        return running ? QString::number(p.cpuPercent, 'g', 3) + QLatin1Char('%') : QString();
    case Column::Memory:
        return running ? QLocale::system().formattedDataSize(p.memoryBytes, 1, QLocale::DataSizeTraditionalFormat)
                       : QString();
    case Column::Pid:
        return running && p.pid > 0 ? QString::number(p.pid) : QString();
    case Column::Cwd:
        return p.cwd;
    case Column::Count:
        break;
    }
    return {};
}

QVariant Pm2ProcessModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    const Pm2ProcessInfo &p = m_rows.at(index.row());
    const auto column = Column(index.column());

    switch (role) {
    case Qt::DisplayRole:
        return displayText(p, column);
    case SortRole: {
        const bool running = isRunning(p);
        switch (column) {
        case Column::Uptime: return running ? m_nowMs - p.startedAtMs : qint64(-1);
        case Column::Restarts: return p.restarts;
        case Column::Cpu: return running ? p.cpuPercent : -1.0;
        case Column::Memory: return running ? p.memoryBytes : qint64(-1);
        case Column::Pid: return running ? p.pid : qint64(-1);
        case Column::Status: return int(p.status);
        default: return displayText(p, column);
        }
    }
    case StatusRole:
        return int(p.status);
    case Qt::ToolTipRole:
        if (column == Column::Restarts && p.unstableRestarts > 0)
            return tr("%n unstable restart(s): it exited soon after starting", nullptr, p.unstableRestarts);
        if (column == Column::Cwd || column == Column::Name)
            return p.script.isEmpty() ? p.cwd : tr("Script: %1").arg(p.script);
        return {};
    case Qt::TextAlignmentRole:
        switch (column) {
        case Column::Restarts:
        case Column::Cpu:
        case Column::Memory:
        case Column::Pid:
            return QVariant(Qt::AlignRight | Qt::AlignVCenter);
        default:
            return QVariant(Qt::AlignLeft | Qt::AlignVCenter);
        }
    default:
        return {};
    }
}

QVariant Pm2ProcessModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return QAbstractTableModel::headerData(section, orientation, role);
    switch (Column(section)) {
    case Column::Name: return tr("Name");
    case Column::Status: return tr("Status");
    case Column::Uptime: return tr("Uptime");
    case Column::Restarts: return tr("Restarts");
    case Column::Cpu: return tr("CPU");
    case Column::Memory: return tr("Memory");
    case Column::Pid: return tr("PID");
    case Column::Cwd: return tr("Directory");
    case Column::Count: break;
    }
    return {};
}
