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

#include "AcpSessionView.h"

#include "AcpAgentRegistry.h"
#include "AcpConnection.h"
#include "AcpConversationCopy.h"
#include "AcpImageAttachmentList.h"
#include "AcpMessageWidget.h"
#include "AcpPermissionPrompt.h"
#include "AcpPlanWidget.h"
#include "AcpPromptQueueStrip.h"
#include "AcpSessionModel.h"
#include "AcpToolCallCard.h"
#include "AcpTurnFoldRow.h"
#include "ChatFont.h"
#include "AcpTranscriptTruncation.h"
#include "AcpUsageIndicator.h"
#include "AiAgentDock.h"
#include "ApplicationSettings.h"
#include "GoalAgent.h"
#include "NotepadNextApplication.h"
#include "ai/CredentialStore.h"
#include "ai/PromptImprover.h"

#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QEvent>
#include <QDir>
#include <QFileInfo>
#include <QFileDialog>
#include <QFocusEvent>
#include <QFont>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMimeData>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QShortcut>
#include <QStyle>
#include <QTimer>
#include <QMenu>
#include <QCursor>
#include <QAction>
#include <QFontMetrics>
#include <QToolButton>
#include <QVBoxLayout>

#include <utility>

namespace {

// An ask_user call rejected by the server (the agent sees why and retries) or
// cancelled: no form.
bool askUserCallDropped(const QString &status)
{
    return AcpProtocol::toolCallStatusIsTerminal(status) && status != QLatin1String("completed");
}

// Treat any scrollbar value within this many pixels of the maximum as "at
// bottom". Used both for the user's at-bottom recognition (the flag flips on
// when they reach this band) and for the programmatic scroll target.
constexpr int kAtBottomEpsilonPx = 8;
constexpr int kMaxSkillCompletions = 256;

bool isEffortConfigOption(const AcpProtocol::AcpConfigOption &opt)
{
    const QString idLower = opt.id.toLower();
    const QString catLower = opt.category.toLower();
    const QString nameLower = opt.name.toLower();
    return idLower.contains(QLatin1String("effort"))
        || idLower.contains(QLatin1String("reasoning"))
        || catLower.contains(QLatin1String("thought"))
        || catLower.contains(QLatin1String("reasoning"))
        || nameLower.contains(QLatin1String("effort"))
        || nameLower.contains(QLatin1String("reasoning"));
}

bool isModelConfigOption(const AcpProtocol::AcpConfigOption &opt)
{
    const QString idLower = opt.id.toLower();
    const QString catLower = opt.category.toLower();
    return idLower == QLatin1String("model")
        || catLower == QLatin1String("model");
}

QString baseModelId(const QString &modelId)
{
    const int slash = modelId.indexOf(QLatin1Char('/'));
    return slash >= 0 ? modelId.left(slash) : modelId;
}

QString stripEffortSuffix(const QString &label)
{
    static const QRegularExpression re(
        QStringLiteral(R"(\s+\((low|medium|high|xhigh)\)$)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = re.match(label);
    return match.hasMatch() ? label.left(match.capturedStart()).trimmed() : label;
}

const AcpProtocol::AcpConfigOption *findModelConfigOption(
    const QList<AcpProtocol::AcpConfigOption> &opts)
{
    for (const auto &opt : opts) {
        if (isModelConfigOption(opt)) return &opt;
    }
    return nullptr;
}

const AcpProtocol::AcpConfigOption *findEffortConfigOption(
    const QList<AcpProtocol::AcpConfigOption> &opts)
{
    for (const auto &opt : opts) {
        if (isEffortConfigOption(opt)) return &opt;
    }
    return nullptr;
}

// Some agents (pi-acp) dual-advertise thinking as ACP `modes` *and* a
// thought_level/effort config option with the same id set. Host would otherwise
// render two identical "Thinking: high" combos. Hide modes when the catalogs
// are the same; keep both when they differ (Claude Code permission modes +
// separate effort, etc.).
bool modesDuplicateEffortCatalog(
    const QList<AcpProtocol::AcpModeInfo> &modes,
    const AcpProtocol::AcpConfigOption *effortOpt)
{
    if (!effortOpt || modes.isEmpty() || effortOpt->options.isEmpty())
        return false;
    QSet<QString> modeIds;
    for (const auto &m : modes) {
        if (!m.id.isEmpty())
            modeIds.insert(m.id);
    }
    QSet<QString> effortIds;
    for (const auto &ch : effortOpt->options) {
        if (!ch.value.isEmpty())
            effortIds.insert(ch.value);
    }
    return !modeIds.isEmpty() && modeIds == effortIds;
}

// SVG icons that use stroke="currentColor" resolve to opaque black under Qt's
// svg icon engine, so they vanish on dark backgrounds. Re-render the icon at
// the sizes Qt is likely to ask for and tint each pixmap via SourceIn so the
// alpha (the strokes) is preserved while the rgb becomes the palette color.
QIcon tintedIcon(const QString &svgPath, const QColor &color)
{
    QIcon source(svgPath);
    if (source.isNull()) return source;

    QIcon dst;
    const QList<int> sizes{16, 20, 22, 24, 32, 48, 64};
    for (int sz : sizes) {
        QPixmap pm = source.pixmap(sz, sz);
        if (pm.isNull()) continue;
        QPainter p(&pm);
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(pm.rect(), color);
        p.end();
        dst.addPixmap(pm);
    }
    return dst;
}

ApplicationSettings *appSettings()
{
    return qApp ? qApp->findChild<ApplicationSettings *>() : nullptr;
}

// Custom QPlainTextEdit that submits on Enter (Shift+Enter for newline) and
// forwards image pastes/drops to the AcpImageAttachmentList.
class ChatInputEditCb : public QPlainTextEdit
{
public:
    ChatInputEditCb(AcpImageAttachmentList *attachments, QWidget *parent = nullptr)
        : QPlainTextEdit(parent)
        , m_attachments(attachments)
    {
        setPlaceholderText(QObject::tr("Send a message"));
        setTabChangesFocus(true);
    }

    std::function<void()> onSubmit;
    std::function<void()> onRestart;
    std::function<bool(QKeyEvent *)> onKeyFilter;

protected:
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::ShortcutOverride) {
            auto *ke = static_cast<QKeyEvent *>(e);
            if (ke->key() == Qt::Key_N && ke->modifiers() == Qt::ControlModifier && onRestart) {
                e->accept();
                return true;
            }
        }
        if (e->type() == QEvent::KeyPress) {
            auto *ke = static_cast<QKeyEvent *>(e);
            if (ke->key() == Qt::Key_Tab && onKeyFilter && onKeyFilter(ke))
                return true;
        }
        return QPlainTextEdit::event(e);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_N && event->modifiers() == Qt::ControlModifier) {
            if (onRestart) { onRestart(); return; }
        }
        if (onKeyFilter && onKeyFilter(event))
            return;
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
            && !(event->modifiers() & Qt::ShiftModifier)) {
            if (onSubmit) onSubmit();
            return;
        }
        QPlainTextEdit::keyPressEvent(event);
    }

    void insertFromMimeData(const QMimeData *source) override
    {
        if (m_attachments && (source->hasImage() || source->hasUrls())) {
            if (source->hasUrls()) {
                const QList<QUrl> urls = source->urls();
                for (const QUrl &u : urls) {
                    if (u.isLocalFile()) {
                        m_attachments->addFileByPath(u.toLocalFile());
                    }
                }
            }
            if (source->hasImage()) {
                // Snipping Tool / Print Screen put a raw bitmap on the clipboard,
                // not PNG bytes — fall back to the QImage payload and re-encode.
                QByteArray raw = source->data(QStringLiteral("image/png"));
                if (raw.isEmpty()) {
                    const QImage img = qvariant_cast<QImage>(source->imageData());
                    if (!img.isNull()) {
                        QBuffer buf(&raw);
                        buf.open(QIODevice::WriteOnly);
                        img.save(&buf, "PNG");
                    }
                }
                if (!raw.isEmpty()) {
                    m_attachments->tryAddImage(raw, QObject::tr("pasted.png"));
                }
            }
            return;
        }
        QPlainTextEdit::insertFromMimeData(source);
    }

private:
    AcpImageAttachmentList *m_attachments;
};

using AcpTurnFolding::messageHasVisibleContent;

} // namespace

AcpSessionView::AcpSessionView(AcpSessionModel *model,
                               AcpConnection *connection,
                               AcpAgentRegistry *registry,
                               QWidget *parent)
    : QWidget(parent)
    , m_model(model)
    , m_connection(connection)
    , m_registry(registry)
{
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    buildUi();
    wireSignals();
    hydrateFromModel();
    updateImproveButtonState();
}

AcpSessionView::~AcpSessionView() = default;

QSize AcpSessionView::sizeHint() const
{
    return QSize(420, 300);
}

QSize AcpSessionView::minimumSizeHint() const
{
    return QSize(200, 100);
}

