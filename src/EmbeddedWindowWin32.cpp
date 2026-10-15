/*
 * This file is part of NotepadAI.
 * Copyright 2026 NotepadAI contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "EmbeddedWindowWin32.h"

#ifdef Q_OS_WIN
#include <Windows.h>
#include <commctrl.h>

namespace {
constexpr wchar_t EmbedPropertyName[] = L"NotepadAI.EmbeddedWindow.Token";
constexpr UINT_PTR kCaptionGuardId = 0x4e414945;

bool setLong(HWND hwnd, int index, LONG_PTR value)
{
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous = SetWindowLongPtrW(hwnd, index, value);
    return previous != 0 || GetLastError() == ERROR_SUCCESS;
}

bool setParent(HWND hwnd, HWND parent)
{
    SetLastError(ERROR_SUCCESS);
    HWND previous = SetParent(hwnd, parent);
    return previous != nullptr || GetLastError() == ERROR_SUCCESS;
}

bool sameIdentity(const EmbeddedWindowWin32::NativeWindowState &state)
{
    HWND hwnd = reinterpret_cast<HWND>(state.handle);
    DWORD pid = 0;
    const DWORD tid = IsWindow(hwnd) ? GetWindowThreadProcessId(hwnd, &pid) : 0;
    return tid == state.threadId && pid == state.processId;
}

WINDOWPLACEMENT placementFrom(const EmbeddedWindowWin32::NativeWindowState &s)
{
    WINDOWPLACEMENT p{sizeof(WINDOWPLACEMENT)};
    p.flags = s.placementFlags;
    p.showCmd = s.placementShowCmd;
    p.ptMinPosition = {s.placementMinX, s.placementMinY};
    p.ptMaxPosition = {s.placementMaxX, s.placementMaxY};
    p.rcNormalPosition = {s.placementNormalLeft, s.placementNormalTop,
                          s.placementNormalRight, s.placementNormalBottom};
    return p;
}

bool hostClientOnScreen(HWND host, RECT *rect)
{
    if (!GetClientRect(host, rect))
        return false;
    MapWindowPoints(host, HWND_DESKTOP, reinterpret_cast<POINT *>(rect), 2);
    return true;
}

// Make `hwnd` a direct top-level again if something reparented it as a child
// (an app may do that itself); our model never does.
bool ensureTopLevel(HWND hwnd)
{
    return GetAncestor(hwnd, GA_PARENT) == GetDesktopWindow() || setParent(hwnd, nullptr);
}

bool embedOwnedBy(HWND window, HWND topLevel)
{
    HWND root = GetAncestor(window, GA_ROOT);
    return root && GetPropW(root, EmbedPropertyName) && GetWindow(root, GW_OWNER) == topLevel;
}

EmbeddedWindowWin32::ActivationFocusCallback g_activationFocusCallback = nullptr;

LRESULT CALLBACK captionGuardProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                  UINT_PTR id, DWORD_PTR)
{
    switch (message) {
    case WM_NCACTIVATE:
        // One of our embeds is being activated: it is part of this window as
        // far as the user can tell, so keep the caption painted active.
        if (!wParam && lParam && embedOwnedBy(reinterpret_cast<HWND>(lParam), hwnd))
            return DefSubclassProc(hwnd, message, TRUE, lParam);
        break;
    case WM_ACTIVATE:
        // WM_NCACTIVATE's lParam is not guaranteed under visual styles;
        // WM_ACTIVATE's is. Repaint active if the deactivation went to an embed.
        if (LOWORD(wParam) == WA_INACTIVE && lParam && embedOwnedBy(reinterpret_cast<HWND>(lParam), hwnd)) {
            const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
            DefSubclassProc(hwnd, WM_NCACTIVATE, TRUE, 0);
            return result;
        }
        if (LOWORD(wParam) != WA_INACTIVE && !HIWORD(wParam) && g_activationFocusCallback) {
            // Activated while the keyboard is still in an embed: the focus
            // move DefWindowProc would do here waits for the embed's thread.
            HWND focus = GetFocus();
            DWORD focusProcess = 0;
            if (focus && GetWindowThreadProcessId(focus, &focusProcess) && focusProcess != GetCurrentProcessId()
                && g_activationFocusCallback(reinterpret_cast<quintptr>(hwnd), reinterpret_cast<quintptr>(focus)))
                return 0;
        }
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, captionGuardProc, id);
        break;
    default:
        break;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

EmbeddedWindowWin32::ForegroundCallback g_foregroundCallback = nullptr;

void CALLBACK onForegroundEvent(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD)
{
    if (g_foregroundCallback)
        g_foregroundCallback(reinterpret_cast<quintptr>(hwnd));
}
} // namespace
#endif

namespace EmbeddedWindowWin32 {

bool markWindow(quintptr handle, quintptr token)
{
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(handle);
    return IsWindow(hwnd) && !GetPropW(hwnd, EmbedPropertyName)
        && SetPropW(hwnd, EmbedPropertyName, reinterpret_cast<HANDLE>(token));
#else
    Q_UNUSED(handle) Q_UNUSED(token)
    return false;
#endif
}

bool isEmbeddedIn(quintptr targetHandle, quintptr hostHandle)
{
#ifdef Q_OS_WIN
    HWND target = reinterpret_cast<HWND>(targetHandle);
    HWND host = reinterpret_cast<HWND>(hostHandle);
    if (!IsWindow(target) || !IsWindow(host))
        return false;
    return GetWindow(target, GW_OWNER) == GetAncestor(host, GA_ROOT)
        || GetAncestor(target, GA_PARENT) == host;
#else
    Q_UNUSED(targetHandle) Q_UNUSED(hostHandle)
    return false;
#endif
}

bool hostRetainsForeignChild(quintptr hostHandle)
{
#ifdef Q_OS_WIN
    HWND host = reinterpret_cast<HWND>(hostHandle);
    if (!IsWindow(host))
        return false;
    struct Context {
        DWORD ownProcessId;
        bool found;
    } context{GetCurrentProcessId(), false};
    EnumChildWindows(host, [](HWND child, LPARAM data) -> BOOL {
        auto *context = reinterpret_cast<Context *>(data);
        DWORD childProcessId = 0;
        if (IsWindow(child) && GetWindowThreadProcessId(child, &childProcessId)
            && childProcessId != context->ownProcessId) {
            context->found = true;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    return context.found;
#else
    Q_UNUSED(hostHandle)
    return false;
#endif
}

bool sameWindow(const NativeWindowState &state, quintptr token)
{
#ifdef Q_OS_WIN
    return sameIdentity(state) && GetPropW(reinterpret_cast<HWND>(state.handle), EmbedPropertyName)
        == reinterpret_cast<HANDLE>(token);
#else
    Q_UNUSED(state) Q_UNUSED(token)
    return false;
#endif
}

bool ownsWindow(quintptr targetHandle, quintptr token)
{
#ifdef Q_OS_WIN
    HWND target = reinterpret_cast<HWND>(targetHandle);
    return IsWindow(target) && GetPropW(target, EmbedPropertyName) == reinterpret_cast<HANDLE>(token);
#else
    Q_UNUSED(targetHandle) Q_UNUSED(token)
    return false;
#endif
}

bool prepareEmbed(const NativeWindowState &s, quintptr hostHandle, quintptr token)
{
#ifdef Q_OS_WIN
    if (!sameWindow(s, token))
        return false;
    HWND hwnd = reinterpret_cast<HWND>(s.handle);
    HWND host = reinterpret_cast<HWND>(hostHandle);
    HWND topLevel = GetAncestor(host, GA_ROOT);
    RECT rect{};
    if (!topLevel || !hostClientOnScreen(host, &rect))
        return false;
    // Leave minimized / maximized state first, through the app's own window
    // procedure, so it does not keep maximized insets or a minimized frame.
    if ((IsIconic(hwnd) || IsZoomed(hwnd)) && sameWindow(s, token))
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    const LONG_PTR style = (s.originalStyle | WS_POPUP)
        & ~(WS_CHILD | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU
            | WS_MAXIMIZE | WS_MINIMIZE);
    // Owned and without WS_EX_APPWINDOW: no taskbar button, not in Alt-Tab.
    const LONG_PTR exStyle = s.originalExStyle & ~(WS_EX_APPWINDOW | WS_EX_TOPMOST);
    if (!(sameWindow(s, token) && setLong(hwnd, GWL_STYLE, style)
          && sameWindow(s, token) && setLong(hwnd, GWL_EXSTYLE, exStyle)
          && sameWindow(s, token) && setLong(hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(topLevel))
          && GetWindow(hwnd, GW_OWNER) == topLevel && sameWindow(s, token)))
        return false;
    if (s.originalExStyle & WS_EX_TOPMOST)
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    return sameWindow(s, token)
        && SetWindowPos(hwnd, HWND_TOP, rect.left, rect.top, rect.right - rect.left,
                        rect.bottom - rect.top, SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
#else
    Q_UNUSED(s) Q_UNUSED(hostHandle) Q_UNUSED(token)
    return false;
#endif
}

bool restoreExact(const NativeWindowState &s, quintptr token)
{
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(s.handle);
    // Token ownership (a per-embed SetPropW value) is re-verified immediately
    // before EVERY mutation. IsWindow + pid/tid cannot detect HWND recycling
    // within the same process/thread, but a recycled window is a NEW window
    // that does NOT carry our property — so a failed sameWindow() means "this
    // is no longer our window; stop touching it" and we bail without mutating.
    if (!sameWindow(s, token))
        return !sameIdentity(s); // gone → released; alive-but-untagged → fail

    // THE release-defining step: drop our ownership (and any parent). Once it
    // succeeds the window can no longer be destroyed with our top-level and the
    // tab is safe to reap. It is the SOLE gate on the return value: a cosmetic
    // failure below must not report the window as still embedded, or the
    // controller vetoes the close and the host takes the window right back.
    HWND originalOwner = reinterpret_cast<HWND>(s.originalOwner);
    if (!ensureTopLevel(hwnd) || !setLong(hwnd, GWLP_HWNDPARENT, s.originalOwner)
        || GetWindow(hwnd, GW_OWNER) != originalOwner) {
        qWarning("EmbeddedWindow: restoreExact could not drop ownership (err=%lu)", GetLastError());
        return false;
    }

    // Cosmetic best-effort restoration; each is gated on the token so a window
    // recycled after detach is never touched, but none affect the result.
    if (sameWindow(s, token))
        setLong(hwnd, GWL_STYLE, s.originalStyle);
    if (sameWindow(s, token))
        setLong(hwnd, GWL_EXSTYLE, s.originalExStyle);
    if (sameWindow(s, token)) {
        HWND z = (s.originalExStyle & WS_EX_TOPMOST) ? HWND_TOPMOST : HWND_NOTOPMOST;
        SetWindowPos(hwnd, z, s.rectLeft, s.rectTop, s.rectRight - s.rectLeft,
                     s.rectBottom - s.rectTop, SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    if (s.placementValid && sameWindow(s, token)) {
        WINDOWPLACEMENT placement = placementFrom(s);
        SetWindowPlacement(hwnd, &placement);
    }
    if (sameWindow(s, token)) {
        const int show = s.placementValid ? static_cast<int>(s.placementShowCmd) : SW_SHOWNOACTIVATE;
        ShowWindowAsync(hwnd, s.originallyVisible ? (show == SW_HIDE ? SW_SHOWNOACTIVATE : show) : SW_HIDE);
    }
    // Untag is best-effort: if the window was recycled the new owner keeps its
    // property; our host no longer tracks it so no re-grab can occur.
    if (sameWindow(s, token))
        RemovePropW(hwnd, EmbedPropertyName);
    return true;
#else
    Q_UNUSED(s) Q_UNUSED(token)
    return true;
#endif
}

bool failSafeDetach(const NativeWindowState &s, quintptr token)
{
#ifdef Q_OS_WIN
    if (!sameIdentity(s))
        return true;
    HWND hwnd = reinterpret_cast<HWND>(s.handle);
    // Never mutate a window that no longer carries our token. Token loss means
    // either external removal or same-process/thread HWND recycling; neither is
    // safe to distinguish using PID/TID alone. The controller separately reads
    // the hierarchy to decide whether the tab must be retained.
    if (!sameWindow(s, token))
        return false;

    // The release-defining step first (see restoreExact).
    if (!ensureTopLevel(hwnd) || !setLong(hwnd, GWLP_HWNDPARENT, 0) || GetWindow(hwnd, GW_OWNER)) {
        qWarning("EmbeddedWindow: failSafeDetach could not drop ownership (err=%lu)", GetLastError());
        return false;
    }

    // Best-effort cosmetic cleanup, always token-gated.
    if (sameWindow(s, token))
        setLong(hwnd, GWL_STYLE, s.originalStyle & ~WS_CHILD);
    if (sameWindow(s, token))
        setLong(hwnd, GWL_EXSTYLE, s.originalExStyle);
    if (sameWindow(s, token)) {
        const UINT flags = SWP_NOACTIVATE | SWP_FRAMECHANGED
            | (s.originallyVisible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);
        SetWindowPos(hwnd, HWND_TOP, s.rectLeft, s.rectTop,
                     s.rectRight - s.rectLeft, s.rectBottom - s.rectTop, flags);
    }
    if (sameWindow(s, token))
        RemovePropW(hwnd, EmbedPropertyName);
    return true;
#else
    Q_UNUSED(s) Q_UNUSED(token)
    return true;
#endif
}

GeometryCheck checkGeometry(quintptr targetHandle, quintptr hostHandle, quintptr token,
                            EmbedGeometry *wanted)
{
#ifdef Q_OS_WIN
    HWND target = reinterpret_cast<HWND>(targetHandle);
    HWND host = reinterpret_cast<HWND>(hostHandle);
    if (!ownsWindow(targetHandle, token))
        return GeometryCheck::WindowLost;
    HWND topLevel = GetAncestor(host, GA_ROOT);
    if (GetWindow(target, GW_OWNER) != topLevel) {
        // The app dropped or changed its owner: take it back. Rare, so the
        // synchronous cross-thread call is acceptable.
        if (!ensureTopLevel(target)
            || !setLong(target, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(topLevel)))
            return GeometryCheck::TransientError;
        if (!ownsWindow(targetHandle, token))
            return GeometryCheck::WindowLost;
    }
    RECT client{};
    RECT current{};
    if (!topLevel || !hostClientOnScreen(host, &client) || !GetWindowRect(target, &current))
        return GeometryCheck::TransientError;
    EmbedGeometry geometry;
    geometry.x = client.left;
    geometry.y = client.top;
    geometry.width = client.right - client.left;
    geometry.height = client.bottom - client.top;
    geometry.visible = IsWindowVisible(host) && !IsIconic(topLevel) && geometry.width > 0
        && geometry.height > 0;
    if (wanted)
        *wanted = geometry;
    const bool shown = (GetWindowLongPtrW(target, GWL_STYLE) & WS_VISIBLE) != 0;
    if (!geometry.visible)
        return shown ? GeometryCheck::NeedsUpdate : GeometryCheck::Ok;
    return shown && EqualRect(&client, &current) ? GeometryCheck::Ok : GeometryCheck::NeedsUpdate;
#else
    Q_UNUSED(targetHandle) Q_UNUSED(hostHandle) Q_UNUSED(token) Q_UNUSED(wanted)
    return GeometryCheck::WindowLost;
#endif
}

quintptr rootWindow(quintptr windowHandle)
{
#ifdef Q_OS_WIN
    return reinterpret_cast<quintptr>(GetAncestor(reinterpret_cast<HWND>(windowHandle), GA_ROOT));
#else
    Q_UNUSED(windowHandle)
    return 0;
#endif
}

quintptr foregroundWindow()
{
#ifdef Q_OS_WIN
    return reinterpret_cast<quintptr>(GetForegroundWindow());
#else
    return 0;
#endif
}

bool isEmbedOwnedBy(quintptr windowHandle, quintptr topLevelHandle)
{
#ifdef Q_OS_WIN
    return windowHandle && topLevelHandle
        && embedOwnedBy(reinterpret_cast<HWND>(windowHandle), reinterpret_cast<HWND>(topLevelHandle));
#else
    Q_UNUSED(windowHandle) Q_UNUSED(topLevelHandle)
    return false;
#endif
}

quintptr positionChangedWindow(const void *message)
{
#ifdef Q_OS_WIN
    const auto *native = static_cast<const MSG *>(message);
    return native && native->message == WM_WINDOWPOSCHANGED ? reinterpret_cast<quintptr>(native->hwnd) : 0;
#else
    Q_UNUSED(message)
    return 0;
#endif
}

quintptr mouseActivateWindow(const void *message)
{
#ifdef Q_OS_WIN
    const auto *native = static_cast<const MSG *>(message);
    // Qt runs the mouse through WM_POINTER, whose activation message is
    // WM_POINTERACTIVATE (same result codes: PA_NOACTIVATE == MA_NOACTIVATE).
    return native && (native->message == WM_MOUSEACTIVATE || native->message == WM_POINTERACTIVATE)
        ? reinterpret_cast<quintptr>(native->hwnd) : 0;
#else
    Q_UNUSED(message)
    return 0;
#endif
}

qintptr mouseActivateNoActivate()
{
#ifdef Q_OS_WIN
    return MA_NOACTIVATE;
#else
    return 0;
#endif
}

bool installCaptionGuard(quintptr topLevelHandle, ActivationFocusCallback onActivation)
{
#ifdef Q_OS_WIN
    g_activationFocusCallback = onActivation;
    return topLevelHandle
        && SetWindowSubclass(reinterpret_cast<HWND>(topLevelHandle), captionGuardProc, kCaptionGuardId, 0);
#else
    Q_UNUSED(topLevelHandle) Q_UNUSED(onActivation)
    return false;
#endif
}

void removeCaptionGuard(quintptr topLevelHandle)
{
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(topLevelHandle);
    if (IsWindow(hwnd))
        RemoveWindowSubclass(hwnd, captionGuardProc, kCaptionGuardId);
#else
    Q_UNUSED(topLevelHandle)
#endif
}

void paintCaptionActive(quintptr topLevelHandle, bool active)
{
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(topLevelHandle);
    if (IsWindow(hwnd))
        SendMessageW(hwnd, WM_NCACTIVATE, active ? TRUE : FALSE, 0);
#else
    Q_UNUSED(topLevelHandle) Q_UNUSED(active)
#endif
}

quintptr installForegroundHook(ForegroundCallback callback)
{
#ifdef Q_OS_WIN
    if (!callback)
        return 0;
    g_foregroundCallback = callback;
    // Out-of-context: delivered through this thread's message loop; foreground
    // changes are rare, so watching every process costs nothing measurable.
    return reinterpret_cast<quintptr>(SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                                      nullptr, onForegroundEvent, 0, 0,
                                                      WINEVENT_OUTOFCONTEXT));
#else
    Q_UNUSED(callback)
    return 0;
#endif
}

void removeForegroundHook(quintptr hook)
{
#ifdef Q_OS_WIN
    if (hook)
        UnhookWinEvent(reinterpret_cast<HWINEVENTHOOK>(hook));
#else
    Q_UNUSED(hook)
#endif
}

} // namespace EmbeddedWindowWin32
