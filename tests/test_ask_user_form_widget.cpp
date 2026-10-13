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
#include <QFrame>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLayout>
#include <QScrollArea>
#include <QVBoxLayout>

#include "AskUserForm.h"
#include "AskUserFormWidget.h"

namespace {

// Long agent-written text in every control that shows it: option labels and
// descriptions, a dropdown, confirm labels, an unbreakable token.
const char *kLongForm = R"({"title":"Round 2: nhiều key, /models, vòng đời key","questions":[
 {"id":"Q4","title":"Lọc /models thế nào? /v1/models hiện không liệt kê group","type":"single",
  "body":"Router chuyển nguyên request lên upstream: `muse-wonder-1.0-contributor-preview-build-0000000000`.",
  "options":[{"value":"a","label":"Bỏ lọc, /models giữ passthrough như cũ, không đổi hành vi cho client nào",
              "description":"Không đổi gì. Lọc chỉ có nghĩa khi router tự liệt kê group, mà việc đó là một feature riêng."},
             {"value":"b","label":"Giữ passthrough, nhưng nếu khớp group thì lọc theo tên của group có key"}],
  "recommended":"a","recommendation":"Bỏ lọc."},
 {"id":"Q11","title":"Mỗi key gồm những trường nào?","type":"multi",
  "options":[{"value":"a","label":"Tên (bắt buộc, không trùng trong group) + key + created_at + last_used_at"},
             {"value":"b","label":"Như lựa chọn đầu, thêm nút bật/tắt từng key và ghi chú tự do cho mỗi key"}]},
 {"id":"Q12","title":"Dialog nào?","type":"select",
  "options":[{"value":"a","label":"Bảng: tên, key che, ngày tạo, lần dùng cuối, nút xóa, nút sao chép, nút đổi tên"},
             {"value":"b","label":"Danh sách thẻ"}]},
 {"id":"Q13","title":"Xóa key cũ?","type":"confirm",
  "yesLabel":"Xóa ngay toàn bộ key cũ của group sau khi import xong và ghi log",
  "noLabel":"Giữ lại toàn bộ key cũ cho tới khi người dùng tự xóa bằng tay"}
]})";

// The same form with short text: its minimum width is the chrome's alone.
const char *kShortForm = R"({"title":"Round 2","questions":[
 {"id":"Q4","title":"Lọc?","type":"single","body":"Ngắn.",
  "options":[{"value":"a","label":"Bỏ","description":"Không."},{"value":"b","label":"Giữ"}],
  "recommended":"a","recommendation":"Bỏ."},
 {"id":"Q11","title":"Trường?","type":"multi","options":[{"value":"a","label":"Tên"},{"value":"b","label":"Key"}]},
 {"id":"Q12","title":"Dialog?","type":"select","options":[{"value":"a","label":"Bảng"},{"value":"b","label":"Thẻ"}]},
 {"id":"Q13","title":"Xóa?","type":"confirm","yesLabel":"Xóa","noLabel":"Giữ"}
]})";

AskUserForm::Spec parseSpec(const char *json)
{
    QString error;
    const auto spec = AskUserForm::parseAskUserInput(QJsonDocument::fromJson(json).object(), &error);
    if (!spec)
        qFatal("bad fixture: %s", qPrintable(error));
    return *spec;
}

const QFont &chatFont()
{
    static const QFont font(QStringLiteral("Segoe UI"), 10);
    return font;
}

int minimumWidth(const char *json)
{
    const AskUserForm::Spec spec = parseSpec(json);
    AskUserFormWidget form(spec, AskUserForm::defaultAnswers(spec));
    form.setChatFont(chatFont());
    return form.minimumSizeHint().width();
}

} // namespace

class TestAskUserFormWidget : public QObject
{
    Q_OBJECT

private slots:
    void agentTextDoesNotWidenTheForm();
    void fillsNarrowTranscriptWithoutClipping();
};

// A minimum width above the transcript's makes QBoxLayout lay the form out for
// the minimum width: rows come out squashed, the slack collects underneath.
void TestAskUserFormWidget::agentTextDoesNotWidenTheForm()
{
    QCOMPARE(minimumWidth(kLongForm), minimumWidth(kShortForm));
}

// The transcript as AcpSessionView builds it: a resizable scroll area whose
// host has a fixed width, a vertical layout and a trailing stretch.
void TestAskUserFormWidget::fillsNarrowTranscriptWithoutClipping()
{
    QScrollArea scroll;
    scroll.setWidgetResizable(true);
    scroll.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *host = new QWidget(&scroll);
    auto *layout = new QVBoxLayout(host);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->addStretch();
    scroll.setWidget(host);
    scroll.resize(560, 200); // shorter than the form: the host height is the form's
    scroll.show();
    QVERIFY(QTest::qWaitForWindowExposed(&scroll));

    const AskUserForm::Spec spec = parseSpec(kLongForm);
    auto *form = new AskUserFormWidget(spec, AskUserForm::defaultAnswers(spec), host);
    form->setChatFont(chatFont());
    layout->insertWidget(0, form);

    // Down to the narrowest transcript the chrome allows, and back.
    const int narrow = minimumWidth(kShortForm) + 16;
    const int wide = qMax(540, narrow + 200);
    for (const int width : {wide, narrow, wide}) {
        host->setFixedWidth(width);
        QCoreApplication::processEvents();
        layout->activate();
        const int formWidth = form->width();
        QCOMPARE(formWidth, width - 16);
        QCOMPARE(form->height(), form->heightForWidth(formWidth));
        // No slack below the form beyond the host's margin.
        QCOMPARE(host->height(), form->geometry().bottom() + 1 + 8);
        const auto rows = form->findChildren<QFrame *>(QStringLiteral("auOption"));
        QVERIFY(!rows.isEmpty());
        for (QFrame *row : rows) {
            QVERIFY2(row->height() >= row->layout()->totalHeightForWidth(row->width()),
                     qPrintable(QStringLiteral("width %1: row %2 px, needs %3 px")
                                    .arg(width)
                                    .arg(row->height())
                                    .arg(row->layout()->totalHeightForWidth(row->width()))));
        }
    }
}

QTEST_MAIN(TestAskUserFormWidget)
#include "test_ask_user_form_widget.moc"
