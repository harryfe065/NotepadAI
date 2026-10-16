/*
 * This file is part of NotepadAI.
 * Copyright 2026 NotepadAI contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Live embedded-window test: embeds a real foreign process window
// (embed_test_target) through EmbeddedWindowWin32 + NativeWindowHost and drives
// it with real OS input (SendInput). It needs the interactive desktop and takes
// the foreground for a few seconds, so it only runs when
// NOTEPADAI_INTERACTIVE_TESTS=1; ctest otherwise reports it skipped.
//
// Every SendInput is preceded by a foreground check, so a lost foreground fails
// the test instead of typing into another application.

#include "EmbeddedWindowFocusRouter.h"
#include "EmbeddedWindowWin32.h"
#include "NativeWindowHost.h"

#include "embed_test_target_shared.h"

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QPlainTextEdit>
#include <QStackedWidget>
#include <QTest>
#include <QHBoxLayout>
#include <QVBoxLayout>

#include <cwchar>
#include <memory>

namespace {

class TargetProcess
{
public:
    ~TargetProcess()
    {
        kill();
        if (m_shared)
            UnmapViewOfFile(m_shared);
        if (m_mapping)
            CloseHandle(m_mapping);
    }

    bool start(bool maximized, bool dpiUnaware, bool rawInput = false)
    {
        const std::wstring name = L"Local\\NotepadAIEmbedTest-" + std::to_wstring(GetCurrentProcessId())
            + L"-" + std::to_wstring(++s_counter);
        m_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                       sizeof(EmbedTestShared), name.c_str());
        if (!m_mapping)
            return false;
        m_shared = static_cast<EmbedTestShared *>(
            MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(EmbedTestShared)));
        if (!m_shared)
            return false;
        std::wstring cmd = L"\"" + QStringLiteral(EMBED_TEST_TARGET_PATH).toStdWString()
            + L"\" --shm " + name;
        if (maximized)
            cmd += L" --maximized";
        if (dpiUnaware)
            cmd += L" --dpi-unaware";
        if (rawInput)
            cmd += L" --raw-input";
        STARTUPINFOW si{sizeof(STARTUPINFOW)};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                            &si, &m_process))
            return false;
        QElapsedTimer timer;
        timer.start();
        while (!m_shared->ready && timer.elapsed() < 5000)
            Sleep(10);
        return m_shared->ready && hwnd();
    }

    void kill()
    {
        if (!m_process.hProcess)
            return;
        TerminateProcess(m_process.hProcess, 0);
        WaitForSingleObject(m_process.hProcess, 2000);
        CloseHandle(m_process.hProcess);
        CloseHandle(m_process.hThread);
        m_process = {};
    }

    HWND hwnd() const { return m_shared ? reinterpret_cast<HWND>(m_shared->hwnd) : nullptr; }
    EmbedTestShared *shared() const { return m_shared; }

private:
    static inline int s_counter = 0;
    HANDLE m_mapping = nullptr;
    EmbedTestShared *m_shared = nullptr;
    PROCESS_INFORMATION m_process{};
};

// A real Blender (NOTEPADAI_BLENDER=path to blender.exe) running
// embed_blender_log_events.py, which logs every key event it sees.
class BlenderProcess
{
public:
    ~BlenderProcess() { kill(); }

    bool start()
    {
        m_log = QDir::temp().filePath(QStringLiteral("notepadai-embed-blender-%1.log").arg(GetCurrentProcessId()));
        QFile::remove(m_log);
        const std::wstring cmd = L"\"" + qEnvironmentVariable("NOTEPADAI_BLENDER").toStdWString()
            + L"\" --factory-startup --python \"" + QStringLiteral(EMBED_BLENDER_SCRIPT_PATH).toStdWString() + L"\"";
        const std::wstring env = L"NOTEPADAI_BLENDER_LOG=" + QDir::toNativeSeparators(m_log).toStdWString();
        _wputenv(env.c_str()); // inherited by the child
        STARTUPINFOW si{sizeof(STARTUPINFOW)};
        std::wstring mutableCmd = cmd;
        if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &m_process))
            return false;
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 60000) {
            if (log().contains(QStringLiteral("READY")) && (m_hwnd = findWindow()))
                return true;
            QTest::qWait(100);
        }
        return false;
    }

    void kill()
    {
        if (!m_process.hProcess)
            return;
        TerminateProcess(m_process.hProcess, 0);
        WaitForSingleObject(m_process.hProcess, 5000);
        CloseHandle(m_process.hProcess);
        CloseHandle(m_process.hThread);
        m_process = {};
    }

    HWND hwnd() const { return m_hwnd; }

    QString log() const
    {
        QFile file(m_log);
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
    }

    void clearLog() const
    {
        QFile file(m_log);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            file.close();
    }

private:
    HWND findWindow() const
    {
        struct Context { DWORD pid; HWND found; } context{m_process.dwProcessId, nullptr};
        EnumWindows([](HWND hwnd, LPARAM data) -> BOOL {
            auto *c = reinterpret_cast<Context *>(data);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            wchar_t cls[64]{};
            GetClassNameW(hwnd, cls, 64);
            if (pid == c->pid && IsWindowVisible(hwnd) && std::wcscmp(cls, L"GHOST_WindowClass") == 0) {
                c->found = hwnd;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&context));
        return context.found;
    }

    QString m_log;
    HWND m_hwnd = nullptr;
    PROCESS_INFORMATION m_process{};
};

HWND topHwnd(QWidget *w)
{
    return reinterpret_cast<HWND>(w->window()->winId());
}

bool forceForeground(HWND hwnd)
{
    HWND fg = GetForegroundWindow();
    const DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD me = GetCurrentThreadId();
    const bool attach = fgThread && fgThread != me;
    if (attach)
        AttachThreadInput(me, fgThread, TRUE);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    if (attach)
        AttachThreadInput(me, fgThread, FALSE);
    if (GetForegroundWindow() != hwnd)
        SwitchToThisWindow(hwnd, TRUE);
    return GetForegroundWindow() == hwnd;
}

DWORD windowProcess(HWND hwnd)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid;
}

// True if `hwnd` is `top` or a top-level it owns (the embed, a dialog, a menu).
bool ownedBy(HWND hwnd, HWND top)
{
    HWND root = GetAncestor(hwnd, GA_ROOT);
    return root && (root == top || GetWindow(root, GW_OWNER) == top);
}

// Our process, or the embed (a top-level our window owns).
bool ours(HWND hwnd, HWND top)
{
    return windowProcess(hwnd) == GetCurrentProcessId() || ownedBy(hwnd, top);
}

} // namespace

class TestEmbeddedWindowWin32 : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();

    void sizeTracksHost_data();
    void sizeTracksHost();
    void clickLandsAtClientCoordinates();
    void keyboardFollowsClicks();
    void switchingTabsMovesKeyboard();
    void keyboardAfterReactivation();
    void clickOnNoFocusButtonKeepsKeyboardInEmbed();
    void altTapKeepsKeyboardInEmbed();
    void clickInEmbedClosesQtPopup();
    void menuOpenedFromActiveEmbedStaysOpen();
    void clickIntoEmbedDoesNotBounceFocus();
    void foreignAppActivationDeactivatesEmbed();
    void tabClicksDoNotStallWithBusyTarget();
    void hungEmbedDoesNotFreezeHost_data();
    void hungEmbedDoesNotFreezeHost();
    void releasingHungEmbedDoesNotBlock();
    void tabSwitchDoesNotResendSize();
    void rawInputKeyboardReachesEmbed();
    void embedIsOwnedPopupOutOfTaskbar();
    void followsWindowMoves();
    void hidesWithItsTab();
    void putsBackAnEmbedThatMovedItself();
    void restoresAnEmbedThatMinimizedItself();
    void modalDialogDisablesEmbed();
    void blenderGetsModifierKeys();
    void benchmarkNativeSiblingCost();

private:
    struct Harness {
        QWidget window;
        QLineEdit *edit = nullptr;
        QStackedWidget *stack = nullptr;
        NativeWindowHost *host = nullptr;
        QPlainTextEdit *editor = nullptr;
        QPushButton *embedTab = nullptr;
        QPushButton *editorTab = nullptr;
        QToolButton *noFocusButton = nullptr;
        QPushButton *menuButton = nullptr;
        QMenu *buttonMenu = nullptr;
        EmbeddedWindowFocusRouter router;
        TargetProcess target;
        std::unique_ptr<BlenderProcess> blender;
        EmbeddedWindowWin32::NativeWindowState state;
        quintptr token = 0;
        bool embedded = false;
        bool otherForegroundExpected = false; // test activates another window on purpose
    };

    bool embed(bool maximized = false, bool dpiUnaware = false);
    bool buildHarness();
    bool attachHandle(quintptr handle);
    void pressKeys(std::initializer_list<std::pair<WORD, bool>> keys); // (vk, down)
    void showEmbedTab();
    void showEditorTab();
    bool hostFilledByChild(bool checkReportedSize = true) const;
    QPoint hostPoint(int x, int y) const; // host client px -> screen px
    QPoint widgetCenter(QWidget *w) const;
    void click(QPoint screen);
    void type(wchar_t ch);
    void tapAlt();
    static void settle(int ms = 50);

    std::unique_ptr<Harness> h;
    quintptr m_nextToken = 1;
};

void TestEmbeddedWindowWin32::initTestCase()
{
    if (qEnvironmentVariableIntValue("NOTEPADAI_INTERACTIVE_TESTS") != 1)
        QSKIP("needs the interactive desktop; set NOTEPADAI_INTERACTIVE_TESTS=1");
}

void TestEmbeddedWindowWin32::cleanup()
{
    if (h && h->embedded) {
        // Same order as EmbeddedWindowManager / EmbeddedWindowController.
        h->host->pauseForeignCalls();
        EmbeddedWindowWin32::restoreExact(h->state, h->token);
        h->host->detach();
        // The restored window may have taken the foreground; end it while our
        // window is still around to inherit activation.
        h->target.kill();
        if (h->blender)
            h->blender->kill();
        settle(50);
    }
    h.reset();
}

bool TestEmbeddedWindowWin32::embed(bool maximized, bool dpiUnaware)
{
    return buildHarness() && h->target.start(maximized, dpiUnaware)
        && attachHandle(reinterpret_cast<quintptr>(h->target.hwnd()));
}

bool TestEmbeddedWindowWin32::buildHarness()
{
    h = std::make_unique<Harness>();
    auto *layout = new QVBoxLayout(&h->window);
    // Tab-like buttons: no keyboard focus and they switch pages on release,
    // like ADS dock tabs.
    auto *tabs = new QHBoxLayout;
    h->embedTab = new QPushButton(QStringLiteral("Embed"));
    h->editorTab = new QPushButton(QStringLiteral("Editor"));
    h->noFocusButton = new QToolButton;
    h->noFocusButton->setText(QStringLiteral("noop"));
    h->menuButton = new QPushButton(QStringLiteral("Menu"));
    // Opens on press with popup(), like QMenuBar (QPushButton::setMenu would
    // run a nested exec() loop).
    h->buttonMenu = new QMenu(h->menuButton);
    h->buttonMenu->addAction(QStringLiteral("First"));
    h->buttonMenu->addAction(QStringLiteral("Second"));
    connect(h->menuButton, &QPushButton::pressed, this, [this]() {
        h->buttonMenu->popup(h->menuButton->mapToGlobal(QPoint(0, h->menuButton->height())));
    });
    for (QWidget *b : {static_cast<QWidget *>(h->embedTab), static_cast<QWidget *>(h->editorTab),
                       static_cast<QWidget *>(h->noFocusButton), static_cast<QWidget *>(h->menuButton)}) {
        b->setFocusPolicy(Qt::NoFocus);
        tabs->addWidget(b);
    }
    tabs->addStretch(1);
    connect(h->embedTab, &QPushButton::clicked, this, [this]() { showEmbedTab(); });
    connect(h->editorTab, &QPushButton::clicked, this, [this]() { showEditorTab(); });
    layout->addLayout(tabs);
    h->edit = new QLineEdit;
    h->stack = new QStackedWidget;
    h->host = new NativeWindowHost;
    h->editor = new QPlainTextEdit;
    h->stack->addWidget(h->host);
    h->stack->addWidget(h->editor);
    layout->addWidget(h->edit);
    layout->addWidget(h->stack, 1);
    h->window.resize(900, 640);
    h->window.show();
    return QTest::qWaitForWindowExposed(&h->window);
}

bool TestEmbeddedWindowWin32::attachHandle(quintptr handle)
{
    // Same wiring as EmbeddedWindowManager::platCreateTab / platPrepareAndAttach.
    NativeWindowHost *host = h->host;
    EmbeddedWindowFocusRouter *router = &h->router;
    h->router.registerHost(host, [host]() { host->activateForeign(); });
    host->setFocusRequestCallback([router, host]() { router->request(host); });

    h->token = m_nextToken++;
    if (!EmbeddedWindowWin32::captureState(handle, &h->state)
        || !EmbeddedWindowWin32::markWindow(handle, h->token))
        return false;
    h->embedded = true;
    return EmbeddedWindowWin32::prepareEmbed(h->state, host->nativeHandle(), h->token)
        && host->attach(handle, h->token);
}

void TestEmbeddedWindowWin32::showEmbedTab()
{
    // DockedEditor: tab raise -> focusedDockWidgetChanged -> previewTabActivated
    // -> EmbeddedWindowManager routes focus to the host.
    h->stack->setCurrentWidget(h->host);
    h->host->setFocus(Qt::OtherFocusReason);
    h->router.request(h->host);
}

void TestEmbeddedWindowWin32::showEditorTab()
{
    // DockedEditor: editor tab raise -> editor->grabFocus().
    h->stack->setCurrentWidget(h->editor);
    h->editor->setFocus(Qt::OtherFocusReason);
}

bool TestEmbeddedWindowWin32::hostFilledByChild(bool checkReportedSize) const
{
    HWND host = reinterpret_cast<HWND>(h->host->nativeHandle());
    HWND child = h->target.hwnd();
    RECT hostClient{};
    RECT childRect{};
    if (!GetClientRect(host, &hostClient) || !GetWindowRect(child, &childRect))
        return false;
    MapWindowPoints(HWND_DESKTOP, host, reinterpret_cast<POINT *>(&childRect), 2);
    const EmbedTestShared *s = h->target.shared();
    return EqualRect(&hostClient, &childRect) && IsWindowVisible(child) && !IsZoomed(child) && !IsIconic(child)
        && (!checkReportedSize
            || (s->clientW == hostClient.right && s->clientH == hostClient.bottom));
}

QPoint TestEmbeddedWindowWin32::hostPoint(int x, int y) const
{
    POINT p{x, y};
    ClientToScreen(reinterpret_cast<HWND>(h->host->nativeHandle()), &p);
    return {p.x, p.y};
}

QPoint TestEmbeddedWindowWin32::widgetCenter(QWidget *w) const
{
    const QPointF logical = w->mapTo(w->window(), w->rect().center());
    const qreal dpr = w->window()->devicePixelRatioF();
    POINT p{qRound(logical.x() * dpr), qRound(logical.y() * dpr)};
    ClientToScreen(topHwnd(w), &p);
    return {p.x, p.y};
}

void TestEmbeddedWindowWin32::click(QPoint screen)
{
    HWND top = topHwnd(&h->window);
    POINT pt{screen.x(), screen.y()};
    // Another app (e.g. a running NotepadAI) may grab the foreground between
    // steps; take it back rather than click into a window that is not ours.
    // (Never force it away from our own target: forceForeground attaches to the
    // foreground thread, which would wait on a hung target.)
    if (!ownedBy(GetForegroundWindow(), top) && GetForegroundWindow() != h->target.hwnd()
        && IsWindowVisible(top) && !h->otherForegroundExpected)
        forceForeground(top);
    QVERIFY2(ownedBy(WindowFromPoint(pt), top),
             "click target is not inside the test window; refusing to inject input");
    SetCursorPos(pt.x, pt.y);
    INPUT in[2]{};
    in[0].type = in[1].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    QCOMPARE(SendInput(2, in, sizeof(INPUT)), 2u);
    settle();
}

void TestEmbeddedWindowWin32::type(wchar_t ch)
{
    // Our target counts even while it is not owned (released while hung).
    QVERIFY2(ours(GetForegroundWindow(), topHwnd(&h->window)) || GetForegroundWindow() == h->target.hwnd(),
             "test window lost the foreground; refusing to inject keystrokes");
    INPUT in[2]{};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wScan = in[1].ki.wScan = ch;
    in[0].ki.dwFlags = KEYEVENTF_UNICODE;
    in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    QCOMPARE(SendInput(2, in, sizeof(INPUT)), 2u);
    settle();
}

void TestEmbeddedWindowWin32::tapAlt()
{
    QVERIFY2(ours(GetForegroundWindow(), topHwnd(&h->window)),
             "test window lost the foreground; refusing to inject keystrokes");
    INPUT in[2]{};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = VK_MENU;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    QCOMPARE(SendInput(2, in, sizeof(INPUT)), 2u);
    settle(100);
}

void TestEmbeddedWindowWin32::pressKeys(std::initializer_list<std::pair<WORD, bool>> keys)
{
    // Real virtual keys with scan codes (apps reading raw input ignore
    // KEYEVENTF_UNICODE packets).
    for (const auto &[vk, down] : keys) {
        QVERIFY2(ours(GetForegroundWindow(), topHwnd(&h->window)),
                 "test window lost the foreground; refusing to inject keystrokes");
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
        QCOMPARE(SendInput(1, &in, sizeof(INPUT)), 1u);
        settle(60);
    }
}

void TestEmbeddedWindowWin32::settle(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

void TestEmbeddedWindowWin32::sizeTracksHost_data()
{
    QTest::addColumn<bool>("maximized");
    QTest::addColumn<bool>("dpiUnaware");
    QTest::newRow("normal") << false << false;
    QTest::newRow("maximized-target") << true << false;
    QTest::newRow("dpi-unaware-target") << false << true;
}

void TestEmbeddedWindowWin32::sizeTracksHost()
{
    QFETCH(bool, maximized);
    QFETCH(bool, dpiUnaware);
    QVERIFY(embed(maximized, dpiUnaware));
    // A DPI-unaware target reports its size in its own (virtualised) pixels.
    const bool checkReported = !dpiUnaware;
    QTRY_VERIFY_WITH_TIMEOUT(hostFilledByChild(checkReported), 1500);
    h->window.resize(h->window.width() + 130, h->window.height() + 70);
    QTRY_VERIFY_WITH_TIMEOUT(hostFilledByChild(checkReported), 1500);
    h->window.resize(h->window.width() - 260, h->window.height() - 140);
    QTRY_VERIFY_WITH_TIMEOUT(hostFilledByChild(checkReported), 1500);
}

void TestEmbeddedWindowWin32::clickLandsAtClientCoordinates()
{
    QVERIFY(embed());
    QVERIFY(forceForeground(topHwnd(&h->window)));
    QTRY_VERIFY_WITH_TIMEOUT(hostFilledByChild(), 1500);
    EmbedTestShared *s = h->target.shared();
    click(hostPoint(37, 29));
    QTRY_COMPARE_WITH_TIMEOUT(s->lButtonDowns, 1L, 1000);
    QCOMPARE(s->lastClickX, 37L);
    QCOMPARE(s->lastClickY, 29L);
}

void TestEmbeddedWindowWin32::keyboardFollowsClicks()
{
    QVERIFY(embed());
    QVERIFY(forceForeground(topHwnd(&h->window)));
    QTRY_VERIFY_WITH_TIMEOUT(hostFilledByChild(), 1500);
    EmbedTestShared *s = h->target.shared();

    click(hostPoint(60, 60));
    type(L'a');
    QTRY_COMPARE_WITH_TIMEOUT(s->chars, 1L, 1000);

    click(widgetCenter(h->edit));
    type(L'b');
    QTRY_COMPARE_WITH_TIMEOUT(h->edit->text(), QStringLiteral("b"), 1000);
    QCOMPARE(s->chars, 1L);
    QTRY_COMPARE_WITH_TIMEOUT(s->active, 0L, 1000);

    click(hostPoint(80, 80));
    type(L'c');
    QTRY_COMPARE_WITH_TIMEOUT(s->chars, 2L, 1000);
    QCOMPARE(s->lastChar, static_cast<LONG>(L'c'));
    QCOMPARE(h->edit->text(), QStringLiteral("b"));
}

void TestEmbeddedWindowWin32::switchingTabsMovesKeyboard()
{
    QVERIFY(embed());
    QVERIFY(forceForeground(topHwnd(&h->window)));
    QTRY_VERIFY_WITH_TIMEOUT(hostFilledByChild(), 1500);
    EmbedTestShared *s = h->target.shared();

    click(hostPoint(60, 60));
    type(L'a');
    QTRY_COMPARE_WITH_TIMEOUT(s->chars, 1L, 1000);

    showEditorTab();
    settle();
    type(L'x');
    QTRY_COMPARE_WITH_TIMEOUT(h->editor->toPlainText(), QStringLiteral("x"), 1000);
    QCOMPARE(s->chars, 1L);
    QTRY_COMPARE_WITH_TIMEOUT(s->active, 0L, 1000);

    showEmbedTab();
    settle();
    type(L'y');
    QTRY_COMPARE_WITH_TIMEOUT(s->chars, 2L, 1000);
    QCOMPARE(h->editor->toPlainText(), QStringLiteral("x"));
}

void TestEmbeddedWindowWin32::keyboardAfterReactivation()
{
    QVERIFY(embed());
    QTRY_VERIFY_WITH_TIMEOUT(hostFilledByChild(), 1500);
    EmbedTestShared *s = h->target.shared();

    QWidget other;
    other.resize(200, 120);
    other.move(h->window.geometry().bottomRight() + QPoint(20, 20));
    other.show();
    QVERIFY(QTest::qWaitForWindowExposed(&other));
    QVERIFY(forceForeground(topHwnd(&other)));
    settle();
    QTRY_COMPARE_WITH_TIMEOUT(s->active, 0L, 1000);

    // Clicking into the embed of an inactive NotepadAI must activate it AND
    // leave the keyboard on the embed (not on Qt's restored focus widget).
    h->otherForegroundExpected = true;
    click(hostPoint(70, 70));
    h->otherForegroundExpected = false;
    QTRY_VERIFY_WITH_TIMEOUT(ownedBy(GetForegroundWindow(), topHwnd(&h->window)), 1000);
    type(L'k');
    QTRY_COMPARE_WITH_TIMEOUT(s->chars, 1L, 1000);
    QVERIFY(h->edit->text().isEmpty());
}

void TestEmbeddedWindowWin32::clickOnNoFocusButtonKeepsKeyboardInEmbed()
{
    QVERIFY(embed());
    QVERIFY(forceForeground(topHwnd(&h->window)));
    QTRY_VERIFY_WITH_TIMEOUT(hostFilledByChild(), 1500);
    EmbedTestShared *s = h->target.shared();
    click(hostPoint(60, 60));
    type(L'a');
    QTRY_COMPARE_WITH_TIMEOUT(s->chars, 1L, 1000);

    // A toolbar-style button takes no focus: the embed keeps the keyboard.
    click(widgetCenter(h->noFocusButton));
    settle(100);
    type(L'q');
    QTRY_COMPARE_WITH_TIMEOUT(s->chars, 2L, 1000);
    QVERIFY(h->edit->text().isEmpty());
}

void TestEmbeddedWindowWin32::altTapKeepsKeyboardInEmbed()
{
    QVERIFY(embed());
    QVERIFY(forceForeground(topHwnd(&h->window)));
    QTRY_VERIFY_WITH_TIMEOUT(hostFilledByChild(), 1500);
    EmbedTestShared *s = h->target.shared();
    click(hostPoint(60, 60));
    type(L'a');
    QTRY_COMPARE_WITH_TIMEOUT(s->chars, 1L, 1000);

    // A bare Alt tap in the embedded app must not park the keyboard in a
    // NotepadAI (system) menu loop.
    tapAlt();
    type(L'z');
    QTRY_COMPARE_WITH_TIMEOUT(s->chars, 2L, 1000);
    QCOMPARE(s->lastChar, static_cast<LONG>(L'z'));
}

void TestEmbeddedWindowWin32::clickInEmbedClosesQtPopup()
{
    QVERIFY(embed());
    QVERIFY(forceForeground(topHwnd(&h->window)));
    QTRY_VERIFY_WITH_TIMEOUT(hostFilledByChild(), 1500);
    EmbedTestShared *s = h->target.shared();

    QMenu menu;
    menu.addAction(QStringLiteral("Item"));
    menu.popup(h->host->mapToGlobal(QPoint(h->host->width() - 160, h->host->height() - 80)));
    QTRY_VERIFY_WITH_TIMEOUT(menu.isVisible(), 1000);
    // Like any Windows menu, a click outside closes it (and is consumed)...
    click(hostPoint(40, 40));
    QTRY_VERIFY_WITH_TIMEOUT(!menu.isVisible(), 1000);
    // ...after which the embed takes clicks and the keyboard again.
    click(hostPoint(40, 40));
    QTRY_VERIFY_WITH_TIMEOUT(s->lButtonDowns >= 1, 1000);
    type(L'm');
    QTRY_COMPARE_WITH_TIMEOUT(s->lastChar, static_cast<LONG>(L'm'), 1000);
}

void TestEmbeddedWindowWin32::clickIntoEmbedDoesNotBounceFocus()
{
    QVERIFY(embed());
    QVERIFY(forceForeground(topHwnd(&h->window)));
    QTRY_VERIFY_WITH_TIMEOUT(hostFilledByChild(), 1500);
    EmbedTestShared *s = h->target.shared();

    // From Qt focus into the embed: the app gets focus once and keeps it (no
    // round trip through NotepadAI's top-level, which apps see as a blur that
    // cancels drags and modal operations).
    click(widgetCenter(h->edit));
    settle(200);
    const LONG killsBefore = s->killFocus;
    click(hostPoint(60, 60));
    settle(300);
    QCOMPARE(s->killFocus, killsBefore);

    // Clicking again inside the focused embed changes nothing either.
    click(hostPoint(90, 90));
    settle(300);
    QCOMPARE(s->killFocus, killsBefore);
    type(L'b');
    QTRY_COMPARE_WITH_TIMEOUT(s->lastChar, static_cast<LONG>(L'b'), 1000);
}

void TestEmbeddedWindowWin32::foreignAppActivationDeactivatesEmbed()
{
    QVERIFY(embed());
    QVERIFY(forceForeground(topHwnd(&h->window)));
    QTRY_VERIFY_WITH_TIMEOUT(hostFilledByChild(), 1500);
    EmbedTestShared *s = h->target.shared();
    click(hostPoint(60, 60));
    type(L'a');
    QTRY_COMPARE_WITH_TIMEOUT(s->chars, 1L, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(s->active, 1L, 1000);

    // Another application takes the foreground: the embed must learn it is
    // inactive, and get the keyboard back when NotepadAI is clicked again.
    TargetProcess other;
    QVERIFY(other.start(false, false));
    // Keep it clear of the embed so the click below lands on NotepadAI.
    RECT frame{};
    GetWindowRect(topHwnd(&h->window), &frame);
    SetWindowPos(other.hwnd(), nullptr, frame.right - 220, frame.bottom - 160, 200, 140,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    QVERIFY(forceForeground(other.hwnd()));
    QTRY_COMPARE_WITH_TIMEOUT(s->active, 0L, 1000);
    h->otherForegroundExpected = true;
    click(hostPoint(80, 80));
    h->otherForegroundExpected = false;
    QTRY_VERIFY_WITH_TIMEOUT(ownedBy(GetForegroundWindow(), topHwnd(&h->window)), 1000);
    type(L'k');
    QTRY_COMPARE_WITH_TIMEOUT(s->lastChar, static_cast<LONG>(L'k'), 1000);
    QTRY_COMPARE_WITH_TIMEOUT(s->active, 1L, 1000);

    // Alt-tab style: away and back without touching the embed. The keyboard
    // must land in the embed again, not in NotepadAI's top-level.
    QVERIFY(forceForeground(other.hwnd()));
    QTRY_COMPARE_WITH_TIMEOUT(s->active, 0L, 1000);
    QVERIFY(forceForeground(topHwnd(&h->window)));
    settle(200);
    type(L'j');
    QTRY_COMPARE_WITH_TIMEOUT(s->lastChar, static_cast<LONG>(L'j'), 1000);
    QTRY_COMPARE_WITH_TIMEOUT(s->active, 1L, 1000);
    QVERIFY(h->edit->text().isEmpty());
}

void TestEmbeddedWindowWin32::menuOpenedFromActiveEmbedStaysOpen()
{
    QVERIFY(embed());
    QVERIFY(forceForeground(topHwnd(&h->window)));
    QTRY_VERIFY_WITH_TIMEOUT(hostFilledByChild(), 1500);
    EmbedTestShared *s = h->target.shared();
    click(hostPoint(60, 60));
    QTRY_COMPARE_WITH_TIMEOUT(s->active, 1L, 1000);

    // The click that opens the menu reaches NotepadAI while the embed is the
    // active window; activating NotepadAI afterwards must not close the menu.
    click(widgetCenter(h->menuButton));
    settle(400);
    QVERIFY(h->buttonMenu->isVisible());
    // NotepadAI (the menu or its window) takes the foreground from the embed.
    QTRY_VERIFY_WITH_TIMEOUT(GetForegroundWindow() != h->target.hwnd()
                                 && windowProcess(GetForegroundWindow()) == GetCurrentProcessId(), 1000);
    QVERIFY(h->buttonMenu->isVisible());
    QTRY_COMPARE_WITH_TIMEOUT(s->active, 0L, 1000);
    pressKeys({{VK_ESCAPE, true}, {VK_ESCAPE, false}});
    QTRY_VERIFY_WITH_TIMEOUT(!h->buttonMenu->isVisible(), 1000);
}

void TestEmbeddedWindowWin32::tabClicksDoNotStallWithBusyTarget()
{
    QVERIFY(embed());
    QVERIFY(forceForeground(topHwnd(&h->window)));
    QTRY_VERIFY_WITH_TIMEOUT(hostFilledByChild(), 1500);
    EmbedTestShared *s = h->target.shared();
    click(hostPoint(60, 60));
    type(L'a');
    QTRY_COMPARE_WITH_TIMEOUT(s->chars, 1L, 1000);

    // Measure the worst event-loop stall while the user clicks between tabs
    // and the foreign UI thread is busy 100 ms out of every ~116 ms (a very
    // heavy app). The old code waited for it on every switch.
    //
    // Known limit: switching away while the worker is still handing focus to
    // the busy app (within ~one of its message delays) makes the hide of our
    // host wait for it once: Windows has to send it WM_KILLFOCUS and every
    // focus change away from a window waits for its thread. Clicks here are
    // spaced past that window.
    QElapsedTimer clock;
    clock.start();
    qint64 last = 0;
    qint64 worst = 0;
    QTimer meter;
    meter.setTimerType(Qt::PreciseTimer);
    meter.setInterval(1);
    connect(&meter, &QTimer::timeout, this, [&]() {
        const qint64 now = clock.elapsed();
        worst = qMax(worst, now - last);
        last = now;
    });
    InterlockedExchange(&s->busyMs, 100);
    settle(100);
    last = clock.elapsed();
    meter.start();
    for (int i = 0; i < 3; ++i) {
            click(widgetCenter(h->editorTab));
        settle(500);
            click(widgetCenter(h->embedTab));
        settle(500);
    }
    meter.stop();
    InterlockedExchange(&s->busyMs, 0);
    qInfo("worst event-loop stall while switching tabs: %lld ms", worst);
    QVERIFY2(worst < 50, qPrintable(QStringLiteral("GUI thread stalled %1 ms").arg(worst)));

    // And the keyboard still lands where the last tab put it.
    QTRY_VERIFY_WITH_TIMEOUT(h->stack->currentWidget() == h->host, 1000);
    settle(300);
    type(L'w');
    QTRY_COMPARE_WITH_TIMEOUT(s->lastChar, static_cast<LONG>(L'w'), 2000);
}

void TestEmbeddedWindowWin32::hungEmbedDoesNotFreezeHost_data()
{
    QTest::addColumn<bool>("embedHasKeyboard");
    QTest::newRow("embed-active") << true;
    QTest::newRow("notepadai-active") << false;
}

void TestEmbeddedWindowWin32::hungEmbedDoesNotFreezeHost()
{
    QFETCH(bool, embedHasKeyboard);
    QVERIFY(embed());
    QVERIFY(forceForeground(topHwnd(&h->window)));
    QTRY_VERIFY_WITH_TIMEOUT(hostFilledByChild(), 1500);
    EmbedTestShared *s = h->target.shared();
    click(hostPoint(60, 60));
    type(L'a');
    QTRY_COMPARE_WITH_TIMEOUT(s->chars, 1L, 1000);
    if (!embedHasKeyboard) {
        click(widgetCenter(h->edit));
        QTRY_VERIFY_WITH_TIMEOUT(h->edit->hasFocus(), 1000);
    }

    // The embedded app stops pumping messages (Blender running a long script),
    // with the mouse over it as the user would have it.
    constexpr LONG kHangMs = 6000;
    InterlockedExchange(&s->hangOnceMs, kHangMs);
    QTRY_VERIFY_WITH_TIMEOUT(s->hanging == 1, 1000);
    const QPoint over = hostPoint(80, 80);
    SetCursorPos(over.x(), over.y());
    settle(50);
    SetCursorPos(over.x() + 20, over.y() + 20);
    // Long enough for the host to tell hung from busy (about a second, like a
    // user noticing Blender is stuck).
    settle(1000);

    // NotepadAI must keep working: its event loop runs, and a click on one of
    // its widgets plus a keystroke land while the embed is still hung.
    QElapsedTimer clock;
    clock.start();
    qint64 last = 0;
    qint64 worst = 0;
    QTimer meter;
    meter.setTimerType(Qt::PreciseTimer);
    meter.setInterval(1);
    connect(&meter, &QTimer::timeout, this, [&]() {
        const qint64 now = clock.elapsed();
        worst = qMax(worst, now - last);
        last = now;
    });
    meter.start();
    h->edit->clear();
    click(widgetCenter(h->edit));
    type(L'z');
    QElapsedTimer typed;
    typed.start();
    while (!h->edit->text().contains(QLatin1Char('z')) && typed.elapsed() < 1500)
        settle(10);
    const qint64 latency = clock.elapsed();
    const bool stillHung = s->hanging == 1;
    meter.stop();
    qInfo("hung embed: keystroke after %lld ms, worst event-loop stall %lld ms, embed still hung: %d",
          latency, worst, stillHung);
    const bool landed = h->edit->text().contains(QLatin1Char('z'));

    // Let the hang end before cleanup releases the window.
    QTRY_VERIFY_WITH_TIMEOUT(s->hanging == 0, kHangMs + 2000);
    QVERIFY2(stillHung, "the hang ended before the check; the test proves nothing");
    QVERIFY2(landed && latency < 1500,
             qPrintable(QStringLiteral("NotepadAI input stuck behind the hung embed (%1 ms)").arg(latency)));
    QVERIFY2(worst < 250, qPrintable(QStringLiteral("GUI thread stalled %1 ms").arg(worst)));

    // Once it answers again it is embedded as before: owned, over its host,
    // taking clicks and the keyboard.
    QTRY_VERIFY_WITH_TIMEOUT(GetWindow(h->target.hwnd(), GW_OWNER) == topHwnd(&h->window), 2000);
    QTRY_VERIFY_WITH_TIMEOUT(hostFilledByChild(false), 1500);
    const auto aboveOwner = [this]() {
        for (HWND w = GetWindow(topHwnd(&h->window), GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV)) {
            if (w == h->target.hwnd())
                return true;
        }
        return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(aboveOwner(), 1500);
    const LONG charsBefore = s->chars;
    click(hostPoint(60, 60));
    type(L'r');
    QTRY_COMPARE_WITH_TIMEOUT(s->chars, charsBefore + 1, 1500);
    QCOMPARE(s->lastChar, static_cast<LONG>(L'r'));
}

void TestEmbeddedWindowWin32::releasingHungEmbedDoesNotBlock()
{
    QVERIFY(embed());
    QVERIFY(forceForeground(topHwnd(&h->window)));
    QTRY_VERIFY_WITH_TIMEOUT(hostFilledByChild(), 1500);
    EmbedTestShared *s = h->target.shared();
    constexpr LONG kHangMs = 5000;
    InterlockedExchange(&s->hangOnceMs, kHangMs);
    QTRY_VERIFY_WITH_TIMEOUT(s->hanging == 1, 1000);
    settle(1000);

    // The user closes the tab of the hung app (manager order: pause, release,
    // detach).
    QElapsedTimer clock;
    clock.start();
    h->host->pauseForeignCalls();
    const bool released = EmbeddedWindowWin32::restoreExact(h->state, h->token);
    h->host->detach();
    const qint64 took = clock.elapsed();
    const bool stillHung = s->hanging == 1;
    h->embedded = !released;
    qInfo("releasing a hung embed took %lld ms (still hung: %d)", took, stillHung);
    QVERIFY(released);
    QVERIFY(GetWindow(h->target.hwnd(), GW_OWNER) == nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(s->hanging == 0, kHangMs + 2000);
    QVERIFY2(stillHung, "the hang ended before the check; the test proves nothing");
    QVERIFY2(took < 500, qPrintable(QStringLiteral("GUI thread blocked %1 ms releasing it").arg(took)));
}

void TestEmbeddedWindowWin32::tabSwitchDoesNotResendSize()
{
    QVERIFY(embed());
    QTRY_VERIFY_WITH_TIMEOUT(hostFilledByChild(), 1500);
    EmbedTestShared *s = h->target.shared();
    settle(200);
    const LONG before = s->sizeMsgs;
    for (int i = 0; i < 5; ++i) {
        showEditorTab();
        settle(30);
        showEmbedTab();
        settle(30);
    }
    settle(200);
    QVERIFY(hostFilledByChild());
    QCOMPARE(s->sizeMsgs - before, 0L);
}

void TestEmbeddedWindowWin32::benchmarkNativeSiblingCost()
{
    // One native widget (an embed host or a WebView2 host) inside a docked
    // layout. Unless AA_DontCreateNativeWidgetSiblings is set, Qt makes every
    // sibling of every ancestor native too, for the rest of the session.
    const auto build = [](QWidget &window, QStackedWidget *&stack) {
        auto *split = new QSplitter(Qt::Horizontal);
        stack = new QStackedWidget;
        QString text;
        for (int i = 0; i < 3000; ++i)
            text += QStringLiteral("line %1 of some editor text that wraps nowhere\n").arg(i);
        for (int i = 0; i < 12; ++i) {
            auto *page = new QWidget;
            auto *pageLayout = new QVBoxLayout(page);
            auto *edit = new QPlainTextEdit;
            edit->setPlainText(text);
            pageLayout->addWidget(edit);
            stack->addWidget(page);
        }
        stack->addWidget(new NativeWindowHost);
        auto *right = new QSplitter(Qt::Vertical);
        right->addWidget(new QPlainTextEdit(text));
        right->addWidget(new QLineEdit);
        split->addWidget(stack);
        split->addWidget(right);
        auto *layout = new QVBoxLayout(&window);
        layout->addWidget(split, 1);
        layout->addWidget(new QPlainTextEdit(text));
        window.resize(1100, 760);
        window.show();
    };
    const auto countNative = [](QWidget &window) {
        int n = 0;
        for (QWidget *w : window.findChildren<QWidget *>())
            n += w->internalWinId() ? 1 : 0;
        return n;
    };
    const auto measure = [&](bool dontCreateSiblings, double *switchMs, double *resizeMs, int *natives) {
        QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings, dontCreateSiblings);
        QWidget window;
        QStackedWidget *stack = nullptr;
        build(window, stack);
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        stack->widget(stack->count() - 1)->winId(); // the native host exists
        settle(200);
        *natives = countNative(window);
        QElapsedTimer t;
        t.start();
        constexpr int kSwitches = 60;
        for (int i = 0; i < kSwitches; ++i) {
            stack->setCurrentIndex(i % 12);
            QCoreApplication::processEvents();
            QCoreApplication::processEvents();
        }
        *switchMs = double(t.nsecsElapsed()) / 1e6 / kSwitches;
        t.restart();
        constexpr int kResizes = 30;
        for (int i = 0; i < kResizes; ++i) {
            window.resize(1100 + (i % 2) * 60, 760 + (i % 2) * 40);
            QCoreApplication::processEvents();
            QCoreApplication::processEvents();
        }
        *resizeMs = double(t.nsecsElapsed()) / 1e6 / kResizes;
    };

    double switchOff = 0, resizeOff = 0, switchOn = 0, resizeOn = 0;
    int nativeOff = 0, nativeOn = 0;
    for (int round = 0; round < 2; ++round) {
        double s = 0, r = 0;
        measure(false, &s, &r, &nativeOff);
        switchOff += s / 2;
        resizeOff += r / 2;
        measure(true, &s, &r, &nativeOn);
        switchOn += s / 2;
        resizeOn += r / 2;
    }
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings, false);
    qInfo("native siblings forced : %d native widgets, tab switch %.2f ms, resize %.2f ms",
          nativeOff, switchOff, resizeOff);
    qInfo("AA_DontCreateNativeWidgetSiblings: %d native widgets, tab switch %.2f ms, resize %.2f ms",
          nativeOn, switchOn, resizeOn);
}

void TestEmbeddedWindowWin32::rawInputKeyboardReachesEmbed()
{
    // Apps such as Blender read the keyboard only through raw input
    // (WM_INPUT) and ignore WM_KEYDOWN.
    QVERIFY(buildHarness());
    QVERIFY(h->target.start(false, false, true));
    QVERIFY(attachHandle(reinterpret_cast<quintptr>(h->target.hwnd())));
    QVERIFY(forceForeground(topHwnd(&h->window)));
    QTRY_VERIFY_WITH_TIMEOUT(hostFilledByChild(), 1500);
    EmbedTestShared *s = h->target.shared();
    click(hostPoint(60, 60));
    settle(200);
    pressKeys({{VK_LSHIFT, true}, {'Z', true}, {'Z', false}, {VK_LSHIFT, false}});
    QTRY_VERIFY_WITH_TIMEOUT(s->rawKeyDowns >= 2, 1000);
    QCOMPARE(s->lastRawVKey, static_cast<LONG>('Z'));
}

void TestEmbeddedWindowWin32::embedIsOwnedPopupOutOfTaskbar()
{
    QVERIFY(embed());
    QTRY_VERIFY_WITH_TIMEOUT(hostFilledByChild(), 1500);
    HWND t = h->target.hwnd();
    // A top-level owned by our window (raw input follows the foreground window,
    // so the embed must be able to become it), no frame, not in the taskbar.
    QCOMPARE(GetWindow(t, GW_OWNER), topHwnd(&h->window));
    QCOMPARE(GetAncestor(t, GA_PARENT), GetDesktopWindow());
    const LONG_PTR style = GetWindowLongPtrW(t, GWL_STYLE);
    QVERIFY(style & WS_POPUP);
    QVERIFY(!(style & (WS_CHILD | WS_CAPTION | WS_THICKFRAME)));
    QVERIFY(!(GetWindowLongPtrW(t, GWL_EXSTYLE) & WS_EX_APPWINDOW));
}

void TestEmbeddedWindowWin32::followsWindowMoves()
{
    QVERIFY(embed());
    QTRY_VERIFY_WITH_TIMEOUT(hostFilledByChild(), 1500);
    h->window.move(h->window.pos() + QPoint(60, 40));
    QTRY_VERIFY_WITH_TIMEOUT(hostFilledByChild(), 1500);
    // A layout change that moves the host on screen without resizing it (only
    // its ancestors move; the host itself gets no Qt event).
    auto *layout = h->window.layout();
    const QMargins margins = layout->contentsMargins();
    layout->setContentsMargins(margins.left() + 30, margins.top(), margins.right(), margins.bottom());
    h->window.resize(h->window.width() + 30, h->window.height());
    QTRY_VERIFY_WITH_TIMEOUT(hostFilledByChild(), 1500);
}

void TestEmbeddedWindowWin32::hidesWithItsTab()
{
    QVERIFY(embed());
    QTRY_VERIFY_WITH_TIMEOUT(hostFilledByChild(), 1500);
    HWND t = h->target.hwnd();
    showEditorTab();
    QTRY_VERIFY_WITH_TIMEOUT(!IsWindowVisible(t), 1000);
    showEmbedTab();
    QTRY_VERIFY_WITH_TIMEOUT(hostFilledByChild(), 1000);
    h->window.showMinimized();
    QTRY_VERIFY_WITH_TIMEOUT(!IsWindowVisible(t), 1000);
    h->window.showNormal();
    QTRY_VERIFY_WITH_TIMEOUT(hostFilledByChild(), 1500);
}

// The app moves or resizes its own window (Blender applies the window size a
// .blend stores, Win+arrow snaps the foreground embed): the watchdog tick
// (EmbeddedWindowManager::platTick -> sync) must put it back over the host,
// even though the host itself did not move.
void TestEmbeddedWindowWin32::putsBackAnEmbedThatMovedItself()
{
    QVERIFY(embed());
    QTRY_VERIFY_WITH_TIMEOUT(hostFilledByChild(), 1500);
    HWND t = h->target.hwnd();
    RECT r{};
    QVERIFY(GetWindowRect(t, &r));
    QVERIFY(SetWindowPos(t, nullptr, r.left + 400, r.top + 300, 200, 150,
                         SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER));
    QVERIFY(!hostFilledByChild(false));
    QTRY_VERIFY_WITH_TIMEOUT((h->host->sync(), hostFilledByChild()), 1500);
}

// The app minimizes itself (Win+Down on the foreground embed): the host stays
// empty unless the tick restores it.
void TestEmbeddedWindowWin32::restoresAnEmbedThatMinimizedItself()
{
    QVERIFY(embed());
    QTRY_VERIFY_WITH_TIMEOUT(hostFilledByChild(), 1500);
    HWND t = h->target.hwnd();
    ShowWindow(t, SW_SHOWMINNOACTIVE);
    QTRY_VERIFY_WITH_TIMEOUT(IsIconic(t), 1000);
    QTRY_VERIFY_WITH_TIMEOUT((h->host->sync(), hostFilledByChild()), 1500);
}

void TestEmbeddedWindowWin32::modalDialogDisablesEmbed()
{
    QVERIFY(embed());
    QTRY_VERIFY_WITH_TIMEOUT(hostFilledByChild(), 1500);
    HWND t = h->target.hwnd();
    QVERIFY(IsWindowEnabled(t));
    // Qt's modality never disables native windows; an enabled embed could be
    // activated over the dialog.
    QDialog dialog(&h->window);
    dialog.setWindowModality(Qt::ApplicationModal);
    dialog.resize(220, 120);
    dialog.show();
    QTRY_VERIFY_WITH_TIMEOUT(!IsWindowEnabled(t), 1000);
    dialog.close();
    QTRY_VERIFY_WITH_TIMEOUT(IsWindowEnabled(t), 1000);
}

void TestEmbeddedWindowWin32::blenderGetsModifierKeys()
{
    if (qEnvironmentVariableIsEmpty("NOTEPADAI_BLENDER"))
        QSKIP("set NOTEPADAI_BLENDER to blender.exe to run");
    QVERIFY(buildHarness());
    h->blender = std::make_unique<BlenderProcess>();
    QVERIFY2(h->blender->start(), "Blender did not start / log READY");
    QVERIFY(attachHandle(reinterpret_cast<quintptr>(h->blender->hwnd())));
    QVERIFY(forceForeground(topHwnd(&h->window)));
    settle(1500); // let Blender relayout at the new size

    const QPoint viewport = hostPoint(h->host->width() * h->window.devicePixelRatioF() / 2,
                                      h->host->height() * h->window.devicePixelRatioF() / 2);
    const auto shiftZ = [&]() {
        h->blender->clearLog();
        pressKeys({{VK_LSHIFT, true}, {'Z', true}, {'Z', false}, {VK_LSHIFT, false}});
        settle(400);
        return h->blender->log();
    };

    click(viewport);
    settle(300);
    QString log = shiftZ();
    qInfo("after clicking the viewport:\n%s", qPrintable(log));
    QVERIFY2(log.contains(QStringLiteral("LEFT_SHIFT PRESS")), "Blender never saw Shift go down");
    QVERIFY2(log.contains(QStringLiteral("Z PRESS shift=1")), "Blender saw Z without Shift");

    // After a round trip through a Qt widget.
    click(widgetCenter(h->edit));
    settle(300);
    click(viewport);
    settle(300);
    log = shiftZ();
    qInfo("after a round trip through Qt:\n%s", qPrintable(log));
    QVERIFY2(log.contains(QStringLiteral("LEFT_SHIFT PRESS")), "Blender never saw Shift go down");
    QVERIFY2(log.contains(QStringLiteral("Z PRESS shift=1")), "Blender saw Z without Shift");
}

QTEST_MAIN(TestEmbeddedWindowWin32)

#include "test_embedded_window_win32.moc"
