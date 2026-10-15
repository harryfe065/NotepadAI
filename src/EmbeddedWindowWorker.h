/*
 * This file is part of NotepadAI.
 * Copyright 2026 NotepadAI contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QtGlobal>

#include <memory>

// Runs the cross-thread Win32 calls an embedded foreign window needs (move,
// resize, show/hide, activate, enable) on a dedicated thread, one per embed.
//
// The embed is owned by our top-level, and a cross-thread owner relationship
// attaches the two threads' input queues. With a shared queue, SetWindowPos
// (even SWP_ASYNCWINDOWPOS) and activation run synchronously against the
// foreign UI thread, so calling them on the GUI thread froze NotepadAI for as
// long as the embedded app was busy. This thread absorbs those waits instead.
//
// Requests are coalesced to the latest desired state (lock-free atomics + one
// auto-reset event): a drag or resize storm applies only its last rect, and an
// activation superseded before the thread ran it is skipped. The thread never
// touches Qt objects and is never joined: if the foreign app hangs, its worker
// stays blocked (holding only its own state) until the app wakes or exits.
// Header is free of <Windows.h>; handles cross as quintptr.
class EmbeddedWindowWorker final
{
public:
    EmbeddedWindowWorker(quintptr targetHandle, quintptr token);
    ~EmbeddedWindowWorker(); // stops the thread without waiting for it

    EmbeddedWindowWorker(const EmbeddedWindowWorker &) = delete;
    EmbeddedWindowWorker &operator=(const EmbeddedWindowWorker &) = delete;

    // Starts the thread and attaches it to the target's input queue (needed to
    // move the keyboard focus out of the target). False on failure.
    bool start();

    // Place the target at (x, y, width, height) in screen pixels, shown or
    // hidden. Never activates it.
    void requestGeometry(int x, int y, int width, int height, bool visible);
    // Make the target the foreground window, but only while one of OUR
    // windows is in front (never steal the foreground from another app).
    void requestActivate();
    // Make `window` (one of OUR top-levels) the foreground window and move the
    // keyboard focus to it if the target holds it. Both wait for the target's
    // thread; doing them here keeps those waits off the GUI thread.
    void requestForeground(quintptr window);
    // Enable or disable the target (disabled while a NotepadAI modal dialog is
    // up, so it cannot be activated over the dialog).
    void requestEnabled(bool enabled);

    // Stop issuing calls and wait up to waitMs for an in-flight call to end.
    // Used before the GUI thread releases the window, so the worker cannot
    // move or activate it mid-release. True if the worker is idle.
    bool pause(int waitMs);
    void resume();

private:
    struct State;
    std::shared_ptr<State> m_state;
};
