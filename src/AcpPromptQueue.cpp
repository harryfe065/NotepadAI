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

#include "AcpPromptQueue.h"

#include <algorithm>
#include <utility>

AcpPromptQueue::SendKind AcpPromptQueue::classifySend(bool processing, bool hasContent, bool nativeGoalSlash)
{
    if (!hasContent)
        return SendKind::Ignore;
    if (processing)
        return nativeGoalSlash ? SendKind::SidePrompt : SendKind::Enqueue;
    return SendKind::SendNow;
}

void AcpPromptQueue::enqueue(Item item)
{
    if (item.host)
        m_items.insert(hostCount(), std::move(item));
    else
        m_items.push_back(std::move(item));
}

int AcpPromptQueue::hostCount() const
{
    const auto it = std::find_if(m_items.cbegin(), m_items.cend(), [](const Item &item) { return !item.host; });
    return static_cast<int>(it - m_items.cbegin());
}

int AcpPromptQueue::visibleCount() const
{
    return m_items.size() - hostCount();
}

int AcpPromptQueue::indexOfVisible(int visibleIndex) const
{
    if (visibleIndex < 0)
        return -1;
    const int index = hostCount() + visibleIndex;
    return index < m_items.size() ? index : -1;
}

std::optional<AcpPromptQueue::Item> AcpPromptQueue::dequeue()
{
    if (m_items.isEmpty())
        return std::nullopt;
    return m_items.takeFirst();
}

bool AcpPromptQueue::removeAt(int index)
{
    if (index < 0 || index >= m_items.size())
        return false;
    m_items.removeAt(index);
    return true;
}

std::optional<AcpPromptQueue::Item> AcpPromptQueue::takeNextIfIdle(bool processing)
{
    if (processing)
        return std::nullopt;
    return dequeue();
}

bool AcpPromptQueue::containsMarker(const QString &marker) const
{
    return std::any_of(m_items.cbegin(), m_items.cend(),
                       [&marker](const Item &item) { return item.marker == marker; });
}

int AcpPromptQueue::size() const
{
    return m_items.size();
}
