/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef DOTENV_H
#define DOTENV_H

#include <QList>
#include <QPair>
#include <QString>

// .env text as the user types it for terminal tasks and PM2 processes:
// KEY=VALUE per line, `#` comments and blank lines skipped, key and value
// trimmed, one pair of surrounding quotes ('…' or "…") stripped. Lines
// without a key are skipped. Order is kept; a later duplicate wins when the
// pairs are applied in order.
QList<QPair<QString, QString>> parseDotEnv(const QString &text);

#endif // DOTENV_H
