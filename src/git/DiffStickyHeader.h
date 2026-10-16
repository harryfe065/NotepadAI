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

#ifndef DIFF_STICKY_HEADER_H
#define DIFF_STICKY_HEADER_H

#include <QByteArray>
#include <QColor>
#include <QFont>
#include <QRectF>
#include <QWidget>

#include <array>

class QToolButton;

// The file header pinned over the top of a diff editor's viewport while the
// reader is inside that file (DiffView). Paints the header row's own text and
// styles, so it looks like the row it stands for, plus Copy path / Open file.
class DiffStickyHeader : public QWidget
{
    Q_OBJECT
public:
    struct StyleLook {
        QColor fg;
        bool   bold = false;
        bool   strike = false;
    };
    using Looks = std::array<StyleLook, 32>;

    explicit DiffStickyHeader(QWidget *parent);

    // Header row text (UTF-8, no '\n') and one style byte per text byte.
    void setRow(const QByteArray &text, const QByteArray &styles);
    void setLooks(const Looks &looks, const QColor &background, const QColor &rule);
    // Editor font and line height; `textStart` is the x of the text column.
    void setMetrics(const QFont &font, int lineHeight, int textStart);

signals:
    void chevronClicked();
    void pathClicked();
    void copyPathClicked();
    void openClicked();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void changeEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void layoutRuns();
    void placeButtons();
    void retint();

    struct Run {
        QString text;
        quint8  style = 0;
        qreal   x = 0;
        qreal   width = 0;
    };
    QVector<Run> m_runs;
    QByteArray m_text;
    QByteArray m_styles;
    Looks      m_looks{};
    QColor     m_background;
    QColor     m_rule;
    QFont      m_font;
    QFont      m_boldFont;
    int        m_lineHeight = 16;
    int        m_textStart = 0;
    QRectF     m_chevronRect;
    QRectF     m_pathRect;
    QToolButton *m_copy = nullptr;
    QToolButton *m_open = nullptr;
};

#endif // DIFF_STICKY_HEADER_H