void AcpSessionView::buildUi()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(8, 8, 8, 8);
    outer->setSpacing(6);

    // 1. Status banner.
    m_banner = new QFrame(this);
    m_banner->setObjectName(QStringLiteral("AcpStatusBanner"));
    // Soft warm backgrounds (Bootstrap "alert" pair) hold across themes; in
    // dark mode the widget text role flips to white, so we must pin a dark
    // text colour on the banner contents in the warning/error variants —
    // otherwise we get white-on-pink / white-on-yellow. Bootstrap's matching
    // text tokens are #856404 (warning) and #721c24 (error).
    m_banner->setStyleSheet(QStringLiteral(
        "QFrame#AcpStatusBanner { background: transparent; border: none; border-radius: 4px; padding: 0px; }"
        "QFrame#AcpStatusBanner[bannerKind=\"warning\"] { background: #fff3cd; border: 1px solid #ffeeba; padding: 4px; }"
        "QFrame#AcpStatusBanner[bannerKind=\"warning\"] QLabel,"
        "QFrame#AcpStatusBanner[bannerKind=\"warning\"] QToolButton { color: #856404; }"
        "QFrame#AcpStatusBanner[bannerKind=\"warning\"] QToolButton:hover {"
        " color: #533f03; border: 1px solid #856404; }"
        "QFrame#AcpStatusBanner[bannerKind=\"error\"] { background: #f8d7da; border: 1px solid #f5c6cb; padding: 4px; }"
        "QFrame#AcpStatusBanner[bannerKind=\"error\"] QLabel,"
        "QFrame#AcpStatusBanner[bannerKind=\"error\"] QToolButton { color: #721c24; }"
        "QFrame#AcpStatusBanner[bannerKind=\"error\"] QToolButton:hover {"
        " color: #491217; border: 1px solid #721c24; }"));
    auto *banL = new QHBoxLayout(m_banner);
    banL->setContentsMargins(0, 0, 0, 0);
    banL->setSpacing(4);
    m_bannerLabel = new QLabel(m_banner);
    m_bannerLabel->setWordWrap(true);
    m_bannerClose = new QToolButton(m_banner);
    m_bannerClose->setText(QStringLiteral("×"));
    m_bannerClose->setAutoRaise(true);
    m_bannerClose->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_bannerClose->setToolTip(tr("Close"));
    m_bannerClose->setAccessibleName(tr("Close"));
    connect(m_bannerClose, &QToolButton::clicked, this, &AcpSessionView::clearBanner);
    banL->addWidget(m_bannerLabel, 1);
    banL->addWidget(m_bannerClose, 0, Qt::AlignTop);
    m_banner->setProperty("bannerKind", QStringLiteral("info"));
    m_banner->hide();
    outer->addWidget(m_banner);

    // 2. Transcript area.
    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setFrameShape(QFrame::NoFrame);

    m_transcriptHost = new QWidget(m_scroll);
    m_transcriptHost->installEventFilter(this);
    m_transcriptLayout = new QVBoxLayout(m_transcriptHost);
    m_transcriptLayout->setContentsMargins(4, 4, 4, 4);
    m_transcriptLayout->setSpacing(6);

    // Inline heartbeat indicator. Lives at the tail of the transcript so it
    // visually trails the freshest bubble/card. Hidden outside processing;
    // shown while the agent is busy. Italic + muted = "status whisper".
    m_elapsedLabel = new QLabel(m_transcriptHost);
    m_elapsedLabel->setStyleSheet(QStringLiteral(
        "QLabel { color: palette(placeholder-text); font-style: italic; }"));
    m_elapsedLabel->setToolTip(tr("Time since the last new event from the agent"));
    // The row carries a "Stop" link while stoppable background tasks exist.
    m_elapsedLabel->setTextFormat(Qt::RichText);
    m_elapsedLabel->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    connect(m_elapsedLabel, &QLabel::linkActivated,
            this, &AcpSessionView::onBackgroundStopRequested);
    m_elapsedLabel->hide();
    m_transcriptLayout->addWidget(m_elapsedLabel);

    m_goalElapsedLabel = new QLabel(m_transcriptHost);
    m_goalElapsedLabel->setStyleSheet(QStringLiteral(
        "QLabel { color: rgb(180, 140, 50); font-style: italic; font-weight: 600; font-size: 11px; }"));
    m_goalElapsedLabel->hide();
    m_transcriptLayout->addWidget(m_goalElapsedLabel);

    m_transcriptLayout->addStretch();

    m_scroll->setWidget(m_transcriptHost);
    syncTranscriptHostWidth();
    outer->addWidget(m_scroll, 1);

    m_planWidget = new AcpPlanWidget(this);
    m_planWidget->hide();
    connect(m_planWidget, &AcpPlanWidget::resumeRequested,
            this, &AcpSessionView::onPlanResumeRequested);
    outer->addWidget(m_planWidget);

    // Jump-to-bottom overlay. Parented to the scroll area's viewport (not the
    // transcript host) so the button stays pinned to the bottom-right of the
    // visible region instead of scrolling away with the content.
    m_jumpToBottom = new QToolButton(m_scroll->viewport());
    m_jumpToBottom->setText(QStringLiteral("↓"));
    m_jumpToBottom->setToolTip(tr("Jump to bottom"));
    m_jumpToBottom->setAutoRaise(false);
    m_jumpToBottom->setStyleSheet(QStringLiteral(
        "QToolButton { background: palette(button); border: 1px solid palette(mid); border-radius: 12px; min-width: 24px; min-height: 24px; }"));
    m_jumpToBottom->hide();
    m_scroll->viewport()->installEventFilter(this);
    connect(m_jumpToBottom, &QToolButton::clicked, this, &AcpSessionView::onJumpToBottomClicked);

    if (auto *vbar = m_scroll->verticalScrollBar()) {
        // User-driven scroll updates the stick-to-bottom flag. Our own
        // setValue() calls are bracketed by m_programmaticScroll so they
        // don't flip the flag based on the value we just wrote.
        connect(vbar, &QScrollBar::valueChanged, this, [this](int v) {
            if (m_programmaticScroll) return;
            auto *vb = m_scroll ? m_scroll->verticalScrollBar() : nullptr;
            if (!vb) return;
            m_stickToBottom = (v >= vb->maximum() - kAtBottomEpsilonPx);
            updateJumpButtonVisibility();
        });
        // Content growth (new bubble, streamed chunk reflow) changes the
        // range. When stuck, follow the new bottom; otherwise leave the
        // viewport where the user parked it.
        connect(vbar, &QScrollBar::rangeChanged, this, [this](int, int) {
            if (m_stickToBottom) {
                scrollToBottomDeferred();
            } else {
                updateJumpButtonVisibility();
            }
        });
    }

    // 3. Selectors (model / mode / effort). Constructed here so other
    // buildUi() steps and hydrateFromModel() can populate them; placed into
    // the button row below so the chat surface keeps a single chrome row.
    // AdjustToContents lets each combo hug its current selection — no fixed
    // pixel widths, no row of equally-stretched dropdowns competing with the
    // Send button for space.
    const QString kCompactComboCss = QStringLiteral(
        "QComboBox { padding: 1px 4px; border: 1px solid palette(mid); border-radius: 3px; }"
        "QComboBox::drop-down { width: 14px; }");
    m_modelCombo = new QComboBox(this);
    m_modelCombo->setToolTip(tr("Model"));
    m_modelCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_modelCombo->setStyleSheet(kCompactComboCss);
    m_modelCombo->hide();
    m_modeCombo = new QComboBox(this);
    m_modeCombo->setToolTip(tr("Mode"));
    m_modeCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_modeCombo->setStyleSheet(kCompactComboCss);
    m_modeCombo->hide();
    m_effortCombo = new QComboBox(this);
    m_effortCombo->setToolTip(tr("Effort"));
    m_effortCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_effortCombo->setStyleSheet(kCompactComboCss);
    m_effortCombo->hide();

    // 4. Auto-approve checkbox. The toggle is dangerous (the agent can run
    // any tool unattended), so the checked state must be unmistakable — but
    // a full-bleed warning-yellow block is chrome-shouting. Per the DNA, the
    // soft warning palette is reserved for transient banners; here we settle
    // for warning-tone bold text + the checkbox indicator itself.
    m_autoApproveCheck = new QCheckBox(tr("Auto-approve permissions"), this);
    m_autoApproveCheck->setToolTip(tr("Automatically allow all tool calls this agent requests"));
    m_autoApproveCheck->setStyleSheet(QStringLiteral(
        "QCheckBox:checked { color: #856404; font-weight: 600; }"));
    m_newWorktreeCheck = new QCheckBox(tr("New worktree"), this);
    m_newWorktreeCheck->setToolTip(tr("Do this conversation in a new git worktree, then merge and clean up"));
    m_advisorBtn = new QCheckBox(tr("Advisor"), this);
    m_advisorBtn->setToolTip(tr("Let the agent consult a stronger model (sends /advisor)"));
    // King gold when on, matching the advisor tool-call card.
    m_advisorBtn->setStyleSheet(QStringLiteral(
        "QCheckBox:checked { color: #FFD700; font-weight: 600; }"));
    m_advisorBtn->hide(); // shown only while the agent advertises /advisor
    // clicked, not toggled: programmatic resets must not send a command.
    connect(m_advisorBtn, &QCheckBox::clicked, this, &AcpSessionView::onAdvisorClicked);
    auto *autoApproveRow = new QHBoxLayout();
    autoApproveRow->setContentsMargins(0, 0, 0, 0);
    autoApproveRow->setSpacing(6);
    autoApproveRow->addWidget(m_autoApproveCheck);
    autoApproveRow->addWidget(m_newWorktreeCheck);
    autoApproveRow->addWidget(m_advisorBtn);
    autoApproveRow->addStretch();
    m_usageIndicator = new AcpUsageIndicator(this);
    autoApproveRow->addWidget(m_usageIndicator);
    outer->addLayout(autoApproveRow);

    // 5. Image attachments.
    m_attachmentList = new AcpImageAttachmentList(this);
    outer->addWidget(m_attachmentList);

    // 5b. Goal status row — hidden until a goal is active.
    m_goalStatusRow = new QFrame(this);
    m_goalStatusRow->setObjectName(QStringLiteral("goalStatusRow"));
    m_goalStatusRow->setFrameShape(QFrame::NoFrame);
    m_goalStatusRow->setStyleSheet(QStringLiteral(
        "QFrame { background: rgba(180, 140, 50, 32); border: 1px solid rgba(180, 140, 50, 60); border-radius: 4px; }"));
    auto *goalRowLayout = new QHBoxLayout(m_goalStatusRow);
    goalRowLayout->setContentsMargins(8, 8, 8, 8);
    goalRowLayout->setSpacing(6);
    m_goalStatusLabel = new QLabel(m_goalStatusRow);
    m_goalStatusLabel->setObjectName(QStringLiteral("goalStatusLabel"));
    m_goalStatusLabel->setStyleSheet(QStringLiteral(
        "QLabel { background: transparent; border: none; color: rgb(180, 140, 50); font-weight: 600; font-size: 11px; }"));
    goalRowLayout->addWidget(m_goalStatusLabel, 1);
    m_goalStopBtn = new QToolButton(m_goalStatusRow);
    m_goalStopBtn->setObjectName(QStringLiteral("goalStopBtn"));
    m_goalStopBtn->setText(tr("Stop"));
    m_goalStopBtn->setAutoRaise(true);
    m_goalStopBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_goalStopBtn->setStyleSheet(QStringLiteral(
        "QToolButton { background: transparent; border: none; color: rgb(180, 100, 50); font-size: 11px; font-weight: 600; padding: 1px 6px; border-radius: 3px; }"
        "QToolButton:hover { background: rgba(180, 100, 50, 40); }"));
    m_goalStopBtn->setToolTip(tr("Stop the running goal"));
    connect(m_goalStopBtn, &QToolButton::clicked, this, &AcpSessionView::goalStopRequested);
    goalRowLayout->addWidget(m_goalStopBtn);

    m_goalStatusRow->hide();
    outer->addWidget(m_goalStatusRow);

    m_queueStrip = new AcpPromptQueueStrip(this);
    connect(m_queueStrip, &AcpPromptQueueStrip::removeRequested, this, [this](int index) {
        if (index < 0 || index >= m_promptQueue.size())
            return;
        const bool deferredGoal = m_promptQueue.items().at(index).fromGoalAgent;
        m_promptQueue.removeAt(index);
        refreshQueueStrip();
        if (deferredGoal)
            emit deferredGoalRemoved();
        emit promptQueueChanged();
    });
    outer->addWidget(m_queueStrip);

    // 6. Input.
    auto *cb = new ChatInputEditCb(m_attachmentList, this);
    cb->onSubmit = [this]() { onSendClicked(); };
    cb->onRestart = [this]() { emit restartSessionRequested(); };
    cb->onKeyFilter = [this](QKeyEvent *ke) -> bool {
        // Esc cancels prompt improvement if streaming.
        if (ke->key() == Qt::Key_Escape
            && m_promptImprover
            && m_promptImprover->state() == ai::PromptImprover::State::Streaming) {
            onImproveClicked(); // toggles to cancel
            return true;
        }
        if (!m_commandPopup || !m_commandPopup->isVisible())
            return false;
        switch (ke->key()) {
        case Qt::Key_Up: {
            int cur = m_commandPopup->currentRow();
            int found = -1;
            for (int i = cur - 1; i >= 0; --i) {
                if (!m_commandPopup->item(i)->isHidden()) { found = i; break; }
            }
            if (found < 0) {
                for (int i = m_commandPopup->count() - 1; i > cur; --i) {
                    if (!m_commandPopup->item(i)->isHidden()) { found = i; break; }
                }
            }
            if (found >= 0) m_commandPopup->setCurrentRow(found);
            return true;
        }
        case Qt::Key_Down: {
            int cur = m_commandPopup->currentRow();
            int found = -1;
            for (int i = cur + 1; i < m_commandPopup->count(); ++i) {
                if (!m_commandPopup->item(i)->isHidden()) { found = i; break; }
            }
            if (found < 0) {
                for (int i = 0; i < cur; ++i) {
                    if (!m_commandPopup->item(i)->isHidden()) { found = i; break; }
                }
            }
            if (found >= 0) m_commandPopup->setCurrentRow(found);
            return true;
        }
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Tab:
            acceptCommandCompletion();
            return true;
        case Qt::Key_Escape:
            hideCommandPopup();
            return true;
        case Qt::Key_Space:
            acceptCommandCompletion();
            return true;
        default:
            return false;
        }
    };
    cb->setMaximumHeight(120);
    m_input = cb;
    m_input->installEventFilter(this);
    m_input->viewport()->installEventFilter(this);
    outer->addWidget(m_input);

    // Slash-command completion popup (frameless, positioned above input).
    m_commandPopup = new QListWidget(this);
    m_commandPopup->setWindowFlags(Qt::ToolTip);
    m_commandPopup->setFocusPolicy(Qt::NoFocus);
    m_commandPopup->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_commandPopup->hide();

    // 6b. Floating improve-prompt button (child of m_input frame, bottom-right).
    // Parent is m_input (not viewport) so it stays fixed when content scrolls.
    m_improveBtn = new QToolButton(m_input);
    m_improveBtn->setAutoRaise(true);
    m_improveBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_improveBtn->setText(QStringLiteral("✨"));
    m_improveBtn->setToolTip(tr("Improve prompt with AI (Ctrl+I)"));
    m_improveBtn->setAccessibleName(tr("Improve prompt with AI"));
    m_improveBtn->setAccessibleDescription(tr("Rewrite the current prompt to be clearer (Ctrl+I)"));
    m_improveBtn->setCursor(Qt::PointingHandCursor);
    m_improveBtn->setStyleSheet(QStringLiteral(
        "QToolButton { background: transparent; border: none; padding: 2px; border-radius: 3px; font-size: 14px; }"
        "QToolButton:hover { background: palette(midlight); }"
        "QToolButton:disabled { opacity: 0.3; }"));
    m_improveBtn->hide();
    connect(m_improveBtn, &QToolButton::clicked, this, &AcpSessionView::onImproveClicked);

    // Ctrl+I shortcut for improve (scoped to input widget).
    m_improveShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_I), m_input);
    m_improveShortcut->setContext(Qt::WidgetShortcut);
    connect(m_improveShortcut, &QShortcut::activated, this, &AcpSessionView::onImproveClicked);

    // 7. Button row + usage. Attach is a secondary chrome action — render it
    // as an icon-only flat tool-button so Send remains the only prominent
    // button and the row stays compact.
    auto *btnRow = new QHBoxLayout();
    btnRow->setContentsMargins(0, 0, 0, 0);
    btnRow->setSpacing(4);
    m_attachBtn = new QToolButton(this);
    m_attachBtn->setAutoRaise(true);
    m_attachBtn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_attachBtn->setToolTip(tr("Attach an image (or paste from clipboard)"));
    m_attachBtn->setStyleSheet(QStringLiteral(
        "QToolButton { color: palette(placeholder-text); padding: 4px; border: 1px solid transparent; border-radius: 3px; }"
        "QToolButton:hover { color: palette(text); border: 1px solid palette(mid); }"));
    rebuildAttachIcon();
    m_cancelBtn = new QPushButton(tr("Cancel"), this);
    m_sendBtn = new QPushButton(tr("Send"), this);
    m_cancelBtn->setEnabled(false);
    m_cancelBtn->hide();
    btnRow->addWidget(m_attachBtn);
    btnRow->addWidget(m_modelCombo);
    btnRow->addWidget(m_modeCombo);
    btnRow->addWidget(m_effortCombo);
    btnRow->addStretch();

    m_elapsedTimer = new QTimer(this);
    m_elapsedTimer->setInterval(100); // 0.1 s precision
    connect(m_elapsedTimer, &QTimer::timeout, this, &AcpSessionView::onElapsedTick);

    m_goalElapsedTimer = new QTimer(this);
    m_goalElapsedTimer->setInterval(100);
    connect(m_goalElapsedTimer, &QTimer::timeout, this, [this]() {
        m_goalElapsedMs += 100;
        if (m_goalElapsedLabel) {
            const int whole = m_goalElapsedMs / 1000;
            const int tenths = (m_goalElapsedMs % 1000) / 100;
            m_goalElapsedLabel->setText(tr("Goal running · %1.%2s").arg(whole).arg(tenths));
        }
    });

    // Input-placeholder busy clock — repaints the elapsed-time placeholder once
    // a second while any agent is working. The underlying span is measured by
    // the monotonic m_busyClock; this timer only triggers the text refresh, so
    // a missed tick (event-loop stall) self-corrects on the next fire.
    m_busyPlaceholderTimer = new QTimer(this);
    m_busyPlaceholderTimer->setInterval(1000);
    connect(m_busyPlaceholderTimer, &QTimer::timeout,
            this, &AcpSessionView::updateBusyPlaceholderText);

    btnRow->addWidget(m_cancelBtn);
    btnRow->addWidget(m_sendBtn);

    m_sendWithGoalBtn = new QToolButton(this);
    m_sendWithGoalBtn->setText(tr("Goal"));
    m_sendWithGoalBtn->setAutoRaise(true);
    m_sendWithGoalBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_sendWithGoalBtn->setToolTip(tr("Start Goal on this session — sends the composer text if you typed one"));
    m_sendWithGoalBtn->setStyleSheet(QStringLiteral(
        "QToolButton { color: rgb(180, 140, 50); padding: 1px 6px; border: 1px solid rgba(180, 140, 50, 80); border-radius: 3px; }"
        "QToolButton:hover { color: rgb(200, 160, 60); background: rgba(180, 140, 50, 32); border: 1px solid rgba(180, 140, 50, 120); }"));
    connect(m_sendWithGoalBtn, &QToolButton::clicked, this, &AcpSessionView::sendWithGoalRequested);
    btnRow->addWidget(m_sendWithGoalBtn);

    outer->addLayout(btnRow);

    connect(m_sendBtn,   &QPushButton::clicked, this, &AcpSessionView::onSendClicked);
    connect(m_cancelBtn, &QPushButton::clicked, this, &AcpSessionView::onCancelClicked);
    connect(m_attachBtn, &QToolButton::clicked, this, &AcpSessionView::onAttachClicked);

    // Auto-approve state from registry.
    if (m_registry) {
        m_autoApproveCheck->setChecked(m_registry->autoApprovePolicy() == QLatin1String("allowAll"));
    }
    connect(m_autoApproveCheck, &QCheckBox::toggled,
            this, &AcpSessionView::onAutoApproveToggled);
    // clicked, not toggled: a programmatic setChecked must not yank focus
    // away from the transcript or a permission prompt.
    connect(m_newWorktreeCheck, &QCheckBox::clicked, this, [this]() {
        if (m_input) m_input->setFocus();
    });

    // Send-button enable state tracks attachment list non-empty / input text.
    connect(m_input, &QPlainTextEdit::textChanged, this, [this]() {
        updateSendButton();
        filterCommandPopup();
        updateImproveButtonState();
    });
    connect(m_attachmentList, &AcpImageAttachmentList::contentsChanged, this, [this]() {
        updateSendButton();
    });
    connect(m_attachmentList, &AcpImageAttachmentList::imageRejected, this, [this](const QString &reason) {
        setBanner(reason, BannerKind::Warning);
        QTimer::singleShot(2500, this, &AcpSessionView::clearBanner);
    });

    // Selector connections.
    connect(m_modelCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &AcpSessionView::onModelComboChanged);
    connect(m_modeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &AcpSessionView::onModeComboChanged);
    connect(m_effortCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &AcpSessionView::onEffortComboChanged);

    // Apply the chat font preference to transcript + input, and follow live
    // preference edits. The chat font may be independent of the editor font:
    // when "Use default font" is on it tracks the editor's Default Font (the
    // first three signals); when off it uses the ChatFont/* settings (the last
    // four). Connecting all of them keeps the view live either way. Chrome
    // (banner, selectors, buttons) keeps the system font.
    if (auto *settings = appSettings()) {
        connect(settings, &ApplicationSettings::fontNameChanged,
                this, &AcpSessionView::applyChatFont);
        connect(settings, &ApplicationSettings::fontSizeChanged,
                this, &AcpSessionView::applyChatFont);
        connect(settings, &ApplicationSettings::fontHintingChanged,
                this, &AcpSessionView::applyChatFont);
        connect(settings, &ApplicationSettings::chatFontUseDefaultChanged,
                this, &AcpSessionView::applyChatFont);
        connect(settings, &ApplicationSettings::chatFontFamilyChanged,
                this, &AcpSessionView::applyChatFont);
        connect(settings, &ApplicationSettings::chatFontSizePtChanged,
                this, &AcpSessionView::applyChatFont);
        connect(settings, &ApplicationSettings::chatFontSharpenChanged,
                this, &AcpSessionView::applyChatFont);
    }
    applyChatFont();

    // Prompt improver — uses the same AI provider as commit message generation.
    {
        auto *npApp = qobject_cast<NotepadNextApplication *>(qApp);
        auto *settings = appSettings();
        ai::CredentialStore *credStore = npApp ? npApp->getCredentialStore() : nullptr;
        if (settings) {
            m_promptImprover = new ai::PromptImprover(settings, credStore, this);
            connect(m_promptImprover, &ai::PromptImprover::finished,
                    this, &AcpSessionView::onImproveFinished);
            connect(m_promptImprover, &ai::PromptImprover::errorOccurred,
                    this, &AcpSessionView::onImproveError);
            connect(m_promptImprover, &ai::PromptImprover::stateChanged,
                    this, [this]() { updateImproveButtonState(); });
            connect(m_promptImprover, &ai::PromptImprover::imagesBudgeted,
                    this, [this](int sent, int total) {
                setBanner(tr("Prompt improvement: sent %1 of %2 images (10 MB limit)")
                          .arg(sent).arg(total), BannerKind::Info);
                QTimer::singleShot(4000, this, &AcpSessionView::clearBanner);
            });
        }
    }
}

void AcpSessionView::wireSignals()
{
    if (m_model) {
        connect(m_model, &AcpSessionModel::messageAppended,
                this, &AcpSessionView::onMessageAppended);
        connect(m_model, &AcpSessionModel::messageChunkAppended,
                this, &AcpSessionView::onMessageChunkAppended);
        connect(m_model, &AcpSessionModel::messageReplaced,
                this, &AcpSessionView::onMessageReplaced);
        connect(m_model, &AcpSessionModel::thoughtAppended,
                this, &AcpSessionView::onThoughtAppended);
        connect(m_model, &AcpSessionModel::thoughtChunkAppended,
                this, &AcpSessionView::onThoughtChunkAppended);
        connect(m_model, &AcpSessionModel::toolCallAddedOrUpdated,
                this, &AcpSessionView::onToolCallAddedOrUpdated);
        connect(m_model, &AcpSessionModel::planUpdated,
                this, &AcpSessionView::onPlanUpdated);
        connect(m_model, &AcpSessionModel::usageChanged,
                this, &AcpSessionView::onUsageChanged);
        connect(m_model, &AcpSessionModel::metadataChanged,
                this, &AcpSessionView::onMetadataChanged);
        connect(m_model, &AcpSessionModel::currentModeChanged,
                this, &AcpSessionView::onCurrentModeChanged);
        connect(m_model, &AcpSessionModel::isProcessingChanged,
                this, &AcpSessionView::onIsProcessingChanged);
        connect(m_model, &AcpSessionModel::backgroundTaskCountChanged,
                this, &AcpSessionView::onBackgroundTaskCountChanged);
        connect(m_model, &AcpSessionModel::turnEnded,
                this, &AcpSessionView::onTurnEnded);
        connect(m_model, &AcpSessionModel::goalJudgeTurnRemoved,
                this, &AcpSessionView::onGoalJudgeTurnRemoved);
    }

    if (m_connection) {
        connect(m_connection, &AcpConnection::permissionRequested,
                this, &AcpSessionView::onPermissionRequested);
        connect(m_connection, &AcpConnection::elicitationRequested,
                this, &AcpSessionView::onElicitationRequested);
        connect(m_connection, &AcpConnection::elicitationClosed,
                this, &AcpSessionView::onElicitationClosed);
        connect(m_connection, &AcpConnection::requestFailed,
                this, &AcpSessionView::onRequestFailed);
        connect(m_connection, &AcpConnection::scheduledJobsChanged,
                this, &AcpSessionView::onScheduledJobsChanged);
        connect(m_connection, &AcpConnection::errorOccurred,
                this, &AcpSessionView::onErrorOccurred);
        connect(m_connection, &AcpConnection::planReceived,
                this, [this](const QList<AcpProtocol::AcpPlanEntry> &entries) {
            resetElapsed();
            if (!m_planWidget)
                return;
            m_planWidget->setEntries(entries);
            m_planWidget->setAgentIdle(!m_model || !m_model->isProcessing());
            m_planWidget->setVisible(!entries.isEmpty());
        });
    }

    if (m_registry) {
        connect(m_registry, &AcpAgentRegistry::autoApprovePolicyChanged,
                this, &AcpSessionView::onAutoApprovePolicyChanged);
    }
}

