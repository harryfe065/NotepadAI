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

#ifndef DIFF_VIEW_APP_H
#define DIFF_VIEW_APP_H

class ApplicationSettings;
class QString;
class ScintillaNext;

// The diff view's link to the application, in its own translation unit so
// tests and benches can link DiffView without NotepadNextApplication.
namespace DiffViewApp {

// The application settings, or null outside the application.
ApplicationSettings *settings();

// Colours `editor` for the language its file extension names (no-op outside
// the application).
void applyLanguageForPath(ScintillaNext *editor, const QString &relPath);

} // namespace DiffViewApp

#endif // DIFF_VIEW_APP_H
