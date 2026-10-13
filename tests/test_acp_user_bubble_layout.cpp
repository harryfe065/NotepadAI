/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <QtTest>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>

#include "AcpMessageWidget.h"
#include "AcpProtocol.h"

namespace {

AcpMessageWidget *addUserBubble(QWidget *host, QVBoxLayout *layout, const QString &text)
{
    auto *bubble = new AcpMessageWidget(QStringLiteral("user"), host);
    bubble->setChatFont(QFont(QStringLiteral("Segoe UI"), 10));
    AcpProtocol::AcpContentBlock block;
    block.text = text;
    bubble->setContent({block});
    layout->insertWidget(layout->count() - 1, bubble);
    layout->setAlignment(bubble, Qt::AlignRight); // as AcpSessionView places prompts
    return bubble;
}

} // namespace

class TestAcpUserBubbleLayout : public QObject
{
    Q_OBJECT

private slots:
    void longPromptIsNeverClipped();
};

// The transcript as AcpSessionView builds it. Two ways the last lines got
// clipped: a path that cannot wrap set the bubble's minimum width above the
// transcript's, and the height was measured at the full transcript width while
// AlignRight narrowed the bubble to its hint (32 px less).
void TestAcpUserBubbleLayout::longPromptIsNeverClipped()
{
    QScrollArea scroll;
    scroll.setWidgetResizable(true);
    scroll.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll.setFrameShape(QFrame::NoFrame);
    auto *host = new QWidget(&scroll);
    auto *layout = new QVBoxLayout(host);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(6);
    layout->addStretch();
    scroll.setWidget(host);
    scroll.resize(560, 60); // shorter than the transcript: the host height is the content's
    scroll.show();
    QVERIFY(QTest::qWaitForWindowExposed(&scroll));

    AcpMessageWidget *shortBubble = addUserBubble(host, layout, QStringLiteral("ok"));
    AcpMessageWidget *bubble = addUserBubble(
        host, layout,
        QStringLiteral(R"(/diagnosing-bugs C:\Users\me\Documents\ShareX\Screenshots\2026-10\NotepadAI_McAGPadMLr.png )"
                       R"(render lỗi Ask user và khi lỗi này xảy ra thì bottom spacing cũng tăng )"
                       R"(C:\Users\me\Documents\ShareX\Screenshots\2026-10\NotepadAI_x2x7KWXmtu.png)"));

    for (int width = 200; width <= 800; width += 3) {
        host->setFixedWidth(width);
        QCoreApplication::processEvents();
        layout->activate();
        // Short prompts still hug their text.
        QVERIFY(shortBubble->width() < width / 2);
        QVERIFY(bubble->width() <= width - 8);
        const auto labels = bubble->findChildren<QLabel *>();
        QVERIFY(!labels.isEmpty());
        for (QLabel *label : labels) {
            if (!label->isVisible())
                continue;
            QVERIFY2(label->height() >= label->heightForWidth(label->width()),
                     qPrintable(QStringLiteral("width %1: text %2 px, needs %3 px")
                                    .arg(width)
                                    .arg(label->height())
                                    .arg(label->heightForWidth(label->width()))));
        }
        // No slack below the last bubble beyond the host's margin.
        QCOMPARE(host->height(), bubble->geometry().bottom() + 1 + 4);
    }
}

QTEST_MAIN(TestAcpUserBubbleLayout)
#include "test_acp_user_bubble_layout.moc"