void AcpSessionView::hydrateFromModel()
{
    if (!m_model) return;

    // Walk existing timeline entries and recreate widgets in order. Over the
    // visible cap we skip agent events (not user/goal/system) so a long history
    // doesn't instantiate thousands of markdown bubbles on session load.
    const auto &timeline = m_model->timeline();
    const auto &messages = m_model->messages();
    const auto &toolCalls = m_model->toolCalls();
    // Finished turns fold to a "Worked for Xs" row: their tool calls and
    // intermediate messages are never instantiated until the user expands.
    // An idle session folds its newest turn too; a mid-turn session gets the
    // live row from onIsProcessingChanged and folds in onTurnEnded.
    const QVector<AcpTurnFolding::Turn> turns = AcpTurnFolding::compute(
        timeline, messages, toolCalls, !m_model->isProcessing());
    m_foldedMask.fill(0, timeline.size());
    m_foldedCount = 0;
    refreshResponseActions(!m_model->isProcessing()); // before widgets exist: they pick it up
    QHash<int, const AcpTurnFolding::Turn *> foldAt; // first folded entry -> its turn
    for (const AcpTurnFolding::Turn &turn : turns) {
        // The row sits above the first folded entry, or above the report when
        // nothing folds.
        foldAt.insert(turn.folded.isEmpty() ? turn.survivor : turn.folded.first(), &turn);
        setFoldedMask(turn.folded, true);
    }
    const AcpTranscriptTruncation::Plan plan =
        AcpTranscriptTruncation::compute(timeline, messages,
                                         AcpTranscriptTruncation::kMaxVisibleEvents,
                                         &m_foldedMask);
    recordTruncation(plan);

    const QFont font = chatFont();
    int pendingHidden = 0;
    for (int i = 0; i < timeline.size(); ++i) {
        const auto fold = foldAt.constFind(i);
        if (fold != foldAt.constEnd()) {
            if (pendingHidden > 0) {
                insertTimelineWidget(makeTruncationPlaceholder(pendingHidden), false);
                pendingHidden = 0;
            }
            insertTimelineWidget(makeFoldRow(*fold.value()), false);
        }
        if (m_foldedMask.at(i) != 0)
            continue;
        if (plan.visible.at(i) == 0) {
            ++pendingHidden;
            continue;
        }
        if (pendingHidden > 0) {
            insertTimelineWidget(makeTruncationPlaceholder(pendingHidden), false);
            pendingHidden = 0;
        }
        if (QWidget *w = createEntryWidget(timeline.at(i), font))
            insertTimelineWidget(w, false);
    }
    if (pendingHidden > 0) {
        insertTimelineWidget(makeTruncationPlaceholder(pendingHidden), false);
    }
    restoreExpiredForm();
    syncTranscriptHostWidth();

    if (m_model->usage().has_value()) {
        m_usageIndicator->setUsage(m_model->usage());
    }

    onMetadataChanged();
    onIsProcessingChanged(m_model->isProcessing());
}

void AcpSessionView::setBanner(const QString &text, BannerKind kind)
{
    if (!m_banner) return;
    if (text.isEmpty()) {
        clearBanner();
        return;
    }
    m_bannerLabel->setText(text);
    QString kindStr;
    switch (kind) {
    case BannerKind::Info:    kindStr = QStringLiteral("info"); break;
    case BannerKind::Warning: kindStr = QStringLiteral("warning"); break;
    case BannerKind::Error:   kindStr = QStringLiteral("error"); break;
    }
    m_banner->setProperty("bannerKind", kindStr);
    // Re-polish parent + every child whose color is selected via the
    // [bannerKind=...] descendant rules. Without polishing the children,
    // Qt keeps their previously-resolved palette text colour.
    m_banner->style()->unpolish(m_banner);
    m_banner->style()->polish(m_banner);
    for (QWidget *child : m_banner->findChildren<QWidget *>()) {
        child->style()->unpolish(child);
        child->style()->polish(child);
    }
    m_banner->show();
}

void AcpSessionView::clearBanner()
{
    if (!m_banner) return;
    m_bannerLabel->clear();
    m_banner->setProperty("bannerKind", QStringLiteral("info"));
    m_banner->style()->unpolish(m_banner);
    m_banner->style()->polish(m_banner);
    for (QWidget *child : m_banner->findChildren<QWidget *>()) {
        child->style()->unpolish(child);
        child->style()->polish(child);
    }
    m_banner->hide();
}

void AcpSessionView::rebind(AcpSessionModel *model, AcpConnection *connection)
{
    // Detach from old model/connection — manager has (or is about to) delete
    // them, so disconnecting defends against late queued signals.
    if (m_model) {
        disconnect(m_model, nullptr, this, nullptr);
    }
    if (m_connection) {
        disconnect(m_connection, nullptr, this, nullptr);
        // Also sever the debug-dialog's live-update lambda which uses
        // m_debugDialog as context (not `this`), so the generic disconnect
        // above doesn't cover it.
        if (m_debugDialog) {
            disconnect(m_connection, &AcpConnection::debugLogAppended,
                       m_debugDialog, nullptr);
        }
    }

    m_model = model;
    m_connection = connection;
    m_savedPrefsApplied = false;
    ++m_scheduledRev; // the cached tooltip described the previous connection's jobs

    // Clear the transcript: drop bubbles, cards, plan, permission prompts.
    // Leave the trailing stretch in place, and skip the inline heartbeat
    // (it's owned by the view, reused across session swaps).
    while (m_transcriptLayout->count() > 1) {
        QLayoutItem *item = m_transcriptLayout->itemAt(0);
        if (!item) break;
        QWidget *w = item->widget();
        if (w == m_elapsedLabel) {
            // The heartbeat is always the last widget before the stretch,
            // so reaching it means we're done.
            break;
        }
        m_transcriptLayout->takeAt(0);
        if (w) w->deleteLater();
        delete item;
    }
    m_messageWidgets.clear();
    m_toolCallCards.clear();
    m_currentGroupCards.clear();
    m_truncationPlaceholders.clear();
    m_truncatedMessageIndices.clear();
    m_truncatedToolCallIds.clear();
    m_reportMessages.clear();
    m_foldedTurns.clear();
    m_expandQueue.clear();
    if (m_expandPump) m_expandPump->stop();
    m_foldedMask.clear();
    m_foldedCount = 0;
    m_liveRow.clear(); // already queued for deletion with the rest of the layout
    m_liveTurnBegin = -1;
    if (m_advisorBtn) m_advisorBtn->setChecked(false); // new session: advisor state unknown
    if (m_planWidget) {
        m_planWidget->setEntries({});
        m_planWidget->hide();
    }
    m_activeThought.clear();
    if (m_activePermissionPrompt) {
        m_activePermissionPrompt->deleteLater();
        m_activePermissionPrompt.clear();
    }
    // Form widgets went with the layout; the old connection answers nothing.
    m_pendingForm.clear();
    m_pendingFormKey.clear();
    m_askUserCandidates.clear();
    m_askUserShown.clear();
    m_carryAnswers.clear();

    clearBanner();
    if (m_usageIndicator) {
        m_usageIndicator->setUsage(std::nullopt);
    }

    // Fresh session → reset the auto-scroll flag to its default.
    m_stickToBottom = true;

    // Stop and hide the heartbeat indicator; onIsProcessingChanged will
    // restart it if the new session is already mid-turn.
    if (m_elapsedTimer) m_elapsedTimer->stop();
    if (m_elapsedLabel) m_elapsedLabel->hide();
    resetElapsed();

    // Stop goal heartbeat on rebind.
    if (m_goalElapsedTimer) m_goalElapsedTimer->stop();
    if (m_goalElapsedLabel) m_goalElapsedLabel->hide();
    clearGoalStatus();

    // If the debug-log popup is open, repoint its content at the new
    // connection's log so the user sees the freshly-restarted session.
    // Also rewire the live-update connection to the new AcpConnection.
    if (m_debugDialog && m_debugDialogText) {
        if (m_connection) {
            m_debugDialogText->setPlainText(m_connection->debugLog().join(QLatin1Char('\n')));
            // Wire live updates from the new connection.
            QPointer<AcpSessionView> self(this);
            connect(m_connection, &AcpConnection::debugLogAppended, m_debugDialog,
                    [self](const QString &line) {
                if (!self || !self->m_debugDialogText) return;
                const bool onlyGoal = self->m_debugDialogOnlyGoal
                                      && self->m_debugDialogOnlyGoal->isChecked();
                if (onlyGoal) return;
                QScrollBar *bar = self->m_debugDialogText->verticalScrollBar();
                const bool wasAtBottom = bar ? (bar->value() == bar->maximum()) : true;
                self->m_debugDialogText->appendPlainText(line);
                if (wasAtBottom && bar) bar->setValue(bar->maximum());
            });
        } else {
            m_debugDialogText->setPlainText(tr("(no active connection)"));
        }
    }

    // Restore Send/Cancel default visibility.
    onIsProcessingChanged(model ? model->isProcessing() : false);

    // Re-attach signals to the fresh model + connection and re-hydrate from
    // the (presumably empty) new model state.
    wireSignals();
    hydrateFromModel();
}

void AcpSessionView::insertTimelineWidget(QWidget *w, bool syncWidth)
{
    if (!w || !m_transcriptLayout) return;
    m_transcriptLayout->insertWidget(timelineTailIndex(w), w);
    // Prompts hug their text and sit on the right; AcpMessageWidget::sizeHint
    // supplies the width.
    if (const auto *msg = qobject_cast<AcpMessageWidget *>(w);
        msg && msg->role() == QLatin1String("user")) {
        m_transcriptLayout->setAlignment(w, Qt::AlignRight);
    }
    if (syncWidth)
        syncTranscriptHostWidth();
}

int AcpSessionView::timelineTailIndex(QWidget *exclude) const
{
    if (!m_transcriptLayout)
        return 0;
    QWidget *const anchors[] = { m_elapsedLabel, m_goalElapsedLabel };
    for (QWidget *a : anchors) {
        if (!a || a == exclude)
            continue;
        const int idx = m_transcriptLayout->indexOf(a);
        if (idx >= 0)
            return idx;
    }
    const int n = m_transcriptLayout->count();
    return n > 0 ? n - 1 : 0;
}

void AcpSessionView::syncTranscriptHostWidth()
{
    if (!m_scroll || !m_transcriptHost) return;
    QWidget *vp = m_scroll->viewport();
    if (!vp) return;
    const int width = vp->width();
    if (width > 0 && m_transcriptHost->width() != width) {
        m_transcriptHost->setFixedWidth(width);
    }
}

void AcpSessionView::removeTranscriptWidget(QWidget *w)
{
    if (!w || !m_transcriptLayout) return;
    const int idx = m_transcriptLayout->indexOf(w);
    if (idx >= 0) {
        QLayoutItem *item = m_transcriptLayout->takeAt(idx);
        delete item;
    }
    w->deleteLater();
}

// Builds (and registers) the widget for one timeline entry; the caller places
// it. Null when the entry renders nothing (empty bubble, unknown tool call).
QWidget *AcpSessionView::createEntryWidget(const AcpTimelineEntry &entry, const QFont &font)
{
    if (!m_model) return nullptr;
    if (entry.kind == AcpTimelineEntry::Kind::Message) {
        const auto &messages = m_model->messages();
        if (entry.messageIndex < 0 || entry.messageIndex >= messages.size())
            return nullptr;
        const AcpMessage &msg = messages.at(entry.messageIndex);
        if (!messageHasVisibleContent(msg))
            return nullptr;
        auto *w = new AcpMessageWidget(msg.role, m_transcriptHost);
        w->setChatFont(font); // styled widget: must set font explicitly
        if (msg.fromGoalAgent
            || msg.marker == QLatin1String(kAcpMarkerGoalAchieved)) {
            w->setFromGoalAgent(true);
        } else if (msg.fromGoalJudge) {
            w->setGoalTint(true);
        }
        w->setContent(msg.content);
        applyMessageMarker(w, entry.messageIndex);
        m_messageWidgets.insert(entry.messageIndex, w);
        attachResponseActions(entry.messageIndex, w);
        return w;
    }
    const auto it = m_model->toolCalls().constFind(entry.toolCallId);
    if (it == m_model->toolCalls().constEnd())
        return nullptr;
    auto *card = new AcpToolCallCard(it.value(), m_transcriptHost);
    card->setChatFont(font); // styled widget: must set font explicitly
    if (isGoalJudgeToolId(it.key()))
        card->setGoalTint(true);
    m_toolCallCards.insert(entry.toolCallId, card);
    return card;
}

void AcpSessionView::removeEntryWidget(const AcpTimelineEntry &entry)
{
    if (entry.kind == AcpTimelineEntry::Kind::Message) {
        if (AcpMessageWidget *w = m_messageWidgets.take(entry.messageIndex)) {
            if (m_activeThought == w)
                m_activeThought.clear();
            removeTranscriptWidget(w);
        }
    } else if (AcpToolCallCard *c = m_toolCallCards.take(entry.toolCallId)) {
        m_currentGroupCards.removeAll(c);
        removeTranscriptWidget(c);
    }
}

void AcpSessionView::refreshResponseActions(bool includeNewest)
{
    if (!m_model) return;
    const QVector<int> reports = AcpConversationCopy::finalReports(
        m_model->timeline(), m_model->messages(), includeNewest);
    QSet<int> next(reports.cbegin(), reports.cend());
    for (int idx : std::as_const(m_reportMessages)) {
        if (next.contains(idx))
            continue;
        if (AcpMessageWidget *w = m_messageWidgets.value(idx, nullptr))
            w->setResponseActions(false);
    }
    m_reportMessages = std::move(next);
    // Unchanged widgets return early; a report without a widget (truncated,
    // not yet built) picks its state up in attachResponseActions.
    for (int idx : reports) {
        if (AcpMessageWidget *w = m_messageWidgets.value(idx, nullptr))
            w->setResponseActions(true);
    }
}

void AcpSessionView::attachResponseActions(int messageIndex, AcpMessageWidget *w)
{
    if (w->role() != QLatin1String("assistant"))
        return;
    connect(w, &AcpMessageWidget::copyConversationRequested, this, [this, messageIndex]() {
        if (!m_model) return;
        QGuiApplication::clipboard()->setText(AcpConversationCopy::buildMarkdown(
            m_model->timeline(), m_model->messages(), messageIndex));
    });
    if (m_reportMessages.contains(messageIndex))
        w->setResponseActions(true);
}

void AcpSessionView::setFoldedMask(const QVector<int> &entries, bool folded)
{
    if (!m_model) return;
    const auto &timeline = m_model->timeline();
    if (m_foldedMask.size() < timeline.size())
        m_foldedMask.resize(timeline.size());
    for (int i : entries)
        setEntryFolded(i, folded);
}

void AcpSessionView::setEntryFolded(int i, bool folded)
{
    if (!m_model) return;
    const auto &timeline = m_model->timeline();
    if (i < 0 || i >= timeline.size())
        return;
    if (m_foldedMask.size() < timeline.size())
        m_foldedMask.resize(timeline.size());
    const std::uint8_t want = folded ? 1 : 0;
    if (m_foldedMask.at(i) == want)
        return;
    m_foldedMask[i] = want;
    m_foldedCount += folded ? 1 : -1;
    // Updates for a folded entry must not resurrect its widget: reuse the
    // truncation suppression sets that every update path already consults.
    const AcpTimelineEntry &e = timeline.at(i);
    if (e.kind == AcpTimelineEntry::Kind::Message) {
        if (folded) m_truncatedMessageIndices.insert(e.messageIndex);
        else m_truncatedMessageIndices.remove(e.messageIndex);
    } else {
        if (folded) m_truncatedToolCallIds.insert(e.toolCallId);
        else m_truncatedToolCallIds.remove(e.toolCallId);
    }
}

// adopt: the turn's live row, already placed in the layout and carrying the
// measured duration; otherwise a fresh row is created (the caller places it).
AcpTurnFoldRow *AcpSessionView::makeFoldRow(const AcpTurnFolding::Turn &turn, AcpTurnFoldRow *adopt)
{
    AcpTurnFoldRow *row = adopt ? adopt : new AcpTurnFoldRow(turn.workedMs, turn.toolCalls, m_transcriptHost);
    row->setChatFont(chatFont());
    row->setExpandable(!turn.folded.isEmpty());
    const int begin = turn.begin;
    connect(row, &AcpTurnFoldRow::expandedChanged, this,
            [this, begin](bool nowExpanded) { setTurnExpanded(begin, nowExpanded); });
    FoldedTurn ft;
    ft.row = row;
    ft.entries = turn.folded;
    m_foldedTurns.insert(begin, ft);
    return row;
}

// includeNewest: the newest turn has ended, fold it too.
void AcpSessionView::foldCompletedTurns(bool includeNewest)
{
    if (!m_model || !m_transcriptLayout) return;
    const auto &timeline = m_model->timeline();
    const QVector<AcpTurnFolding::Turn> turns = AcpTurnFolding::compute(
        timeline, m_model->messages(), m_model->toolCalls(), includeNewest);
    bool any = false;
    for (const AcpTurnFolding::Turn &turn : turns) {
        if (m_foldedTurns.contains(turn.begin))
            continue; // already folded (possibly re-expanded by the user)
        foldTurn(turn);
        any = true;
    }
    // A live row that foldTurn did not adopt heads a turn with nothing to
    // fold, or one that is no longer the newest.
    if (m_liveRow && (includeNewest
                      || m_liveTurnBegin != AcpTurnFolding::newestTurnBegin(timeline, m_model->messages()))) {
        dropLiveTurnRow();
        any = true;
    }
    if (!any) return;

    // Folded entries no longer count toward the render cap and any hidden-gap
    // label that only covered them is stale.
    const AcpTranscriptTruncation::Plan plan = AcpTranscriptTruncation::compute(
        timeline, m_model->messages(), AcpTranscriptTruncation::kMaxVisibleEvents,
        &m_foldedMask);
    if (!m_truncationPlaceholders.isEmpty() || !plan.gaps.isEmpty())
        rebuildTruncationPlaceholders(plan);
    syncTranscriptHostWidth();
}

