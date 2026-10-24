/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "DotEnv.h"

#include <QStringView>

QList<QPair<QString, QString>> parseDotEnv(const QString &text)
{
    QList<QPair<QString, QString>> result;
    if (text.isEmpty())
        return result;
    for (const QStringView rawLine : QStringView(text).split(QLatin1Char('\n'))) {
        const QStringView line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        const qsizetype eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;
        const QStringView key = line.left(eq).trimmed();
        if (key.isEmpty())
            continue;
        QStringView value = line.mid(eq + 1).trimmed();
        if (value.size() >= 2) {
            const QChar first = value.front();
            if ((first == QLatin1Char('"') || first == QLatin1Char('\'')) && value.back() == first)
                value = value.mid(1, value.size() - 2);
        }
        result.append({key.toString(), value.toString()});
    }
    return result;
}
