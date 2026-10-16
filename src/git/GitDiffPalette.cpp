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

#include "GitDiffPalette.h"

const GitDiffPalette &GitDiffPalette::light()
{
    static const GitDiffPalette p = []() {
        GitDiffPalette x;
        // Primer light tokens
        x.fgAdded       = QColor(0x1A, 0x7F, 0x37);   // success.fg
        x.fgModified    = QColor(0x09, 0x69, 0xDA);   // accent.fg
        x.fgDeleted     = QColor(0x65, 0x6D, 0x76);   // fg.muted (gray)
        x.fgRenamed     = QColor(0x82, 0x50, 0xDF);   // done.fg (purple)
        x.fgUntracked   = QColor(0x1A, 0x7F, 0x37);
        x.fgConflict    = QColor(0xCF, 0x22, 0x2E);   // danger.fg
        x.fgPlus        = QColor(0x1A, 0x7F, 0x37);
        x.fgMinus       = QColor(0xCF, 0x22, 0x2E);

        // diffBlob.additionLineBgColor / deletionLineBgColor (GitHub light)
        x.bgAddLine     = QColor(0xDA, 0xFB, 0xE1);   // #dafbe1
        x.bgDelLine     = QColor(0xFF, 0xEB, 0xE9);   // #ffebe9
        x.bgAddWord     = QColor(0xAC, 0xEE, 0xBB);   // #aceebb
        x.bgDelWord     = QColor(0xFF, 0xCE, 0xCB);   // #ffcecb
        x.fgHunkHeader  = QColor(0x65, 0x6D, 0x76);
        x.bgHunkHeader  = QColor(0xDD, 0xF4, 0xFF);   // accent.muted
        x.fgGutter      = QColor(0x8C, 0x95, 0x9F);
        x.bgGutter      = QColor(0xE4, 0xE4, 0xE4);   // matches editor lineNumberBack

        x.canvasBg      = QColor(0xFF, 0xFF, 0xFF);   // #ffffff
        x.canvasFg      = QColor(0x1F, 0x23, 0x28);   // fg.default

        // Commit header tokens (light): meta = fg.muted (#656d76),
        // body = fg.default near-black (#1f2328).
        x.commitMetaFg  = QColor(0x65, 0x6D, 0x76);
        x.commitBodyFg  = QColor(0x1F, 0x23, 0x28);
        return x;
    }();
    return p;
}

const GitDiffPalette &GitDiffPalette::dark()
{
    static const GitDiffPalette p = []() {
        GitDiffPalette x;
        // Primer dark tokens
        x.fgAdded       = QColor(0x3F, 0xB9, 0x50);   // success.fg
        x.fgModified    = QColor(0x58, 0xA6, 0xFF);   // accent.fg
        x.fgDeleted     = QColor(0x8B, 0x94, 0x9E);   // fg.muted (gray)
        x.fgRenamed     = QColor(0xA3, 0x71, 0xF7);   // done.fg
        x.fgUntracked   = QColor(0x3F, 0xB9, 0x50);
        x.fgConflict    = QColor(0xF8, 0x51, 0x49);   // danger.fg
        x.fgPlus        = QColor(0x3F, 0xB9, 0x50);
        x.fgMinus       = QColor(0xF8, 0x51, 0x49);

        // Diff backgrounds pre-blended against canvasBg (#1E1E1E) so markers
        // paint opaquely. Source: GitHub rgba(46,160,67,0.15/0.40) for add,
        // rgba(248,81,73,0.15/0.40) for del, word layer over line layer.
        x.bgAddLine     = QColor(0x20, 0x32, 0x24);   // 15% green over canvas
        x.bgDelLine     = QColor(0x3F, 0x26, 0x24);   // 15% red over canvas
        x.bgAddWord     = QColor(0x26, 0x5E, 0x30);   // 40% green over bgAddLine
        x.bgDelWord     = QColor(0x89, 0x37, 0x33);   // 40% red over bgDelLine
        x.fgHunkHeader  = QColor(0x8B, 0x94, 0x9E);
        x.bgHunkHeader  = QColor(0x38, 0x8B, 0xFD, 0x40); // accent.muted alpha
        x.fgGutter      = QColor(0x6E, 0x76, 0x81);
        x.bgGutter      = QColor(0x25, 0x25, 0x26);   // matches editor lineNumberBack

        x.canvasBg      = QColor(0x1E, 0x1E, 0x1E);   // matches editor defaultBack
        x.canvasFg      = QColor(0xD4, 0xD4, 0xD4);   // matches editor defaultFore

        // Commit header tokens (dark): meta = fg.muted (#8b949e),
        // body = fg.default near-white (#e6edf3).
        x.commitMetaFg  = QColor(0x8B, 0x94, 0x9E);
        x.commitBodyFg  = QColor(0xE6, 0xED, 0xF3);
        return x;
    }();
    return p;
}

