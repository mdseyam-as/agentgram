/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>

namespace BotStudio {

// A visual bot is a scheme of message blocks (the "flow"), stored as JSON
// and turned into bot code for the regular runtime:
// {
//   start: blockId, buttonsStyle: "inline" | "keyboard",
//   blocks: [{ id, type, title, text, media, x, y, next, delay, notifyOwner,
//     saveChoice, buttons: [{ text, goto | url, value }],
//     input: { enabled, variable, kind, next }, ... }],
//   products: [{ id, title, price, category, description, photo, url }],
//   shop: { currency, delivery, freeFrom, minOrder, ordersChat,
//     starsRate, providerToken },
//   commands: [{ command, description, goto }],
//   keywords: [{ words: "a, b", goto }],
//   fallback: { mode: start|block|text|ai, goto, text, prompt },
// }

struct FlowTemplate {
	QString id;
	QString emoji;
	QString title;
	QString description;
	QJsonObject flow;
};

[[nodiscard]] const std::vector<FlowTemplate> &FlowTemplates();
[[nodiscard]] QJsonObject EmptyFlow();

[[nodiscard]] QString GenerateFlowCode(const QJsonObject &flow);
// Returns a list of problems a user should fix, empty when fine.
[[nodiscard]] QStringList ValidateFlow(const QJsonObject &flow);
[[nodiscard]] QJsonObject NormalizeFlow(QJsonObject flow);

[[nodiscard]] QJsonArray FlowBlocks(const QJsonObject &flow);
[[nodiscard]] QJsonObject FlowBlock(const QJsonObject &flow, const QString &id);
[[nodiscard]] int FlowBlockIndex(const QJsonObject &flow, const QString &id);
void SetFlowBlock(QJsonObject &flow, const QJsonObject &block);
void RemoveFlowBlock(QJsonObject &flow, const QString &id);
[[nodiscard]] QString NewFlowBlockId(const QJsonObject &flow);
[[nodiscard]] QString FlowBlockTitle(const QJsonObject &flow, const QString &id);
// The id the engine uses for a product (its id or, without one, its title).
[[nodiscard]] QString FlowProductKey(const QJsonObject &product);

} // namespace BotStudio