void AcpSessionView::foldTurn(const AcpTurnFolding::Turn &turn)
{
    const auto &timeline = m_model->timeline();
    // The row takes the slot of the first folded widget that exists; fall back
    // to the report, then to whatever follows the turn.
    QWidget *before = nullptr;
    for (int i : turn.folded) {
        before = widgetForTimelineEntry(timeline.at(i));
        if (before)
            break;
    }
    if (!before && turn.survivor >= 0)
        before = widgetForTimelineEntry(timeline.at(turn.survivor));
    if (!before && turn.end < timeline.size())
        before = widgetForTimelineEntry(timeline.at(turn.end));
    // The live row already sits above the turn's entries: finalize it in place.
    AcpTurnFoldRow *live = nullptr;
    if (m_liveRow && m_liveTurnBegin == turn.begin) {
        live = m_liveRow;
        m_liveRow.clear();
        m_liveTurnBegin = -1;
        live->finishLive();
    }
    const int at = before ? m_transcriptLayout->indexOf(before) : -1;
    AcpTurnFoldRow *row = makeFoldRow(turn, live);
    if (!live) {
        if (at >= 0)
            m_transcriptLayout->insertWidget(at, row);
        else
            insertTimelineWidget(row, false);
    }

    for (int i : turn.folded)
        removeEntryWidget(timeline.at(i));
    setFoldedMask(turn.folded, true);
}

// The turn in flight gets a live "Worked for Xs" row right under its prompt;
// its entries stay rendered below it until the turn ends and foldTurn adopts
// the row. Idempotent: called when the prompt lands and when processing starts,
// whichever comes second creates it.
void AcpSessionView::ensureLiveTurnRow()
{
    if (!m_model || !m_transcriptLayout || !m_model->isProcessing()) return;
    const auto &timeline = m_model->timeline();
    const auto &messages = m_model->messages();
    const int begin = AcpTurnFolding::newestTurnBegin(timeline, messages);
    if (begin < 0 || m_foldedTurns.contains(begin)) return;
    if (m_liveRow) {
        if (m_liveTurnBegin == begin) return;
        dropLiveTurnRow();
    }
    const AcpTimelineEntry &anchor = timeline.at(begin);
    if (anchor.kind != AcpTimelineEntry::Kind::Message
        || anchor.messageIndex < 0 || anchor.messageIndex >= messages.size()) {
        return;
    }
    const AcpMessage &msg = messages.at(anchor.messageIndex);
    if (msg.role != QLatin1String("user")) return; // agent-only prefix: no prompt to head
    QWidget *anchorWidget = widgetForTimelineEntry(anchor);
    const int at = anchorWidget ? m_transcriptLayout->indexOf(anchorWidget) : -1;
    if (at < 0) return;

    // Tool calls already in the turn (session reopened mid-turn); the live
    // count then grows by one per new card in onToolCallAddedOrUpdated.
    m_liveToolCalls = 0;
    for (int i = begin; i < timeline.size(); ++i) {
        const AcpTimelineEntry &e = timeline.at(i);
        if (e.kind == AcpTimelineEntry::Kind::ToolCall && !isGoalJudgeToolId(e.toolCallId))
            ++m_liveToolCalls;
    }
    auto *row = new AcpTurnFoldRow(-1, m_liveToolCalls, m_transcriptHost);
    row->setChatFont(chatFont());
    row->startLive(msg.timestamp);
    m_transcriptLayout->insertWidget(at + 1, row);
    m_liveRow = row;
    m_liveTurnBegin = begin;
    syncTranscriptHostWidth();
}

void AcpSessionView::dropLiveTurnRow()
{
    if (m_liveRow)
        removeTranscriptWidget(m_liveRow);
    m_liveRow.clear();
    m_liveTurnBegin = -1;
}

void AcpSessionView::setTurnExpanded(int turnBegin, bool expanded)
{
    auto it = m_foldedTurns.find(turnBegin);
    if (it == m_foldedTurns.end() || !m_model || !m_transcriptLayout) return;
    FoldedTurn &ft = it.value();
    if (ft.expanded == expanded) return;
    ft.expanded = expanded;
    if (ft.row) ft.row->setExpanded(expanded);

    if (expanded) {
        // A turn can hold hundreds of tool calls and bubbles; building them all
        // in this click handler froze the UI for seconds. The first slice runs
        // now (the rows right under the fold row), the rest one slice per
        // event-loop pass so paint and input keep flowing. Every entry is built.
        ft.nextEntry = 0;
        if (!materializeTurnSlice(turnBegin, chatFont())) {
            if (!m_expandQueue.contains(turnBegin))
                m_expandQueue.append(turnBegin);
            if (!m_expandPump) {
                m_expandPump = new QTimer(this);
                m_expandPump->setSingleShot(true);
                m_expandPump->setInterval(0);
                connect(m_expandPump, &QTimer::timeout, this, &AcpSessionView::pumpExpandQueue);
            }
            m_expandPump->start();
        }
    } else {
        // Collapsing mid-expansion drops the pending slices; entries never
        // built have no widget and are still folded.
        ft.nextEntry = -1;
        m_expandQueue.removeOne(turnBegin);
        const auto &timeline = m_model->timeline();
        for (int i : ft.entries) {
            if (i >= 0 && i < timeline.size())
                removeEntryWidget(timeline.at(i));
        }
        setFoldedMask(ft.entries, true);
    }
    syncTranscriptHostWidth();
}

bool AcpSessionView::materializeTurnSlice(int turnBegin, const QFont &font)
{
    // ~1/3 of a 60 Hz frame: leaves the rest for the layout pass and paint the
    // new widgets trigger. Always builds at least one entry so a slice that
    // overruns (a large diff card) still makes progress.
    constexpr qint64 kSliceBudgetNs = 6'000'000;
    auto it = m_foldedTurns.find(turnBegin);
    if (it == m_foldedTurns.end() || !m_model || !m_transcriptLayout)
        return true;
    FoldedTurn &ft = it.value();
    if (!ft.expanded || ft.nextEntry < 0)
        return true;

    const auto &timeline = m_model->timeline();
    const int count = ft.entries.size();
    // Insert after the last built entry still on screen (truncation may have
    // dropped some between slices), else right under the fold row.
    QWidget *anchor = nullptr;
    for (int k = qMin(ft.nextEntry, count) - 1; k >= 0 && !anchor; --k) {
        const int i = ft.entries.at(k);
        if (i >= 0 && i < timeline.size())
            anchor = widgetForTimelineEntry(timeline.at(i));
    }
    if (!anchor)
        anchor = ft.row;
    const int anchorIdx = anchor ? m_transcriptLayout->indexOf(anchor) : -1;
    int at = anchorIdx >= 0 ? anchorIdx + 1 : -1; // -1: append at the tail

    QElapsedTimer clock;
    clock.start();
    while (ft.nextEntry < count) {
        const int i = ft.entries.at(ft.nextEntry++);
        if (i < 0 || i >= timeline.size())
            continue;
        setEntryFolded(i, false);
        const AcpTimelineEntry &entry = timeline.at(i);
        if (QWidget *existing = widgetForTimelineEntry(entry)) {
            // Rare: an update re-created it already. Keep order after it.
            const int idx = m_transcriptLayout->indexOf(existing);
            if (idx >= 0)
                at = idx + 1;
        } else if (QWidget *w = createEntryWidget(entry, font)) {
            if (at >= 0)
                m_transcriptLayout->insertWidget(at++, w);
            else
                insertTimelineWidget(w, false);
        }
        if (clock.nsecsElapsed() >= kSliceBudgetNs)
            break;
    }
    if (ft.nextEntry < count)
        return false;
    ft.nextEntry = -1;
    return true;
}

void AcpSessionView::pumpExpandQueue()
{
    if (m_expandQueue.isEmpty())
        return;
    const int turnBegin = m_expandQueue.first();
    if (materializeTurnSlice(turnBegin, chatFont()))
        m_expandQueue.removeFirst();
    syncTranscriptHostWidth();
    if (!m_expandQueue.isEmpty())
        m_expandPump->start();
}

// A goal-judge turn is removed from the tail of the timeline; a fold recorded
// for entries that no longer exist must go with it.
void AcpSessionView::pruneStaleFoldedTurns()
{
    if (!m_model) return;
    const int n = m_model->timeline().size();
    for (auto it = m_foldedTurns.begin(); it != m_foldedTurns.end();) {
        bool stale = it.key() >= n;
        for (int i : it.value().entries)
            stale = stale || i >= n;
        if (!stale) {
            ++it;
            continue;
        }
        if (it.value().row)
            removeTranscriptWidget(it.value().row);
        it = m_foldedTurns.erase(it);
    }
    if (m_liveRow && m_liveTurnBegin >= n)
        dropLiveTurnRow();
    if (m_foldedMask.size() > n)
        m_foldedMask.resize(n);
    m_foldedCount = 0;
    for (std::uint8_t v : m_foldedMask)
        m_foldedCount += v != 0 ? 1 : 0;
}

QWidget *AcpSessionView::widgetForTimelineEntry(const AcpTimelineEntry &entry) const
{
    if (entry.kind == AcpTimelineEntry::Kind::Message)
        return m_messageWidgets.value(entry.messageIndex, nullptr);
    return m_toolCallCards.value(entry.toolCallId, nullptr);
}

QLabel *AcpSessionView::makeTruncationPlaceholder(int hiddenCount)
{
    auto *lab = new QLabel(m_transcriptHost);
    lab->setObjectName(QStringLiteral("acpTruncationGap"));
    lab->setAlignment(Qt::AlignHCenter);
    lab->setWordWrap(true);
    lab->setStyleSheet(QStringLiteral(
        "QLabel { color: palette(placeholder-text); font-style: italic; padding: 4px 0px; }"));
    lab->setText(hiddenCount == 1
                     ? tr("1 event hidden")
                     : tr("%1 events hidden").arg(hiddenCount));
    lab->setFont(chatFont());
    m_truncationPlaceholders.append(lab);
    return lab;
}

void AcpSessionView::recordTruncation(const AcpTranscriptTruncation::Plan &plan)
{
    if (!m_model) return;
    const auto &timeline = m_model->timeline();
    const int n = qMin(plan.visible.size(), timeline.size());
    for (int i = 0; i < n; ++i) {
        if (plan.visible.at(i) != 0)
            continue;
        const AcpTimelineEntry &e = timeline.at(i);
        if (e.kind == AcpTimelineEntry::Kind::Message)
            m_truncatedMessageIndices.insert(e.messageIndex);
        else
            m_truncatedToolCallIds.insert(e.toolCallId);
    }
}

void AcpSessionView::rebuildTruncationPlaceholders(const AcpTranscriptTruncation::Plan &plan)
{
    for (QLabel *lab : m_truncationPlaceholders) {
        if (lab)
            removeTranscriptWidget(lab);
    }
    m_truncationPlaceholders.clear();
    if (!m_model || plan.gaps.isEmpty() || !m_transcriptLayout)
        return;

    const auto &timeline = m_model->timeline();
    for (const AcpTranscriptTruncation::Plan::Gap &gap : plan.gaps) {
        QLabel *lab = makeTruncationPlaceholder(gap.hiddenCount);
        QWidget *before = nullptr;
        if (gap.beforeIndex >= 0 && gap.beforeIndex < timeline.size())
            before = widgetForTimelineEntry(timeline.at(gap.beforeIndex));
        if (!before)
            before = m_elapsedLabel;
        const int idx = before ? m_transcriptLayout->indexOf(before) : -1;
        if (idx >= 0)
            m_transcriptLayout->insertWidget(idx, lab);
        else
            insertTimelineWidget(lab, false);
    }
}

void AcpSessionView::maybeTruncateTranscript()
{
    if (!m_model || !m_transcriptLayout) return;
    const auto &timeline = m_model->timeline();
    const int n = timeline.size() - m_foldedCount; // folded entries have no widgets
    const bool alreadyTruncating = !m_truncatedMessageIndices.isEmpty()
                                   || !m_truncatedToolCallIds.isEmpty()
                                   || !m_truncationPlaceholders.isEmpty();
    if (!alreadyTruncating) {
        if (n <= AcpTranscriptTruncation::kMaxVisibleEvents
                    + AcpTranscriptTruncation::kLiveHysteresis)
            return;
    } else if (n <= AcpTranscriptTruncation::kMaxVisibleEvents) {
        return;
    }

    const AcpTranscriptTruncation::Plan plan = AcpTranscriptTruncation::compute(
        timeline, m_model->messages(), AcpTranscriptTruncation::kMaxVisibleEvents,
        &m_foldedMask);
    bool anyRemoved = false;
    const int lim = qMin(plan.visible.size(), n);
    for (int i = 0; i < lim; ++i) {
        if (plan.visible.at(i) != 0)
            continue;
        const AcpTimelineEntry &e = timeline.at(i);
        if (e.kind == AcpTimelineEntry::Kind::Message) {
            m_truncatedMessageIndices.insert(e.messageIndex);
            if (AcpMessageWidget *w = m_messageWidgets.take(e.messageIndex)) {
                if (m_activeThought == w)
                    m_activeThought.clear();
                removeTranscriptWidget(w);
                anyRemoved = true;
            }
        } else {
            m_truncatedToolCallIds.insert(e.toolCallId);
            if (AcpToolCallCard *c = m_toolCallCards.take(e.toolCallId)) {
                m_currentGroupCards.removeAll(c);
                removeTranscriptWidget(c);
                anyRemoved = true;
            }
        }
    }

    if (anyRemoved || m_truncationPlaceholders.isEmpty())
        rebuildTruncationPlaceholders(plan);
    else
        recordTruncation(plan);

    if (anyRemoved)
        syncTranscriptHostWidth();
}

void AcpSessionView::appendMessageWidget(int idx)
{
    if (!m_model) return;
    if (idx < 0 || idx >= m_model->messages().size()) return;
    if (m_truncatedMessageIndices.contains(idx)) return;
    const AcpMessage &msg = m_model->messages().at(idx);
    if (!messageHasVisibleContent(msg))
        return;

    auto *w = new AcpMessageWidget(msg.role, m_transcriptHost);
    w->setChatFont(chatFont()); // styled widget: must set font explicitly
    if (msg.fromGoalAgent
        || msg.marker == QLatin1String(kAcpMarkerGoalAchieved)) {
        w->setFromGoalAgent(true);
    } else if (msg.fromGoalJudge) {
        w->setGoalTint(true);
    }
    w->setContent(msg.content);
    applyMessageMarker(w, idx);
    insertTimelineWidget(w);
    m_messageWidgets.insert(idx, w);
    attachResponseActions(idx, w);

    // A new prompt closes the previous turn for review: fold it to "Worked for Xs".
    if (msg.role == QLatin1String("user")) {
        foldCompletedTurns();
        ensureLiveTurnRow();
        refreshResponseActions(false);
    }

    // If a thought was streaming and an assistant message starts, collapse it.
    if (msg.role == QLatin1String("assistant") && !m_activeThought.isNull()) {
        m_activeThought->markStreamingDone();
        m_activeThought.clear();
    }

    maybeTruncateTranscript();
    scrollToBottomDeferred();
}

void AcpSessionView::onMessageAppended(int idx)
{
    resetElapsed();
    appendMessageWidget(idx);
}

void AcpSessionView::onMessageChunkAppended(int idx, const QString &chunk)
{
    auto *w = m_messageWidgets.value(idx, nullptr);
    if (!w) {
        if (m_truncatedMessageIndices.contains(idx)) return;
        appendMessageWidget(idx);
        w = m_messageWidgets.value(idx, nullptr);
    }
    if (w) {
        w->appendChunk(chunk);
        if (w->role() == QLatin1String("assistant") && !m_activeThought.isNull()) {
            m_activeThought->markStreamingDone();
            m_activeThought.clear();
        }
        scrollToBottomDeferred();
    }
}

void AcpSessionView::onMessageReplaced(int idx, const QString &fullText)
{
    auto *w = m_messageWidgets.value(idx, nullptr);
    if (!w) {
        if (m_truncatedMessageIndices.contains(idx)) return;
        appendMessageWidget(idx);
        w = m_messageWidgets.value(idx, nullptr);
    }
    if (w) {
        if (fullText.trimmed().isEmpty()) {
            m_messageWidgets.remove(idx);
            removeTranscriptWidget(w);
            return;
        }
        w->setText(fullText);
        if (w->role() == QLatin1String("assistant") && !m_activeThought.isNull()) {
            m_activeThought->markStreamingDone();
            m_activeThought.clear();
        }
        scrollToBottomDeferred();
    }
}

void AcpSessionView::onThoughtAppended(int idx)
{
    resetElapsed();
    appendMessageWidget(idx);
    auto *w = m_messageWidgets.value(idx, nullptr);
    if (w && w->role() == QLatin1String("thought")) {
        m_activeThought = w;
    }
}

void AcpSessionView::onThoughtChunkAppended(int idx, const QString &chunk)
{
    auto *w = m_messageWidgets.value(idx, nullptr);
    if (!w) {
        if (m_truncatedMessageIndices.contains(idx)) return;
        appendMessageWidget(idx);
        w = m_messageWidgets.value(idx, nullptr);
        if (w && w->role() == QLatin1String("thought")) {
            m_activeThought = w;
        }
    }
    if (w) {
        w->appendChunk(chunk);
        scrollToBottomDeferred();
    }
}

