/*
 * This file is part of NotepadAI.
 * Copyright 2026 NotepadAI contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "EmbeddedWindowWorker.h"

#ifdef Q_OS_WIN
#include "EmbeddedWindowWin32.h"

#include <Windows.h>

#include <atomic>
#include <system_error>
#include <thread>

struct EmbeddedWindowWorker::State {
    HWND target = nullptr;
    quintptr token = 0;
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr); // auto-reset
    HANDLE idle = CreateEventW(nullptr, TRUE, TRUE, nullptr);   // manual, signalled
    HANDLE started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<bool> attached{false};
    std::atomic<bool> stop{false};
    std::atomic<bool> paused{false};
    // x | y << 16 | width << 32 | height << 48, each a 16-bit field
    std::atomic<quint64> geometry{0};
    // generation << 1 | visible; published after `geometry`
    std::atomic<quint64> geometryStamp{0};
    std::atomic<quint64> activateSeq{0};
    std::atomic<quint64> foregroundSeq{0};
    std::atomic<quintptr> foregroundWindow{0};
    std::atomic<int> enabled{-1}; // -1: never requested

    ~State()
    {
        CloseHandle(wake);
        CloseHandle(idle);
        CloseHandle(started);
    }

    bool focusInsideTarget() const
    {
        HWND focus = GetFocus();
        return focus && (focus == target || IsChild(target, focus));
    }

    // Activate `window` (ours) and give it the keyboard if the target has it.
    void bringForward(HWND window) const
    {
        if (!IsWindow(window))
            return;
        if (GetForegroundWindow() != window)
            SetForegroundWindow(window);
        if (focusInsideTarget())
            SetFocus(window);
    }

    bool ownsTarget() const
    {
        return EmbeddedWindowWin32::ownsWindow(reinterpret_cast<quintptr>(target), token);
    }

    static void run(const std::shared_ptr<State> &self);
};

void EmbeddedWindowWorker::State::run(const std::shared_ptr<State> &self)
{
    State &s = *self;
    // SetFocus only works for windows on the calling thread's input queue:
    // join the one our top-level already shares with the target.
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    const DWORD foreignThread = GetWindowThreadProcessId(s.target, nullptr);
    s.attached = foreignThread && AttachThreadInput(GetCurrentThreadId(), foreignThread, TRUE);
    SetEvent(s.started);
    if (!s.attached)
        return;
    quint64 appliedStamp = 0;
    quint64 appliedRect = ~quint64(0);
    quint64 appliedActivate = 0;
    quint64 appliedForeground = 0;
    int appliedEnabled = -1;

    while (WaitForSingleObject(s.wake, INFINITE) == WAIT_OBJECT_0 && !s.stop) {
        ResetEvent(s.idle);
        if (s.paused || !s.ownsTarget()) {
            SetEvent(s.idle);
            continue;
        }

        const int enabled = s.enabled.load();
        if (enabled != appliedEnabled && enabled >= 0) {
            appliedEnabled = enabled;
            EnableWindow(s.target, enabled ? TRUE : FALSE);
        }

        const quint64 stamp = s.geometryStamp.load(std::memory_order_acquire);
        if (stamp != appliedStamp && !s.paused) {
            appliedStamp = stamp;
            const quint64 g = s.geometry.load(std::memory_order_relaxed);
            const auto field = [g](int shift) { return static_cast<int>(static_cast<qint16>((g >> shift) & 0xffff)); };
            const bool visible = stamp & 1;
            // Same rect (a tab switch): only show/hide, so the app gets no
            // move/size messages at all.
            const UINT keep = g == appliedRect ? SWP_NOMOVE | SWP_NOSIZE : 0;
            appliedRect = g;
            // NOZORDER: an owned window always stays above its owner; keep its
            // place among our other owned windows (dialogs, menus, docks).
            const UINT flags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | keep;
            if (visible) {
                SetWindowPos(s.target, nullptr, field(0), field(16), field(32), field(48),
                             flags | SWP_SHOWWINDOW);
            } else {
                if (!keep)
                    SetWindowPos(s.target, nullptr, field(0), field(16), field(32), field(48), flags);
                // Hiding the foreground window from another thread leaves it
                // the (invisible) foreground window, keyboard included: hand
                // activation back to our window so the keyboard follows the
                // tab switch.
                const bool wasForeground = GetForegroundWindow() == s.target;
                ShowWindow(s.target, SW_HIDE);
                if (wasForeground)
                    s.bringForward(GetWindow(s.target, GW_OWNER));
            }
        }

        const quint64 foregroundSeq = s.foregroundSeq.load();
        if (foregroundSeq != appliedForeground && !s.paused) {
            appliedForeground = foregroundSeq;
            s.bringForward(reinterpret_cast<HWND>(s.foregroundWindow.load()));
        }

        const quint64 activate = s.activateSeq.load();
        if (activate != appliedActivate && !s.paused) {
            appliedActivate = activate;
            // Only take the foreground from the window that hosts us: a late
            // request must never pull NotepadAI over another app, nor the
            // embed over one of our dialogs.
            if (GetForegroundWindow() == GetWindow(s.target, GW_OWNER)
                && (GetWindowLongPtrW(s.target, GWL_STYLE) & WS_VISIBLE) && IsWindowEnabled(s.target))
                SetForegroundWindow(s.target);
        }
        SetEvent(s.idle);
    }
    AttachThreadInput(GetCurrentThreadId(), foreignThread, FALSE);
}

EmbeddedWindowWorker::EmbeddedWindowWorker(quintptr targetHandle, quintptr token)
    : m_state(std::make_shared<State>())
{
    m_state->target = reinterpret_cast<HWND>(targetHandle);
    m_state->token = token;
}

EmbeddedWindowWorker::~EmbeddedWindowWorker()
{
    m_state->stop = true;
    SetEvent(m_state->wake);
}

bool EmbeddedWindowWorker::start()
{
    if (!m_state->wake || !m_state->idle || !m_state->started)
        return false;
    try {
        std::thread(&State::run, m_state).detach();
    } catch (const std::system_error &) {
        return false;
    }
    // Attaching is a quick kernel call; the bound only guards a pathological
    // scheduler stall.
    return WaitForSingleObject(m_state->started, 2000) == WAIT_OBJECT_0 && m_state->attached;
}

void EmbeddedWindowWorker::requestGeometry(int x, int y, int width, int height, bool visible)
{
    const auto field = [](int v, int shift) {
        return static_cast<quint64>(static_cast<quint16>(static_cast<qint16>(qBound(-32768, v, 32767)))) << shift;
    };
    m_state->geometry.store(field(x, 0) | field(y, 16) | field(width, 32) | field(height, 48),
                            std::memory_order_relaxed);
    quint64 current = m_state->geometryStamp.load();
    quint64 next = 0;
    do {
        next = (((current >> 1) + 1) << 1) | (visible ? 1 : 0);
    } while (!m_state->geometryStamp.compare_exchange_weak(current, next, std::memory_order_release));
    SetEvent(m_state->wake);
}

void EmbeddedWindowWorker::requestActivate()
{
    m_state->activateSeq.fetch_add(1);
    SetEvent(m_state->wake);
}

void EmbeddedWindowWorker::requestEnabled(bool enabled)
{
    m_state->enabled = enabled ? 1 : 0;
    SetEvent(m_state->wake);
}

void EmbeddedWindowWorker::requestForeground(quintptr window)
{
    m_state->foregroundWindow = window;
    m_state->foregroundSeq.fetch_add(1);
    SetEvent(m_state->wake);
}

bool EmbeddedWindowWorker::pause(int waitMs)
{
    m_state->paused = true;
    return WaitForSingleObject(m_state->idle, static_cast<DWORD>(qMax(0, waitMs))) == WAIT_OBJECT_0;
}

void EmbeddedWindowWorker::resume()
{
    m_state->paused = false;
    SetEvent(m_state->wake);
}

#else

struct EmbeddedWindowWorker::State {
};

EmbeddedWindowWorker::EmbeddedWindowWorker(quintptr, quintptr)
    : m_state(std::make_shared<State>())
{
}

EmbeddedWindowWorker::~EmbeddedWindowWorker() = default;

bool EmbeddedWindowWorker::start()
{
    return false;
}

void EmbeddedWindowWorker::requestGeometry(int, int, int, int, bool) {}
void EmbeddedWindowWorker::requestActivate() {}
void EmbeddedWindowWorker::requestForeground(quintptr) {}
void EmbeddedWindowWorker::requestEnabled(bool) {}

bool EmbeddedWindowWorker::pause(int)
{
    return true;
}

void EmbeddedWindowWorker::resume() {}

#endif
