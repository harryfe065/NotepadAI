/*
 * This file is part of NotepadAI.
 * Copyright 2026 NotepadAI contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <memory>

class EmbeddedWindowWorker;
class NativeWindowHostInput;

// Placeholder widget (one per embed tab) that a foreign window is kept over.
//
// The foreign window stays a top-level, owned by our top-level (see
// EmbeddedWindowWin32), so activation, keyboard, IME and raw input are native:
// a click on it activates it, a click on NotepadAI activates NotepadAI. The host
// keeps it exactly over its client area (any move of any of our windows
// re-checks it), shows/hides it with the tab, disables it while a modal dialog
// blocks our window, and activates it when the tab is chosen. Qt focus sits on
// the host while the embed is active, so coming back to NotepadAI (Alt-Tab, a
// click on a toolbar) hands the keyboard back to it. All calls that need the
// foreign thread run on an EmbeddedWindowWorker; the GUI thread never waits.
class NativeWindowHost final : public QWidget
{
public:
    using ChildLostCallback = std::function<void()>;
    using FocusRequestCallback = std::function<void()>;

    explicit NativeWindowHost(QWidget *parent = nullptr);
    ~NativeWindowHost() override;

    // Forces the host HWND to exist, then starts tracking the (already
    // prepared) foreign window. The manager owns the identity token.
    bool attach(quintptr targetHandle, quintptr token);
    void detach();
    // Bracket a GUI-thread release of the foreign window: no worker call may
    // move or activate it mid-release.
    void pauseForeignCalls();
    void resumeForeignCalls();
    // Activate the embed (asynchronously) if the host is visible and still
    // Qt's focus once no mouse button or popup is in the way.
    void activateForeign();
    // Re-check the embed against the host; moves/resizes/shows it only if it
    // differs. False if the window is gone or a Win32 read failed.
    bool sync();
    quintptr nativeHandle() const;
    void setChildLostCallback(ChildLostCallback callback);
    void setFocusRequestCallback(FocusRequestCallback callback);

protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    friend class NativeWindowHostInput;

    void responsiveChanged(bool responding);
    void foregroundChanged(quintptr foreground);
    bool embedKeepsKeyboard() const;
    void clickedWhileForeignActive(quintptr topLevel);
    void stopWaitingForPress();
    void decideAfterClick();
    void topLevelActivatedWithForeignFocus(quintptr topLevel);
    void stopTracking();
    void reportChildLost();

    quintptr m_targetHandle = 0;
    quintptr m_token = 0;
    quintptr m_topLevelHandle = 0;
    QPointer<QWidget> m_filteredWindow;
    std::unique_ptr<EmbeddedWindowWorker> m_worker;
    QTimer m_activateRetry;
    QTimer m_clickDecision;
    QElapsedTimer m_clickElapsed;
    quintptr m_clickTopLevel = 0;
    bool m_waitingForPress = false;
    ChildLostCallback m_childLost;
    FocusRequestCallback m_focusRequest;
    bool m_lossReported = false;
    // The embed's thread is hung and the embed is unowned until it answers.
    bool m_unresponsive = false;
    quint64 m_probeGeneration = 0;
};
