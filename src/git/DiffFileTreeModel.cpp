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

#include "DiffFileTreeModel.h"

#include "GitDiffPalette.h"
#include "GitStatusModel.h"

#include <QBrush>
#include <QHash>
#include <QSortFilterProxyModel>

#include <algorithm>
#include <functional>

// Flat, immutable tree. nodes[0] is the invisible root; each node's children
// are contiguous in `children`, so index() and rowCount() are O(1).
struct DiffFileTreeModel::Tree {
    struct Node {
        QString name;
        qint32  parent = -1;
        qint32  row = 0;            // position among its siblings
        qint32  firstChild = 0;     // into children
        qint32  childCount = 0;
        qint32  file = -1;          // -1 for a directory
    };
    QVector<Node>   nodes;
    QVector<qint32> children;
    QVector<qint32> nodeOfFile;
    QVector<GitDiffParser::FileSpan> files;
};

namespace {

// Trie node used while building.
struct Draft {
    QString name;
    qint32  file = -1;
    std::vector<qint32> kids;
    QHash<QString, qint32> dirs;
};

bool isDir(const Draft &d) { return d.file < 0; }

void emitChildren(const std::vector<Draft> &drafts, qint32 draft, qint32 outNode, DiffFileTreeModel::Tree &tree)
{
    struct Kid {
        qint32 draft;     // the draft whose children the emitted node takes
        QString name;     // compacted name
        qint32 file;
    };
    std::vector<Kid> kids;
    kids.reserve(drafts[draft].kids.size());
    for (qint32 k : drafts[draft].kids) {
        QString name = drafts[k].name;
        qint32 at = k;
        // a/b/c with a and b holding nothing else become one row "a/b/c"
        while (isDir(drafts[at]) && drafts[at].kids.size() == 1 && isDir(drafts[drafts[at].kids.front()])) {
            at = drafts[at].kids.front();
            name += QLatin1Char('/') + drafts[at].name;
        }
        kids.push_back(Kid{at, std::move(name), drafts[at].file});
    }
    std::sort(kids.begin(), kids.end(), [](const Kid &a, const Kid &b) {
        const bool ad = a.file < 0, bd = b.file < 0;
        if (ad != bd) return ad;
        const int c = a.name.compare(b.name, Qt::CaseInsensitive);
        return c != 0 ? c < 0 : a.name < b.name;
    });

    auto &out = tree.nodes[outNode];
    out.firstChild = static_cast<qint32>(tree.children.size());
    out.childCount = static_cast<qint32>(kids.size());
    const qint32 first = out.firstChild;
    tree.children.resize(first + out.childCount);
    for (qint32 r = 0; r < static_cast<qint32>(kids.size()); ++r) {
        DiffFileTreeModel::Tree::Node node;
        node.name = kids[r].name;
        node.parent = outNode;
        node.row = r;
        node.file = kids[r].file;
        const qint32 id = static_cast<qint32>(tree.nodes.size());
        tree.nodes.append(std::move(node));
        tree.children[first + r] = id;
        if (kids[r].file >= 0) tree.nodeOfFile[kids[r].file] = id;
    }
    for (qint32 r = 0; r < static_cast<qint32>(kids.size()); ++r) {
        if (kids[r].file < 0) emitChildren(drafts, kids[r].draft, tree.children[first + r], tree);
    }
}

} // namespace

DiffFileTreeModel::DiffFileTreeModel(QObject *parent) : QAbstractItemModel(parent) {}
DiffFileTreeModel::~DiffFileTreeModel() = default;

std::shared_ptr<const DiffFileTreeModel::Tree> DiffFileTreeModel::build(const QVector<GitDiffParser::FileSpan> &files)
{
    auto tree = std::make_shared<Tree>();
    tree->files = files;
    tree->nodeOfFile.fill(-1, files.size());

    std::vector<Draft> drafts(1);
    for (qint32 i = 0; i < files.size(); ++i) {
        const QStringList parts = QString::fromUtf8(files[i].path()).split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (parts.isEmpty()) continue;
        qint32 at = 0;
        for (qsizetype p = 0; p + 1 < parts.size(); ++p) {
            const auto it = drafts[at].dirs.constFind(parts[p]);
            if (it != drafts[at].dirs.constEnd()) {
                at = *it;
                continue;
            }
            const qint32 id = static_cast<qint32>(drafts.size());
            drafts.push_back(Draft{parts[p], -1, {}, {}});
            drafts[at].dirs.insert(parts[p], id);
            drafts[at].kids.push_back(id);
            at = id;
        }
        const qint32 id = static_cast<qint32>(drafts.size());
        drafts.push_back(Draft{parts.last(), i, {}, {}});
        drafts[at].kids.push_back(id);
    }

    tree->nodes.reserve(static_cast<qsizetype>(drafts.size()));
    tree->nodes.append(Tree::Node{});
    emitChildren(drafts, 0, 0, *tree);
    return tree;
}

