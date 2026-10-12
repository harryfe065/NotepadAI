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

#include "AcpConversationCopy.h"

#include "GoalAgent.h"

namespace AcpConversationCopy {

namespace {

const AcpMessage *messageAt(const AcpTimelineEntry &entry, const QVector<AcpMessage> &messages)
{
    if (entry.kind != AcpTimelineEntry::Kind::Message
        || entry.messageIndex < 0 || entry.messageIndex >= messages.size()) {
        return nullptr;
    }
    return &messages.at(entry.messageIndex);
}

// Allocation-free: scans the blocks in place instead of joining and trimming.
bool hasVisibleText(const AcpMessage &msg)
{
    for (const auto &block : msg.content) {
        if (block.kind == AcpProtocol::AcpContentBlock::Kind::Image)
            return true; // rendered as "[image]"
        for (const QChar c : block.text) {
            if (!c.isSpace())
                return true;
        }
    }
    return false;
}

bool isReportCandidate(const AcpMessage &msg)
{
    return msg.role == QLatin1String("assistant") && !msg.fromGoalJudge && hasVisibleText(msg);
}

void appendSection(QString &out, QLatin1StringView heading, const QString &text)
{
    const QString body = text.trimmed();
    if (body.isEmpty())
        return;
    if (!out.isEmpty())
        out += QLatin1String("\n\n");
    out += QLatin1String("## ");
    out += heading;
    out += QLatin1String("\n\n");
    out += body;
}

} // namespace

QString messageText(const AcpMessage &msg)
{
    QString out;
    for (const auto &block : msg.content) {
        if (block.kind == AcpProtocol::AcpContentBlock::Kind::Text)
            out += block.text;
        else
            out += QLatin1String("[image]");
    }
    return out;
}

QVector<int> finalReports(const QVector<AcpTimelineEntry> &timeline,
                          const QVector<AcpMessage> &messages,
                          bool includeNewest)
{
    QVector<int> out;
    int survivor = -1;
    for (const AcpTimelineEntry &entry : timeline) {
        const AcpMessage *m = messageAt(entry, messages);
        if (!m)
            continue;
        if (m->role == QLatin1String("user")) {
            if (survivor >= 0)
                out.append(survivor);
            survivor = -1;
        } else if (isReportCandidate(*m)) {
            survivor = entry.messageIndex;
        }
    }
    if (includeNewest && survivor >= 0)
        out.append(survivor);
    return out;
}

QString buildMarkdown(const QVector<AcpTimelineEntry> &timeline,
                      const QVector<AcpMessage> &messages,
                      int upToMessage)
{
    QString out;
    int pending = -1; // the current turn's report so far
    auto flush = [&]() {
        if (pending < 0)
            return;
        appendSection(out, QLatin1StringView("Assistant"),
                      GoalAgent::nativeGoalDisplayText(messageText(messages.at(pending))));
        pending = -1;
    };
    bool found = false;
    for (const AcpTimelineEntry &entry : timeline) {
        const AcpMessage *m = messageAt(entry, messages);
        if (!m)
            continue;
        if (m->role == QLatin1String("user")) {
            flush();
            appendSection(out, QLatin1StringView("User"), messageText(*m));
        } else if (isReportCandidate(*m)) {
            pending = entry.messageIndex;
        }
        if (entry.messageIndex == upToMessage) {
            found = true;
            break;
        }
    }
    if (!found)
        return {};
    flush();
    return out;
}

} // namespace AcpConversationCopy