const GitDiffPalette &GitDiffPalette::current(bool isDark)
{
    return isDark ? dark() : light();
}

QColor GitDiffPalette::blend(const QColor &canvas, const QColor &rgba)
{
    const int a = rgba.alpha();
    auto mix = [a](int under, int over) { return (under * (255 - a) + over * a + 127) / 255; };
    return QColor(mix(canvas.red(), rgba.red()), mix(canvas.green(), rgba.green()),
                  mix(canvas.blue(), rgba.blue()));
}

namespace {

QColor alpha(QRgb rgb, int percent)
{
    QColor c(rgb);
    c.setAlpha(percent * 255 / 100);
    return c;
}

// The canvas moved `percent` points of HSL lightness away from its own
// lightness (towards white on dark canvases, black on light ones).
QColor shifted(const QColor &canvas, bool dark, int percent)
{
    int h = 0, s = 0, l = 0, a = 0;
    canvas.getHsl(&h, &s, &l, &a);
    const int delta = 255 * percent / 100;
    return QColor::fromHsl(h, s, qBound(0, dark ? l + delta : l - delta, 255), a);
}

} // namespace

DiffViewColors DiffViewColors::make(bool dark)
{
    const GitDiffPalette &p = GitDiffPalette::current(dark);
    const QColor canvas = p.canvasBg;
    auto over = [&](QRgb rgb, int percent) { return GitDiffPalette::blend(canvas, alpha(rgb, percent)); };

    DiffViewColors c;
    c.canvas = canvas;
    c.text = p.canvasFg;
    c.muted = p.commitMetaFg;
    if (dark) {
        c.addLine = over(0x2EA043, 15);
        c.addNumber = over(0x3FB950, 30);
        c.addWord = over(0x2EA043, 40);
        c.delLine = over(0xF85149, 10);
        c.delNumber = over(0xF85149, 30);
        c.delWord = over(0xF85149, 40);
        c.hunkLine = over(0x388BFD, 10);
        c.hunkText = QColor(0x91, 0x98, 0xA1);
        c.plus = QColor(0x3F, 0xB9, 0x50);
        c.minus = QColor(0xF8, 0x51, 0x49);
    } else {
        c.addLine = QColor(0xDA, 0xFB, 0xE1);
        c.addNumber = QColor(0xAC, 0xEE, 0xBB);
        c.addWord = QColor(0xAC, 0xEE, 0xBB);
        c.delLine = QColor(0xFF, 0xEB, 0xE9);
        c.delNumber = QColor(0xFF, 0xCE, 0xCB);
        c.delWord = QColor(0xFF, 0xCE, 0xCB);
        c.hunkLine = QColor(0xDD, 0xF4, 0xFF);
        c.hunkText = QColor(0x59, 0x63, 0x6E);
        c.plus = QColor(0x1A, 0x7F, 0x37);
        c.minus = QColor(0xD1, 0x24, 0x2F);
    }
    c.squareNeutral = GitDiffPalette::blend(canvas, QColor(c.text.red(), c.text.green(), c.text.blue(), 64));
    c.header = shifted(canvas, dark, 6);
    c.rule = GitDiffPalette::blend(canvas, QColor(c.text.red(), c.text.green(), c.text.blue(), 48));
    c.link = p.fgModified;
    c.statusAdded = p.fgAdded;
    c.statusModified = p.fgModified;
    c.statusDeleted = p.fgMinus;
    c.statusRenamed = p.fgRenamed;
    return c;
}
