/*
 * This file is part of NotepadAI.
 * Copyright 2026 NotepadAI contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "NativeWindowHost.h"

#include "EmbeddedWindowWin32.h"
#include "EmbeddedWindowWorker.h"

#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QEvent>
#include <QHash>
#include <QList>

#include <utility>

namespace {
// How long a release waits for an in-flight worker call (only a hung foreign
// app gets near it; the release then goes ahead, token-gated as before).
constexpr int kPauseWaitMs = 250;
// Re-check interval while a mouse button or popup delays an activation.
constexpr int kActivateRetryMs = 15;
// How long to wait for Qt to deliver a press before deciding anyway (a
// title-bar press never reaches Qt).
constexpr int kClickDecisionTimeoutMs = 100;
} // namespace

// App-wide plumbing for the hosts that currently track an embed: a native event
// filter (any move of one of our windows re-checks the embeds above it), the
// caption guard on each hosting top-level, and one foreground WinEvent hook.
// Exists only while a host is attached, so it costs nothing otherwise. GUI
// thread only.
class NativeWindowHostInput final : public QAbstractNativeEventFilter
{
public:
    static void add(NativeWindowHost *host)
    {
        NativeWindowHostInput *&self = instance();
        if (!self) {
            self = new NativeWindowHostInput;
            qApp->installNativeEventFilter(self);
            self->m_foregroundHook = EmbeddedWindowWin32::installForegroundHook(&NativeWindowHostInput::onForeground);
        }
        if (self->m_hosts.contains(host))
            return;
        self->m_hosts.append(host);
        if (const quintptr top = host->m_topLevelHandle) {
            TopLevel &entry = self->m_topLevels[top];
            if (entry.hosts++ == 0)
                EmbeddedWindowWin32::installCaptionGuard(top, &NativeWindowHostInput::onActivationWithForeignFocus);
        }
    }

    static void remove(NativeWindowHost *host)
    {
        NativeWindowHostInput *&self = instance();
        if (!self || !self->m_hosts.removeOne(host))
            return;
        if (const quintptr top = host->m_topLevelHandle) {
            auto entry = self->m_topLevels.find(top);
            if (entry != self->m_topLevels.end() && --entry->hosts <= 0) {
                if (entry->captionForcedActive)
                    EmbeddedWindowWin32::paintCaptionActive(top, false);
                self->m_topLevels.erase(entry);
                EmbeddedWindowWin32::removeCaptionGuard(top);
            }
        }
        if (self->m_hosts.isEmpty()) {
            EmbeddedWindowWin32::removeForegroundHook(self->m_foregroundHook);
            qApp->removeNativeEventFilter(self);
            delete self;
            self = nullptr;
        }
    }

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override
    {
        Q_UNUSED(eventType)
        if (const quintptr clicked = EmbeddedWindowWin32::mouseActivateWindow(message)) {
            // A click on one of our windows while an embed is the active
            // window: activating ours right here would make the GUI thread
            // wait for the embed's thread to process its deactivation (the
            // owner relationship shares the two input queues). Let Qt handle
            // the click without activation; the host then decides.
            const quintptr foreground = EmbeddedWindowWin32::foregroundWindow();
            for (NativeWindowHost *host : std::as_const(m_hosts)) {
                if (host->m_worker && foreground
                    && EmbeddedWindowWin32::rootWindow(foreground) == host->m_targetHandle) {
                    host->clickedWhileForeignActive(EmbeddedWindowWin32::rootWindow(clicked));
                    if (result)
                        *result = EmbeddedWindowWin32::mouseActivateNoActivate();
                    return true;
                }
            }
            return false;
        }
        // Moving the top-level, a splitter, a dock, the DPI... any of our
        // windows changing position may move a host on screen without the host
        // itself getting a Qt event. The check is pure reads.
        const quintptr moved = EmbeddedWindowWin32::positionChangedWindow(message);
        if (!moved)
            return false;
        const quintptr top = EmbeddedWindowWin32::rootWindow(moved);
        const QList<NativeWindowHost *> hosts = m_hosts;
        for (NativeWindowHost *host : hosts) {
            if (host->m_topLevelHandle == top)
                host->sync();
        }
        return false;
    }

private:
    struct TopLevel {
        int hosts = 0;
        bool captionForcedActive = false;
    };

    static NativeWindowHostInput *&instance()
    {
        static NativeWindowHostInput *self = nullptr;
        return self;
    }

    static bool onActivationWithForeignFocus(quintptr topLevel, quintptr focus)
    {
        NativeWindowHostInput *self = instance();
        if (!self)
            return false;
        const quintptr focusRoot = EmbeddedWindowWin32::rootWindow(focus);
        for (NativeWindowHost *host : std::as_const(self->m_hosts)) {
            if (host->m_worker && host->m_targetHandle == focusRoot) {
                host->topLevelActivatedWithForeignFocus(topLevel);
                return true;
            }
        }
        return false;
    }

    static void onForeground(quintptr foreground)
    {
        NativeWindowHostInput *self = instance();
        if (!self)
            return;
        // Caption: our top-level looks active while one of its embeds is.
        for (auto it = self->m_topLevels.begin(); it != self->m_topLevels.end(); ++it) {
            const bool embedActive = EmbeddedWindowWin32::isEmbedOwnedBy(foreground, it.key());
            if (embedActive) {
                EmbeddedWindowWin32::paintCaptionActive(it.key(), true);
                it->captionForcedActive = true;
            } else if (it->captionForcedActive) {
                it->captionForcedActive = false;
                if (foreground != it.key())
                    EmbeddedWindowWin32::paintCaptionActive(it.key(), false);
            }
        }
        const QList<NativeWindowHost *> hosts = self->m_hosts;
        for (NativeWindowHost *host : hosts)
            host->foregroundChanged(foreground);
    }

    QList<NativeWindowHost *> m_hosts;
    QHash<quintptr, TopLevel> m_topLevels;
    quintptr m_foregroundHook = 0;
};

NativeWindowHost::NativeWindowHost(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_NativeWindow);
    setAutoFillBackground(true);
    setFocusPolicy(Qt::StrongFocus);
    m_activateRetry.setSingleShot(true);
    QObject::connect(&m_activateRetry, &QTimer::timeout, this, [this]() { activateForeign(); });
    m_clickDecision.setSingleShot(true);
    QObject::connect(&m_clickDecision, &QTimer::timeout, this, [this]() { decideAfterClick(); });
}

NativeWindowHost::~NativeWindowHost()
{
    stopTracking();
}

bool NativeWindowHost::attach(quintptr targetHandle, quintptr token)
{
    // A host is normally single-use; make accidental reuse drop the old state.
    stopTracking();
    if (!EmbeddedWindowWin32::ownsWindow(targetHandle, token) || !nativeHandle())
        return false;
    auto worker = std::make_unique<EmbeddedWindowWorker>(targetHandle, token);
    if (!worker->start()) {
        qWarning("EmbeddedWindow: could not start the worker for the embedded window");
        return false;
    }
    m_targetHandle = targetHandle;
    m_token = token;
    m_topLevelHandle = EmbeddedWindowWin32::rootWindow(nativeHandle());
    m_lossReported = false;
    m_worker = std::move(worker);
    if (!sync()) {
        stopTracking();
        return false;
    }
    m_filteredWindow = window();
    m_filteredWindow->installEventFilter(this);
    NativeWindowHostInput::add(this);
    // The user just picked it: give it the keyboard if we are in front.
    if (isVisible() && EmbeddedWindowWin32::rootWindow(EmbeddedWindowWin32::foregroundWindow()) == m_topLevelHandle) {
        setFocus(Qt::OtherFocusReason);
        activateForeign();
    }
    return true;
}

void NativeWindowHost::detach()
{
    stopTracking();
    m_lossReported = false;
}

void NativeWindowHost::pauseForeignCalls()
{
    if (m_worker && !m_worker->pause(kPauseWaitMs))
        qWarning("EmbeddedWindow: embedded app is not responding; releasing it anyway");
}

void NativeWindowHost::resumeForeignCalls()
{
    if (m_worker)
        m_worker->resume();
}

void NativeWindowHost::activateForeign()
{
    if (!m_worker || !isVisible() || QApplication::focusWidget() != this)
        return;
    // Wait for the click (or the menu it opened) that brought us here to end:
    // it may move Qt focus elsewhere, and activating the embed under an open
    // menu would close it.
    if (QGuiApplication::mouseButtons() != Qt::NoButton || QApplication::activePopupWidget()) {
        m_activateRetry.start(kActivateRetryMs);
        return;
    }
    m_worker->requestActivate();
}

quintptr NativeWindowHost::nativeHandle() const
{
#ifdef Q_OS_WIN
    // winId() deliberately forces creation; effectiveWinId() can be zero
    // before Qt has materialized a native host.
    return static_cast<quintptr>(const_cast<NativeWindowHost *>(this)->winId());
#else
    return 0;
#endif
}

void NativeWindowHost::setChildLostCallback(ChildLostCallback callback)
{
    m_childLost = std::move(callback);
}

void NativeWindowHost::setFocusRequestCallback(FocusRequestCallback callback)
{
    m_focusRequest = std::move(callback);
}

bool NativeWindowHost::event(QEvent *event)
{
    const QEvent::Type type = event->type();
    const bool result = QWidget::event(event);
    if (!m_targetHandle)
        return result;
    switch (type) {
    case QEvent::Resize:
    case QEvent::Move:
    case QEvent::Show:
    case QEvent::Hide:
    case QEvent::WinIdChange:
        // Only touches the embed when its rect or visibility actually
        // differs, so a tab switch costs the embedded app nothing.
        sync();
        break;
    case QEvent::FocusIn:
        // ADS tab activation and FocusIn can arrive in the same event-loop
        // turn; the manager's router coalesces them.
        if (m_focusRequest)
            m_focusRequest();
        else
            activateForeign();
        break;
    default:
        break;
    }
    return result;
}

bool NativeWindowHost::eventFilter(QObject *watched, QEvent *event)
{
    if (m_waitingForPress && watched->isWindowType()
        && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::NonClientAreaMouseButtonPress)) {
        stopWaitingForPress();
        m_clickDecision.start(0); // after Qt has handled this press
    }
    // Qt implements modality itself (no EnableWindow), so a foreign window
    // would stay clickable over a modal dialog: disable it while we are blocked.
    if (watched == m_filteredWindow && m_worker) {
        if (event->type() == QEvent::WindowBlocked)
            m_worker->requestEnabled(false);
        else if (event->type() == QEvent::WindowUnblocked)
            m_worker->requestEnabled(true);
    }
    return QWidget::eventFilter(watched, event);
}

bool NativeWindowHost::sync()
{
#ifdef Q_OS_WIN
    if (!m_targetHandle || !m_worker || !nativeHandle())
        return false;
    EmbeddedWindowWin32::EmbedGeometry wanted;
    switch (EmbeddedWindowWin32::checkGeometry(m_targetHandle, nativeHandle(), m_token, &wanted)) {
    case EmbeddedWindowWin32::GeometryCheck::Ok:
        return true;
    case EmbeddedWindowWin32::GeometryCheck::NeedsUpdate:
        m_worker->requestGeometry(wanted.x, wanted.y, wanted.width, wanted.height, wanted.visible);
        return true;
    case EmbeddedWindowWin32::GeometryCheck::TransientError:
        return false;
    case EmbeddedWindowWin32::GeometryCheck::WindowLost:
        reportChildLost();
        return false;
    }
    return false;
#else
    return false;
#endif
}

void NativeWindowHost::foregroundChanged(quintptr foreground)
{
    // The embed became active (clicked, or Windows restored it as our last
    // active popup): put Qt's focus on the host, so returning to NotepadAI
    // hands the keyboard back to the embed. While our window is inactive this
    // only records the focus; it does not activate anything.
    if (m_targetHandle && EmbeddedWindowWin32::rootWindow(foreground) == m_targetHandle
        && QApplication::focusWidget() != this && isVisible())
        setFocus(Qt::OtherFocusReason);
}

bool NativeWindowHost::embedKeepsKeyboard() const
{
    // window()->focusWidget() is the focus Qt recorded for our window; unlike
    // QApplication::focusWidget() it is kept while the window is inactive.
    return isVisible() && window()->focusWidget() == this && !QApplication::activePopupWidget();
}

void NativeWindowHost::clickedWhileForeignActive(quintptr topLevel)
{
    m_clickTopLevel = topLevel;
    m_clickElapsed.start();
    // Qt queues the press and delivers it later; watch for the delivery
    // (only for the few milliseconds until the decision) and decide right
    // after Qt has handled it.
    if (!m_waitingForPress) {
        m_waitingForPress = true;
        qApp->installEventFilter(this);
    }
    m_clickDecision.start(kClickDecisionTimeoutMs);
}

void NativeWindowHost::stopWaitingForPress()
{
    if (m_waitingForPress) {
        m_waitingForPress = false;
        qApp->removeEventFilter(this);
    }
}

void NativeWindowHost::decideAfterClick()
{
    // Runs right after Qt handled the press (or on timeout: a title-bar press
    // never reaches Qt). The press decided whether the click moved the focus
    // (an editor, a line edit) or opened a popup (a menu).
    stopWaitingForPress();
    if (!m_worker)
        return;
    // A toolbar button, a splitter, the title bar: the embed keeps the
    // keyboard and stays the active window. Otherwise NotepadAI takes it.
    if (!embedKeepsKeyboard())
        m_worker->requestForeground(m_clickTopLevel);
}

void NativeWindowHost::topLevelActivatedWithForeignFocus(quintptr topLevel)
{
    // Windows activated our top-level (Alt-Tab, taskbar, our own request)
    // while the keyboard is still in the embed. Exactly one outcome, never
    // both (they would bounce activation back and forth): the embed keeps the
    // keyboard if Qt's focus is on its host, else NotepadAI gets it.
    if (embedKeepsKeyboard())
        m_worker->requestActivate();
    else
        m_worker->requestForeground(topLevel);
}

void NativeWindowHost::stopTracking()
{
    NativeWindowHostInput::remove(this);
    if (m_filteredWindow) {
        m_filteredWindow->removeEventFilter(this);
        m_filteredWindow = nullptr;
    }
    m_activateRetry.stop();
    m_clickDecision.stop();
    stopWaitingForPress();
    m_worker.reset();
    m_targetHandle = 0;
    m_token = 0;
    m_topLevelHandle = 0;
}

void NativeWindowHost::reportChildLost()
{
    if (m_lossReported || !m_targetHandle)
        return;
    m_lossReported = true;
    stopTracking();
    if (m_childLost)
        m_childLost();
}