QSortFilterProxyModel *DiffFileTreeModel::createFilterProxy(QObject *parent)
{
    auto *proxy = new QSortFilterProxyModel(parent);
    // Directories have an empty PathRole: they show only above a matching file.
    proxy->setFilterRole(PathRole);
    proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxy->setRecursiveFilteringEnabled(true);
    return proxy;
}

void DiffFileTreeModel::setTree(std::shared_ptr<const Tree> tree)
{
    beginResetModel();
    m_tree = std::move(tree);
    endResetModel();
}

void DiffFileTreeModel::setDarkPalette(bool dark)
{
    if (dark == m_dark) return;
    m_dark = dark;
    if (!m_tree || m_tree->nodes.size() <= 1) return;
    // Status colours only; the structure is unchanged.
    std::function<void(const QModelIndex &)> touch = [&](const QModelIndex &parent) {
        const int rows = rowCount(parent);
        if (rows == 0) return;
        emit dataChanged(index(0, 0, parent), index(rows - 1, 0, parent), {Qt::ForegroundRole});
        for (int r = 0; r < rows; ++r) {
            const QModelIndex i = index(r, 0, parent);
            if (rowCount(i) > 0) touch(i);
        }
    };
    touch({});
}

QModelIndex DiffFileTreeModel::indexOfFile(int file) const
{
    if (!m_tree || file < 0 || file >= m_tree->nodeOfFile.size()) return {};
    const qint32 node = m_tree->nodeOfFile[file];
    if (node <= 0) return {};
    return createIndex(m_tree->nodes[node].row, 0, quintptr(node));
}

QModelIndex DiffFileTreeModel::index(int row, int column, const QModelIndex &parent) const
{
    if (!m_tree || column != 0 || row < 0) return {};
    const qint32 p = parent.isValid() ? static_cast<qint32>(parent.internalId()) : 0;
    const Tree::Node &node = m_tree->nodes[p];
    if (row >= node.childCount) return {};
    return createIndex(row, 0, quintptr(m_tree->children[node.firstChild + row]));
}

QModelIndex DiffFileTreeModel::parent(const QModelIndex &child) const
{
    if (!m_tree || !child.isValid()) return {};
    const qint32 p = m_tree->nodes[static_cast<qint32>(child.internalId())].parent;
    if (p <= 0) return {};
    return createIndex(m_tree->nodes[p].row, 0, quintptr(p));
}

int DiffFileTreeModel::rowCount(const QModelIndex &parent) const
{
    if (!m_tree || parent.column() > 0) return 0;
    return m_tree->nodes[parent.isValid() ? static_cast<qint32>(parent.internalId()) : 0].childCount;
}

int DiffFileTreeModel::columnCount(const QModelIndex &) const { return 1; }

QVariant DiffFileTreeModel::data(const QModelIndex &index, int role) const
{
    if (!m_tree || !index.isValid()) return {};
    const Tree::Node &node = m_tree->nodes[static_cast<qint32>(index.internalId())];
    if (node.file < 0) {
        switch (role) {
            case Qt::DisplayRole:
            case NameRole: return node.name;
            case FileIndexRole: return -1;
            case PathRole: return QString();
            case GitStatusModel::AddedLinesRole:
            case GitStatusModel::DeletedLinesRole: return -1;
            case GitStatusModel::IsBinaryRole:
            case GitStatusModel::IsSectionRole:
            case GitStatusModel::IsSubmoduleRole: return false;
            default: return {};
        }
    }

    const GitDiffParser::FileSpan &f = m_tree->files[node.file];
    switch (role) {
        case Qt::DisplayRole: return QString(QLatin1Char(f.status)) + QStringLiteral("  ") + node.name;
        case NameRole: return node.name;
        case Qt::ToolTipRole: {
            const QString path = QString::fromUtf8(f.path());
            if ((f.status == 'R' || f.status == 'C') && !f.oldPath.isEmpty() && f.oldPath != f.newPath)
                return QString::fromUtf8(f.oldPath) + QStringLiteral(" → ") + path;
            return path;
        }
        case Qt::ForegroundRole: {
            const GitDiffPalette &pal = GitDiffPalette::current(m_dark);
            switch (f.status) {
                case 'A': return QBrush(pal.fgAdded);
                case 'D': return QBrush(pal.fgDeleted);
                case 'R':
                case 'C': return QBrush(pal.fgRenamed);
                default: return QBrush(pal.fgModified);
            }
        }
        case FileIndexRole: return node.file;
        case PathRole: return QString::fromUtf8(f.path());
        case GitStatusModel::AddedLinesRole: return f.binary ? -1 : f.added;
        case GitStatusModel::DeletedLinesRole: return f.binary ? -1 : f.deleted;
        case GitStatusModel::IsBinaryRole: return f.binary;
        case GitStatusModel::IsSectionRole: return false;
        case GitStatusModel::IsSubmoduleRole: return f.submodule;
        case GitStatusModel::SubAddedLinesRole:
        case GitStatusModel::SubDeletedLinesRole: return -1;
        default: return {};
    }
}

Qt::ItemFlags DiffFileTreeModel::flags(const QModelIndex &index) const
{
    return index.isValid() ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags;
}
