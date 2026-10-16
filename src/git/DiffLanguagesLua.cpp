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

#include "DiffLanguagesLua.h"

#include "LuaState.h"
#include "NotepadNextApplication.h"

#include <QCoreApplication>
#include <QThread>

namespace DiffLanguages {

std::shared_ptr<const Catalog> catalog()
{
    Q_ASSERT(QCoreApplication::instance()->thread() == QThread::currentThread());
    static std::shared_ptr<const Catalog> cached;
    if (cached) return cached;

    auto *app = qobject_cast<NotepadNextApplication *>(qApp);
    LuaState *lua = app ? app->getLuaState() : nullptr;
    if (!lua) return std::make_shared<const Catalog>(QVector<LanguageDef>{});

    // Fields \31, list items \29, languages \30 (Catalog::fromSerialized).
    const QString data = lua->executeAndReturn<QString>(R"(
    local F, I, R = "\31", "\29", "\30"
    local function list(t)
        if not t then return "" end
        local o = {}
        for _, v in ipairs(t) do o[#o + 1] = tostring(v) end
        return table.concat(o, I)
    end
    local recs = {}
    for name, L in pairs(languages) do
        local styles, kws, props = {}, {}, {}
        if L.styles then
            for _, s in pairs(L.styles) do
                if s.id then
                    styles[#styles + 1] = tostring(s.id) .. "," .. tostring(s.fgColor or 0) .. ","
                                          .. tostring(s.fontStyle or 0)
                end
            end
        end
        if L.keywords then
            for id, kw in pairs(L.keywords) do kws[#kws + 1] = tostring(id) .. "=" .. tostring(kw) end
        end
        if L.properties then
            for k, v in pairs(L.properties) do props[#props + 1] = tostring(k) .. "=" .. tostring(v) end
        end
        recs[#recs + 1] = table.concat({name, L.lexer or "", list(L.filenames), list(L.filename_patterns),
                                        list(L.extensions), table.concat(styles, I), table.concat(kws, I),
                                        table.concat(props, I)}, F)
    end
    return table.concat(recs, R)
    )");
    cached = Catalog::fromSerialized(data.toUtf8());
    return cached;
}

} // namespace DiffLanguages
