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

#include "PromptAssembler.h"

#include <QRegularExpression>

namespace ai {

QString PromptAssembler::defaultTemplate()
{
    return QStringLiteral(
        "You are an expert at writing git commit messages in the Conventional Commits format.\n"
        "\n"
        "Generate a commit message for the following changes. Follow these rules strictly:\n"
        "- Header: <type>(<scope>): <subject>, no more than 50 characters in total, imperative mood, no trailing period.\n"
        "- Body (optional): explain WHY, not WHAT. Wrap at 72 characters. Use bullet points if multiple concerns.\n"
        "- No trailing whitespace.\n"
        "- Deliver the message ONLY by calling the submit_commit_message tool (type, optional scope, subject, optional body). Never write it as text.\n"
        "\n"
        "{{rules}}\n"
        "{{subject_hint}}\n"
        "Diff:\n"
        "```\n"
        "{{diff}}\n"
        "```\n");
}

QString PromptAssembler::renderRulesBlock(const QString &rulesContent)
{
    const QString trimmed = rulesContent.trimmed();
    if (trimmed.isEmpty()) return QString();
    return QStringLiteral("Project rules (follow these):\n%1\n").arg(trimmed);
}

QString PromptAssembler::renderSubjectHintBlock(const QString &subjectLine)
{
    const QString trimmed = subjectLine.trimmed();
    if (trimmed.isEmpty()) return QString();
    return QStringLiteral(
        "The user has typed the draft below. Treat it as guidance: keep its intent and reuse its "
        "wording for the subject where it fits. Your tool call replaces the draft.\n"
        "Draft: %1\n").arg(trimmed);
}

QString PromptAssembler::renderDiffBlock(const QByteArray &compressedDiff)
{
    return QString::fromUtf8(compressedDiff);
}

QString PromptAssembler::assemble(const QString &templateBody,
                                  const QString &rulesContent,
                                  const QString &subjectLine,
                                  const QByteArray &compressedDiff)
{
    QString out = templateBody;
    out.replace(QLatin1String("{{rules}}"),        renderRulesBlock(rulesContent));
    out.replace(QLatin1String("{{subject_hint}}"), renderSubjectHintBlock(subjectLine));
    out.replace(QLatin1String("{{diff}}"),         renderDiffBlock(compressedDiff));

    // Collapse runs of 3+ newlines to exactly 2 so empty sections don't leave
    // visible blank gaps in the rendered prompt.
    static const QRegularExpression kBlankRuns(QStringLiteral("\n{3,}"));
    out.replace(kBlankRuns, QStringLiteral("\n\n"));

    return out;
}

} // namespace ai
