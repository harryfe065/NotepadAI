/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef PM2_PROCESS_MODEL_H
#define PM2_PROCESS_MODEL_H

#include <QAbstractTableModel>
#include <QHash>
#include <QVector>

#include "Pm2Protocol.h"

// pm2's process list as a table. Every list the helper sends is applied as
// an edit (rows keyed by pm_id: removed, edited in place, appended), never a
// reset, so selection, scroll and sorting survive the 1.5 s refresh.
class Pm2ProcessModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum class Column : int { Name, Status, Uptime, Restarts, Cpu, Memory, Pid, Cwd, Count };
    enum Role : int {
        SortRole = Qt::UserRole + 1, // numeric value for numeric columns
        StatusRole,                  // Pm2Status as int
    };

    explicit Pm2ProcessModel(QObject *parent = nullptr);

    void setProcesses(const QList<Pm2ProcessInfo> &processes);
    // Wall clock (ms since epoch) the Uptime column is measured against.
    void setNow(qint64 nowMs);

    const Pm2ProcessInfo &processAt(int row) const { return m_rows.at(row); }
    int rowForId(int pmId) const { return m_rowOfId.value(pmId, -1); }
    const QVector<Pm2ProcessInfo> &processes() const { return m_rows; }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

private:
    QString displayText(const Pm2ProcessInfo &p, Column column) const;
    void reindex();

    QVector<Pm2ProcessInfo> m_rows;
    QHash<int, int> m_rowOfId;
    qint64 m_nowMs = 0;
};

#endif // PM2_PROCESS_MODEL_H
