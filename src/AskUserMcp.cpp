/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "AskUserMcp.h"

#include "AskUserForm.h"
#include "McpStdio.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>

namespace {

QJsonArray tools()
{
    return QJsonArray{AskUserForm::toolDefinition()};
}

QString invalidInputText(const QString &reason)
{
    return QStringLiteral("Invalid ask_user input: %1. Fix it and call ask_user again.").arg(reason);
}

QJsonObject call(const QJsonObject &params)
{
    const QString name = params.value(QStringLiteral("name")).toString();
    if (name != QLatin1String(AskUserForm::kToolName))
        return McpStdio::textResult(QStringLiteral("Unknown tool: %1").arg(name), true);
    QString error;
    if (!AskUserForm::parseAskUserInput(params.value(QStringLiteral("arguments")).toObject(), &error))
        return McpStdio::textResult(invalidInputText(error), true);
    return McpStdio::textResult(QLatin1String(AskUserMcp::kShownGuide), false);
}

const McpStdio::Server kServer{AskUserForm::kServerName, &tools, &call};

// Structural check only; the chat re-validates with parseAskUserInput.
constexpr char kNodeBody[] = R"JS(
const TYPES = ["single","multi","select","proposal","confirm","text","textarea","number"];
const CHOICE = ["single","multi","select"];
function check(args) {
  let qs = args && args.questions;
  if (typeof qs === "string") { try { qs = JSON.parse(qs); } catch (e) {} }
  if (!Array.isArray(qs)) return '"questions" must be an array of question objects';
  if (qs.length === 0) return '"questions" is empty';
  if (qs.length > CFG.max) return "at most " + CFG.max + " questions per call; split the round";
  for (let i = 0; i < qs.length; i++) {
    const q = qs[i];
    if (!q || typeof q !== "object" || Array.isArray(q)) return "question " + (i + 1) + " is not an object";
    const id = q.id != null && String(q.id).trim() ? String(q.id).trim() : "Q" + (i + 1);
    const type = q.type == null || q.type === "" ? "single" : String(q.type).toLowerCase();
    if (!q.title && !q.body) return 'question "' + id + '": title is missing';
    if (TYPES.indexOf(type) < 0) return 'question "' + id + '": unknown type "' + q.type + '"';
    if (CHOICE.indexOf(type) >= 0 && (!Array.isArray(q.options) || q.options.length === 0))
      return 'question "' + id + '": type ' + type + " needs a non-empty options array";
  }
  return "";
}
function text(t, isError) { return { content: [{ type: "text", text: t }], isError: isError }; }
function reply(id, result) { return { jsonrpc: "2.0", id: id, result: result }; }
function handle(m) {
  const id = m.id;
  const p = m.params || {};
  switch (m.method) {
  case "initialize":
    return reply(id, { protocolVersion: p.protocolVersion || "2025-11-25", capabilities: { tools: {} },
                       serverInfo: { name: CFG.name, version: "1" } });
  case "tools/list": return reply(id, { tools: [CFG.tool] });
  case "tools/call": {
    if (p.name !== CFG.tool.name) return reply(id, text("Unknown tool: " + p.name, true));
    const err = check(p.arguments);
    return reply(id, err ? text("Invalid ask_user input: " + err + ". Fix it and call ask_user again.", true)
                         : text(CFG.guide, false));
  }
  case "ping": return reply(id, {});
  }
  if (id === undefined || id === null) return null;
  return { jsonrpc: "2.0", id: id, error: { code: -32601, message: "Method not found" } };
}
let buf = "";
process.stdin.setEncoding("utf8");
process.stdin.on("data", function (chunk) {
  buf += chunk;
  let nl;
  while ((nl = buf.indexOf("\n")) >= 0) {
    const line = buf.slice(0, nl).trim();
    buf = buf.slice(nl + 1);
    if (!line) continue;
    let msg;
    try { msg = JSON.parse(line); } catch (e) { continue; }
    const out = handle(msg);
    if (out) process.stdout.write(JSON.stringify(out) + "\n");
  }
});
)JS";

} // namespace

QJsonObject AskUserMcp::handle(const QJsonObject &msg)
{
    return McpStdio::dispatch(kServer, msg);
}

int AskUserMcp::run()
{
    return McpStdio::run(kServer);
}

QString AskUserMcp::nodeScript()
{
    const QJsonObject cfg{
        {QStringLiteral("name"), QLatin1String(AskUserForm::kServerName)},
        {QStringLiteral("tool"), AskUserForm::toolDefinition()},
        {QStringLiteral("guide"), QLatin1String(kShownGuide)},
        {QStringLiteral("max"), AskUserForm::kMaxQuestions},
    };
    return QStringLiteral("\"use strict\";\nconst CFG = ")
        + QString::fromUtf8(QJsonDocument(cfg).toJson(QJsonDocument::Compact))
        + QStringLiteral(";") + QString::fromUtf8(kNodeBody);
}

QJsonObject AskUserMcp::serverEntry(bool remote)
{
    QJsonObject entry{
        {QStringLiteral("name"), QLatin1String(AskUserForm::kServerName)},
        // ACP requires an array here; strict agents reject a missing env.
        {QStringLiteral("env"), QJsonArray{}},
    };
    if (remote) {
        entry.insert(QStringLiteral("command"), QStringLiteral("/bin/sh"));
        entry.insert(QStringLiteral("args"), QJsonArray{
            QStringLiteral("-c"),
            QStringLiteral("command -v node >/dev/null 2>&1 || exit 127; exec node -e \"$0\""),
            nodeScript(),
        });
    } else {
        entry.insert(QStringLiteral("command"), QCoreApplication::applicationFilePath());
        entry.insert(QStringLiteral("args"), QJsonArray{QLatin1String(kCliFlag)});
    }
    return entry;
}
