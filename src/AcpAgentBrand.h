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

#ifndef ACP_AGENT_BRAND_H
#define ACP_AGENT_BRAND_H

#include <QLatin1String>
#include <QString>
#include <QStringView>

#include <array>
#include <utility>

#include "AcpAgentDefinition.h"

// Bundled agent marks. `AcpAgentDefinition::icon` holds one of these keys, a
// path to an image file, or nothing (detect the brand from the definition).
// Pure Qt Core so the detection is unit-testable without a GUI.
struct AcpAgentBrandInfo
{
    const char *key;
    const char *displayName;
};

// Order is the picker order and the detection priority: "oh-my-pi" must be
// tested before "pi", and "claude"/"codex" win over everything else.
inline constexpr std::array<AcpAgentBrandInfo, 5> kAcpAgentBrands{{
    {"claude-code", "Claude Code"},
    {"codex", "Codex"},
    {"opencode", "OpenCode"},
    {"oh-my-pi", "Oh My Pi"},
    {"pi", "Pi"},
}};

inline const std::array<AcpAgentBrandInfo, 5> &acpAgentBrands()
{
    return kAcpAgentBrands;
}

inline bool isAcpAgentBrandKey(const QString &key)
{
    for (const AcpAgentBrandInfo &info : kAcpAgentBrands) {
        if (key == QLatin1String(info.key)) {
            return true;
        }
    }
    return false;
}

namespace acpagentbrand_detail {

// True when `token` appears in `hay` as a whole alphanumeric run, so "pi"
// matches "pi-acp" and "C:\tools\pi.exe" but not "pipx" or "spinner".
inline bool hasToken(QStringView hay, QLatin1String token)
{
    const qsizetype n = hay.size();
    qsizetype i = 0;
    while (i < n) {
        while (i < n && !hay.at(i).isLetterOrNumber()) {
            ++i;
        }
        const qsizetype start = i;
        while (i < n && hay.at(i).isLetterOrNumber()) {
            ++i;
        }
        if (i - start == token.size() && hay.mid(start, i - start) == token) {
            return true;
        }
    }
    return false;
}

// Substring search that ignores '-', '_' and whitespace in `hay`, so
// "open code", "open-code" and "opencode" all match "opencode".
inline bool containsSquashed(QStringView hay, QLatin1String needle)
{
    const qsizetype n = hay.size();
    const qsizetype m = needle.size();
    for (qsizetype start = 0; start < n; ++start) {
        qsizetype j = 0;
        for (qsizetype i = start; i < n && j < m; ++i) {
            const QChar c = hay.at(i);
            if (c == QLatin1Char('-') || c == QLatin1Char('_') || c.isSpace()) {
                if (j == 0) {
                    break;
                }
                continue;
            }
            if (c != needle.at(j)) {
                break;
            }
            ++j;
        }
        if (j == m) {
            return true;
        }
    }
    return false;
}

} // namespace acpagentbrand_detail

// Brand guessed from id, name, command and args. Empty when nothing matches.
inline QString detectAcpAgentBrand(const AcpAgentDefinition &def)
{
    using namespace acpagentbrand_detail;

    qsizetype len = def.id.size() + def.name.size() + def.command.size() + 3;
    for (const QString &arg : def.args) {
        len += arg.size() + 1;
    }
    QString hay;
    hay.reserve(len);
    hay += def.id;
    hay += QLatin1Char(' ');
    hay += def.name;
    hay += QLatin1Char(' ');
    hay += def.command;
    for (const QString &arg : def.args) {
        hay += QLatin1Char(' ');
        hay += arg;
    }
    hay = std::move(hay).toLower();

    if (hay.contains(QLatin1String("claude"))) {
        return QStringLiteral("claude-code");
    }
    if (hay.contains(QLatin1String("codex"))) {
        return QStringLiteral("codex");
    }
    if (containsSquashed(hay, QLatin1String("opencode"))) {
        return QStringLiteral("opencode");
    }
    if (containsSquashed(hay, QLatin1String("ohmypi")) || hasToken(hay, QLatin1String("omp"))) {
        return QStringLiteral("oh-my-pi");
    }
    if (hasToken(hay, QLatin1String("pi"))) {
        return QStringLiteral("pi");
    }
    return QString();
}

// Effective bundled mark: an explicit brand key wins, an empty icon falls back
// to detection, and a custom image path has no brand.
inline QString acpAgentBrandKey(const AcpAgentDefinition &def)
{
    const QString icon = def.icon.trimmed();
    if (icon.isEmpty()) {
        return detectAcpAgentBrand(def);
    }
    return isAcpAgentBrandKey(icon) ? icon : QString();
}

#endif // ACP_AGENT_BRAND_H
