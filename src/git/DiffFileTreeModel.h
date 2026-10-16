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

#ifndef DIFF_FILE_TREE_MODEL_H
#define DIFF_FILE_TREE_MODEL_H

#include <cstdint>
#include "GitDiffParser.h"

#include <QAbstractItemModel>

#include <memory>

class QSortFilterProxyModel;

// The changed-files tree of the commit view. The tree is built on the pool
// (build) from the parser's file table and shown read-only.
class DiffFileTreeModel : public QAbstractItemModel
{
    Q_OBJECT
public:
    struct Tree;

    enum Roles : std::uint16_t {
        NameRole = Qt::UserRole + 200,   // the row's own name ("src/git", "A.cpp")
        FileIndexRole,                   // index into the file table, -1 for a directory
        PathRole,                        // full path of a file, empty for a directory
    };

    explicit DiffFileTreeModel(QObject *parent = nullptr);
    ~DiffFileTreeModel() override;

    // Directories before files, case-insensitive order, single-child
    // directory chains compacted. Pure; safe on any thread.
    static std::shared_ptr<const Tree> build(const QVector<GitDiffParser::FileSpan> &files);

    // A proxy filtering files by a case-insensitive substring of their path,
    // keeping the directories above every match.
    static QSortFilterProxyModel *createFilterProxy(QObject *parent);

    void setTree(std::shared_ptr<const Tree> tree);
    void setDarkPalette(bool dark);

    QModelIndex indexOfFile(int file) const;

    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

private:
    std::shared_ptr<const Tree> m_tree;
    bool m_dark = false;
};

#endif // DIFF_FILE_TREE_MODEL_H
