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

#include <QtTest>
#include <QDockWidget>
#include <QLabel>
#include <QPointer>
#include <QSignalSpy>
#include <QToolButton>

#include "widgets/AiDockTitleBar.h"

class TestAiDockTitleBar : public QObject
{
    Q_OBJECT

private slots:
    void title_followsDockWindowTitle();
    void agentButton_hiddenUntilPushed_thenNameThenSwitchTarget();
    void agentButton_clickAsksForMenu();
    void dockButtons_followFeatures();
    void closeButton_andMiddleClick_closeTheDock();
};

namespace {

QToolButton *agentButton(AiDockTitleBar &bar)
{
    return bar.findChild<QToolButton *>(QLatin1String(AiDockTitleBar::kAgentButtonName));
}

QList<QToolButton *> chromeButtons(AiDockTitleBar &bar)
{
    QList<QToolButton *> out;
    for (QToolButton *btn : bar.findChildren<QToolButton *>(Qt::FindDirectChildrenOnly)) {
        if (btn->objectName() != QLatin1String(AiDockTitleBar::kAgentButtonName))
            out.append(btn);
    }
    return out; // float, close
}

} // namespace

void TestAiDockTitleBar::title_followsDockWindowTitle()
{
    QDockWidget dock;
    dock.setWindowTitle(QStringLiteral("notepad-ade"));
    auto *bar = new AiDockTitleBar(&dock);
    dock.setTitleBarWidget(bar);
    auto *title = bar->findChild<QLabel *>();
    QVERIFY(title);
    QCOMPARE(title->text(), QStringLiteral("notepad-ade"));
    dock.setWindowTitle(QStringLiteral("notepad-ade:"));
    QCOMPARE(title->text(), QStringLiteral("notepad-ade:"));
}

void TestAiDockTitleBar::agentButton_hiddenUntilPushed_thenNameThenSwitchTarget()
{
    QDockWidget dock;
    AiDockTitleBar bar(&dock);
    QToolButton *agent = agentButton(bar);
    QVERIFY(agent);
    QVERIFY(agent->isHidden());

    AiDockTitleBar::AgentButton button;
    button.name = QStringLiteral("CC 0x317374");
    button.tooltip = QStringLiteral("Agent: CC 0x317374");
    bar.setAgentButton(button);
    QVERIFY(!agent->isHidden());
    QVERIFY(bar.agentButtonText().startsWith(QStringLiteral("CC 0x317374")));
    QCOMPARE(agent->toolTip(), button.tooltip);

    button.switchingTo = QStringLiteral("CC muse");
    bar.setAgentButton(button);
    QVERIFY(bar.agentButtonText().startsWith(QStringLiteral("→ CC muse")));

    bar.setAgentButton({});
    QVERIFY(agent->isHidden());
}

void TestAiDockTitleBar::agentButton_clickAsksForMenu()
{
    QDockWidget dock;
    AiDockTitleBar bar(&dock);
    AiDockTitleBar::AgentButton button;
    button.name = QStringLiteral("CC nullmastermind");
    bar.setAgentButton(button);
    QSignalSpy menu(&bar, &AiDockTitleBar::agentMenuRequested);
    agentButton(bar)->click();
    QCOMPARE(menu.count(), 1);
}

void TestAiDockTitleBar::dockButtons_followFeatures()
{
    QDockWidget dock;
    dock.setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetFloatable);
    AiDockTitleBar bar(&dock);
    const QList<QToolButton *> buttons = chromeButtons(bar);
    QCOMPARE(buttons.size(), 2);
    QVERIFY(!buttons.at(0)->isHidden());
    QVERIFY(!buttons.at(1)->isHidden());
    dock.setFeatures(QDockWidget::DockWidgetClosable);
    QVERIFY(buttons.at(0)->isHidden()); // float
    QVERIFY(!buttons.at(1)->isHidden());
}

void TestAiDockTitleBar::closeButton_andMiddleClick_closeTheDock()
{
    QDockWidget dock;
    auto *bar = new AiDockTitleBar(&dock);
    dock.setTitleBarWidget(bar);
    dock.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dock));
    chromeButtons(*bar).at(1)->click();
    QVERIFY(dock.isHidden());

    dock.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dock));
    QTest::mouseClick(bar, Qt::MiddleButton, Qt::NoModifier, QPoint(4, bar->height() / 2));
    QVERIFY(dock.isHidden());
}

QTEST_MAIN(TestAiDockTitleBar)
#include "test_ai_dock_title_bar.moc"
