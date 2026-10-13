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

#ifndef ACP_TOOL_CALL_CARD_H
#define ACP_TOOL_CALL_CARD_H

#include <QFont>
#include <QFrame>
#include <QHash>
#include <QJsonObject>
#include <QString>

#include "AcpProtocol.h"

class MarkdownView;
class QHBoxLayout;
class QLabel;
class QTextBrowser;
class QTimer;
class QToolButton;
class QVBoxLayout;
class QShowEvent;

class AcpToolCallCard : public QFrame
{
    Q_OBJECT
    Q_PROPERTY(bool goalAgent READ isFromGoalAgent)

public:
    explicit AcpToolCallCard(const AcpProtocol::AcpToolCall &initial, QWidget *parent = nullptr);

    void apply(const AcpProtocol::AcpToolCallUpdate &update);

    QString id() const { return m_id; }
    QString status() const { return m_status; }
    int groupId() const { return m_groupId; }

    void setFromGoalAgent(bool goal);
    void setGoalTint(bool on);
    bool isFromGoalAgent() const { return m_fromGoalAgent; }

    void setCollapsed(bool collapsed);
    bool isCollapsed() const { return m_collapsed; }
    bool shouldPreserveExpanded() const { return m_userToggled || m_autoExpandedForDiff; }

    // Apply the chat (Default Font) typeface explicitly. Required because this
    // card and its inner QTextBrowser both carry a stylesheet, and styled
    // widgets do NOT inherit a parent's setFont() — Qt re-resolves their font
    // from the application default. So the transcript host's font never reaches
    // the card; we push it down per-widget (title/status QLabels + the body
    // QTextBrowser document font) here, and thread its family/size into the
    // hardcoded HTML the body renders.
    void setChatFont(const QFont &font);

    // Header title for any tool call ("Command: ls", "Agent Explore: …").
    static QString enrichedTitle(const AcpProtocol::AcpToolCall &tc);

    // Agent/Task cards: the calls their subagent made nest inside, never in
    // the transcript. Collapsed, the card shows how many there are and, while
    // the subagent runs, a muted "now" line (nowLine: its current call's
    // title). The nested cards are built only once the card is opened.
    void setSubagentActivity(int toolCount, const QString &nowLine);
    // Takes ownership; appended in arrival order under the instructions.
    void addChildCard(AcpToolCallCard *child);
    AcpToolCallCard *childCard(const QString &id) const { return m_childCards.value(id, nullptr); }
    int childCardCount() const { return static_cast<int>(m_childCards.size()); }

signals:
    // Opened with fewer nested cards built than the subagent made calls.
    void childCardsRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    // QStackedWidget (session tabs) hides inactive pages without a size change,
    // so resizeEvent does not re-run on tab switch. Re-fit on show so an
    // expanded diff/output body that streamed in the background keeps its
    // height instead of staying pinned to the header.
    void showEvent(QShowEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void ensureBody();
    void refreshHeader();
    void scheduleBodyRender();
    void flushBodyRender();
    void rerenderBody();
    void refitBodyHeight();
    void scheduleRefit();
    bool hasDiffContent() const;
    bool isTerminalStatus() const;
    void maybeAutoExpandForDiff();
    QString statusGlyph() const;
    QString computeEnrichedTitle() const;
    void refreshDuration();
    int headerRowIndex() const;
    void refreshNowLine();
    void refreshSubagentRows();
    int rowAfterHeader() const;
    bool isSubagentCard() const;
    void setNested(bool nested);
    void applyNestedMetrics();
    void refreshSubagentMark();
    void applyContentIndent();
    void refreshPrompt();
    int fitPromptBox(int width);

    QString m_id;
    QString m_title;
    QString m_name; // wire tool name (Bash / Grep / Read ...); empty for agents that omit it
    QString m_kind;
    QString m_status;
    bool m_backgroundRunning = false; // an async task this call started is still alive
    int m_groupId = 0;
    qint64 m_durationMs = -1;
    QJsonArray m_content;
    QJsonObject m_rawInput;
    QJsonObject m_rawOutput;

    // Chat (Default Font) typeface pushed in via setChatFont(). Body HTML is
    // re-rendered with this font's family/size; default-constructed (and unused
    // for HTML) until the first setChatFont() call.
    QFont m_chatFont;
    bool m_chatFontSet = false;

    bool m_collapsed = false;
    bool m_fromGoalAgent = false;
    bool m_userToggled = false;          // user has explicitly clicked the chevron
    bool m_autoExpandedForDiff = false;  // we've already auto-expanded once for a completed diff
    bool m_bodyDirty = true;             // body document is stale vs m_content/rawInput/rawOutput
    bool m_refitScheduled = false;       // a deferred refit is already queued
    QTimer *m_renderTimer = nullptr;     // coalesces expensive QTextDocument body renders; built with m_body

    QLabel *m_statusIcon = nullptr;
    QLabel *m_agentIcon = nullptr;       // bot icon: shown on cards that run subagents
    int m_contentIndent = 0;             // subagent card: rows below the header start at the title
    QLabel *m_titleLabel = nullptr;
    QLabel *m_durationLabel = nullptr;
    QToolButton *m_expandBtn = nullptr;
    QTextBrowser *m_body = nullptr;      // null until first expand (ensureBody)
    QVBoxLayout *m_outer = nullptr;
    QHBoxLayout *m_headerLayout = nullptr;

    // Subagent (Agent/Task) state; all null/empty on ordinary cards.
    int m_subagentToolCount = 0;
    QString m_subagentNowLine;
    QLabel *m_nowLabel = nullptr;         // built with the first activity
    QFrame *m_childHost = nullptr;        // built with the first nested card
    QHash<QString, AcpToolCallCard *> m_childCards; // owned via m_childHost
    bool m_nested = false;                // a row inside an Agent card: compact metrics
    // The instructions the subagent was given, above its calls; built on the
    // first expand of a card that has them.
    QWidget *m_promptBox = nullptr;
    MarkdownView *m_promptView = nullptr;
    QToolButton *m_promptMoreBtn = nullptr;
    bool m_promptExpanded = false;        // long instructions show in full
    QLabel *m_resultCaption = nullptr;    // "Result" above the body; subagent cards only
    MarkdownView *m_markdownBody = nullptr; // m_body when it renders the report as markdown
    bool m_hasResultText = false;
};

#endif // ACP_TOOL_CALL_CARD_H