void AcpSessionView::onToolCallAddedOrUpdated(const QString &toolCallId)
{
    if (!m_model) return;
    resetElapsed();
    auto it = m_model->toolCalls().find(toolCallId);
    if (it == m_model->toolCalls().end()) return;
    const AcpProtocol::AcpToolCall &tc = it.value();
    trackAskUserToolCall(tc);

    auto *card = m_toolCallCards.value(toolCallId, nullptr);
    if (!card) {
        if (m_truncatedToolCallIds.contains(toolCallId)) return;
        card = new AcpToolCallCard(tc, m_transcriptHost);
        card->setChatFont(chatFont()); // styled widget: must set font explicitly
        if (isGoalJudgeToolId(toolCallId))
            card->setGoalTint(true);
        insertTimelineWidget(card);
        m_toolCallCards.insert(toolCallId, card);
        m_currentGroupCards.append(card);
        if (m_liveRow && !isGoalJudgeToolId(toolCallId))
            m_liveRow->setToolCallCount(++m_liveToolCalls);
        maybeTruncateTranscript();
        scrollToBottomDeferred();
    } else {
        // Apply as an update: build an update payload from the latest state.
        AcpProtocol::AcpToolCallUpdate upd;
        upd.id = tc.id;
        if (!tc.name.isEmpty()) {
            upd.name = tc.name;
        }
        if (!tc.title.isEmpty()) {
            upd.title = tc.title;
        }
        if (!tc.kind.isEmpty()) {
            upd.kind = tc.kind;
        }
        upd.status = tc.status;
        upd.backgroundRunning = tc.backgroundRunning;
        if (tc.durationMs >= 0) {
            upd.durationMs = tc.durationMs;
        }
        upd.content = tc.content;
        if (!tc.rawInput.isEmpty()) {
            upd.rawInput = tc.rawInput;
        }
        if (!tc.rawOutput.isEmpty()) {
            upd.rawOutput = tc.rawOutput;
        }
        card->apply(upd);
    }
}

void AcpSessionView::onPlanUpdated()
{
    // The plan is actually populated via the connection's planReceived signal
    // (which carries the entries). The model's planUpdated() carries no
    // entries; we keep the slot wired for symmetry with future model changes.
}

void AcpSessionView::onPlanResumeRequested(const QString &prompt)
{
    if (!m_connection || !m_model) return;
    if (m_model->isProcessing()) return;

    m_model->appendUserMessage(prompt, {});
    m_connection->sendPrompt(prompt, {});
    m_stickToBottom = true;
    scrollToBottomDeferred();
}

void AcpSessionView::onUsageChanged()
{
    if (m_model) m_usageIndicator->setUsage(m_model->usage());
}

void AcpSessionView::onMetadataChanged()
{
    if (!m_model) return;
    m_updatingSelectors = true;

    const auto &configOpts = m_model->configOptions();
    const AcpProtocol::AcpConfigOption *modelOpt = findModelConfigOption(configOpts);
    const AcpProtocol::AcpConfigOption *effortOpt = findEffortConfigOption(configOpts);

    // Models combo. Codex exposes base models via configOptions.model and
    // reasoning effort separately (reasoning_effort). Its legacy models array
    // still lists every model×effort pair ("gpt-5.5 (medium)", id "gpt-5.5/medium").
    // Prefer the config option; when falling back to the models array, collapse
    // to unique base models whenever effort is configured separately.
    m_modelCombo->clear();
    m_modelConfigOptionId.clear();

    if (modelOpt) {
        m_modelConfigOptionId = modelOpt->id;
        for (const auto &ch : modelOpt->options) {
            const QString label = ch.name.isEmpty() ? ch.value : ch.name;
            if (label.isEmpty()) continue;
            if (m_modelCombo->findData(ch.value) >= 0) continue;
            m_modelCombo->addItem(label, ch.value);
        }
        const QString currentVal = modelOpt->currentValue.toString();
        if (!currentVal.isEmpty()) {
            const int idx = m_modelCombo->findData(currentVal);
            if (idx >= 0) m_modelCombo->setCurrentIndex(idx);
        }
    }

    if (m_modelCombo->count() == 0) {
        const auto &models = m_model->availableModels();
        bool collapseEffort = effortOpt != nullptr;
        if (!collapseEffort) {
            for (const auto &m : models) {
                if (m.id.contains(QLatin1Char('/'))) {
                    collapseEffort = true;
                    break;
                }
            }
        }

        for (const auto &m : models) {
            QString id = m.id;
            QString label = m.name.isEmpty() ? m.id : m.name;
            if (collapseEffort) {
                id = baseModelId(id);
                label = stripEffortSuffix(label);
                if (id.isEmpty()) continue;
            }
            if (m_modelCombo->findData(id) >= 0) continue;
            m_modelCombo->addItem(label, id);
        }

        QString currentId = m_model->currentModelId();
        if (!currentId.isEmpty()) {
            if (collapseEffort) currentId = baseModelId(currentId);
            const int idx = m_modelCombo->findData(currentId);
            if (idx >= 0) m_modelCombo->setCurrentIndex(idx);
        }
    }
    m_modelCombo->setVisible(m_modelCombo->count() > 0);

    // Modes combo.
    m_modeCombo->clear();
    const auto &modes = m_model->availableModes();
    for (const auto &m : modes) {
        m_modeCombo->addItem(m.name.isEmpty() ? m.id : m.name, m.id);
    }
    if (!m_model->currentModeId().isEmpty()) {
        const int idx = m_modeCombo->findData(m_model->currentModeId());
        if (idx >= 0) m_modeCombo->setCurrentIndex(idx);
    }
    m_modeCombo->setVisible(!modes.isEmpty()
                            && !modesDuplicateEffortCatalog(modes, effortOpt));

    // Effort/reasoning combo (Claude Code, Codex reasoning_effort, etc).
    m_effortCombo->clear();
    m_effortConfigOptionId.clear();
    if (effortOpt) {
        m_effortConfigOptionId = effortOpt->id;
        for (const auto &ch : effortOpt->options) {
            const QString label = ch.name.isEmpty() ? ch.value : ch.name;
            if (label.isEmpty()) continue;
            if (m_effortCombo->findData(ch.value) >= 0) continue;
            m_effortCombo->addItem(label, ch.value);
        }
        const QString currentVal = effortOpt->currentValue.toString();
        if (!currentVal.isEmpty()) {
            const int idx = m_effortCombo->findData(currentVal);
            if (idx >= 0) m_effortCombo->setCurrentIndex(idx);
        }
    }
    m_effortCombo->setVisible(m_effortCombo->count() > 0);

    m_updatingSelectors = false;
    updateAdvisorButton();

    // Once the agent's catalogs have arrived, push any saved per-agent
    // selections back into the session (one-shot per session).
    applySavedPreferences();

    // After the initial restore, persist any agent-side config changes
    // (e.g. Codex CLI's config_option_update for model/mode/effort) so the
    // next session/new picks them up.
    if (m_savedPrefsApplied && m_registry && m_connection) {
        const QString agentId = m_connection->definition().id;
        if (!agentId.isEmpty()) {
            const QString curModel = m_model->currentModelId();
            if (!curModel.isEmpty()) {
                const QString modelKey = m_modelConfigOptionId.isEmpty()
                    ? QStringLiteral("model") : m_modelConfigOptionId;
                m_registry->setAgentPreference(agentId, modelKey, curModel);
            }

            const QString curMode = m_model->currentModeId();
            if (!curMode.isEmpty())
                m_registry->setAgentPreference(agentId, QStringLiteral("mode"), curMode);

            if (!m_effortConfigOptionId.isEmpty()) {
                for (const auto &opt : m_model->configOptions()) {
                    if (opt.id == m_effortConfigOptionId) {
                        const QString val = opt.currentValue.toString();
                        if (!val.isEmpty())
                            m_registry->setAgentPreference(agentId, m_effortConfigOptionId, val);
                        break;
                    }
                }
            }
        }
    }
}

void AcpSessionView::updateAdvisorButton()
{
    if (!m_advisorBtn || !m_model) return;
    const bool offered = AcpProtocol::commandsIncludeAdvisor(m_model->availableCommands());
    if (!offered && m_advisorBtn->isChecked())
        m_advisorBtn->setChecked(false);
    m_advisorBtn->setVisible(offered);
}

void AcpSessionView::onAdvisorClicked(bool on)
{
    sendAdvisorCommand(on);
    if (m_input) m_input->setFocus(); // the checkbox took focus; hand it back to the chat input
}

void AcpSessionView::sendAdvisorCommand(bool on)
{
    if (!m_connection || !m_model || !m_modelCombo) return;
    const int idx = m_modelCombo->currentIndex();
    submitSlashCommand(AcpProtocol::advisorCommandText(
        on, m_modelCombo->itemData(idx).toString(), m_modelCombo->itemText(idx)));
}

// Same routing as typing the command and pressing Send: queued while a turn
// is running, dispatched immediately otherwise.
void AcpSessionView::submitSlashCommand(const QString &text)
{
    if (!m_connection || !m_model) return;
    if (m_model->isProcessing()) {
        m_promptQueue.enqueue({text, {}});
        refreshQueueStrip();
        return;
    }
    dispatchPrompt(text, {});
}

void AcpSessionView::onCurrentModeChanged(const QString &modeId)
{
    if (!m_modeCombo) return;
    const int idx = m_modeCombo->findData(modeId);
    if (idx >= 0 && idx != m_modeCombo->currentIndex()) {
        m_updatingSelectors = true;
        m_modeCombo->setCurrentIndex(idx);
        m_updatingSelectors = false;
    }
}

void AcpSessionView::onIsProcessingChanged(bool processing)
{
    if (m_sendBtn)
        updateSendButton();
    if (m_cancelBtn) {
        m_cancelBtn->setEnabled(processing);
        m_cancelBtn->setVisible(processing);
    }
    // Heartbeat indicator: the 100 ms tick runs while the agent is processing
    // or background tasks are alive (updateElapsedLabel owns start/stop).
    if (processing) {
        resetElapsed();
        ensureLiveTurnRow();
        // Hide goal heartbeat while agent heartbeat is active.
        if (m_goalElapsedLabel) m_goalElapsedLabel->hide();
    } else {
        updateElapsedLabel();
        // Restore goal heartbeat if goal timer is still running, and reset
        // the counter so it shows time since the last turn ended.
        if (m_goalElapsedTimer && m_goalElapsedTimer->isActive()) {
            m_goalElapsedMs = 0;
            if (m_goalElapsedLabel) {
                m_goalElapsedLabel->setText(tr("Goal running · 0.0s"));
                m_goalElapsedLabel->show();
            }
        }
    }
    if (m_planWidget) m_planWidget->setAgentIdle(!processing);

    // Recompute the input busy-placeholder against the new ACP state (it also
    // depends on m_goalRunning, so the union is resolved inside).
    refreshBusyPlaceholder();

    if (!processing && m_promptQueue.size() > 0)
        scheduleFlushQueuedPrompt();
}

void AcpSessionView::onTurnEnded(int groupId)
{
    Q_UNUSED(groupId);
    // An adapter that never reports completion: the turn end is the signal.
    if (!m_askUserCandidates.isEmpty() && m_model) {
        const QSet<QString> candidates = std::exchange(m_askUserCandidates, {});
        for (const QString &id : candidates) {
            const auto tc = m_model->toolCalls().constFind(id);
            if (tc != m_model->toolCalls().constEnd()
                && !askUserCallDropped(tc->status)) {
                showAskUserToolCallForm(tc.value());
            }
        }
    }
    foldCompletedTurns(true); // the turn is over: fold it like every other
    refreshResponseActions(true);
    scrollToBottomDeferred();
    if (m_currentGroupCards.size() > 3) {
        for (auto *card : m_currentGroupCards) {
            if (card && !card->shouldPreserveExpanded()) card->setCollapsed(true);
        }
    }
    m_currentGroupCards.clear();
}

void AcpSessionView::onGoalJudgeTurnRemoved(const QVector<int> &messageIndices,
                                           const QStringList &toolCallIds)
{
    pruneStaleFoldedTurns();
    for (int idx : messageIndices) {
        if (AcpMessageWidget *w = m_messageWidgets.take(idx)) {
            if (m_activeThought == w)
                m_activeThought.clear();
            removeTranscriptWidget(w);
        }
    }
    for (const QString &id : toolCallIds) {
        if (AcpToolCallCard *c = m_toolCallCards.take(id)) {
            m_currentGroupCards.removeAll(c);
            removeTranscriptWidget(c);
        }
    }
}

void AcpSessionView::onPermissionRequested(const AcpProtocol::AcpPermissionRequest &req)
{
    resetElapsed();
    if (m_registry && m_registry->autoApprovePolicy() == QLatin1String("allowAll")) {
        // Connection already auto-approved before emitting (defense in depth).
        return;
    }
    auto *prompt = new AcpPermissionPrompt(req, m_transcriptHost);
    insertTimelineWidget(prompt);
    m_activePermissionPrompt = prompt;
    connect(prompt, &AcpPermissionPrompt::choiceMade,
            this, [this, prompt](const QString &reqId, const QString &outcome, const QString &optionId) {
        if (m_connection) {
            m_connection->respondToPermission(reqId, outcome, optionId);
        }
        prompt->deleteLater();
        if (m_activePermissionPrompt == prompt) {
            m_activePermissionPrompt.clear();
        }
    });
    scrollToBottomDeferred();
}

// ---------- Interactive question forms -------------------------------

void AcpSessionView::trackAskUserToolCall(const AcpProtocol::AcpToolCall &tc)
{
    if (m_askUserShown.contains(tc.id) || isGoalJudgeToolId(tc.id))
        return;
    if (!m_askUserCandidates.contains(tc.id)
        && !AskUserForm::isAskUserToolCall(tc.name, tc.title, tc.rawInput)) {
        return;
    }
    if (askUserCallDropped(tc.status)) {
        m_askUserCandidates.remove(tc.id);
        m_askUserShown.insert(tc.id);
        return;
    }
    if (tc.status == QLatin1String("completed")) {
        showAskUserToolCallForm(tc);
        return;
    }
    m_askUserCandidates.insert(tc.id);
}

void AcpSessionView::showAskUserToolCallForm(const AcpProtocol::AcpToolCall &tc)
{
    m_askUserCandidates.remove(tc.id);
    if (m_askUserShown.contains(tc.id))
        return;
    m_askUserShown.insert(tc.id);
    QString error;
    const auto spec = AskUserForm::parseAskUserInput(AskUserForm::askUserArguments(tc.rawInput, tc.rawOutput), &error);
    if (!spec) {
        // A form is already waiting on the user: a stray, unparseable call must
        // not start another turn (the agent would run on without the answers).
        if (m_pendingForm)
            return;
        // The agent believes the form is on screen and waits: tell it.
        sendHostPrompt(QLatin1String(AskUserForm::kAnswersHeader)
                           + QStringLiteral("\nThe form could not be shown: %1. Fix the input and call "
                                            "ask_user again.").arg(error),
                       {});
        return;
    }
    presentForm(*spec, FormSource::ToolCall, tc.id);
}

void AcpSessionView::onElicitationRequested(const AcpProtocol::AcpElicitationRequest &req)
{
    resetElapsed();
    presentForm(AskUserForm::fromElicitation(req.message, req.requestedSchema),
                FormSource::Elicitation, req.requestId);
}

void AcpSessionView::onElicitationClosed(const QString &requestId)
{
    if (m_pendingForm && m_pendingFormSource == FormSource::Elicitation && m_pendingFormKey == requestId)
        settlePendingForm(AskUserFormWidget::State::Cancelled, tr("Cancelled by the agent"));
}

bool AcpSessionView::isUnattended() const
{
    return m_goalRunning || m_scheduledUnattended;
}

bool AcpSessionView::holdsGoalEvaluation() const
{
    return m_promptQueue.containsMarker(QLatin1String(AskUserForm::kMarkerNoHuman));
}

void AcpSessionView::presentForm(const AskUserForm::Spec &spec, FormSource source, const QString &key)
{
    if (m_pendingForm) {
        m_carryAnswers = m_pendingForm->answers();
        settlePendingForm(AskUserFormWidget::State::Superseded, {}, QStringLiteral("decline"));
    }
    // Revised ask_user rounds keep what the user already chose (same ids).
    const QVector<AskUserForm::Answer> initial = source == FormSource::ToolCall && !m_carryAnswers.isEmpty()
        ? AskUserForm::carryOver(spec, m_carryAnswers)
        : AskUserForm::defaultAnswers(spec);
    m_carryAnswers.clear();

    auto *form = new AskUserFormWidget(spec, initial, m_transcriptHost);
    form->setChatFont(chatFont());
    insertTimelineWidget(form);
    scrollToBottomDeferred();

    if (isUnattended()) {
        const bool goal = m_goalRunning;
        form->setState(AskUserFormWidget::State::Cancelled,
                       goal ? tr("Cancelled — goal agent active") : tr("Cancelled — unattended session"));
        if (source == FormSource::Elicitation && m_connection)
            m_connection->respondToElicitation(key, QStringLiteral("decline"));
        if (!holdsGoalEvaluation()) {
            sendHostPrompt(AskUserForm::noHumanPrompt(goal), QLatin1String(AskUserForm::kMarkerNoHuman),
                           {}, goal);
        }
        return;
    }

    m_pendingForm = form;
    m_pendingFormSource = source;
    m_pendingFormKey = key;
    connect(form, &AskUserFormWidget::submitted, this, &AcpSessionView::onFormSubmitted);
    connect(form, &AskUserFormWidget::dismissed, this, &AcpSessionView::onFormDismissed);
    connect(form, &AskUserFormWidget::clarifyRequested, this, &AcpSessionView::onFormClarify);
    connect(form, &AskUserFormWidget::escapePressed, this, [this]() {
        if (m_input)
            m_input->setFocus();
    });
    // Take focus only from an empty composer the user is sitting in.
    if (m_input && m_input->hasFocus() && m_input->toPlainText().trimmed().isEmpty())
        form->focusFirstQuestion();
}

void AcpSessionView::settlePendingForm(AskUserFormWidget::State state, const QString &detail,
                                       const QString &elicitationAction)
{
    if (m_pendingForm && m_pendingFormSource == FormSource::Elicitation && m_connection
        && !elicitationAction.isEmpty()) {
        m_connection->respondToElicitation(m_pendingFormKey, elicitationAction);
    }
    if (m_pendingForm)
        m_pendingForm->setState(state, detail);
    m_pendingForm.clear();
    m_pendingFormKey.clear();
}

void AcpSessionView::onFormSubmitted(const QVector<AskUserForm::Answer> &answers)
{
    if (!m_pendingForm)
        return;
    const AskUserForm::Spec spec = m_pendingForm->spec();
    const FormSource source = m_pendingFormSource;
    const QString key = m_pendingFormKey;
    settlePendingForm(AskUserFormWidget::State::Answered, AskUserForm::answerSummary(spec, answers));
    if (source == FormSource::Elicitation) {
        if (m_connection) {
            m_connection->respondToElicitation(key, QStringLiteral("accept"),
                                               AskUserForm::elicitationContent(spec, answers));
        }
    } else {
        sendHostPrompt(AskUserForm::formatAnswers(spec, answers), QLatin1String(AskUserForm::kMarkerAnswers));
    }
    if (m_input)
        m_input->setFocus();
}

