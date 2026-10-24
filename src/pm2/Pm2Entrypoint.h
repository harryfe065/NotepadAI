/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef PM2_ENTRYPOINT_H
#define PM2_ENTRYPOINT_H

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <optional>

// pm2's own options for a process. Changing any of them recreates the process
// (pm2 cannot change them on restart).
struct Pm2Options
{
    bool autorestart = true;
    bool watch = false;
    QString maxMemoryRestart; // pm2 syntax ("512M"); empty = none
    int restartDelayMs = 0;

    bool operator==(const Pm2Options &) const = default;
};

// The keys both the entrypoint and the helper protocol use for Pm2Options.
QJsonObject pm2OptionsToJson(const Pm2Options &options);

// A PM2 process NotepadAI created: everything its entrypoint needs.
struct Pm2Definition
{
    QString name;
    QString command;
    // "" = the system shell (cmd.exe / /bin/sh), else "cmd", "powershell",
    // "pwsh", "bash" or "sh".
    QString shell;
    QString cwd;     // absolute
    QString envText; // raw .env text, kept as typed so Edit shows it back
    Pm2Options options;

    bool operator==(const Pm2Definition &) const = default;
};

// The entrypoint is a self-contained CommonJS script pm2 runs with node: it
// spawns the command through the chosen shell in `cwd` with the env applied,
// exits with the command's exit code and kills the command's process tree
// when pm2 stops it. Its second line carries the definition as JSON so Edit
// reads it back; nothing else is stored anywhere.
namespace Pm2Entrypoint {

QByteArray generate(const Pm2Definition &def);
// nullopt when `text` is not an entrypoint NotepadAI wrote.
std::optional<Pm2Definition> parse(const QByteArray &text);

} // namespace Pm2Entrypoint

#endif // PM2_ENTRYPOINT_H
