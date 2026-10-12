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

#ifndef ACP_AGENT_ICONS_H
#define ACP_AGENT_ICONS_H

#include <QIcon>
#include <QString>

class QPalette;
struct AcpAgentDefinition;

// Icon for a bundled brand key (see AcpAgentBrand.h). Monochrome marks are
// tinted with the palette's ButtonText and OpenCode picks its light or dark
// variant, so call again after a PaletteChange. An empty or unknown key gives
// the generic agent glyph. GUI thread only (results are cached).
QIcon acpAgentBrandIcon(const QString &brandKey, const QPalette &palette);

// Icon for an agent: its custom image file when `icon` is a readable path,
// otherwise the explicit or detected brand mark, otherwise the generic glyph.
QIcon acpAgentIcon(const AcpAgentDefinition &def, const QPalette &palette);

#endif // ACP_AGENT_ICONS_H
