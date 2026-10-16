/*
 * This file is part of NotepadAI.
 * Copyright 2026 NotepadAI contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// Shared-memory layout between test_embedded_window_win32 and its foreign
// process stand-in (embed_test_target). The target writes what it received;
// the test writes the knobs (busyMs). All fields are LONG so the Interlocked*
// family can touch them from either side.
#include <windows.h>

struct EmbedTestShared {
    volatile LONG64 hwnd;
    volatile LONG ready;
    volatile LONG busyMs;          // test -> target: sleep this long per 16 ms tick
    volatile LONG chars;           // WM_CHAR accepted
    volatile LONG charsDropped;    // WM_CHAR dropped by the activation gate
    volatile LONG lastChar;
    volatile LONG lButtonDowns;
    volatile LONG lastClickX;      // client coords, physical px
    volatile LONG lastClickY;
    volatile LONG sizeMsgs;        // WM_SIZE count
    volatile LONG clientW;         // GetClientRect at the last WM_SIZE
    volatile LONG clientH;
    volatile LONG setFocus;
    volatile LONG killFocus;
    volatile LONG active;          // own activation flag (WM_ACTIVATE / WM_NCACTIVATE)
    volatile LONG mouseMoves;
    volatile LONG rawKeyDowns;     // WM_INPUT keyboard make events (--raw-input)
    volatile LONG lastRawVKey;
    volatile LONG hangOnceMs;      // test -> target: stop pumping messages this long, once
    volatile LONG hanging;         // target -> test: 1 while that hang lasts
};