void AcpSessionView::onFormDismissed()
{
    if (!m_pendingForm)
        return;
    const FormSource source = m_pendingFormSource;
    const QString key = m_pendingFormKey;
    settlePendingForm(AskUserFormWidget::State::Dismissed);
    if (source == FormSource::Elicitation) {
        if (m_connection)
            m_connection->respondToElicitation(key, QStringLiteral("decline"));
    } else {
        sendHostPrompt(AskUserForm::dismissPrompt({}), QLatin1String(AskUserForm::kMarkerAnswers));
    }
}

void AcpSessionView::onFormClarify(int questionIndex)
{
    if (!m_pendingForm || questionIndex < 0 || questionIndex >= m_pendingForm->spec().questions.size())
        return;
    const AskUserForm::Spec spec = m_pendingForm->spec();
    const AskUserForm::Question &q = spec.questions.at(questionIndex);
    QVector<AskUserForm::Answer> answers = m_pendingForm->answers();
    const FormSource source = m_pendingFormSource;
    const QString key = m_pendingFormKey;
    settlePendingForm(AskUserFormWidget::State::ClarifyRequested);
    if (source == FormSource::Elicitation) {
        // The free-text companion carries the request; the agent re-asks.
        // The value stays: codex-acp marks every question required.
        if (!q.otherFieldKey.isEmpty())
            answers[questionIndex].other = AskUserForm::clarifyPrompt(q);
        else
            answers[questionIndex].note = AskUserForm::clarifyPrompt(q);
        if (m_connection) {
            m_connection->respondToElicitation(key, QStringLiteral("accept"),
                                               AskUserForm::elicitationContent(spec, answers));
        }
        return;
    }
    m_carryAnswers = answers;
    sendHostPrompt(AskUserForm::clarifyPrompt(q), QLatin1String(AskUserForm::kMarkerAnswers));
}

void AcpSessionView::sendHostPrompt(const QString &text, const QString &marker, const QString &wireText,
                                    bool fromGoalAgent)
{
    // Always through the queue: a flush is deferred one event-loop pass, so a
    // goal judge reacting to the same promptEnded still sees it queued.
    AcpPromptQueue::Item item;
    item.text = text;
    item.marker = marker;
    item.wireText = wireText;
    item.fromGoalAgent = fromGoalAgent;
    m_promptQueue.enqueue(std::move(item));
    refreshQueueStrip();
    emit promptQueueChanged();
    if (!m_model || !m_model->isProcessing())
        scheduleFlushQueuedPrompt();
}

void AcpSessionView::applyMessageMarker(AcpMessageWidget *w, int messageIndex)
{
    if (!m_model || messageIndex < 0 || messageIndex >= m_model->messages().size())
        return;
    if (m_model->messages().at(messageIndex).marker == QLatin1String(AskUserForm::kMarkerAnswers))
        w->setCollapsedSummary(AskUserForm::summaryFromAnswersText(w->plainText()));
}

void AcpSessionView::restoreExpiredForm()
{
    // Restored history: the newest ask_user call with no reply after it was
    // never answered; its session is gone, so show it read-only.
    if (!m_model)
        return;
    const auto &timeline = m_model->timeline();
    const auto &messages = m_model->messages();
    const auto &toolCalls = m_model->toolCalls();
    for (qsizetype i = timeline.size() - 1; i >= 0; --i) {
        const AcpTimelineEntry &entry = timeline.at(i);
        if (entry.kind == AcpTimelineEntry::Kind::Message) {
            if (entry.messageIndex >= 0 && entry.messageIndex < messages.size()
                && messages.at(entry.messageIndex).role == QLatin1String("user")) {
                return;
            }
            continue;
        }
        const auto tc = toolCalls.constFind(entry.toolCallId);
        if (tc == toolCalls.constEnd() || isGoalJudgeToolId(tc->id)
            || !AskUserForm::isAskUserToolCall(tc->name, tc->title, tc->rawInput)) {
            continue;
        }
        m_askUserShown.insert(tc->id);
        if (askUserCallDropped(tc->status))
            return;
        const auto spec = AskUserForm::parseAskUserInput(AskUserForm::askUserArguments(tc->rawInput, tc->rawOutput));
        if (!spec)
            return;
        auto *form = new AskUserFormWidget(*spec, AskUserForm::defaultAnswers(*spec), m_transcriptHost);
        form->setChatFont(chatFont());
        form->setState(AskUserFormWidget::State::Expired);
        insertTimelineWidget(form, false);
        return;
    }
}

void AcpSessionView::onRequestFailed(const QString &message)
{
    const QString trimmed = message.trimmed();
    setBanner(trimmed.isEmpty()
                  ? tr("Agent request failed — open Debug for details")
                  : tr("Agent request failed — %1").arg(trimmed),
              BannerKind::Error);
}

void AcpSessionView::onErrorOccurred(AcpErrorClassifier::AcpErrorKind kind, const QString &friendly)
{
    BannerKind bk = BannerKind::Warning;
    if (kind == AcpErrorClassifier::AcpErrorKind::SpawnFailed
        || kind == AcpErrorClassifier::AcpErrorKind::InitFailed) {
        bk = BannerKind::Error;
    }
    setBanner(friendly, bk);
}

void AcpSessionView::insertTextToInput(const QString &text)
{
    if (!m_input) return;
    QTextCursor cursor = m_input->textCursor();
    const int pos = cursor.position();
    const QString doc = m_input->toPlainText();

    if (pos > 0) {
        const QChar before = doc.at(pos - 1);
        if (before != QLatin1Char(' ') && before != QLatin1Char('\n')) {
            cursor.insertText(QStringLiteral(" "));
        }
    }

    QString toInsert = text;
    const bool atEnd = pos >= doc.size();
    const QChar after = atEnd ? QLatin1Char('\n') : doc.at(pos);
    if (toInsert.endsWith(QLatin1Char(' ')) && (after == QLatin1Char(' ') || after == QLatin1Char('\n'))) {
        toInsert.chop(1);
    }

    m_input->setTextCursor(cursor);
    m_input->insertPlainText(toInsert);
    m_input->setFocus();
}

QString AcpSessionView::peekInputText() const
{
    if (!m_input) return {};
    return m_input->toPlainText().trimmed();
}

QString AcpSessionView::takeInputText()
{
    if (!m_input) return {};
    const QString text = m_input->toPlainText().trimmed();
    m_input->clear();
    return text;
}

QVector<QPair<QByteArray, QString>> AcpSessionView::peekInputImages() const
{
    if (!m_attachmentList) return {};
    return m_attachmentList->peekAll();
}

QVector<QPair<QByteArray, QString>> AcpSessionView::takeInputImages()
{
    if (!m_attachmentList) return {};
    return m_attachmentList->takeAll();
}

QStringList AcpSessionView::goalDebugLog() const
{
    // parentWidget() returns QDockWidget's internal container, not the dock
    // itself. Walk up the ancestor chain to find the owning AiAgentDock.
    QWidget *w = parentWidget();
    while (w) {
        if (auto *dock = qobject_cast<AiAgentDock *>(w))
            return dock->goalDebugLog();
        w = w->parentWidget();
    }
    return {};
}

void AcpSessionView::setGoalActive(int criterionIndex, int totalCriteria, int iteration, int maxIterations)
{
    if (!m_goalStatusRow) return;
    m_goalRunning = true;
    if (m_pendingForm) {
        // The goal drives the session from now on; nobody answers this form.
        settlePendingForm(AskUserFormWidget::State::Cancelled, tr("Cancelled — goal agent active"),
                          QStringLiteral("decline"));
    }
    refreshBusyPlaceholder();
    QString text;
    if (totalCriteria > 1) {
        text = tr("Goal %1/%2 · iter %3/%4")
                   .arg(criterionIndex).arg(totalCriteria)
                   .arg(iteration).arg(maxIterations);
    } else {
        text = tr("Goal · iter %1/%2").arg(iteration).arg(maxIterations);
    }
    m_goalStatusLabel->setText(text);
    m_goalStopBtn->show();
    m_goalStatusRow->show();
    ++m_goalTerminalGeneration; // invalidate any pending auto-hide from a prior goal

    // Start the goal transcript heartbeat only on first activation.
    // Subsequent calls (iteration/criterion updates) keep the counter running.
    bool alreadyRunning = m_goalElapsedTimer && m_goalElapsedTimer->isActive();
    if (!alreadyRunning) {
        m_goalElapsedMs = 0;
        if (m_goalElapsedLabel) {
            m_goalElapsedLabel->setText(tr("Goal running · 0.0s"));
            bool agentProcessing = m_model && m_model->isProcessing();
            if (!agentProcessing) {
                m_goalElapsedLabel->show();
            }
        }
        if (m_goalElapsedTimer) {
            m_goalElapsedTimer->start();
        }
    }
}

void AcpSessionView::setGoalTerminal(const QString &statusText)
{
    if (!m_goalStatusRow) return;
    m_goalRunning = false;
    refreshBusyPlaceholder();
    m_goalStatusLabel->setText(statusText);
    m_goalStopBtn->hide();
    m_goalStatusRow->show();

    // Stop the goal transcript heartbeat.
    if (m_goalElapsedTimer) m_goalElapsedTimer->stop();
    if (m_goalElapsedLabel) m_goalElapsedLabel->hide();

    const int gen = ++m_goalTerminalGeneration;
    QTimer::singleShot(5000, this, [this, gen]() {
        if (gen != m_goalTerminalGeneration) return;
        clearGoalStatus();
    });
}

void AcpSessionView::clearGoalStatus()
{
    if (!m_goalStatusRow) return;
    m_goalRunning = false;
    refreshBusyPlaceholder();
    m_goalStatusRow->hide();
    if (m_goalElapsedTimer) m_goalElapsedTimer->stop();
    if (m_goalElapsedLabel) m_goalElapsedLabel->hide();
}

QString AcpSessionView::applyNewWorktreeInstruction(const QString &displayText) const
{
    if (!m_newWorktreeCheck || !m_newWorktreeCheck->isChecked())
        return displayText;
    return GoalAgent::nativeGoalWireText(displayText, true);
}

void AcpSessionView::updateSendButton()
{
    if (!m_sendBtn)
        return;
    const QString text = m_input ? m_input->toPlainText() : QString();
    const bool hasContent = !text.trimmed().isEmpty()
        || (m_attachmentList && m_attachmentList->isNonEmpty());
    m_sendBtn->setEnabled(hasContent);
}

void AcpSessionView::refreshQueueStrip()
{
    if (!m_queueStrip)
        return;
    QStringList previews;
    previews.reserve(m_promptQueue.size());
    for (const auto &item : m_promptQueue.items()) {
        QString preview = item.text.trimmed();
        const int nl = preview.indexOf(QLatin1Char('\n'));
        if (nl >= 0)
            preview = preview.left(nl);
        if (preview.isEmpty() && !item.images.isEmpty())
            preview = tr("Attached image");
        previews.append(preview);
    }
    m_queueStrip->setItems(previews);
}

void AcpSessionView::scheduleFlushQueuedPrompt()
{
    if (m_flushQueuedScheduled)
        return;
    m_flushQueuedScheduled = true;
    QPointer<AcpSessionView> guard(this);
    QTimer::singleShot(0, this, [guard]() {
        if (!guard)
            return;
        guard->m_flushQueuedScheduled = false;
        guard->flushQueuedPrompt();
        if (!guard)
            return;
        if (guard->m_model && !guard->m_model->isProcessing()
            && guard->m_promptQueue.size() > 0) {
            guard->scheduleFlushQueuedPrompt();
        }
    });
}

void AcpSessionView::flushQueuedPrompt()
{
    if (!m_connection || !m_model)
        return;
    const bool processing = m_model->isProcessing();
    auto item = m_promptQueue.takeNextIfIdle(processing);
    if (!item)
        return;
    refreshQueueStrip();
    const bool fromGoalAgent = item->fromGoalAgent;
    dispatchPrompt(item->text, item->images, fromGoalAgent, item->marker, item->wireText);
    if (fromGoalAgent)
        emit deferredGoalDispatched();
}

void AcpSessionView::enqueueFollowUp(const QString &text, bool fromGoalAgent)
{
    if (text.isEmpty())
        return;
    AcpPromptQueue::Item item;
    item.text = text;
    item.fromGoalAgent = fromGoalAgent;
    m_promptQueue.enqueue(std::move(item));
    refreshQueueStrip();
    emit promptQueueChanged();
    if (!m_model || !m_model->isProcessing())
        scheduleFlushQueuedPrompt();
}

int AcpSessionView::queuedPromptCount() const
{
    return m_promptQueue.size();
}

void AcpSessionView::dispatchPrompt(const QString &text,
                                    const QVector<QPair<QByteArray, QString>> &images,
                                    bool fromGoalAgent,
                                    const QString &marker,
                                    const QString &wireText)
{
    if (!m_connection || !m_model)
        return;
    // A typed `/advisor ...` flips the toggle just like clicking it would.
    if (m_advisorBtn) {
        if (const auto advisorOn = AcpProtocol::parseAdvisorCommand(text))
            m_advisorBtn->setChecked(*advisorOn);
    }
    const QString wire = applyNewWorktreeInstruction(wireText.isEmpty() ? text : wireText);
    m_model->appendUserMessage(text, images, fromGoalAgent, marker);
    QList<QPair<QByteArray, QString>> imageList;
    imageList.reserve(images.size());
    for (const auto &p : images)
        imageList.append(p);
    m_connection->sendPrompt(wire, imageList);
    m_currentGroupCards.clear();
    emit inputFocused();
    m_stickToBottom = true;
    scrollToBottomDeferred();
}

void AcpSessionView::onSendClicked()
{
    if (!m_connection || !m_model || !m_input)
        return;

    const QString text = m_input->toPlainText().trimmed();
    const bool hasContent = !text.isEmpty()
        || (m_attachmentList && m_attachmentList->isNonEmpty());
    const bool processing = m_model->isProcessing();
    const auto kind = AcpPromptQueue::classifySend(
        processing, hasContent,
        GoalAgent::nativeGoalUsesSidePrompt(text, m_model->agentAdvertisesGoalCommand()));

    // The user is here: forms in this session are answered from now on.
    QString wireText;
    if (kind != AcpPromptQueue::SendKind::Ignore) {
        m_scheduledUnattended = false;
        if (m_pendingForm && kind != AcpPromptQueue::SendKind::SidePrompt) {
            // A composer reply while a form is open answers it in free form. An
            // elicitation blocks the turn: decline it and let the text queue.
            if (m_pendingFormSource == FormSource::ToolCall && !text.isEmpty())
                wireText = AskUserForm::freeformPrompt(text);
            settlePendingForm(AskUserFormWidget::State::AnsweredInChat, {}, QStringLiteral("decline"));
        }
    }

    switch (kind) {
    case AcpPromptQueue::SendKind::Ignore:
        return;
    case AcpPromptQueue::SendKind::Enqueue: {
        QVector<QPair<QByteArray, QString>> images =
            m_attachmentList ? m_attachmentList->takeAll()
                             : QVector<QPair<QByteArray, QString>>{};
        AcpPromptQueue::Item item{text, std::move(images)};
        item.wireText = wireText;
        m_promptQueue.enqueue(std::move(item));
        m_input->clear();
        updateSendButton();
        refreshQueueStrip();
        emit inputFocused();
        return;
    }
    case AcpPromptQueue::SendKind::SidePrompt:
        m_model->appendUserMessage(text, {});
        m_connection->sendSidePrompt(applyNewWorktreeInstruction(text));
        m_input->clear();
        updateSendButton();
        emit inputFocused();
        m_stickToBottom = true;
        scrollToBottomDeferred();
        return;
    case AcpPromptQueue::SendKind::SendNow: {
        QVector<QPair<QByteArray, QString>> images =
            m_attachmentList ? m_attachmentList->takeAll()
                             : QVector<QPair<QByteArray, QString>>{};
        m_input->clear();
        updateSendButton();
        dispatchPrompt(text, images, false, {}, wireText);
        return;
    }
    default:
        return;
    }
}

void AcpSessionView::onCancelClicked()
{
    if (m_pendingForm && m_pendingFormSource == FormSource::Elicitation)
        settlePendingForm(AskUserFormWidget::State::Cancelled, {}, QStringLiteral("cancel"));
    if (m_connection) m_connection->cancelPrompt();
    // Let the owning dock stop any GoalAgent running on this session. Cancel
    // means "stop everything", not just the current ACP turn — otherwise the
    // goal supervisor would simply re-prompt the target after the cancel.
    emit cancelRequested();
}

void AcpSessionView::onAttachClicked()
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this,
        tr("Attach images"),
        QString(),
        tr("Images (*.png *.jpg *.jpeg *.gif *.webp)"));
    for (const QString &path : files) {
        m_attachmentList->addFileByPath(path);
    }
}

void AcpSessionView::onAutoApproveToggled(bool checked)
{
    if (!m_registry) return;
    m_registry->setAutoApprovePolicy(checked
                                         ? QStringLiteral("allowAll")
                                         : QStringLiteral("manual"));
}

void AcpSessionView::onAutoApprovePolicyChanged(const QString &policy)
{
    if (!m_autoApproveCheck) return;
    const bool wantChecked = (policy == QLatin1String("allowAll"));
    if (m_autoApproveCheck->isChecked() != wantChecked) {
        QSignalBlocker blocker(m_autoApproveCheck);
        m_autoApproveCheck->setChecked(wantChecked);
    }
}

