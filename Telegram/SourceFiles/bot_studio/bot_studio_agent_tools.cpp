/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "bot_studio/bot_studio_agent_tools.h"

#include "base/call_delayed.h"
#include "bot_studio/bot_studio_flow.h"
#include "bot_studio/bot_studio_manager.h"
#include "bot_studio/bot_studio_window.h"
#include "main/main_session.h"
#include "window/window_session_controller.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>

namespace BotStudio {
namespace {

using AiAgent::ToolCall;
using AiAgent::ToolDefinition;
using AiAgent::ToolPresentation;
using AiAgent::ToolResult;

constexpr auto kDefaultLogs = 40;
constexpr auto kMaxLogs = 200;
constexpr auto kMaxStorageText = 20000;

[[nodiscard]] QJsonObject Prop(const QString &type, const QString &about) {
	return QJsonObject{
		{ u"type"_q, type },
		{ u"description"_q, about },
	};
}

[[nodiscard]] QJsonObject Schema(
		std::vector<std::pair<QString, QJsonObject>> properties,
		QStringList required = {}) {
	auto props = QJsonObject();
	for (auto &[name, value] : properties) {
		props.insert(name, value);
	}
	return QJsonObject{
		{ u"type"_q, u"object"_q },
		{ u"properties"_q, props },
		{ u"required"_q, QJsonArray::fromStringList(required) },
	};
}

[[nodiscard]] QJsonObject BotProp() {
	return Prop(
		u"string"_q,
		u"Bot id from bot_studio_list, or the bot @username."_q);
}

[[nodiscard]] ToolResult Ok(const QJsonObject &object) {
	return { QString::fromUtf8(
		QJsonDocument(object).toJson(QJsonDocument::Compact)) };
}

[[nodiscard]] ToolResult Fail(const QString &text) {
	return { text, true };
}

[[nodiscard]] QString LevelName(LogLevel level) {
	switch (level) {
	case LogLevel::Info: return u"log"_q;
	case LogLevel::Warning: return u"warn"_q;
	case LogLevel::Error: return u"error"_q;
	case LogLevel::Incoming: return u"update"_q;
	case LogLevel::System: return u"studio"_q;
	}
	return QString();
}

[[nodiscard]] QJsonArray Logs(
		not_null<Studio*> studio,
		const QString &id,
		int limit,
		bool errorsOnly = false,
		size_t skip = 0) {
	const auto &all = studio->logs(id);
	auto selected = std::vector<const LogEntry*>();
	for (auto i = skip; i < all.size(); ++i) {
		const auto &entry = all[i];
		if (!errorsOnly
			|| entry.level == LogLevel::Error
			|| entry.level == LogLevel::Warning) {
			selected.push_back(&entry);
		}
	}
	if (int(selected.size()) > limit) {
		selected.erase(
			selected.begin(),
			selected.end() - limit);
	}
	auto result = QJsonArray();
	for (const auto entry : selected) {
		result.push_back(entry->when.toString(u"HH:mm:ss"_q)
			+ ' '
			+ LevelName(entry->level)
			+ ": "
			+ entry->text);
	}
	return result;
}

[[nodiscard]] QString StatusName(BotStatus status) {
	switch (status) {
	case BotStatus::Stopped: return u"stopped"_q;
	case BotStatus::Starting: return u"starting"_q;
	case BotStatus::Running: return u"running"_q;
	case BotStatus::Error: return u"error"_q;
	}
	return QString();
}

[[nodiscard]] QJsonObject Describe(
		not_null<Studio*> studio,
		const BotInfo &bot) {
	auto result = QJsonObject{
		{ u"bot"_q, bot.id },
		{ u"name"_q, bot.name },
		{ u"username"_q, u"@"_q + bot.username },
		{ u"status"_q, StatusName(studio->status(bot.id)) },
		{ u"autostart"_q, bot.enabled },
		{ u"mode"_q, bot.visual() ? u"visual"_q : u"code"_q },
		{ u"code_lines"_q, int(bot.code.count('\n') + 1) },
		{ u"errors_in_log"_q, studio->errorCount(bot.id) },
		{ u"link"_q, u"https://t.me/"_q + bot.username },
	};
	const auto error = studio->statusError(bot.id);
	if (!error.isEmpty()) {
		result.insert(u"status_error"_q, error);
	}
	return result;
}

[[nodiscard]] const BotInfo *Resolve(
		not_null<Studio*> studio,
		const QJsonObject &args,
		QString *error) {
	const auto query = args.value(u"bot"_q).toVariant().toString();
	const auto result = studio->find(query);
	if (!result) {
		*error = studio->bots().empty()
			? u"There are no bots in Bot Studio yet. Create one with "
				"bot_studio_create."_q
			: u"Bot not found: %1. Use bot_studio_list."_q.arg(query);
	}
	return result;
}

// Waits for the start and returns the state with the logs since `skip`.
void ReportAfterStart(
		not_null<Studio*> studio,
		const QString &id,
		size_t skip,
		QJsonObject extra,
		Fn<void(ToolResult)> done) {
	const auto weak = base::make_weak(studio);
	studio->whenStarted(id, [=] {
		// Give the top-level code and first API calls a moment to log.
		base::call_delayed(1500, [=] {
			const auto strong = weak.get();
			const auto bot = strong ? strong->bot(id) : nullptr;
			if (!bot) {
				done(Fail(u"The bot was removed."_q));
				return;
			}
			auto result = Describe(strong, *bot);
			for (auto i = extra.begin(); i != extra.end(); ++i) {
				result.insert(i.key(), i.value());
			}
			result.insert(u"log"_q, Logs(strong, id, kDefaultLogs, false, std::min(skip, strong->logs(id).size())));
			done(Ok(result));
		});
	});
}

} // namespace

bool IsAgentTool(const QString &name) {
	return name.startsWith(u"bot_studio_"_q);
}

std::vector<ToolDefinition> AgentToolDefinitions() {
	auto result = std::vector<ToolDefinition>();
	const auto add = [&](QString name, QString about, QJsonObject schema) {
		result.push_back({ name, about, schema });
	};
	add(u"bot_studio_docs"_q,
		u"Full reference of the Bot Studio JavaScript bot API. Call it "
		"before writing or changing bot code."_q,
		Schema({}));
	add(u"bot_studio_list"_q,
		u"List bots in Bot Studio with their status."_q,
		Schema({}));
	add(u"bot_studio_create"_q,
		u"Add a Telegram bot to Bot Studio by its BotFather token, with a "
		"visual scheme (flow, preferred for simple bots: the user can edit "
		"it in the visual constructor) or JavaScript code, and start it. "
		"The bot then runs inside Agentgram, no deploy needed."_q,
		Schema({
			{ u"token"_q, Prop(u"string"_q, u"Bot token from @BotFather."_q) },
			{ u"flow"_q, Prop(u"object"_q, u"Visual scheme, see bot_studio_docs. Use either flow or code."_q) },
			{ u"code"_q, Prop(u"string"_q, u"Bot JavaScript code. If both are empty, a starter template is used."_q) },
			{ u"start"_q, Prop(u"boolean"_q, u"Start the bot right away, default true."_q) },
		}, { u"token"_q }));
	add(u"bot_studio_get"_q,
		u"Get a bot's full code, status and recent log."_q,
		Schema({ { u"bot"_q, BotProp() } }, { u"bot"_q }));
	add(u"bot_studio_set_code"_q,
		u"Replace the whole code of a bot. The code is syntax-checked; a "
		"running bot restarts with it. Returns the status and the log "
		"after the restart, check it for errors."_q,
		Schema({
			{ u"bot"_q, BotProp() },
			{ u"code"_q, Prop(u"string"_q, u"Complete new JavaScript code of the bot."_q) },
		}, { u"bot"_q, u"code"_q }));
	add(u"bot_studio_set_flow"_q,
		u"Replace the visual scheme (flow) of a bot; switches a code bot "
		"to the visual constructor. The scheme is validated; a running bot "
		"restarts. Returns the status, problems and the log after restart."_q,
		Schema({
			{ u"bot"_q, BotProp() },
			{ u"flow"_q, Prop(u"object"_q, u"Complete scheme: start, blocks, commands, keywords, fallback, buttonsStyle."_q) },
		}, { u"bot"_q, u"flow"_q }));
	add(u"bot_studio_start"_q,
		u"Start a bot (it also starts with Agentgram from now on)."_q,
		Schema({ { u"bot"_q, BotProp() } }, { u"bot"_q }));
	add(u"bot_studio_stop"_q,
		u"Stop a bot."_q,
		Schema({ { u"bot"_q, BotProp() } }, { u"bot"_q }));
	add(u"bot_studio_logs"_q,
		u"Read a bot's log: incoming updates, console output, errors."_q,
		Schema({
			{ u"bot"_q, BotProp() },
			{ u"limit"_q, Prop(u"integer"_q, u"Max entries, default 40, max 200."_q) },
			{ u"errors_only"_q, Prop(u"boolean"_q, u"Only errors and warnings."_q) },
		}, { u"bot"_q }));
	add(u"bot_studio_storage"_q,
		u"Read (action=get) or wipe (action=clear) the bot's db storage."_q,
		Schema({
			{ u"bot"_q, BotProp() },
			{ u"action"_q, Prop(u"string"_q, u"get or clear."_q) },
		}, { u"bot"_q }));
	add(u"bot_studio_delete"_q,
		u"Remove a bot from Bot Studio with its storage (the Telegram bot "
		"itself stays in @BotFather)."_q,
		Schema({ { u"bot"_q, BotProp() } }, { u"bot"_q }));
	add(u"bot_studio_open"_q,
		u"Open the Bot Studio window for the user, optionally on a bot."_q,
		Schema({ { u"bot"_q, BotProp() } }));
	return result;
}

ToolPresentation PresentAgentTool(
		not_null<Main::Session*> session,
		const ToolCall &call) {
	const auto args = QJsonDocument::fromJson(
		call.arguments.toUtf8()).object();
	const auto studio = Studio::Get(session);
	const auto botName = [&] {
		const auto query = args.value(u"bot"_q).toVariant().toString();
		const auto bot = studio->find(query);
		return bot
			? (u"@"_q + bot->username)
			: query.isEmpty()
			? u"бот"_q
			: query;
	};
	const auto &name = call.name;
	if (name == u"bot_studio_docs"_q) {
		return { u"Читаю справку Bot Studio"_q };
	} else if (name == u"bot_studio_list"_q) {
		return { u"Смотрю ботов в Bot Studio"_q };
	} else if (name == u"bot_studio_create"_q) {
		return { u"Добавляю бота в Bot Studio"_q };
	} else if (name == u"bot_studio_get"_q) {
		return { u"Открываю код "_q + botName() };
	} else if (name == u"bot_studio_set_code"_q) {
		const auto code = args.value(u"code"_q).toString();
		return {
			u"Обновляю код "_q + botName(),
			u"%1 строк кода"_q.arg(code.count('\n') + 1),
		};
	} else if (name == u"bot_studio_set_flow"_q) {
		const auto blocks = args.value(u"flow"_q).toObject().value(
			u"blocks"_q).toArray().size();
		return {
			u"Обновляю схему "_q + botName(),
			u"Блоков в схеме: %1"_q.arg(blocks),
		};
	} else if (name == u"bot_studio_start"_q) {
		return { u"Запускаю "_q + botName() };
	} else if (name == u"bot_studio_stop"_q) {
		return { u"Останавливаю "_q + botName() };
	} else if (name == u"bot_studio_logs"_q) {
		return { u"Смотрю логи "_q + botName() };
	} else if (name == u"bot_studio_storage"_q) {
		return (args.value(u"action"_q).toString() == u"clear"_q)
			? ToolPresentation{
				u"Очистить данные "_q + botName(),
				u"Все сохранённые ботом данные будут удалены."_q,
				true,
			}
			: ToolPresentation{ u"Смотрю данные "_q + botName() };
	} else if (name == u"bot_studio_delete"_q) {
		return {
			u"Удалить "_q + botName() + u" из Bot Studio"_q,
			u"Код и данные бота будут удалены. Сам бот в Telegram "
				"останется."_q,
			true,
		};
	} else if (name == u"bot_studio_open"_q) {
		return { u"Открываю Bot Studio"_q };
	}
	return { name };
}

void ExecuteAgentTool(
		not_null<Main::Session*> session,
		const ToolCall &call,
		Window::SessionController *window,
		Fn<void(ToolResult)> done) {
	const auto args = QJsonDocument::fromJson(
		call.arguments.toUtf8()).object();
	const auto studio = Studio::Get(session);
	const auto &name = call.name;
	auto error = QString();

	if (name == u"bot_studio_docs"_q) {
		done({ Runtime::Documentation() });
		return;
	} else if (name == u"bot_studio_list"_q) {
		auto list = QJsonArray();
		for (const auto &bot : studio->bots()) {
			list.push_back(Describe(studio, bot));
		}
		done(Ok(QJsonObject{ { u"bots"_q, list } }));
		return;
	} else if (name == u"bot_studio_create"_q) {
		const auto code = args.value(u"code"_q).toString();
		auto flow = args.value(u"flow"_q).toObject();
		if (flow.isEmpty() && args.value(u"flow"_q).isString()) {
			flow = QJsonDocument::fromJson(
				args.value(u"flow"_q).toString().toUtf8()).object();
		}
		if (!flow.isEmpty()) {
			const auto syntax = Runtime::Check(GenerateFlowCode(flow));
			if (!syntax.isEmpty()) {
				done(Fail(u"The flow is invalid:\n"_q + syntax));
				return;
			}
		} else if (!code.trimmed().isEmpty()) {
			const auto syntax = Runtime::Check(code);
			if (!syntax.isEmpty()) {
				done(Fail(u"Syntax error, the bot was not created:\n"_q
					+ syntax));
				return;
			}
		}
		const auto start = !args.contains(u"start"_q)
			|| args.value(u"start"_q).toBool();
		studio->create(
			args.value(u"token"_q).toString(),
			code,
			flow,
			[=](QString id, QString error) {
				if (!error.isEmpty()) {
					auto result = QJsonObject{ { u"error"_q, error } };
					if (!id.isEmpty()) {
						result.insert(u"existing_bot"_q, id);
					}
					done({ QString::fromUtf8(QJsonDocument(result).toJson(
						QJsonDocument::Compact)), true });
					return;
				}
				if (start) {
					const auto skip = studio->logs(id).size();
					studio->start(id);
					ReportAfterStart(studio, id, skip, {}, done);
				} else if (const auto bot = studio->bot(id)) {
					done(Ok(Describe(studio, *bot)));
				}
			});
		return;
	} else if (name == u"bot_studio_open"_q) {
		if (!window) {
			done(Fail(u"No app window is available."_q));
			return;
		}
		const auto bot = studio->find(
			args.value(u"bot"_q).toVariant().toString());
		ShowWindow(window, bot ? bot->id : QString());
		done(Ok(QJsonObject{ { u"opened"_q, true } }));
		return;
	}

	const auto bot = Resolve(studio, args, &error);
	if (!bot) {
		done(Fail(error));
		return;
	}
	const auto id = bot->id;
	if (name == u"bot_studio_get"_q) {
		auto result = Describe(studio, *bot);
		if (bot->visual()) {
			result.insert(u"flow"_q, studio->flow(id));
			result.insert(
				u"note"_q,
				u"Visual bot: change it with bot_studio_set_flow; the "
				"code is generated from the flow."_q);
		} else {
			result.insert(u"code"_q, bot->code);
		}
		result.insert(u"log"_q, Logs(studio, id, kDefaultLogs));
		done(Ok(result));
	} else if (name == u"bot_studio_set_code"_q) {
		const auto code = args.value(u"code"_q).toString();
		if (code.trimmed().isEmpty()) {
			done(Fail(u"code is empty."_q));
			return;
		}
		const auto skip = studio->logs(id).size();
		const auto syntax = studio->setCode(id, code);
		if (!syntax.isEmpty()) {
			done(Fail(u"Syntax error, the code was saved but the bot was "
				"not restarted:\n"_q + syntax));
			return;
		}
		if (bot->enabled) {
			ReportAfterStart(studio, id, skip, {}, done);
		} else {
			auto result = Describe(studio, *bot);
			result.insert(
				u"note"_q,
				u"Code saved. The bot is stopped, start it with "
				"bot_studio_start."_q);
			done(Ok(result));
		}
	} else if (name == u"bot_studio_set_flow"_q) {
		auto flow = args.value(u"flow"_q).toObject();
		if (flow.isEmpty() && args.value(u"flow"_q).isString()) {
			flow = QJsonDocument::fromJson(
				args.value(u"flow"_q).toString().toUtf8()).object();
		}
		if (FlowBlocks(flow).isEmpty()) {
			done(Fail(u"flow must be an object with a non-empty blocks "
				"array."_q));
			return;
		}
		const auto skip = studio->logs(id).size();
		const auto syntax = studio->setFlow(id, flow);
		if (!syntax.isEmpty()) {
			done(Fail(u"The flow produced invalid code:\n"_q + syntax));
			return;
		}
		auto extra = QJsonObject();
		const auto problems = ValidateFlow(studio->flow(id));
		if (!problems.isEmpty()) {
			extra.insert(u"problems"_q, QJsonArray::fromStringList(problems));
		}
		if (bot->enabled) {
			ReportAfterStart(studio, id, skip, extra, done);
		} else {
			auto result = Describe(studio, *bot);
			for (auto i = extra.begin(); i != extra.end(); ++i) {
				result.insert(i.key(), i.value());
			}
			result.insert(
				u"note"_q,
				u"Scheme saved. The bot is stopped, start it with "
				"bot_studio_start."_q);
			done(Ok(result));
		}
	} else if (name == u"bot_studio_start"_q) {
		const auto skip = studio->logs(id).size();
		studio->start(id);
		ReportAfterStart(studio, id, skip, {}, done);
	} else if (name == u"bot_studio_stop"_q) {
		studio->stop(id);
		done(Ok(Describe(studio, *studio->bot(id))));
	} else if (name == u"bot_studio_logs"_q) {
		const auto limit = std::clamp(
			args.value(u"limit"_q).toInt(kDefaultLogs),
			1,
			kMaxLogs);
		auto result = Describe(studio, *bot);
		result.insert(u"log"_q, Logs(
			studio,
			id,
			limit,
			args.value(u"errors_only"_q).toBool()));
		done(Ok(result));
	} else if (name == u"bot_studio_storage"_q) {
		if (args.value(u"action"_q).toString() == u"clear"_q) {
			studio->clearStorage(id);
			done(Ok(QJsonObject{ { u"cleared"_q, true } }));
		} else {
			auto text = QString::fromUtf8(studio->storage(id));
			if (text.size() > kMaxStorageText) {
				text = text.left(kMaxStorageText) + u"…(truncated)"_q;
			}
			done({ text });
		}
	} else if (name == u"bot_studio_delete"_q) {
		studio->remove(id);
		done(Ok(QJsonObject{ { u"deleted"_q, true } }));
	} else {
		done(Fail(u"Unknown tool: "_q + name));
	}
}

QString AgentPromptSection() {
	return uR"(
Bot Studio (bot_studio_* tools): the user can build Telegram bots right in
Agentgram. A bot is a JavaScript file that Agentgram runs locally (no server,
no deploy) while the app is open. Workflow:
- Call bot_studio_docs before building a bot. Prefer a visual scheme
  (flow) for menus, FAQ, forms, bookings and AI chats: the user can then
  edit it in the visual constructor. Write JavaScript code only for logic
  a scheme can't express. For a visual bot use bot_studio_set_flow, never
  bot_studio_set_code (that would turn it into a code bot).
- New bot without a token: ask the user to approve, then talk to @BotFather
  with send_message (/newbot, then a display name, then a username ending in
  "bot"), read its replies with get_messages and pass the token from the last
  reply to bot_studio_create. Never show the token in your answer.
- After bot_studio_create / bot_studio_set_code read the returned log; fix
  errors and set the code again. Tell the user the bot link (t.me/username)
  and how to test it. Use bot_studio_logs to debug after the user tried it.
- Bots can use ai.ask / ai.chat with the AI provider configured here.
)"_q;
}

} // namespace BotStudio
