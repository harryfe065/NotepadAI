#include "GoalVerdictMcp.h"

#include "GoalActionParser.h"
#include "McpStdio.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace {

QJsonObject toolDef()
{
    QJsonObject statusProp{
        {QStringLiteral("type"), QStringLiteral("string")},
        {QStringLiteral("enum"), QJsonArray{QStringLiteral("continue"), QStringLiteral("complete"),
                                           QStringLiteral("restart")}},
        {QStringLiteral("description"),
         QStringLiteral("continue if the criterion is not yet met; complete if it is met; "
                        "restart if the coding-agent session must be restarted.")},
    };
    QJsonObject textProp{
        {QStringLiteral("type"), QStringLiteral("string")},
        {QStringLiteral("description"),
         QStringLiteral("If continue: first-person follow-up to the coding agent. "
                        "If complete: brief reason. If restart: prompt after restart.")},
    };
    QJsonObject schema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("status"), statusProp},
            {QStringLiteral("text"), textProp},
        }},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("status"), QStringLiteral("text")}},
    };
    return QJsonObject{
        {QStringLiteral("name"), QLatin1String(GoalActionParser::kVerdictToolName)},
        {QStringLiteral("description"),
         QStringLiteral("Submit your evaluation, then stop. This ends your turn. "
                        "Do not implement the follow-up yourself.")},
        {QStringLiteral("inputSchema"), schema},
    };
}

QJsonArray tools()
{
    return QJsonArray{toolDef()};
}

QJsonObject call(const QJsonObject &)
{
    return McpStdio::textResult(QLatin1String(GoalVerdictMcp::kStopGuide), false);
}

const McpStdio::Server kServer{"goal-verdict", &tools, &call};

} // namespace

QJsonObject GoalVerdictMcp::handle(const QJsonObject &msg)
{
    return McpStdio::dispatch(kServer, msg);
}

QByteArray GoalVerdictMcp::encodeReply(const QByteArray &jsonRequest, bool newlineFraming)
{
    return McpStdio::encodeReply(kServer, jsonRequest, newlineFraming);
}

int GoalVerdictMcp::run()
{
    return McpStdio::run(kServer);
}
