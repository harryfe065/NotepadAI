/*
 * This file is part of NotepadAI.
 * Copyright 2026 NotepadAI contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Foreign-process stand-in for test_embedded_window_win32: a plain top-level
// Win32 window that records the input, size and focus messages it receives into
// named shared memory. Behaves like the self-drawn apps users embed (Blender,
// Chromium, Godot): keyboard input is only accepted while its own activation
// flag (from WM_ACTIVATE / WM_NCACTIVATE) is set.
//
// Usage: embed_test_target --shm <name> [--maximized] [--dpi-unaware] [--raw-input]

#include "embed_test_target_shared.h"

#include <shellapi.h>

#include <cwchar>

namespace {
EmbedTestShared *g_shared = nullptr;

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_TIMER:
        if (const LONG busy = g_shared->busyMs; busy > 0)
            Sleep(static_cast<DWORD>(busy));
        return 0;
    case WM_NCACTIVATE:
        InterlockedExchange(&g_shared->active, wParam ? 1 : 0);
        break;
    case WM_ACTIVATE:
        InterlockedExchange(&g_shared->active, LOWORD(wParam) != WA_INACTIVE ? 1 : 0);
        break;
    case WM_SETFOCUS:
        InterlockedIncrement(&g_shared->setFocus);
        break;
    case WM_KILLFOCUS:
        InterlockedIncrement(&g_shared->killFocus);
        break;
    case WM_INPUT: {
        RAWINPUT raw{};
        UINT size = sizeof(raw);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &raw, &size,
                            sizeof(RAWINPUTHEADER)) != UINT(-1)
            && raw.header.dwType == RIM_TYPEKEYBOARD && !(raw.data.keyboard.Flags & RI_KEY_BREAK)) {
            InterlockedExchange(&g_shared->lastRawVKey, raw.data.keyboard.VKey);
            InterlockedIncrement(&g_shared->rawKeyDowns);
        }
        break;
    }
    case WM_CHAR:
        if (g_shared->active) {
            InterlockedIncrement(&g_shared->chars);
            InterlockedExchange(&g_shared->lastChar, static_cast<LONG>(wParam));
        } else {
            InterlockedIncrement(&g_shared->charsDropped);
        }
        return 0;
    case WM_LBUTTONDOWN:
        InterlockedExchange(&g_shared->lastClickX, static_cast<short>(LOWORD(lParam)));
        InterlockedExchange(&g_shared->lastClickY, static_cast<short>(HIWORD(lParam)));
        InterlockedIncrement(&g_shared->lButtonDowns);
        // Like most apps: a click focuses the clicked window.
        SetFocus(hwnd);
        return 0;
    case WM_MOUSEMOVE:
        InterlockedIncrement(&g_shared->mouseMoves);
        return 0;
    case WM_SIZE: {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        InterlockedExchange(&g_shared->clientW, rc.right - rc.left);
        InterlockedExchange(&g_shared->clientH, rc.bottom - rc.top);
        InterlockedIncrement(&g_shared->sizeMsgs);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        FillRect(dc, &ps.rcPaint, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int)
{
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t *shmName = nullptr;
    bool maximized = false;
    bool dpiUnaware = false;
    bool rawInput = false;
    for (int i = 1; argv && i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--shm") == 0 && i + 1 < argc)
            shmName = argv[++i];
        else if (std::wcscmp(argv[i], L"--maximized") == 0)
            maximized = true;
        else if (std::wcscmp(argv[i], L"--dpi-unaware") == 0)
            dpiUnaware = true;
        else if (std::wcscmp(argv[i], L"--raw-input") == 0)
            rawInput = true;
    }
    SetProcessDpiAwarenessContext(dpiUnaware ? DPI_AWARENESS_CONTEXT_UNAWARE
                                             : DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!shmName)
        return 2;
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, shmName);
    if (!mapping)
        return 3;
    g_shared = static_cast<EmbedTestShared *>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(EmbedTestShared)));
    if (!g_shared)
        return 4;

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    wc.lpszClassName = L"NotepadAIEmbedTestTarget";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"EmbedTestTarget", WS_OVERLAPPEDWINDOW,
                                100, 100, 640, 480, nullptr, nullptr, instance, nullptr);
    if (!hwnd)
        return 5;
    SetTimer(hwnd, 1, 16, nullptr);
    if (rawInput) {
        // Like Blender's GHOST: keyboard through raw input, following focus.
        RAWINPUTDEVICE device{};
        device.usUsagePage = 0x01;
        device.usUsage = 0x06;
        RegisterRawInputDevices(&device, 1, sizeof(device));
    }
    ShowWindow(hwnd, maximized ? SW_SHOWMAXIMIZED : SW_SHOWNOACTIVATE);
    InterlockedExchange64(&g_shared->hwnd, reinterpret_cast<LONG64>(hwnd));
    InterlockedExchange(&g_shared->ready, 1);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    UnmapViewOfFile(g_shared);
    CloseHandle(mapping);
    return 0;
}
