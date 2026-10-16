/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace AiAgent {

// A small subset of Markdown that language models produce: headings,
// lists, quotes, fenced code, inline code, emphasis and links.
[[nodiscard]] TextWithEntities MarkdownToEntities(const QString &markdown);

} // namespace AiAgent
