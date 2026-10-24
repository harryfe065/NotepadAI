/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "Pm2Entrypoint.h"

#include "DotEnv.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace {

constexpr int kFormatVersion = 1;
constexpr char kHeader[] =
    "/* NotepadAI PM2 entrypoint v1. Written by Terminal > PM2 Processes; edit it there. */";
constexpr char kDefPrefix[] = "const def = ";

// Everything after the definition line. Static: the definition is the only
// part that changes, so a process's behaviour is fully described by it.
constexpr char kBody[] = R"JS('use strict';
const { spawn, spawnSync } = require('child_process');
const isWin = process.platform === 'win32';

function commandLine(shell, command) {
  const ps = ['-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-Command', command];
  switch (shell) {
    case 'powershell': return { file: 'powershell.exe', args: ps };
    case 'pwsh': return { file: isWin ? 'pwsh.exe' : 'pwsh', args: ps };
    case 'bash': return { file: 'bash', args: ['-c', command] };
    case 'sh': return { file: 'sh', args: ['-c', command] };
    case 'cmd': break;
    default: if (!isWin) return { file: '/bin/sh', args: ['-c', command] };
  }
  // Same form node uses for shell: true; /s strips exactly the outer quotes.
  return { file: process.env.ComSpec || 'cmd.exe', args: ['/d', '/s', '/c', '"' + command + '"'], verbatim: true };
}

const line = commandLine(def.shell, def.command);
const child = spawn(line.file, line.args, {
  cwd: def.cwd || undefined,
  env: Object.assign({}, process.env, def.env),
  stdio: 'inherit',
  windowsHide: true,
  windowsVerbatimArguments: !!line.verbatim,
  // POSIX: own process group, so stopping kills everything the command started.
  detached: !isWin,
});

let stopping = false;
function killTree(signal) {
  if (!child.pid || child.exitCode !== null || child.signalCode !== null) return;
  if (isWin) {
    spawnSync('taskkill', ['/pid', String(child.pid), '/T', '/F'], { stdio: 'ignore', windowsHide: true });
    return;
  }
  try { process.kill(-child.pid, signal); } catch (e) { try { child.kill(signal); } catch (e2) {} }
}

for (const sig of ['SIGINT', 'SIGTERM', 'SIGHUP']) {
  process.on(sig, () => { stopping = true; killTree(sig === 'SIGHUP' ? 'SIGTERM' : sig); });
}
process.on('exit', () => killTree('SIGKILL'));

child.on('error', (err) => {
  console.error('Could not start the command: ' + err.message);
  process.exit(127);
});
child.on('exit', (code) => process.exit(code !== null ? code : (stopping ? 0 : 1)));
)JS";

Pm2Options optionsFromJson(const QJsonObject &o)
{
    Pm2Options out;
    out.autorestart = o.value(QStringLiteral("autorestart")).toBool(true);
    out.watch = o.value(QStringLiteral("watch")).toBool(false);
    out.maxMemoryRestart = o.value(QStringLiteral("maxMemoryRestart")).toString();
    out.restartDelayMs = o.value(QStringLiteral("restartDelayMs")).toInt(0);
    return out;
}

} // namespace

QJsonObject pm2OptionsToJson(const Pm2Options &o)
{
    return QJsonObject{
        {QStringLiteral("autorestart"), o.autorestart},
        {QStringLiteral("watch"), o.watch},
        {QStringLiteral("maxMemoryRestart"), o.maxMemoryRestart},
        {QStringLiteral("restartDelayMs"), o.restartDelayMs},
    };
}

namespace Pm2Entrypoint {

QByteArray generate(const Pm2Definition &def)
{
    QJsonObject env;
    for (const auto &[key, value] : parseDotEnv(def.envText))
        env.insert(key, value);

    const QJsonObject json{
        {QStringLiteral("v"), kFormatVersion},
        {QStringLiteral("name"), def.name},
        {QStringLiteral("command"), def.command},
        {QStringLiteral("shell"), def.shell},
        {QStringLiteral("cwd"), def.cwd},
        {QStringLiteral("envText"), def.envText},
        {QStringLiteral("env"), env},
        {QStringLiteral("options"), pm2OptionsToJson(def.options)},
    };

    // Compact JSON escapes every control character, so the definition is one
    // line, and JSON is a valid JS expression (Node >= 10 accepts U+2028/9).
    QByteArray out;
    const QByteArray defJson = QJsonDocument(json).toJson(QJsonDocument::Compact);
    out.reserve(int(sizeof(kHeader) + sizeof(kDefPrefix) + sizeof(kBody)) + defJson.size() + 4);
    out.append(kHeader).append('\n');
    out.append(kDefPrefix).append(defJson).append(";\n");
    out.append(kBody);
    return out;
}

std::optional<Pm2Definition> parse(const QByteArray &text)
{
    const qsizetype firstEnd = text.indexOf('\n');
    if (firstEnd < 0)
        return std::nullopt;
    QByteArrayView first(text.constData(), firstEnd);
    if (first.endsWith('\r'))
        first.chop(1);
    if (first != QByteArrayView(kHeader))
        return std::nullopt;

    qsizetype secondEnd = text.indexOf('\n', firstEnd + 1);
    if (secondEnd < 0)
        secondEnd = text.size();
    QByteArrayView second(text.constData() + firstEnd + 1, secondEnd - firstEnd - 1);
    if (second.endsWith('\r'))
        second.chop(1);
    if (!second.startsWith(kDefPrefix) || !second.endsWith(';'))
        return std::nullopt;
    second = second.sliced(qsizetype(sizeof(kDefPrefix) - 1));
    second.chop(1);

    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(second.toByteArray(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return std::nullopt;
    const QJsonObject json = doc.object();
    if (json.value(QStringLiteral("v")).toInt() != kFormatVersion)
        return std::nullopt;

    Pm2Definition def;
    def.name = json.value(QStringLiteral("name")).toString();
    def.command = json.value(QStringLiteral("command")).toString();
    if (def.name.isEmpty() || def.command.isEmpty())
        return std::nullopt;
    def.shell = json.value(QStringLiteral("shell")).toString();
    def.cwd = json.value(QStringLiteral("cwd")).toString();
    def.envText = json.value(QStringLiteral("envText")).toString();
    def.options = optionsFromJson(json.value(QStringLiteral("options")).toObject());
    return def;
}

} // namespace Pm2Entrypoint