void AcpSessionView::onModelComboChanged(int index)
{
    if (m_updatingSelectors || !m_connection || index < 0) return;
    const QString id = m_modelCombo->itemData(index).toString();
    if (id.isEmpty()) return;
    const QString prefKey = m_modelConfigOptionId.isEmpty()
        ? QStringLiteral("model") : m_modelConfigOptionId;
    if (!m_modelConfigOptionId.isEmpty()) {
        m_connection->setConfigOption(m_modelConfigOptionId, id);
    } else {
        m_connection->setModel(id);
    }
    if (m_registry) {
        m_registry->setAgentPreference(m_connection->definition().id, prefKey, id);
    }
    // The advisor tracks the selected model's level: re-point it.
    if (m_advisorBtn && m_advisorBtn->isVisible() && m_advisorBtn->isChecked())
        sendAdvisorCommand(true);
}


void AcpSessionView::onModeComboChanged(int index)
{
    if (m_updatingSelectors || !m_connection || index < 0) return;
    const QString id = m_modeCombo->itemData(index).toString();
    if (id.isEmpty()) return;
    m_connection->setMode(id);
    if (m_registry) {
        m_registry->setAgentPreference(m_connection->definition().id,
                                       QStringLiteral("mode"), id);
    }
}

void AcpSessionView::onEffortComboChanged(int index)
{
    if (m_updatingSelectors || !m_connection || index < 0) return;
    if (m_effortConfigOptionId.isEmpty()) return;
    const QString val = m_effortCombo->itemData(index).toString();
    m_connection->setConfigOption(m_effortConfigOptionId, val);
    if (m_registry && !val.isEmpty()) {
        m_registry->setAgentPreference(m_connection->definition().id,
                                       m_effortConfigOptionId, val);
    }
}


void AcpSessionView::applySavedPreferences()
{
    if (m_savedPrefsApplied) return;
    if (!m_registry || !m_connection || !m_model) return;
    // Wait until the agent's catalogs have been populated, otherwise we can't
    // tell whether a saved id is still valid.
    const bool catalogsReady = !m_model->availableModels().isEmpty()
        || !m_model->availableModes().isEmpty()
        || !m_model->configOptions().isEmpty();
    if (!catalogsReady) return;

    m_savedPrefsApplied = true;
    const QString agentId = m_connection->definition().id;
    if (agentId.isEmpty()) return;

    // Model: send only if the saved id exists in the catalog and differs.
    // Config-option-backed pickers (Claude Code) restore via session/set_config.
    const QString modelPrefKey = m_modelConfigOptionId.isEmpty()
        ? QStringLiteral("model") : m_modelConfigOptionId;
    const QString savedModel = m_registry->agentPreference(agentId, modelPrefKey);
    const QString savedModelBase = baseModelId(savedModel);
    const QString currentModelBase = baseModelId(m_model->currentModelId());
    if (!savedModel.isEmpty() && savedModelBase != currentModelBase) {
        if (!m_modelConfigOptionId.isEmpty()) {
            for (const auto &opt : m_model->configOptions()) {
                if (opt.id != m_modelConfigOptionId) continue;
                for (const auto &ch : opt.options) {
                    if (ch.value == savedModel || ch.value == savedModelBase) {
                        m_connection->setConfigOption(m_modelConfigOptionId, ch.value);
                        break;
                    }
                }
                break;
            }
        } else {
            for (const auto &m : m_model->availableModels()) {
                if (m.id == savedModel || baseModelId(m.id) == savedModelBase) {
                    m_connection->setModel(m.id.contains(QLatin1Char('/'))
                                               ? baseModelId(m.id)
                                               : m.id);
                    break;
                }
            }
        }
    }

    // Mode.
    const QString savedMode = m_registry->agentPreference(agentId, QStringLiteral("mode"));
    if (!savedMode.isEmpty() && savedMode != m_model->currentModeId()) {
        for (const auto &m : m_model->availableModes()) {
            if (m.id == savedMode) {
                m_connection->setMode(savedMode);
                break;
            }
        }
    }

    // Effort / reasoning level: keyed by the matched config-option id.
    if (!m_effortConfigOptionId.isEmpty()) {
        const QString savedEffort = m_registry->agentPreference(agentId, m_effortConfigOptionId);
        if (!savedEffort.isEmpty()) {
            for (const auto &opt : m_model->configOptions()) {
                if (opt.id != m_effortConfigOptionId) continue;
                if (opt.currentValue.toString() == savedEffort) break;
                for (const auto &ch : opt.options) {
                    if (ch.value == savedEffort) {
                        m_connection->setConfigOption(m_effortConfigOptionId, savedEffort);
                        break;
                    }
                }
                break;
            }
        }
    }
}

void AcpSessionView::onJumpToBottomClicked()
{
    if (!m_scroll) return;
    // Explicit user intent to re-engage the stream → re-stick.
    m_stickToBottom = true;
    auto *vbar = m_scroll->verticalScrollBar();
    if (vbar) {
        m_programmaticScroll = true;
        vbar->setValue(vbar->maximum());
        m_programmaticScroll = false;
    }
    updateJumpButtonVisibility();
}

void AcpSessionView::resetElapsed()
{
    m_elapsedMs = 0;
    updateElapsedLabel();
}

void AcpSessionView::onElapsedTick()
{
    if (m_model && m_model->isProcessing())
        m_elapsedMs += 100;
    updateElapsedLabel();
}

void AcpSessionView::onBackgroundTaskCountChanged(int)
{
    updateElapsedLabel();
}

void AcpSessionView::onBackgroundStopRequested()
{
    if (!m_connection)
        return;
    // Snapshot now; a task that ends before the click makes stop a no-op.
    const auto tasks = m_connection->backgroundTasks();
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    const QFontMetrics fm(menu->font());
    const QPointer<AcpConnection> conn(m_connection);
    int offered = 0;
    for (const auto &task : tasks) {
        if (!task.canStop)
            continue;
        QString name = task.name.isEmpty() ? task.taskId : task.name;
        name = fm.elidedText(name, Qt::ElideRight, 360).replace(QLatin1Char('&'), QStringLiteral("&&"));
        QAction *act = menu->addAction(QStringLiteral("⏹ ") + tr("Stop: %1").arg(name));
        const QString taskId = task.taskId;
        connect(act, &QAction::triggered, this, [conn, taskId]() {
            if (conn)
                conn->stopBackgroundTask(taskId);
        });
        ++offered;
    }
    if (offered == 0) {
        delete menu;
        return;
    }
    menu->popup(QCursor::pos());
}

void AcpSessionView::onScheduledJobsChanged()
{
    ++m_scheduledRev;
    updateElapsedLabel();
}

void AcpSessionView::updateElapsedLabel()
{
    if (!m_elapsedLabel) return;
    const bool processing = m_model && m_model->isProcessing();
    const int background = m_model ? m_model->backgroundTaskCount() : 0;
    const auto scheduledJobs = m_connection ? m_connection->scheduledJobs()
                                            : QVector<AcpProtocol::AcpScheduledJob>();
    const int scheduled = static_cast<int>(scheduledJobs.size());

    QString text;
    if (processing) {
        text = QStringLiteral("%1.%2s").arg(m_elapsedMs / 1000).arg((m_elapsedMs % 1000) / 100);
    }
    // Background wait clock: starts on the 0→N edge and runs to the right of
    // the label until every task has finished, whatever the turn state.
    if (background > 0 && !m_bgWaitClock.isValid())
        m_bgWaitClock.start();
    else if (background == 0 && m_bgWaitClock.isValid())
        m_bgWaitClock.invalidate();
    if (m_elapsedTimer && (processing || background > 0) != m_elapsedTimer->isActive()) {
        if (processing || background > 0)
            m_elapsedTimer->start();
        else
            m_elapsedTimer->stop();
    }
    if (background > 0) {
        if (!text.isEmpty())
            text += QStringLiteral("  ·  ");
        text += tr("📡 Waiting background (%1)  %2")
                    .arg(background)
                    .arg(AcpProtocol::formatWaitDuration(m_bgWaitClock.elapsed()));
        if (m_connection && m_connection->hasStoppableBackgroundTask())
            text += QStringLiteral("  <a href=\"stop\">⏹ ") + tr("Stop") + QStringLiteral("</a>");
    }
    // Wake-ups / cron jobs the agent set for itself. Display only (the agent
    // cannot be told to drop them) and no ticking clock: absolute times live
    // in the tooltip, so an idle session with a daily cron costs nothing.
    if (scheduled > 0) {
        if (!text.isEmpty())
            text += QStringLiteral("  ·  ");
        text += tr("⏰ Scheduled (%1)").arg(scheduled);
    }
    // Called per streamed event: only touch the tooltip on a state edge or
    // when the scheduled-job list changed.
    if ((background > 0) != m_elapsedTipIsBackground || m_scheduledRev != m_elapsedTipScheduledRev) {
        m_elapsedTipIsBackground = background > 0;
        m_elapsedTipScheduledRev = m_scheduledRev;
        // Rich text so a prompt containing "<" or "&" cannot be misparsed.
        QStringList lines;
        if (m_elapsedTipIsBackground)
            lines << tr("Background tasks started by the agent are still running").toHtmlEscaped();
        else if (scheduled == 0)
            lines << tr("Time since the last new event from the agent").toHtmlEscaped();
        if (scheduled > 0) {
            if (!lines.isEmpty())
                lines << QString();
            lines << tr("Scheduled by the agent (cannot be cancelled from here):").toHtmlEscaped();
            for (const auto &job : scheduledJobs)
                lines << QStringLiteral("• ") + AcpProtocol::formatScheduledJob(job).toHtmlEscaped();
        }
        const QString tip = QStringLiteral("<div>") + lines.join(QStringLiteral("<br>"))
                          + QStringLiteral("</div>");
        m_elapsedLabel->setToolTip(tip);
    }
    m_elapsedLabel->setVisible(processing || background > 0 || scheduled > 0);
    if (m_elapsedLabel->text() != text)
        m_elapsedLabel->setText(text);
}

void AcpSessionView::refreshBusyPlaceholder()
{
    if (!m_input) return;

    // Union of every running-agent surface. Either an in-flight ACP turn or an
    // active goal keeps the input "busy"; the placeholder reflects that union,
    // not whichever finished last.
    const bool acpBusy = m_model && m_model->isProcessing();
    const bool busy = acpBusy || m_goalRunning;

    if (busy == m_busyPlaceholderActive) {
        // No edge — the other surface was already keeping us busy. Leave the
        // monotonic clock untouched so the elapsed span keeps accumulating.
        return;
    }
    m_busyPlaceholderActive = busy;

    if (busy) {
        m_busyClock.start();                 // idle→busy edge: anchor the clock once
        updateBusyPlaceholderText();         // paint 0m 0s immediately, don't wait 1 s
        m_busyPlaceholderTimer->start();
    } else {
        m_busyPlaceholderTimer->stop();
        // Busy→idle edge. setInputPlaceholder() handles the viewport
        // invalidation so the input reverts to the idle text immediately
        // instead of staying frozen on the last "Agent is working… (Xm Ys)".
        setInputPlaceholder(tr("Send a message"));
    }
    updateImproveButtonState();
}

void AcpSessionView::updateBusyPlaceholderText()
{
    if (!m_input || !m_busyPlaceholderActive) return;
    const qint64 elapsedSec = m_busyClock.elapsed() / 1000;
    const qint64 minutes = elapsedSec / 60;
    const qint64 seconds = elapsedSec % 60;
    setInputPlaceholder(tr("Agent is working… (%1m %2s)").arg(minutes).arg(seconds));
}

void AcpSessionView::setInputPlaceholder(const QString &text)
{
    if (!m_input) return;
    m_input->setPlaceholderText(text);
    // QPlainTextEdit::setPlaceholderText() does not reliably invalidate the
    // viewport on a pure text change (its internal repaint is conditional and
    // varies across Qt 6.5↔6.10), so without this poke a placeholder transition
    // stays frozen on whatever was last painted by an unrelated relayout. The
    // placeholder is drawn in the viewport's paintEvent — invalidate it
    // directly, but only when it's actually on screen (empty document); when
    // the user has typed text the placeholder is hidden and Qt repaints the
    // content edit itself. Routed through one helper so no transition can skip
    // the poke. At most one repaint per second while busy — negligible cost.
    if (m_input->document()->isEmpty()) {
        m_input->viewport()->update();
    }
}

void AcpSessionView::onShowDebugLogClicked()
{
    // Find the owning AiAgentDock so we can scope the dialog title and goal
    // log signals to this specific session/tab.
    AiAgentDock *ownerDock = nullptr;
    for (QWidget *w = parentWidget(); w; w = w->parentWidget()) {
        if (auto *d = qobject_cast<AiAgentDock *>(w)) { ownerDock = d; break; }
    }

    auto refresh = [this]() {
        if (!m_debugDialogText) return;
        if (m_debugDialogOnlyGoal && m_debugDialogOnlyGoal->isChecked()) {
            m_debugDialogText->setPlainText(goalDebugLog().join(QLatin1Char('\n')));
        } else if (m_connection) {
            QStringList combined = m_connection->debugLog();
            combined.append(goalDebugLog());
            m_debugDialogText->setPlainText(combined.join(QLatin1Char('\n')));
        } else {
            m_debugDialogText->setPlainText(goalDebugLog().join(QLatin1Char('\n')));
        }
        m_debugDialogText->verticalScrollBar()->setValue(
            m_debugDialogText->verticalScrollBar()->maximum());
    };

    if (m_debugDialog) {
        refresh();
        m_debugDialog->show();
        m_debugDialog->raise();
        m_debugDialog->activateWindow();
        return;
    }

    auto *dlg = new QDialog(window());
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    // Per-session title so multiple debug windows (one per AI tab) are
    // distinguishable in the OS task list and on screen. Falls back to a
    // generic title when the owning dock can't be located.
    if (ownerDock && !ownerDock->windowTitle().isEmpty()) {
        dlg->setWindowTitle(tr("ACP Debug Log — %1").arg(ownerDock->windowTitle()));
    } else {
        dlg->setWindowTitle(tr("ACP Debug Log"));
    }
    dlg->resize(800, 500);

    auto *layout = new QVBoxLayout(dlg);
    layout->setContentsMargins(8, 8, 8, 8);

    auto *text = new QPlainTextEdit(dlg);
    text->setReadOnly(true);
    text->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont mono = text->font();
    mono.setFamily(QStringLiteral("Consolas"));
    mono.setStyleHint(QFont::Monospace);
    text->setFont(mono);
    layout->addWidget(text, 1);

    auto *btnRow = new QHBoxLayout();
    auto *refreshBtn = new QPushButton(tr("Refresh"), dlg);
    auto *copyBtn = new QPushButton(tr("Copy all"), dlg);
    auto *clearBtn = new QPushButton(tr("Clear buffer"), dlg);
    auto *onlyGoalCheck = new QCheckBox(tr("Only Goal"), dlg);
    auto *closeBtn = new QPushButton(tr("Close"), dlg);
    btnRow->addWidget(refreshBtn);
    btnRow->addWidget(copyBtn);
    btnRow->addWidget(clearBtn);
    btnRow->addWidget(onlyGoalCheck);
    btnRow->addStretch();
    btnRow->addWidget(closeBtn);
    layout->addLayout(btnRow);

    m_debugDialog = dlg;
    m_debugDialogText = text;
    m_debugDialogOnlyGoal = onlyGoalCheck;

    QPointer<AcpSessionView> self(this);
    connect(refreshBtn, &QPushButton::clicked, dlg, [self]() {
        if (!self || !self->m_debugDialogText) return;
        if (self->m_debugDialogOnlyGoal && self->m_debugDialogOnlyGoal->isChecked()) {
            self->m_debugDialogText->setPlainText(self->goalDebugLog().join(QLatin1Char('\n')));
        } else if (self->m_connection) {
            QStringList combined = self->m_connection->debugLog();
            combined.append(self->goalDebugLog());
            self->m_debugDialogText->setPlainText(combined.join(QLatin1Char('\n')));
        } else {
            self->m_debugDialogText->setPlainText(self->goalDebugLog().join(QLatin1Char('\n')));
        }
        self->m_debugDialogText->verticalScrollBar()->setValue(
            self->m_debugDialogText->verticalScrollBar()->maximum());
    });
    connect(onlyGoalCheck, &QCheckBox::toggled, dlg, [self, refreshBtn]() {
        if (refreshBtn) refreshBtn->click();
    });
    connect(copyBtn, &QPushButton::clicked, dlg, [text]() {
        QGuiApplication::clipboard()->setText(text->toPlainText());
    });
    connect(clearBtn, &QPushButton::clicked, dlg, [self, text]() {
        if (self && self->m_connection) {
            self->m_connection->clearDebugLog();
        }
        text->clear();
    });
    connect(closeBtn, &QPushButton::clicked, dlg, &QDialog::close);
    connect(dlg, &QDialog::destroyed, this, [self]() {
        if (self) {
            self->m_debugDialogText = nullptr;
            self->m_debugDialogOnlyGoal = nullptr;
        }
    });

    // Live updates — append each new line as it arrives so the dialog
    // streams events instead of needing a manual Refresh. Each AI tab owns
    // its own AcpConnection, so this scoping naturally keeps tab A's events
    // out of tab B's debug window.
    auto appendLive = [self](const QString &line) {
        if (!self || !self->m_debugDialogText) return;
        const bool onlyGoal = self->m_debugDialogOnlyGoal
                              && self->m_debugDialogOnlyGoal->isChecked();
        if (onlyGoal) return; // goal-only filter; ignore connection-level entries
        QScrollBar *bar = self->m_debugDialogText->verticalScrollBar();
        const bool wasAtBottom = bar ? (bar->value() == bar->maximum()) : true;
        self->m_debugDialogText->appendPlainText(line);
        if (wasAtBottom && bar) bar->setValue(bar->maximum());
    };
    if (m_connection) {
        connect(m_connection, &AcpConnection::debugLogAppended, dlg, appendLive);
    }
    if (ownerDock) {
        connect(ownerDock, &AiAgentDock::goalDebugLogAppended, dlg,
                [self](const QString &line) {
            if (!self || !self->m_debugDialogText) return;
            QScrollBar *bar = self->m_debugDialogText->verticalScrollBar();
            const bool wasAtBottom = bar ? (bar->value() == bar->maximum()) : true;
            self->m_debugDialogText->appendPlainText(line);
            if (wasAtBottom && bar) bar->setValue(bar->maximum());
        });
        // Close the debug dialog when the owning dock is destroyed (user
        // closes the AI tab). The dialog is parented to the main window,
        // not the dock, so it would otherwise survive as an orphan.
        connect(ownerDock, &QObject::destroyed, dlg, &QDialog::close);
    }

    refresh();
    dlg->show();
}

