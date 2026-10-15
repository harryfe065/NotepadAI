/*
 * This file is part of NotepadAI.
 * Copyright 2026 NotepadAI contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>

#include <QList>
#include <QString>
#include <QtGlobal>

// Pure Win32 layer for the embedded-window feature. Deliberately free of Qt
// widgets and Qt Advanced Docking System types so it can be unit-reasoned in
// isolation and so neither NativeWindowHost nor EmbeddedWindowManager pull in
// <Windows.h>. Native handles cross this boundary as opaque quintptr values;
// window attributes are captured into a POD NativeWindowState of fixed-width
// integers (no WINDOWPLACEMENT in the header). On non-Windows every entry point
// is a no-op returning a safe default so the rest of the app still builds.
//
// Embedding model: the foreign window stays a top-level window. It becomes a
// borderless popup OWNED by the host's top-level (so it stays above it, hides
// with it, and is left out of the taskbar and Alt-Tab) and is kept exactly over
// the host's client area. It is not reparented as a WS_CHILD: Windows delivers
// raw input (WM_INPUT, which Blender, Unity and many games read the keyboard
// from) only to the thread that owns the foreground window, so a child window
// never gets it. As a top-level, the embed is activated natively by a click and
// gets the keyboard, IME and raw input like any window.
namespace EmbeddedWindowWin32 {

// A top-level, user-visible, foreign-process window the user may embed.
struct Candidate {
    quintptr handle = 0; // native HWND, opaque here
    QString title;
};

// Snapshot of a foreign window's pre-embed state. Everything needed to restore
// it exactly, or at minimum detach it safely, without ever touching a recycled
// handle (identity is proven by handle + processId + threadId + token).
struct NativeWindowState {
    quintptr handle = 0;
    quint32 processId = 0;
    quint32 threadId = 0;
    quintptr originalParent = 0;
    quintptr originalOwner = 0;
    qint64 originalStyle = 0;
    qint64 originalExStyle = 0;
    qint32 rectLeft = 0;
    qint32 rectTop = 0;
    qint32 rectRight = 0;
    qint32 rectBottom = 0;
    quint32 placementFlags = 0;
    quint32 placementShowCmd = 0;
    qint32 placementMinX = 0;
    qint32 placementMinY = 0;
    qint32 placementMaxX = 0;
    qint32 placementMaxY = 0;
    qint32 placementNormalLeft = 0;
    qint32 placementNormalTop = 0;
    qint32 placementNormalRight = 0;
    qint32 placementNormalBottom = 0;
    bool placementValid = false;
    bool originallyVisible = false;
};

// Where the embed should be: the host's client area in screen pixels, and
// whether the host is currently visible on screen.
struct EmbedGeometry {
    qint32 x = 0;
    qint32 y = 0;
    qint32 width = 0;
    qint32 height = 0;
    bool visible = false;
};

// Result of comparing the embed with where its host wants it.
enum class GeometryCheck : std::uint8_t {
    Ok,             // in place, with the right visibility
    NeedsUpdate,    // alive and owned, but moved, resized or shown wrongly
    TransientError, // a Win32 read failed but the window still looks alive
    WindowLost,     // the window is gone or the token no longer matches
};

// Called for EVENT_SYSTEM_FOREGROUND (GUI thread, any process).
using ForegroundCallback = void (*)(quintptr foregroundWindow);
// Called when a guarded top-level is activated while the keyboard focus is
// still in another process's window (an embed). Return true to take over
// moving the focus (off the GUI thread); false lets DefWindowProc do it.
using ActivationFocusCallback = bool (*)(quintptr topLevel, quintptr focus);

// Alt-tab style set: top-level, visible, titled windows owned by other
// processes. Cold path (menu open); returns [] on non-Windows.
QList<Candidate> enumerateEmbeddableWindows();

// Validate `handle` is embeddable and snapshot its full pre-embed state.
// Returns false (and leaves *out untouched) if the window is not embeddable or
// its state could not be captured.
bool captureState(quintptr handle, NativeWindowState *out);

// Tag the window with our per-embed token via SetPropW. False if the window is
// already tagged or the property could not be set.
bool markWindow(quintptr handle, quintptr token);

// Read-only target-specific test. True iff `targetHandle` exists and is still
// embedded for `hostHandle`: owned by the host's top-level (or, defensively, a
// direct child of the host). Independent of token ownership; used to retain a
// tab while its window would still die with our top-level.
bool isEmbeddedIn(quintptr targetHandle, quintptr hostHandle);

// Read-only hierarchy test. True iff `hostHandle` currently has at least one
// live descendant window owned by another process (an app that reparented
// itself into our host). Destroying the host would destroy it. Does not inspect
// or mutate the embed token; false on non-Windows.
bool hostRetainsForeignChild(quintptr hostHandle);

// True only while the handle still resolves to the *same* native window our
// token+pid+tid describe — guards every mutation against handle recycling.
bool sameWindow(const NativeWindowState &state, quintptr token);

// Read-only ownership test: the window exists and carries our token.
bool ownsWindow(quintptr targetHandle, quintptr token);

// Turn the foreign window into a borderless popup owned by the host's top-level
// and place it over the host. Returns false without partial rollback
// expectations if Windows refuses any step.
bool prepareEmbed(const NativeWindowState &state, quintptr hostHandle, quintptr token);

// Exact restore: original owner, styles, placement, visibility, and untag.
// Returns false only if the release-defining step (dropping our ownership)
// failed while the window was still alive.
bool restoreExact(const NativeWindowState &state, quintptr token);

// Guaranteed best-effort: drop our ownership so our top-level's destruction
// cannot take the foreign window down, make it a plain visible top-level, untag.
// Returns true if the window ended unowned by us (or was already gone).
bool failSafeDetach(const NativeWindowState &state, quintptr token);

// Where the embed should be (host client area in screen pixels, visibility),
// compared with where it is. Takes ownership back first if the app dropped it.
// Pure reads otherwise: nothing reaches the foreign thread.
GeometryCheck checkGeometry(quintptr targetHandle, quintptr hostHandle, quintptr token,
                            EmbedGeometry *wanted);

quintptr rootWindow(quintptr windowHandle);
quintptr foregroundWindow();
// True if `window` is (inside) an embedded foreign window owned by `topLevel`.
bool isEmbedOwnedBy(quintptr windowHandle, quintptr topLevelHandle);

// For a native WM_WINDOWPOSCHANGED: the window that moved. 0 otherwise.
quintptr positionChangedWindow(const void *message);
// For a native WM_MOUSEACTIVATE / WM_POINTERACTIVATE: the window clicked.
// 0 otherwise.
quintptr mouseActivateWindow(const void *message);
// The WM_MOUSEACTIVATE result that processes the click without activating.
qintptr mouseActivateNoActivate();

// Keep `topLevel`'s caption painted active while one of its embeds is the
// active window, and route the focus hand-off on its activation through
// `onActivation` (subclasses its window procedure; idempotent per top-level).
//
// Why the hand-off: DefWindowProc(WM_ACTIVATE) calls SetFocus on the activated
// window, and with the input queue shared with the embed that SetFocus waits
// for the embed's thread to process WM_KILLFOCUS, on the GUI thread.
bool installCaptionGuard(quintptr topLevelHandle, ActivationFocusCallback onActivation);
void removeCaptionGuard(quintptr topLevelHandle);
// Repaint `topLevel`'s caption active or inactive without changing activation.
void paintCaptionActive(quintptr topLevelHandle, bool active);

// Out-of-context EVENT_SYSTEM_FOREGROUND hook (0 on failure).
quintptr installForegroundHook(ForegroundCallback callback);
void removeForegroundHook(quintptr hook);

} // namespace EmbeddedWindowWin32
