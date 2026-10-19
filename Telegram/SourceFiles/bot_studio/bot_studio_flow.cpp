/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "bot_studio/bot_studio_flow.h"

#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>

namespace BotStudio {
namespace {

[[nodiscard]] QByteArray Resource(const QString &name) {
	auto file = QFile(u":/bot_studio/"_q + name);
	return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

[[nodiscard]] bool HasBlock(const QJsonObject &flow, const QString &id) {
	return FlowBlockIndex(flow, id) >= 0;
}

} // namespace

const std::vector<FlowTemplate> &FlowTemplates() {
	static const auto result = [] {
		auto list = std::vector<FlowTemplate>();
		const auto array = QJsonDocument::fromJson(
			Resource(u"flow_templates.json"_q)).array();
		for (const auto &value : array) {
			const auto object = value.toObject();
			list.push_back({
				.id = object.value(u"id"_q).toString(),
				.emoji = object.value(u"emoji"_q).toString(),
				.title = object.value(u"title"_q).toString(),
				.description = object.value(u"description"_q).toString(),
				.flow = object.value(u"flow"_q).toObject(),
			});
		}
		return list;
	}();
	return result;
}

QJsonObject EmptyFlow() {
	for (const auto &entry : FlowTemplates()) {
		if (entry.id == u"blank"_q) {
			return entry.flow;
		}
	}
	return NormalizeFlow(QJsonObject());
}

QJsonObject NormalizeFlow(QJsonObject flow) {
	flow.insert(u"version"_q, 1);
	auto blocks = FlowBlocks(flow);
	if (blocks.isEmpty()) {
		blocks.push_back(QJsonObject{
			{ u"id"_q, u"start"_q },
			{ u"title"_q, u"Старт"_q },
			{ u"text"_q, u"Привет, {first_name}!"_q },
			{ u"x"_q, 40 },
			{ u"y"_q, 40 },
		});
		flow.insert(u"blocks"_q, blocks);
	}
	const auto start = flow.value(u"start"_q).toString();
	if (start.isEmpty() || !HasBlock(flow, start)) {
		flow.insert(
			u"start"_q,
			blocks.first().toObject().value(u"id"_q).toString());
	}
	if (flow.value(u"buttonsStyle"_q).toString() != u"keyboard"_q) {
		flow.insert(u"buttonsStyle"_q, u"inline"_q);
	}
	if (!flow.value(u"commands"_q).isArray()) {
		flow.insert(u"commands"_q, QJsonArray{ QJsonObject{
			{ u"command"_q, u"start"_q },
			{ u"description"_q, u"Начать"_q },
			{ u"goto"_q, flow.value(u"start"_q).toString() },
		} });
	}
	if (!flow.value(u"keywords"_q).isArray()) {
		flow.insert(u"keywords"_q, QJsonArray());
	}
	if (!flow.value(u"fallback"_q).isObject()) {
		flow.insert(u"fallback"_q, QJsonObject{ { u"mode"_q, u"start"_q } });
	}
	return flow;
}

QString GenerateFlowCode(const QJsonObject &flow) {
	const auto json = QString::fromUtf8(QJsonDocument(
		NormalizeFlow(flow)).toJson(QJsonDocument::Indented)).trimmed();
	return u"// Этот бот собран в визуальном конструкторе Bot Studio.\n"
		"// Код создаётся из схемы автоматически: меняйте бота во вкладке\n"
		"// «Конструктор». Чтобы писать код вручную, переключите бота на код.\n\n"
		"const FLOW = "_q
		+ json
		+ u";\n\n"_q
		+ QString::fromUtf8(Resource(u"flow_engine.js"_q));
}

QStringList ValidateFlow(const QJsonObject &flow) {
	auto result = QStringList();
	const auto blocks = FlowBlocks(flow);
	const auto check = [&](const QString &id, const QString &where) {
		if (!id.isEmpty() && !HasBlock(flow, id)) {
			result.push_back(where + u": переход в удалённый блок"_q);
		}
	};
	for (const auto &value : blocks) {
		const auto block = value.toObject();
		const auto title = block.value(u"title"_q).toString();
		const auto where = u"Блок «%1»"_q.arg(title);
		const auto type = block.value(u"type"_q).toString();
		if (type.isEmpty() || type == u"message"_q) {
			if (block.value(u"text"_q).toString().trimmed().isEmpty()
				&& block.value(u"photo"_q).toString().trimmed().isEmpty()
				&& block.value(u"media"_q).toObject().value(
					u"url"_q).toString().trimmed().isEmpty()) {
				result.push_back(where + u": пустое сообщение"_q);
			}
		} else if (type == u"condition"_q) {
			for (const auto &value : block.value(u"conditions"_q).toArray()) {
				const auto condition = value.toObject();
				if (condition.value(u"goto"_q).toString().isEmpty()) {
					result.push_back(where + u": проверке не выбрана ветка"_q);
				}
				if (condition.value(u"variable"_q).toString().isEmpty()) {
					result.push_back(where + u": в проверке не указан ответ"_q);
				}
				check(condition.value(u"goto"_q).toString(), where);
			}
			check(block.value(u"else"_q).toString(), where);
		} else if (type == u"action"_q) {
			for (const auto &value : block.value(u"actions"_q).toArray()) {
				const auto action = value.toObject();
				const auto kind = action.value(u"kind"_q).toString();
				if ((kind == u"set"_q || kind == u"increment"_q)
					&& action.value(u"variable"_q).toString().isEmpty()) {
					result.push_back(where + u": не указана переменная"_q);
				} else if (kind == u"send"_q
					&& action.value(u"chat"_q).toString().isEmpty()) {
					result.push_back(where + u": не указан чат"_q);
				} else if (kind == u"webhook"_q
					&& !action.value(u"url"_q).toString().startsWith(u"http"_q)) {
					result.push_back(where + u": не указан адрес вебхука"_q);
				}
			}
		}
		for (const auto &button : block.value(u"buttons"_q).toArray()) {
			const auto object = button.toObject();
			const auto text = object.value(u"text"_q).toString();
			if (text.trimmed().isEmpty()) {
				result.push_back(where + u": кнопка без названия"_q);
			} else if (object.value(u"url"_q).toString().isEmpty()
				&& object.value(u"goto"_q).toString().isEmpty()) {
				result.push_back(where
					+ u": кнопке «%1» не выбрано действие"_q.arg(text));
			}
			check(object.value(u"goto"_q).toString(), where);
		}
		check(block.value(u"next"_q).toString(), where);
		const auto input = block.value(u"input"_q).toObject();
		if (input.value(u"enabled"_q).toBool()) {
			check(input.value(u"next"_q).toString(), where);
		}
	}
	return result;
}

QJsonArray FlowBlocks(const QJsonObject &flow) {
	return flow.value(u"blocks"_q).toArray();
}

int FlowBlockIndex(const QJsonObject &flow, const QString &id) {
	const auto blocks = FlowBlocks(flow);
	for (auto i = 0; i != blocks.size(); ++i) {
		if (blocks[i].toObject().value(u"id"_q).toString() == id) {
			return i;
		}
	}
	return -1;
}

QJsonObject FlowBlock(const QJsonObject &flow, const QString &id) {
	const auto index = FlowBlockIndex(flow, id);
	return (index >= 0)
		? FlowBlocks(flow)[index].toObject()
		: QJsonObject();
}

void SetFlowBlock(QJsonObject &flow, const QJsonObject &block) {
	auto blocks = FlowBlocks(flow);
	const auto index = FlowBlockIndex(
		flow,
		block.value(u"id"_q).toString());
	if (index >= 0) {
		blocks[index] = block;
	} else {
		blocks.push_back(block);
	}
	flow.insert(u"blocks"_q, blocks);
}

void RemoveFlowBlock(QJsonObject &flow, const QString &id) {
	auto blocks = FlowBlocks(flow);
	const auto index = FlowBlockIndex(flow, id);
	if (index < 0) {
		return;
	}
	blocks.removeAt(index);
	// Drop links that pointed to the removed block.
	for (auto i = 0; i != blocks.size(); ++i) {
		auto block = blocks[i].toObject();
		if (block.value(u"next"_q).toString() == id) {
			block.remove(u"next"_q);
		}
		auto buttons = block.value(u"buttons"_q).toArray();
		for (auto j = 0; j != buttons.size(); ++j) {
			auto button = buttons[j].toObject();
			if (button.value(u"goto"_q).toString() == id) {
				button.remove(u"goto"_q);
				buttons[j] = button;
			}
		}
		block.insert(u"buttons"_q, buttons);
		auto input = block.value(u"input"_q).toObject();
		if (input.value(u"next"_q).toString() == id) {
			input.remove(u"next"_q);
			block.insert(u"input"_q, input);
		}
		if (block.value(u"else"_q).toString() == id) {
			block.remove(u"else"_q);
		}
		auto conditions = block.value(u"conditions"_q).toArray();
		for (auto j = 0; j != conditions.size(); ++j) {
			auto condition = conditions[j].toObject();
			if (condition.value(u"goto"_q).toString() == id) {
				condition.remove(u"goto"_q);
				conditions[j] = condition;
			}
		}
		if (!conditions.isEmpty()) {
			block.insert(u"conditions"_q, conditions);
		}
		blocks[i] = block;
	}
	flow.insert(u"blocks"_q, blocks);
	if (flow.value(u"start"_q).toString() == id) {
		flow.insert(
			u"start"_q,
			blocks.isEmpty()
				? QString()
				: blocks.first().toObject().value(u"id"_q).toString());
	}
	auto commands = flow.value(u"commands"_q).toArray();
	for (auto i = 0; i != commands.size(); ++i) {
		auto command = commands[i].toObject();
		if (command.value(u"goto"_q).toString() == id) {
			command.insert(u"goto"_q, flow.value(u"start"_q).toString());
			commands[i] = command;
		}
	}
	flow.insert(u"commands"_q, commands);
}

QString NewFlowBlockId(const QJsonObject &flow) {
	for (auto i = FlowBlocks(flow).size() + 1;; ++i) {
		const auto id = u"b%1"_q.arg(i);
		if (!HasBlock(flow, id)) {
			return id;
		}
	}
}

QString FlowBlockTitle(const QJsonObject &flow, const QString &id) {
	const auto block = FlowBlock(flow, id);
	const auto title = block.value(u"title"_q).toString().trimmed();
	if (!title.isEmpty()) {
		return title;
	}
	const auto text = block.value(u"text"_q).toString().simplified();
	return text.isEmpty() ? id : text.left(24);
}

} // namespace BotStudio