void AcpSessionView::scrollToBottomDeferred()
{
    if (!m_scroll) return;
    auto *vbar = m_scroll->verticalScrollBar();
    if (!vbar) return;
    if (!m_stickToBottom) {
        updateJumpButtonVisibility();
        return;
    }
    QPointer<AcpSessionView> guard(this);
    QTimer::singleShot(0, this, [guard]() {
        if (!guard) return;
        auto *vb = guard->m_scroll ? guard->m_scroll->verticalScrollBar() : nullptr;
        if (!vb) return;
        guard->m_programmaticScroll = true;
        vb->setValue(vb->maximum());
        guard->m_programmaticScroll = false;
    });
    updateJumpButtonVisibility();
}

void AcpSessionView::updateJumpButtonVisibility()
{
    if (!m_scroll || !m_jumpToBottom) return;
    // The button mirrors the inverse of the stick flag: visible exactly when
    // the user has scrolled away from the bottom, hidden when they're parked
    // there (or auto-scrolling alongside the stream).
    if (!m_stickToBottom) {
        positionJumpButton();
        m_jumpToBottom->show();
        m_jumpToBottom->raise();
    } else {
        m_jumpToBottom->hide();
    }
}

void AcpSessionView::positionJumpButton()
{
    if (!m_jumpToBottom || !m_scroll) return;
    QWidget *vp = m_scroll->viewport();
    if (!vp) return;
    const QSize vpSize = vp->size();
    const QSize btnSize = m_jumpToBottom->sizeHint();
    constexpr int kMargin = 8;
    const int x = qMax(0, vpSize.width() - btnSize.width() - kMargin);
    const int y = qMax(0, vpSize.height() - btnSize.height() - kMargin);
    m_jumpToBottom->move(x, y);
    m_jumpToBottom->resize(btnSize);
}

bool AcpSessionView::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Resize
        && (watched == m_transcriptHost
            || (m_scroll && watched == m_scroll->viewport()))) {
        syncTranscriptHostWidth();
        positionJumpButton();
    }
    if (event->type() == QEvent::Resize
        && (watched == m_input || (m_input && watched == m_input->viewport()))) {
        positionImproveButton();
    }
    if (event->type() == QEvent::FocusIn && watched == m_input) {
        // Only a deliberate focus (mouse click / Tab navigation) counts as the
        // user engaging this AI dock. Focus that merely *returns* to the input
        // after a popup/menu closes (PopupFocusReason / ActiveWindowFocusReason /
        // OtherFocusReason) must NOT trigger the workspace sync — otherwise it
        // silently yanks the active workspace back to this dock's cwd, overriding
        // a workspace the user just opened (e.g. from the recent-workspaces menu).
        const Qt::FocusReason reason = static_cast<QFocusEvent *>(event)->reason();
        if (reason == Qt::MouseFocusReason || reason == Qt::TabFocusReason
            || reason == Qt::BacktabFocusReason) {
            emit inputFocused();
        }
    }
    return QWidget::eventFilter(watched, event);
}

void AcpSessionView::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    switch (event->type()) {
    case QEvent::PaletteChange:
    case QEvent::StyleChange:
    case QEvent::ApplicationPaletteChange:
        rebuildAttachIcon();
        break;
    default:
        break;
    }
}

void AcpSessionView::rebuildAttachIcon()
{
    if (!m_attachBtn) return;
    m_attachBtn->setIcon(tintedIcon(QStringLiteral(":/icons/paperclip.svg"),
                                    palette().color(QPalette::WindowText)));
}

QFont AcpSessionView::chatFont() const
{
    // Shared with the file Markdown preview so both render in the same typeface.
    return chatFontFromSettings(appSettings());
}

void AcpSessionView::applyChatFont()
{
    if (!appSettings()) return;

    const QFont f = chatFont();
    // The transcript host carries no stylesheet, so its setFont() is a cheap,
    // harmless default for any unstyled descendants. But the message bubbles
    // (AcpMessageWidget) AND their inner QTextBrowsers are stylesheet'd, and Qt
    // does NOT propagate an inherited font into a styled widget — so we must
    // push the font into each bubble explicitly via setChatFont().
    if (m_transcriptHost) m_transcriptHost->setFont(f);
    if (m_input) m_input->setFont(f);

    for (AcpMessageWidget *w : m_messageWidgets) {
        if (w) w->setChatFont(f);
    }
    if (m_activeThought) m_activeThought->setChatFont(f);
    for (AcpToolCallCard *c : m_toolCallCards) {
        if (c) c->setChatFont(f);
    }
    for (QLabel *lab : m_truncationPlaceholders) {
        if (lab) lab->setFont(f);
    }
}

bool AcpSessionView::inputKeyEventIsSubmit(QKeyEvent *ke) const
{
    return (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter)
           && !(ke->modifiers() & Qt::ShiftModifier);
}

void AcpSessionView::showCommandPopup()
{
    if (!m_commandPopup || !m_model) return;
    m_commandPopup->clear();

    if (m_completionTrigger == QLatin1Char('$')) {
        const QStringList skills = cachedSkillNames();
        if (skills.isEmpty()) { hideCommandPopup(); return; }
        for (const QString &skill : skills) {
            appendCompletionItem(QStringLiteral("$") + skill, skill);
        }
    } else {
        const auto &cmds = m_model->availableCommands();
        if (cmds.isEmpty()) { hideCommandPopup(); return; }
        for (const auto &cmd : cmds) {
            appendCommandItem(cmd);
        }
    }

    m_commandPopup->setCurrentRow(0);
    m_commandPopup->show();
    resizeCommandPopup();
}

void AcpSessionView::hideCommandPopup()
{
    if (m_commandPopup) m_commandPopup->hide();
}

void AcpSessionView::resizeCommandPopup()
{
    if (!m_commandPopup || !m_commandPopup->isVisible() || !m_input) return;
    int visibleCount = 0;
    for (int i = 0; i < m_commandPopup->count(); ++i) {
        if (!m_commandPopup->item(i)->isHidden()) ++visibleCount;
    }
    if (visibleCount == 0) { hideCommandPopup(); return; }

    const int rowH = m_commandPopup->sizeHintForRow(0);
    const int rows = qMin(visibleCount, 8);
    const int popupH = rowH * rows + 4;
    const QPoint inputTopLeft = m_input->mapToGlobal(QPoint(0, 0));
    m_commandPopup->setGeometry(inputTopLeft.x(), inputTopLeft.y() - popupH,
                                m_input->width(), popupH);
}

void AcpSessionView::filterCommandPopup()
{
    if (!m_input || !m_model) { hideCommandPopup(); return; }
    const QString text = m_input->toPlainText();

    if (text.contains(QLatin1Char('\n'))
        || !(text.startsWith(QLatin1Char('/')) || text.startsWith(QLatin1Char('$')))) {
        hideCommandPopup();
        return;
    }

    m_completionTrigger = text.front();
    const QString prefix = text.mid(1).toLower();

    if (!m_commandPopup->isVisible()) {
        showCommandPopup();
    }

    m_commandPopup->clear();
    if (m_completionTrigger == QLatin1Char('$')) {
        const QStringList skills = cachedSkillNames();
        if (skills.isEmpty()) { hideCommandPopup(); return; }
        for (const QString &skill : skills) {
            if (prefix.isEmpty() || skill.toLower().startsWith(prefix))
                appendCompletionItem(QStringLiteral("$") + skill, skill);
        }
        for (const QString &skill : skills) {
            const QString lower = skill.toLower();
            if (!prefix.isEmpty() && !lower.startsWith(prefix) && lower.contains(prefix))
                appendCompletionItem(QStringLiteral("$") + skill, skill);
        }
    } else {
        const auto &cmds = m_model->availableCommands();
        if (cmds.isEmpty()) { hideCommandPopup(); return; }

        // Rank: names that START WITH the typed prefix come first, names that only
        // CONTAIN it as a substring come after. Original order is preserved within
        // each tier. Empty prefix puts everything in the starts-with tier (no-op
        // reorder), matching the unfiltered popup.
        for (const auto &cmd : cmds) {
            if (prefix.isEmpty() || cmd.name.toLower().startsWith(prefix))
                appendCommandItem(cmd);
        }
        for (const auto &cmd : cmds) {
            const QString lower = cmd.name.toLower();
            if (!prefix.isEmpty() && !lower.startsWith(prefix) && lower.contains(prefix))
                appendCommandItem(cmd);
        }
    }

    if (m_commandPopup->count() == 0) {
        hideCommandPopup();
    } else {
        m_commandPopup->setCurrentRow(0);
        resizeCommandPopup();
    }
}

void AcpSessionView::appendCommandItem(const AcpProtocol::AcpCommandInfo &cmd)
{
    if (!m_commandPopup) return;
    QString label = QStringLiteral("/") + cmd.name;
    if (!cmd.description.isEmpty())
        label += QStringLiteral("  ") + cmd.description;
    appendCompletionItem(label, cmd.name);
}

void AcpSessionView::appendCompletionItem(const QString &label, const QString &value)
{
    if (!m_commandPopup) return;
    auto *item = new QListWidgetItem(label, m_commandPopup);
    item->setData(Qt::UserRole, value);
}

void AcpSessionView::acceptCommandCompletion()
{
    if (!m_commandPopup || !m_commandPopup->isVisible()) return;
    auto *item = m_commandPopup->currentItem();
    if (!item || item->isHidden()) { hideCommandPopup(); return; }

    const QString name = item->data(Qt::UserRole).toString();
    const QChar trigger = m_completionTrigger == QLatin1Char('$')
        ? QLatin1Char('$')
        : QLatin1Char('/');
    m_input->setPlainText(QString(trigger) + name + QStringLiteral(" "));
    QTextCursor cur = m_input->textCursor();
    cur.movePosition(QTextCursor::End);
    m_input->setTextCursor(cur);
    hideCommandPopup();
}

QString AcpSessionView::owningDockWorkingDirectory() const
{
    for (QWidget *w = parentWidget(); w; w = w->parentWidget()) {
        if (auto *dock = qobject_cast<AiAgentDock *>(w)) {
            return dock->workingDirectory();
        }
    }
    return QString();
}

QString AcpSessionView::resolveSkillsDirectory(const QString &workingDirectory) const
{
    if (workingDirectory.isEmpty())
        return {};

    QDir dir(workingDirectory);
    QString previous;
    while (true) {
        const QString candidate = dir.filePath(QStringLiteral(".agents/skills"));
        const QFileInfo info(candidate);
        if (info.exists() && info.isDir())
            return info.absoluteFilePath();

        const QString current = dir.absolutePath();
        if (current == previous || !dir.cdUp())
            break;
        previous = current;
    }
    return {};
}

QStringList AcpSessionView::cachedSkillNames()
{
    const QString cwd = owningDockWorkingDirectory();
    const QString skillsDir = resolveSkillsDirectory(cwd);
    const QFileInfo skillsInfo(skillsDir);
    const QDateTime mtime = skillsInfo.exists() ? skillsInfo.lastModified() : QDateTime();

    if (cwd != m_skillCacheCwd || skillsDir != m_skillCacheDir || mtime != m_skillCacheMtime) {
        m_skillCacheCwd = cwd;
        m_skillCacheDir = skillsDir;
        m_skillCacheMtime = mtime;
        m_skillCacheNames = scanSkillNames(skillsDir);
    }
    return m_skillCacheNames;
}

QStringList AcpSessionView::scanSkillNames(const QString &skillsDirectory) const
{
    if (skillsDirectory.isEmpty())
        return {};

    QDir skillsDir(skillsDirectory);
    if (!skillsDir.exists())
        return {};

    const QFileInfoList entries = skillsDir.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable,
        QDir::Name | QDir::IgnoreCase);

    QStringList names;
    names.reserve(qMin(entries.size(), kMaxSkillCompletions));
    for (const QFileInfo &entry : entries) {
        const QString name = entry.fileName();
        if (name.isEmpty() || name.startsWith(QLatin1Char('.')))
            continue;
        names.append(name);
        if (names.size() >= kMaxSkillCompletions)
            break;
    }
    return names;
}

void AcpSessionView::positionImproveButton()
{
    if (!m_improveBtn || !m_input) return;
    constexpr int kPad = 4;
    const QSize btnSz = m_improveBtn->sizeHint();
    int rightOffset = kPad;
    if (auto *sb = m_input->verticalScrollBar(); sb && sb->isVisible())
        rightOffset += sb->width();
    const int x = m_input->width() - btnSz.width() - rightOffset;
    const int y = m_input->height() - btnSz.height() - kPad;
    m_improveBtn->move(x, y);
    m_improveBtn->raise();
}

void AcpSessionView::updateImproveButtonState()
{
    if (!m_improveBtn || !m_input) return;

    const bool agentBusy = (m_model && m_model->isProcessing()) || m_goalRunning;
    if (agentBusy) {
        m_improveBtn->hide();
        return;
    }

    const QString text = m_input->toPlainText().trimmed();

    if (m_promptImprover && m_promptImprover->state() == ai::PromptImprover::State::Streaming) {
        m_improveBtn->show();
        positionImproveButton();
        m_improveBtn->setEnabled(true);
        return;
    }

    // Hide when bare slash command or skill reference (no arguments).
    static const QRegularExpression bareCmd(QStringLiteral("^\\s*[/\\$]\\w+\\s*$"));
    if (!text.isEmpty() && bareCmd.match(text).hasMatch()) {
        m_improveBtn->hide();
        return;
    }

    m_improveBtn->show();
    positionImproveButton();
    m_improveBtn->setEnabled(true);
    m_improveBtn->setToolTip(text.isEmpty()
        ? tr("Generate prompt with Goal Agent (Ctrl+I)")
        : tr("Improve prompt with AI (Ctrl+I)"));
    m_improveBtn->setAccessibleDescription(text.isEmpty()
        ? tr("Generate a prompt from Goal Agent criteria (Ctrl+I)")
        : tr("Rewrite the current prompt to be clearer (Ctrl+I)"));
}

void AcpSessionView::onImproveClicked()
{
    if (!m_input) return;

    // If already streaming, cancel.
    if (m_promptImprover && m_promptImprover->state() == ai::PromptImprover::State::Streaming) {
        m_promptImprover->cancel();
        m_input->setReadOnly(false);
        m_improveBtn->setText(QStringLiteral("✨"));
        m_improveBtn->setToolTip(tr("Improve prompt with AI (Ctrl+I)"));
        updateImproveButtonState();
        return;
    }

    if ((m_model && m_model->isProcessing()) || m_goalRunning)
        return;

    const QString draft = m_input->toPlainText().trimmed();
    if (draft.isEmpty()) {
        emit generatePromptWithGoalRequested();
        return;
    }

    if (!m_promptImprover) return;

    QString whyNot;
    if (!m_promptImprover->canImprove(&whyNot)) {
        setBanner(whyNot, BannerKind::Warning);
        QTimer::singleShot(5000, this, &AcpSessionView::clearBanner);
        return;
    }

    const auto images = m_attachmentList->peekAll();

    // Use the agent's captured spawn cwd, not the live active workspace.
    const QString workingDir = owningDockWorkingDirectory();

    const QList<AcpProtocol::AcpCommandInfo> commands =
        m_model ? m_model->availableCommands() : QList<AcpProtocol::AcpCommandInfo>{};

    // Build a condensed chat history from recent messages (budget: ~4000 chars).
    QString chatHistory;
    if (m_model) {
        constexpr int kHistoryBudget = 4000;
        const auto &msgs = m_model->messages();
        int totalChars = 0;
        // Walk backwards to get the most recent messages first.
        for (int i = msgs.size() - 1; i >= 0; --i) {
            const auto &msg = msgs[i];
            if (msg.role != QLatin1String("user") && msg.role != QLatin1String("assistant"))
                continue;
            QString text;
            for (const auto &block : msg.content) {
                if (block.kind == AcpProtocol::AcpContentBlock::Kind::Text)
                    text += block.text;
            }
            text = text.trimmed();
            if (text.isEmpty()) continue;
            // Truncate individual messages that are too long.
            if (text.size() > 800)
                text = text.left(800) + QStringLiteral("...");
            const QString entry = QStringLiteral("[%1]: %2\n").arg(msg.role, text);
            if (totalChars + entry.size() > kHistoryBudget)
                break;
            chatHistory.prepend(entry);
            totalChars += entry.size();
        }
        chatHistory = chatHistory.trimmed();
    }

    m_originalDraftBeforeImprove = m_input->toPlainText();
    m_input->setReadOnly(true);
    m_improveBtn->setText(QStringLiteral("■"));
    m_improveBtn->setToolTip(tr("Stop improving (Esc)"));
    m_improveBtn->setEnabled(true);

    m_promptImprover->trigger(draft, workingDir, commands, chatHistory, images);
}

void AcpSessionView::onImproveFinished(const QString &improvedText)
{
    if (!m_input) return;

    // Replace via QTextCursor so Ctrl+Z restores the original.
    QTextCursor cursor(m_input->document());
    cursor.beginEditBlock();
    cursor.movePosition(QTextCursor::Start);
    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    cursor.insertText(improvedText);
    cursor.endEditBlock();

    m_input->setReadOnly(false);
    m_improveBtn->setText(QStringLiteral("✨"));
    m_improveBtn->setToolTip(tr("Improve prompt with AI (Ctrl+I)"));
    m_input->setFocus();
    updateImproveButtonState();
}

void AcpSessionView::onImproveError(const QString &message)
{
    if (!m_input) return;

    m_input->setReadOnly(false);
    m_improveBtn->setText(QStringLiteral("✨"));
    m_improveBtn->setToolTip(tr("Improve prompt with AI (Ctrl+I)"));
    updateImproveButtonState();

    setBanner(message, BannerKind::Warning);
    QTimer::singleShot(4000, this, &AcpSessionView::clearBanner);
}

