/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ai_agent/ai_agent_tools.h"

#include "api/api_chat_participants.h"
#include "api/api_common.h"
#include "api/api_editing.h"
#include "apiwrap.h"
#include "base/unixtime.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_chat_filters.h"
#include "data/data_chat_participant_status.h"
#include "data/data_drafts.h"
#include "data/data_folder.h"
#include "data/data_histories.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/data_web_page.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "ui/text/text_entity.h"
#include "window/window_session_controller.h"

#ifdef AGENTGRAM_BOT_STUDIO
#include "bot_studio/bot_studio_agent_tools.h"
#endif // AGENTGRAM_BOT_STUDIO

#include <QtCore/QJsonDocument>

namespace AiAgent {
namespace {

constexpr auto kMaxMessageText = 3000;
constexpr auto kMaxMessagesPerRequest = 100;
constexpr auto kDefaultMessages = 30;

using Items = std::vector<not_null<HistoryItem*>>;

[[nodiscard]] QJsonObject Prop(const QString &type, const QString &about) {
	auto result = QJsonObject();
	result.insert(u"type"_q, type);
	result.insert(u"description"_q, about);
	return result;
}

[[nodiscard]] QJsonObject IdsProp(const QString &about) {
	auto result = Prop(u"array"_q, about);
	result.insert(u"items"_q, QJsonObject{ { u"type"_q, u"integer"_q } });
	return result;
}

[[nodiscard]] QJsonObject Schema(
		std::vector<std::pair<QString, QJsonObject>> properties,
		QStringList required = {}) {
	auto props = QJsonObject();
	for (auto &[name, value] : properties) {
		props.insert(name, value);
	}
	auto result = QJsonObject();
	result.insert(u"type"_q, u"object"_q);
	result.insert(u"properties"_q, props);
	result.insert(u"required"_q, QJsonArray::fromStringList(required));
	return result;
}

[[nodiscard]] QString ChatIdAbout() {
	return u"Chat id from list_chats/find_chats (a numeric string), "
		"or a public @username."_q;
}

[[nodiscard]] QString ToJson(const QJsonObject &object) {
	return QString::fromUtf8(
		QJsonDocument(object).toJson(QJsonDocument::Compact));
}

[[nodiscard]] ToolResult Ok(const QJsonObject &object) {
	return { ToJson(object), false };
}

[[nodiscard]] ToolResult Error(const QString &text) {
	auto object = QJsonObject();
	object.insert(u"error"_q, text);
	return { ToJson(object), true };
}

[[nodiscard]] QString PeerType(not_null<PeerData*> peer) {
	if (peer->isSelf()) {
		return u"saved_messages"_q;
	} else if (const auto user = peer->asUser()) {
		return user->isBot() ? u"bot"_q : u"user"_q;
	} else if (peer->isChat()) {
		return u"group"_q;
	} else if (peer->isMegagroup()) {
		return peer->isForum() ? u"forum"_q : u"supergroup"_q;
	} else if (peer->isBroadcast()) {
		return u"channel"_q;
	}
	return u"chat"_q;
}

[[nodiscard]] QString DateString(TimeId date) {
	return base::unixtime::parse(date).toString(Qt::ISODate);
}

[[nodiscard]] QString PeerIdString(not_null<PeerData*> peer) {
	return QString::number(peer->id.value);
}

[[nodiscard]] QJsonObject PeerJson(not_null<PeerData*> peer) {
	auto result = QJsonObject();
	result.insert(u"chat_id"_q, PeerIdString(peer));
	result.insert(u"title"_q, peer->isSelf()
		? u"Saved Messages"_q
		: peer->name());
	result.insert(u"type"_q, PeerType(peer));
	const auto username = peer->username();
	if (!username.isEmpty()) {
		result.insert(u"username"_q, u"@"_q + username);
	}
	if (const auto channel = peer->asChannel()) {
		if (channel->membersCountKnown()) {
			result.insert(u"members"_q, channel->membersCount());
		}
	} else if (const auto chat = peer->asChat()) {
		if (chat->count > 0) {
			result.insert(u"members"_q, chat->count);
		}
	} else if (const auto user = peer->asUser()) {
		if (user->isContact()) {
			result.insert(u"contact"_q, true);
		}
	}
	return result;
}

[[nodiscard]] QJsonObject ChatRowJson(not_null<History*> history) {
	auto result = PeerJson(history->peer);
	const auto unread = history->unreadCount();
	if (unread > 0) {
		result.insert(u"unread"_q, unread);
	}
	if (history->muted()) {
		result.insert(u"muted"_q, true);
	}
	if (const auto last = history->lastMessage()) {
		auto text = last->notificationText().text;
		if (text.size() > 160) {
			text = text.left(160) + QChar(0x2026);
		}
		result.insert(u"last_message"_q, text);
		result.insert(u"last_date"_q, DateString(last->date()));
	}
	return result;
}

[[nodiscard]] QJsonObject MessageJson(not_null<HistoryItem*> item) {
	auto result = QJsonObject();
	result.insert(u"id"_q, double(item->id.bare));
	result.insert(u"date"_q, DateString(item->date()));
	const auto from = item->from();
	if (item->out()) {
		result.insert(u"from"_q, u"me"_q);
	} else {
		result.insert(u"from"_q, from->name());
		result.insert(u"from_id"_q, PeerIdString(from));
	}
	if (item->isService()) {
		result.insert(u"service"_q, true);
	}
	auto text = item->notificationText().text;
	if (text.size() > kMaxMessageText) {
		text = text.left(kMaxMessageText) + u" [...]"_q;
	}
	result.insert(u"text"_q, text);
	if (const auto reply = item->replyToId()) {
		result.insert(u"reply_to"_q, double(reply.bare));
	}
	return result;
}

[[nodiscard]] QJsonArray MessagesJson(const Items &items) {
	auto result = QJsonArray();
	for (const auto &item : items) {
		result.push_back(MessageJson(item));
	}
	return result;
}

[[nodiscard]] Items ParseMessages(
		not_null<Main::Session*> session,
		const MTPmessages_Messages &result) {
	auto &owner = session->data();
	auto items = Items();
	const auto process = [&](const auto &data) {
		owner.processUsers(data.vusers());
		owner.processChats(data.vchats());
		for (const auto &message : data.vmessages().v) {
			const auto peerId = PeerFromMessage(message);
			if (!owner.peerLoaded(peerId) || !DateFromMessage(message)) {
				continue;
			}
			const auto item = owner.addNewMessage(
				message,
				MessageFlags(),
				NewMessageType::Existing);
			if (item) {
				items.push_back(item);
			}
		}
	};
	result.match([&](const MTPDmessages_messagesNotModified &) {
	}, [&](const auto &data) {
		process(data);
	});
	return items;
}

[[nodiscard]] std::vector<MsgId> ReadIds(const QJsonValue &value) {
	auto result = std::vector<MsgId>();
	if (value.isArray()) {
		for (const auto &id : value.toArray()) {
			const auto bare = int64(id.toVariant().toLongLong());
			if (bare > 0) {
				result.push_back(MsgId(bare));
			}
		}
	} else {
		const auto bare = int64(value.toVariant().toLongLong());
		if (bare > 0) {
			result.push_back(MsgId(bare));
		}
	}
	return result;
}

[[nodiscard]] TextWithTags PrepareText(const QString &markdown) {
	auto parsed = TextUtilities::ParseEntities(
		markdown,
		TextParseMarkdown | TextParseLinks);
	return {
		parsed.text,
		TextUtilities::ConvertEntitiesToTextTags(parsed.entities),
	};
}

[[nodiscard]] QString Shorten(QString text, int limit = 400) {
	text = text.trimmed();
	if (text.size() > limit) {
		text = text.left(limit) + QChar(0x2026);
	}
	return text;
}

} // namespace

TelegramTools::TelegramTools(not_null<Main::Session*> session)
: _session(session)
, _api(&session->mtp()) {
}

std::vector<ToolDefinition> TelegramTools::definitions() const {
	auto result = std::vector<ToolDefinition>();
	const auto add = [&](QString name, QString about, QJsonObject schema) {
		result.push_back({ name, about, schema });
	};
	add(u"get_context"_q,
		u"Info about the current user (me), the chat currently opened "
		"in the app window and the current local date/time."_q,
		Schema({}));
	add(u"list_chats"_q,
		u"List chats from the user's chat list ordered by last activity. "
		"Returns chat_id, title, type, unread count, last message."_q,
		Schema({
			{ u"limit"_q, Prop(u"integer"_q, u"Max chats, default 30, max 200."_q) },
			{ u"offset"_q, Prop(u"integer"_q, u"Skip this many chats."_q) },
			{ u"filter"_q, Prop(u"string"_q, u"One of: all, unread, users, groups, channels, bots."_q) },
			{ u"archived"_q, Prop(u"boolean"_q, u"List the archive folder instead."_q) },
			{ u"folder_id"_q, Prop(u"integer"_q, u"Only chats of this chat folder (see list_folders)."_q) },
		}));
	add(u"find_chats"_q,
		u"Find chats, contacts, groups and channels by name or username, "
		"locally and on the server."_q,
		Schema({
			{ u"query"_q, Prop(u"string"_q, u"Name or username to search."_q) },
			{ u"limit"_q, Prop(u"integer"_q, u"Max results, default 20."_q) },
		}, { u"query"_q }));
	add(u"get_chat_info"_q,
		u"Detailed info about a chat: title, type, username, members, "
		"description, unread count, my permissions."_q,
		Schema({
			{ u"chat_id"_q, Prop(u"string"_q, ChatIdAbout()) },
		}, { u"chat_id"_q }));
	add(u"get_messages"_q,
		u"Read messages of a chat in chronological order. Use offset_id "
		"to page back to older messages."_q,
		Schema({
			{ u"chat_id"_q, Prop(u"string"_q, ChatIdAbout()) },
			{ u"limit"_q, Prop(u"integer"_q, u"How many latest messages, default 30, max 100."_q) },
			{ u"offset_id"_q, Prop(u"integer"_q, u"Return messages older than this message id."_q) },
			{ u"unread_only"_q, Prop(u"boolean"_q, u"Return only unread incoming messages."_q) },
		}, { u"chat_id"_q }));
	add(u"search_messages"_q,
		u"Full-text search of messages in one chat or across all chats."_q,
		Schema({
			{ u"query"_q, Prop(u"string"_q, u"Text to search for."_q) },
			{ u"chat_id"_q, Prop(u"string"_q, u"Optional chat to search in. "_q + ChatIdAbout()) },
			{ u"limit"_q, Prop(u"integer"_q, u"Max results, default 20, max 100."_q) },
		}, { u"query"_q }));
	add(u"get_unread_overview"_q,
		u"Collect unread messages from several chats at once, useful "
		"for summaries and digests."_q,
		Schema({
			{ u"max_chats"_q, Prop(u"integer"_q, u"Max chats to include, default 10."_q) },
			{ u"per_chat"_q, Prop(u"integer"_q, u"Max messages per chat, default 15."_q) },
			{ u"include_muted"_q, Prop(u"boolean"_q, u"Include muted chats, default false."_q) },
		}));
	add(u"get_chat_members"_q,
		u"List members of a group or supergroup, optionally filtered by "
		"a name query."_q,
		Schema({
			{ u"chat_id"_q, Prop(u"string"_q, ChatIdAbout()) },
			{ u"query"_q, Prop(u"string"_q, u"Optional name filter."_q) },
			{ u"limit"_q, Prop(u"integer"_q, u"Max members, default 50, max 200."_q) },
		}, { u"chat_id"_q }));
	add(u"list_folders"_q,
		u"List the user's chat folders with their ids."_q,
		Schema({}));
	add(u"send_message"_q,
		u"Send a text message to a chat on the user's behalf. Supports "
		"**bold**, __italic__, `code` markdown. The user may be asked "
		"to confirm."_q,
		Schema({
			{ u"chat_id"_q, Prop(u"string"_q, ChatIdAbout()) },
			{ u"text"_q, Prop(u"string"_q, u"Message text."_q) },
			{ u"reply_to"_q, Prop(u"integer"_q, u"Optional message id to reply to."_q) },
		}, { u"chat_id"_q, u"text"_q }));
	add(u"set_draft"_q,
		u"Put text into the message input field of a chat without "
		"sending it, and open that chat, so the user can review it."_q,
		Schema({
			{ u"chat_id"_q, Prop(u"string"_q, ChatIdAbout()) },
			{ u"text"_q, Prop(u"string"_q, u"Draft text."_q) },
		}, { u"chat_id"_q, u"text"_q }));
	add(u"forward_messages"_q,
		u"Forward messages from one chat to another."_q,
		Schema({
			{ u"from_chat_id"_q, Prop(u"string"_q, ChatIdAbout()) },
			{ u"message_ids"_q, IdsProp(u"Message ids to forward."_q) },
			{ u"to_chat_id"_q, Prop(u"string"_q, ChatIdAbout()) },
		}, { u"from_chat_id"_q, u"message_ids"_q, u"to_chat_id"_q }));
	add(u"edit_message"_q,
		u"Edit the text of the user's own message."_q,
		Schema({
			{ u"chat_id"_q, Prop(u"string"_q, ChatIdAbout()) },
			{ u"message_id"_q, Prop(u"integer"_q, u"Message id."_q) },
			{ u"text"_q, Prop(u"string"_q, u"New text."_q) },
		}, { u"chat_id"_q, u"message_id"_q, u"text"_q }));
	add(u"delete_messages"_q,
		u"Delete messages in a chat."_q,
		Schema({
			{ u"chat_id"_q, Prop(u"string"_q, ChatIdAbout()) },
			{ u"message_ids"_q, IdsProp(u"Message ids to delete."_q) },
			{ u"for_everyone"_q, Prop(u"boolean"_q, u"Delete for all participants, default true."_q) },
		}, { u"chat_id"_q, u"message_ids"_q }));
	add(u"pin_message"_q,
		u"Pin or unpin a message in a chat."_q,
		Schema({
			{ u"chat_id"_q, Prop(u"string"_q, ChatIdAbout()) },
			{ u"message_id"_q, Prop(u"integer"_q, u"Message id."_q) },
			{ u"unpin"_q, Prop(u"boolean"_q, u"Unpin instead of pin."_q) },
		}, { u"chat_id"_q, u"message_id"_q }));
	add(u"mark_as_read"_q,
		u"Mark all messages in a chat as read."_q,
		Schema({
			{ u"chat_id"_q, Prop(u"string"_q, ChatIdAbout()) },
		}, { u"chat_id"_q }));
	add(u"create_group"_q,
		u"Create a new group with the given title and members."_q,
		Schema({
			{ u"title"_q, Prop(u"string"_q, u"Group title."_q) },
			{ u"user_ids"_q, Prop(u"array"_q, u"chat_id values of users to add."_q) },
		}, { u"title"_q, u"user_ids"_q }));
	add(u"add_members"_q,
		u"Add users to an existing group or channel."_q,
		Schema({
			{ u"chat_id"_q, Prop(u"string"_q, ChatIdAbout()) },
			{ u"user_ids"_q, Prop(u"array"_q, u"chat_id values of users to add."_q) },
		}, { u"chat_id"_q, u"user_ids"_q }));
	add(u"open_chat"_q,
		u"Open a chat in the app window, optionally at a message."_q,
		Schema({
			{ u"chat_id"_q, Prop(u"string"_q, ChatIdAbout()) },
			{ u"message_id"_q, Prop(u"integer"_q, u"Optional message id to jump to."_q) },
		}, { u"chat_id"_q }));
	for (auto &definition : result) {
		auto properties = definition.parameters.value(
			u"properties"_q).toObject();
		const auto fix = [&](const QString &name) {
			if (properties.contains(name)) {
				auto prop = properties.value(name).toObject();
				prop.insert(u"items"_q, QJsonObject{
					{ u"type"_q, u"string"_q },
				});
				properties.insert(name, prop);
			}
		};
		fix(u"user_ids"_q);
		definition.parameters.insert(u"properties"_q, properties);
	}
#ifdef AGENTGRAM_BOT_STUDIO
	for (auto &definition : BotStudio::AgentToolDefinitions()) {
		result.push_back(std::move(definition));
	}
#endif // AGENTGRAM_BOT_STUDIO
	return result;
}

QString TelegramTools::peerTitle(const QJsonValue &value) const {
	auto text = value.toVariant().toString().trimmed();
	if (text.startsWith('@')) {
		return text;
	}
	const auto bare = text.toULongLong();
	if (bare) {
		if (const auto peer = _session->data().peerLoaded(
				PeerId(PeerIdHelper(bare)))) {
			return peer->isSelf() ? u"Избранное"_q : peer->name();
		}
	}
	return text;
}

ToolPresentation TelegramTools::present(const ToolCall &call) const {
	const auto args = QJsonDocument::fromJson(
		call.arguments.toUtf8()).object();
	const auto chat = [&](const char *key = "chat_id") {
		return u"«"_q
			+ peerTitle(args.value(QString::fromLatin1(key)))
			+ u"»"_q;
	};
	const auto count = [&](const char *key) {
		return int(ReadIds(args.value(QString::fromLatin1(key))).size());
	};
	const auto &name = call.name;
#ifdef AGENTGRAM_BOT_STUDIO
	if (BotStudio::IsAgentTool(name)) {
		return BotStudio::PresentAgentTool(_session, call);
	}
#endif // AGENTGRAM_BOT_STUDIO
	if (name == u"get_context"_q) {
		return { u"Смотрю текущий контекст"_q };
	} else if (name == u"list_chats"_q) {
		return { u"Просматриваю список чатов"_q };
	} else if (name == u"find_chats"_q) {
		return {
			u"Ищу чаты: «%1»"_q.arg(args.value(u"query"_q).toString()),
		};
	} else if (name == u"get_chat_info"_q) {
		return { u"Смотрю информацию о чате "_q + chat() };
	} else if (name == u"get_messages"_q) {
		return { u"Читаю сообщения в "_q + chat() };
	} else if (name == u"search_messages"_q) {
		const auto query = args.value(u"query"_q).toString();
		return {
			args.contains(u"chat_id"_q)
				? u"Ищу «%1» в %2"_q.arg(query, chat())
				: u"Ищу «%1» во всех чатах"_q.arg(query),
		};
	} else if (name == u"get_unread_overview"_q) {
		return { u"Собираю непрочитанные сообщения"_q };
	} else if (name == u"get_chat_members"_q) {
		return { u"Смотрю участников "_q + chat() };
	} else if (name == u"list_folders"_q) {
		return { u"Смотрю папки чатов"_q };
	} else if (name == u"send_message"_q) {
		return {
			u"Отправить сообщение в "_q + chat(),
			Shorten(args.value(u"text"_q).toString(), 1500),
			true,
		};
	} else if (name == u"set_draft"_q) {
		return {
			u"Черновик в "_q + chat(),
			Shorten(args.value(u"text"_q).toString(), 1500),
		};
	} else if (name == u"forward_messages"_q) {
		return {
			u"Переслать %1 сообщ. из %2 в %3"_q.arg(
				QString::number(count("message_ids")),
				chat("from_chat_id"),
				chat("to_chat_id")),
			QString(),
			true,
		};
	} else if (name == u"edit_message"_q) {
		return {
			u"Изменить сообщение в "_q + chat(),
			Shorten(args.value(u"text"_q).toString(), 1500),
			true,
		};
	} else if (name == u"delete_messages"_q) {
		return {
			u"Удалить %1 сообщ. в %2"_q.arg(
				QString::number(count("message_ids")),
				chat()),
			args.value(u"for_everyone"_q).toBool(true)
				? u"Сообщения будут удалены у всех участников."_q
				: u"Сообщения будут удалены только у вас."_q,
			true,
		};
	} else if (name == u"pin_message"_q) {
		return {
			(args.value(u"unpin"_q).toBool()
				? u"Открепить сообщение в "_q
				: u"Закрепить сообщение в "_q) + chat(),
			QString(),
			true,
		};
	} else if (name == u"mark_as_read"_q) {
		return { u"Отмечаю прочитанным "_q + chat() };
	} else if (name == u"create_group"_q) {
		auto members = QStringList();
		for (const auto &id : args.value(u"user_ids"_q).toArray()) {
			members.push_back(peerTitle(id));
		}
		return {
			u"Создать группу «%1»"_q.arg(args.value(u"title"_q).toString()),
			u"Участники: "_q + members.join(u", "_q),
			true,
		};
	} else if (name == u"add_members"_q) {
		auto members = QStringList();
		for (const auto &id : args.value(u"user_ids"_q).toArray()) {
			members.push_back(peerTitle(id));
		}
		return {
			u"Добавить участников в "_q + chat(),
			members.join(u", "_q),
			true,
		};
	} else if (name == u"open_chat"_q) {
		return { u"Открываю "_q + chat() };
	}
	return { name };
}

void TelegramTools::execute(
		const ToolCall &call,
		Window::SessionController *window,
		Fn<void(ToolResult)> done) {
	const auto document = QJsonDocument::fromJson(call.arguments.toUtf8());
	if (!call.arguments.trimmed().isEmpty() && !document.isObject()) {
		done(Error(u"Arguments are not a valid JSON object."_q));
		return;
	}
	const auto args = document.object();
	const auto &name = call.name;
#ifdef AGENTGRAM_BOT_STUDIO
	if (BotStudio::IsAgentTool(name)) {
		BotStudio::ExecuteAgentTool(_session, call, window, std::move(done));
		return;
	}
#endif // AGENTGRAM_BOT_STUDIO
	if (name == u"get_context"_q) {
		getContext(window, std::move(done));
	} else if (name == u"list_chats"_q) {
		listChats(args, std::move(done));
	} else if (name == u"find_chats"_q) {
		findChats(args, std::move(done));
	} else if (name == u"get_chat_info"_q) {
		getChatInfo(args, std::move(done));
	} else if (name == u"get_messages"_q) {
		getMessages(args, std::move(done));
	} else if (name == u"search_messages"_q) {
		searchMessages(args, std::move(done));
	} else if (name == u"get_unread_overview"_q) {
		unreadOverview(args, std::move(done));
	} else if (name == u"get_chat_members"_q) {
		getMembers(args, std::move(done));
	} else if (name == u"list_folders"_q) {
		listFolders(std::move(done));
	} else if (name == u"send_message"_q) {
		sendMessage(args, std::move(done));
	} else if (name == u"set_draft"_q) {
		setDraft(args, window, std::move(done));
	} else if (name == u"forward_messages"_q) {
		forwardMessages(args, std::move(done));
	} else if (name == u"edit_message"_q) {
		editMessage(args, std::move(done));
	} else if (name == u"delete_messages"_q) {
		deleteMessages(args, std::move(done));
	} else if (name == u"pin_message"_q) {
		pinMessage(args, std::move(done));
	} else if (name == u"mark_as_read"_q) {
		markRead(args, std::move(done));
	} else if (name == u"create_group"_q) {
		createGroup(args, std::move(done));
	} else if (name == u"add_members"_q) {
		addMembers(args, window, std::move(done));
	} else if (name == u"open_chat"_q) {
		openChat(args, window, std::move(done));
	} else {
		done(Error(u"Unknown tool: "_q + name));
	}
}

void TelegramTools::resolvePeer(
		const QJsonValue &value,
		Fn<void(PeerData*, QString)> done) {
	auto text = value.toVariant().toString().trimmed();
	if (text.isEmpty()) {
		done(nullptr, u"chat_id is required."_q);
		return;
	}
	if (text.startsWith(u"https://t.me/"_q)) {
		text = '@' + text.mid(13).section('/', 0, 0);
	}
	const auto lower = text.toLower();
	if (lower == u"me"_q || lower == u"self"_q || lower == u"saved"_q) {
		done(_session->user(), QString());
		return;
	}
	if (!text.startsWith('@')) {
		const auto bare = text.toULongLong();
		if (!bare) {
			done(nullptr, u"Invalid chat_id: "_q + text);
			return;
		}
		const auto peer = _session->data().peerLoaded(
			PeerId(PeerIdHelper(bare)));
		if (!peer) {
			done(nullptr, u"Chat not found: "_q + text
				+ u". Use list_chats or find_chats to get a valid chat_id."_q);
			return;
		}
		done(peer, QString());
		return;
	}
	const auto username = text.mid(1);
	if (const auto peer = _session->data().peerByUsername(username)) {
		done(peer, QString());
		return;
	}
	const auto session = _session;
	_api.request(MTPcontacts_ResolveUsername(
		MTP_flags(0),
		MTP_string(username),
		MTPstring()
	)).done([=](const MTPcontacts_ResolvedPeer &result) {
		const auto &data = result.data();
		session->data().processUsers(data.vusers());
		session->data().processChats(data.vchats());
		const auto peer = session->data().peerLoaded(
			peerFromMTP(data.vpeer()));
		done(peer, peer ? QString() : u"Username not found."_q);
	}).fail([=](const MTP::Error &error) {
		done(nullptr, u"Can't resolve @%1: %2"_q.arg(
			username,
			error.type()));
	}).send();
}

void TelegramTools::requestHistory(
		not_null<PeerData*> peer,
		int limit,
		int offsetId,
		Fn<void(Items, QString)> done) {
	const auto session = _session;
	_api.request(MTPmessages_GetHistory(
		peer->input(),
		MTP_int(offsetId),
		MTP_int(0), // offset_date
		MTP_int(0), // add_offset
		MTP_int(limit),
		MTP_int(0), // max_id
		MTP_int(0), // min_id
		MTP_long(0) // hash
	)).done([=](const MTPmessages_Messages &result) {
		auto items = ParseMessages(session, result);
		ranges::sort(items, [](auto a, auto b) {
			return a->id < b->id;
		});
		done(std::move(items), QString());
	}).fail([=](const MTP::Error &error) {
		done({}, error.type());
	}).send();
}

void TelegramTools::ensureMessages(
		not_null<PeerData*> peer,
		std::vector<MsgId> ids,
		Fn<void(Items)> done) {
	const auto collect = [=] {
		auto result = Items();
		for (const auto id : ids) {
			if (const auto item = _session->data().message(peer, id)) {
				result.push_back(item);
			}
		}
		return result;
	};
	auto missing = std::vector<MsgId>();
	for (const auto id : ids) {
		if (!_session->data().message(peer, id)) {
			missing.push_back(id);
		}
	}
	if (missing.empty()) {
		done(collect());
		return;
	}
	const auto left = std::make_shared<int>(int(missing.size()));
	for (const auto id : missing) {
		_session->api().requestMessageData(peer, id, crl::guard(this, [=] {
			if (!--*left) {
				done(collect());
			}
		}));
	}
}

void TelegramTools::getContext(
		Window::SessionController *window,
		Done done) {
	auto result = QJsonObject();
	const auto self = _session->user();
	auto me = QJsonObject();
	me.insert(u"name"_q, self->name());
	me.insert(u"user_id"_q, PeerIdString(self));
	if (!self->username().isEmpty()) {
		me.insert(u"username"_q, u"@"_q + self->username());
	}
	result.insert(u"me"_q, me);
	result.insert(
		u"now"_q,
		QDateTime::currentDateTime().toString(Qt::ISODate));
	if (window) {
		if (const auto peer = window->activeChatCurrent().peer()) {
			if (const auto history = peer->owner().historyLoaded(peer)) {
				result.insert(u"open_chat"_q, ChatRowJson(history));
			} else {
				result.insert(u"open_chat"_q, PeerJson(peer));
			}
		}
	}
	done(Ok(result));
}

void TelegramTools::listChats(const Args &args, Done done) {
	const auto limit = std::clamp(
		args.value(u"limit"_q).toInt(kDefaultMessages),
		1,
		200);
	const auto offset = std::max(args.value(u"offset"_q).toInt(), 0);
	const auto filter = args.value(u"filter"_q).toString(u"all"_q);
	auto &owner = _session->data();
	auto list = (Dialogs::MainList*)nullptr;
	if (args.value(u"archived"_q).toBool()) {
		if (const auto folder = owner.folderLoaded(Data::Folder::kId)) {
			list = folder->chatsList();
		}
	} else if (const auto folderId = args.value(u"folder_id"_q).toInt()) {
		list = owner.chatsFilters().chatsList(folderId);
	} else {
		list = owner.chatsList();
	}
	auto chats = QJsonArray();
	auto skipped = 0;
	auto total = 0;
	if (list) {
		for (const auto &row : *list->indexed()) {
			const auto history = row->key().history();
			if (!history) {
				continue;
			}
			const auto peer = history->peer;
			const auto matches = [&] {
				if (filter == u"unread"_q) {
					return history->unreadCount() > 0
						|| history->unreadMark();
				} else if (filter == u"users"_q) {
					return peer->isUser() && !peer->isBot();
				} else if (filter == u"bots"_q) {
					return peer->isBot();
				} else if (filter == u"groups"_q) {
					return peer->isChat() || peer->isMegagroup();
				} else if (filter == u"channels"_q) {
					return peer->isBroadcast();
				}
				return true;
			}();
			if (!matches) {
				continue;
			}
			++total;
			if (skipped < offset) {
				++skipped;
				continue;
			} else if (chats.size() < limit) {
				chats.push_back(ChatRowJson(history));
			}
		}
	}
	auto result = QJsonObject();
	result.insert(u"chats"_q, chats);
	result.insert(u"total_matching"_q, total);
	if (list && !list->loaded()) {
		result.insert(
			u"note"_q,
			u"The chat list is still loading, older chats may be missing."_q);
	}
	done(Ok(result));
}

void TelegramTools::findChats(const Args &args, Done done) {
	const auto query = args.value(u"query"_q).toString().trimmed();
	const auto limit = std::clamp(args.value(u"limit"_q).toInt(20), 1, 50);
	if (query.isEmpty()) {
		done(Error(u"query is required."_q));
		return;
	}
	const auto lower = query.toLower();
	const auto needle = lower.startsWith('@') ? lower.mid(1) : lower;
	auto seen = base::flat_set<PeerId>();
	auto found = QJsonArray();
	const auto consider = [&](not_null<PeerData*> peer) {
		if (found.size() >= limit || seen.contains(peer->id)) {
			return;
		}
		const auto name = peer->name().toLower();
		const auto username = peer->username().toLower();
		if (name.contains(needle) || username.contains(needle)) {
			seen.emplace(peer->id);
			const auto history = _session->data().historyLoaded(peer);
			found.push_back(history
				? ChatRowJson(history)
				: PeerJson(peer));
		}
	};
	auto &owner = _session->data();
	for (const auto &row : *owner.chatsList()->indexed()) {
		if (const auto history = row->key().history()) {
			consider(history->peer);
		}
	}
	if (const auto folder = owner.folderLoaded(Data::Folder::kId)) {
		for (const auto &row : *folder->chatsList()->indexed()) {
			if (const auto history = row->key().history()) {
				consider(history->peer);
			}
		}
	}
	const auto session = _session;
	const auto local = std::make_shared<QJsonArray>(found);
	const auto localSeen = std::make_shared<base::flat_set<PeerId>>(seen);
	_api.request(MTPcontacts_Search(
		MTP_flags(0),
		MTP_string(needle),
		MTP_int(limit)
	)).done([=](const MTPcontacts_Found &result) {
		const auto &data = result.data();
		session->data().processUsers(data.vusers());
		session->data().processChats(data.vchats());
		auto all = *local;
		const auto add = [&](const MTPPeer &mtp) {
			const auto peer = session->data().peerLoaded(peerFromMTP(mtp));
			if (!peer || localSeen->contains(peer->id) || all.size() >= limit) {
				return;
			}
			localSeen->emplace(peer->id);
			auto json = PeerJson(peer);
			if (!session->data().historyLoaded(peer)) {
				json.insert(u"not_in_chat_list"_q, true);
			}
			all.push_back(json);
		};
		for (const auto &peer : data.vmy_results().v) {
			add(peer);
		}
		for (const auto &peer : data.vresults().v) {
			add(peer);
		}
		auto object = QJsonObject();
		object.insert(u"results"_q, all);
		done(Ok(object));
	}).fail([=](const MTP::Error &error) {
		auto object = QJsonObject();
		object.insert(u"results"_q, *local);
		object.insert(u"server_search_error"_q, error.type());
		done(Ok(object));
	}).send();
}

void TelegramTools::getChatInfo(const Args &args, Done done) {
	resolvePeer(args.value(u"chat_id"_q), [=](PeerData *peer, QString e) {
		if (!peer) {
			done(Error(e));
			return;
		}
		const auto history = _session->data().historyLoaded(peer);
		auto result = history ? ChatRowJson(history) : PeerJson(peer);
		if (!peer->about().isEmpty()) {
			result.insert(u"description"_q, peer->about());
		}
		result.insert(u"can_send_messages"_q, Data::CanSendTexts(peer));
		if (const auto channel = peer->asChannel()) {
			result.insert(u"am_admin"_q, channel->amCreator()
				|| channel->hasAdminRights());
		} else if (const auto chat = peer->asChat()) {
			result.insert(u"am_admin"_q, chat->amCreator()
				|| chat->hasAdminRights());
		} else if (const auto user = peer->asUser()) {
			if (user->isBot()) {
				result.insert(u"bot"_q, true);
			}
		}
		if (peer->about().isEmpty()) {
			_session->api().requestFullPeer(peer);
		}
		done(Ok(result));
	});
}

void TelegramTools::getMessages(const Args &args, Done done) {
	const auto limit = std::clamp(
		args.value(u"limit"_q).toInt(kDefaultMessages),
		1,
		kMaxMessagesPerRequest);
	const auto offsetId = std::max(args.value(u"offset_id"_q).toInt(), 0);
	const auto unreadOnly = args.value(u"unread_only"_q).toBool();
	resolvePeer(args.value(u"chat_id"_q), [=](PeerData *peer, QString e) {
		if (!peer) {
			done(Error(e));
			return;
		}
		auto count = limit;
		const auto history = _session->data().history(peer);
		if (unreadOnly) {
			count = std::min(history->unreadCount(), kMaxMessagesPerRequest);
			if (count <= 0) {
				auto result = PeerJson(peer);
				result.insert(u"messages"_q, QJsonArray());
				result.insert(u"note"_q, u"No unread messages."_q);
				done(Ok(result));
				return;
			}
		}
		requestHistory(peer, count, offsetId, [=](Items items, QString e) {
			if (!e.isEmpty()) {
				done(Error(e));
				return;
			}
			auto result = PeerJson(peer);
			result.insert(u"messages"_q, MessagesJson(items));
			if (!items.empty() && int(items.size()) >= count) {
				result.insert(
					u"older_offset_id"_q,
					double(items.front()->id.bare));
			}
			done(Ok(result));
		});
	});
}

void TelegramTools::searchMessages(const Args &args, Done done) {
	const auto query = args.value(u"query"_q).toString();
	const auto limit = std::clamp(
		args.value(u"limit"_q).toInt(20),
		1,
		kMaxMessagesPerRequest);
	const auto session = _session;
	const auto finish = [=](const MTPmessages_Messages &result) {
		const auto items = ParseMessages(session, result);
		auto list = QJsonArray();
		for (const auto &item : items) {
			auto json = MessageJson(item);
			json.insert(u"chat_id"_q, PeerIdString(item->history()->peer));
			json.insert(u"chat"_q, item->history()->peer->name());
			list.push_back(json);
		}
		auto object = QJsonObject();
		object.insert(u"results"_q, list);
		done(Ok(object));
	};
	const auto fail = [=](const MTP::Error &error) {
		done(Error(error.type()));
	};
	if (!args.contains(u"chat_id"_q)
		|| args.value(u"chat_id"_q).toVariant().toString().isEmpty()) {
		_api.request(MTPmessages_SearchGlobal(
			MTP_flags(0),
			MTP_int(0), // folder_id
			MTPInputChannel(),
			MTP_string(query),
			MTP_inputMessagesFilterEmpty(),
			MTP_int(0), // min_date
			MTP_int(0), // max_date
			MTP_int(0), // offset_rate
			MTP_inputPeerEmpty(),
			MTP_int(0), // offset_id
			MTP_int(limit)
		)).done(finish).fail(fail).send();
		return;
	}
	resolvePeer(args.value(u"chat_id"_q), [=](PeerData *peer, QString e) {
		if (!peer) {
			done(Error(e));
			return;
		}
		_api.request(MTPmessages_Search(
			MTP_flags(0),
			peer->input(),
			MTP_string(query),
			MTP_inputPeerEmpty(), // from_id
			MTP_inputPeerEmpty(), // saved_peer_id
			MTPVector<MTPReaction>(), // saved_reaction
			MTP_int(0), // top_msg_id
			MTP_inputMessagesFilterEmpty(),
			MTP_int(0), // min_date
			MTP_int(0), // max_date
			MTP_int(0), // offset_id
			MTP_int(0), // add_offset
			MTP_int(limit),
			MTP_int(0), // max_id
			MTP_int(0), // min_id
			MTP_long(0) // hash
		)).done(finish).fail(fail).send();
	});
}

void TelegramTools::unreadOverview(const Args &args, Done done) {
	const auto maxChats = std::clamp(
		args.value(u"max_chats"_q).toInt(10),
		1,
		30);
	const auto perChat = std::clamp(
		args.value(u"per_chat"_q).toInt(15),
		1,
		50);
	const auto includeMuted = args.value(u"include_muted"_q).toBool();
	auto targets = std::vector<not_null<History*>>();
	for (const auto &row : *_session->data().chatsList()->indexed()) {
		const auto history = row->key().history();
		if (!history
			|| history->unreadCount() <= 0
			|| (!includeMuted && history->muted())) {
			continue;
		}
		targets.push_back(history);
		if (int(targets.size()) >= maxChats) {
			break;
		}
	}
	if (targets.empty()) {
		auto object = QJsonObject();
		object.insert(u"chats"_q, QJsonArray());
		object.insert(u"note"_q, u"No unread chats."_q);
		done(Ok(object));
		return;
	}
	struct State {
		std::vector<not_null<History*>> targets;
		QJsonArray chats;
		int index = 0;
	};
	const auto state = std::make_shared<State>();
	state->targets = std::move(targets);
	const auto step = std::make_shared<Fn<void()>>();
	*step = [=] {
		if (state->index >= int(state->targets.size())) {
			auto object = QJsonObject();
			object.insert(u"chats"_q, state->chats);
			done(Ok(object));
			*step = nullptr;
			return;
		}
		const auto history = state->targets[state->index++];
		const auto count = std::min(history->unreadCount(), perChat);
		requestHistory(history->peer, count, 0, [=](Items items, QString) {
			auto json = ChatRowJson(history);
			json.insert(u"messages"_q, MessagesJson(items));
			state->chats.push_back(json);
			(*step)();
		});
	};
	(*step)();
}

void TelegramTools::getMembers(const Args &args, Done done) {
	const auto query = args.value(u"query"_q).toString().trimmed();
	const auto limit = std::clamp(args.value(u"limit"_q).toInt(50), 1, 200);
	const auto session = _session;
	resolvePeer(args.value(u"chat_id"_q), [=](PeerData *peer, QString e) {
		if (!peer) {
			done(Error(e));
			return;
		}
		const auto userJson = [=](not_null<UserData*> user) {
			auto json = PeerJson(user);
			json.insert(u"user_id"_q, PeerIdString(user));
			return json;
		};
		if (const auto channel = peer->asChannel()) {
			if (channel->isBroadcast() && !channel->canViewMembers()) {
				done(Error(u"Members of this channel are not visible."_q));
				return;
			}
			const auto filter = query.isEmpty()
				? MTP_channelParticipantsRecent()
				: MTP_channelParticipantsSearch(MTP_string(query));
			_api.request(MTPchannels_GetParticipants(
				channel->inputChannel(),
				filter,
				MTP_int(0),
				MTP_int(limit),
				MTP_long(0)
			)).done([=](const MTPchannels_ChannelParticipants &result) {
				auto list = QJsonArray();
				auto total = 0;
				result.match([&](const MTPDchannels_channelParticipants &d) {
					session->data().processUsers(d.vusers());
					session->data().processChats(d.vchats());
					const auto parsed = Api::ChatParticipants::Parse(
						channel,
						d);
					total = parsed.availableCount;
					for (const auto &participant : parsed.list) {
						const auto member = session->data().peerLoaded(
							participant.id());
						if (!member) {
							continue;
						}
						auto json = PeerJson(member);
						if (participant.isCreator()) {
							json.insert(u"role"_q, u"owner"_q);
						} else if (participant.isCreatorOrAdmin()) {
							json.insert(u"role"_q, u"admin"_q);
						}
						if (!participant.rank().isEmpty()) {
							json.insert(u"rank"_q, participant.rank());
						}
						list.push_back(json);
					}
				}, [](const MTPDchannels_channelParticipantsNotModified &) {
				});
				auto object = PeerJson(channel);
				object.insert(u"members_list"_q, list);
				object.insert(u"total"_q, total);
				done(Ok(object));
			}).fail([=](const MTP::Error &error) {
				done(Error(error.type()));
			}).send();
		} else if (const auto chat = peer->asChat()) {
			_api.request(MTPmessages_GetFullChat(
				chat->inputChat()
			)).done([=](const MTPmessages_ChatFull &result) {
				const auto &data = result.data();
				session->data().processUsers(data.vusers());
				session->data().processChats(data.vchats());
				auto list = QJsonArray();
				data.vfull_chat().match([&](const MTPDchatFull &full) {
					full.vparticipants().match([&](
							const MTPDchatParticipants &participants) {
						for (const auto &p : participants.vparticipants().v) {
							const auto userId = p.match([](const auto &d) {
								return d.vuser_id().v;
							});
							const auto user = session->data().userLoaded(
								UserId(userId));
							if (!user) {
								continue;
							}
							const auto name = user->name().toLower();
							if (!query.isEmpty()
								&& !name.contains(query.toLower())) {
								continue;
							}
							if (list.size() < limit) {
								list.push_back(userJson(user));
							}
						}
					}, [](const auto &) {});
				}, [](const auto &) {});
				auto object = PeerJson(chat);
				object.insert(u"members_list"_q, list);
				done(Ok(object));
			}).fail([=](const MTP::Error &error) {
				done(Error(error.type()));
			}).send();
		} else {
			done(Error(u"This is not a group."_q));
		}
	});
}

void TelegramTools::listFolders(Done done) {
	auto list = QJsonArray();
	for (const auto &filter : _session->data().chatsFilters().list()) {
		auto json = QJsonObject();
		json.insert(u"folder_id"_q, filter.id());
		json.insert(u"title"_q, filter.id()
			? filter.title().text.text
			: u"All chats"_q);
		list.push_back(json);
	}
	auto object = QJsonObject();
	object.insert(u"folders"_q, list);
	done(Ok(object));
}

void TelegramTools::sendMessage(const Args &args, Done done) {
	const auto text = args.value(u"text"_q).toString();
	const auto replyTo = MsgId(
		int64(args.value(u"reply_to"_q).toVariant().toLongLong()));
	if (text.trimmed().isEmpty()) {
		done(Error(u"text is empty."_q));
		return;
	}
	resolvePeer(args.value(u"chat_id"_q), [=](PeerData *peer, QString e) {
		if (!peer) {
			done(Error(e));
			return;
		} else if (!Data::CanSendTexts(peer)) {
			done(Error(u"You can't send messages to this chat."_q));
			return;
		}
		const auto history = _session->data().history(peer);
		auto action = Api::SendAction(history);
		action.clearDraft = false;
		if (replyTo) {
			action.replyTo.messageId = FullMsgId(peer->id, replyTo);
		}
		auto message = Api::MessageToSend(action);
		message.textWithTags = PrepareText(text);
		_session->api().sendMessage(std::move(message));
		auto result = PeerJson(peer);
		result.insert(u"status"_q, u"sent"_q);
		done(Ok(result));
	});
}

void TelegramTools::setDraft(
		const Args &args,
		Window::SessionController *window,
		Done done) {
	const auto text = args.value(u"text"_q).toString();
	const auto weak = base::make_weak(window);
	resolvePeer(args.value(u"chat_id"_q), [=](PeerData *peer, QString e) {
		if (!peer) {
			done(Error(e));
			return;
		}
		const auto history = _session->data().history(peer);
		auto tags = PrepareText(text);
		const auto size = int(tags.text.size());
		history->setLocalDraft(std::make_unique<Data::Draft>(
			tags,
			FullReplyTo(),
			SuggestOptions(),
			MessageCursor(size, size, Ui::kQFixedMax),
			Data::WebPageDraft()));
		if (const auto strong = weak.get()) {
			strong->showPeerHistory(peer);
		}
		_session->changes().historyUpdated(
			history,
			Data::HistoryUpdate::Flag::CloudDraft);
		auto result = PeerJson(peer);
		result.insert(u"status"_q, u"draft_set"_q);
		done(Ok(result));
	});
}

void TelegramTools::forwardMessages(const Args &args, Done done) {
	const auto ids = ReadIds(args.value(u"message_ids"_q));
	if (ids.empty()) {
		done(Error(u"message_ids is empty."_q));
		return;
	}
	const auto toValue = args.value(u"to_chat_id"_q);
	resolvePeer(args.value(u"from_chat_id"_q), [=](PeerData *from, QString e) {
		if (!from) {
			done(Error(e));
			return;
		}
		resolvePeer(toValue, [=](PeerData *to, QString e) {
			if (!to) {
				done(Error(e));
				return;
			} else if (!Data::CanSendAnything(to)) {
				done(Error(u"You can't send messages to the target chat."_q));
				return;
			}
			ensureMessages(from, ids, [=](Items items) {
				if (items.empty()) {
					done(Error(u"Messages not found."_q));
					return;
				}
				auto draft = Data::ResolvedForwardDraft{
					.items = HistoryItemsList(items.begin(), items.end()),
				};
				const auto history = _session->data().history(to);
				_session->api().forwardMessages(
					std::move(draft),
					Api::SendAction(history));
				auto result = PeerJson(to);
				result.insert(u"status"_q, u"forwarded"_q);
				result.insert(u"count"_q, int(items.size()));
				done(Ok(result));
			});
		});
	});
}

void TelegramTools::editMessage(const Args &args, Done done) {
	const auto id = MsgId(
		int64(args.value(u"message_id"_q).toVariant().toLongLong()));
	const auto text = args.value(u"text"_q).toString();
	resolvePeer(args.value(u"chat_id"_q), [=](PeerData *peer, QString e) {
		if (!peer) {
			done(Error(e));
			return;
		}
		ensureMessages(peer, { id }, [=](Items items) {
			if (items.empty()) {
				done(Error(u"Message not found."_q));
				return;
			}
			const auto item = items.front();
			if (!item->allowsEdit(base::unixtime::now())) {
				done(Error(u"This message can't be edited."_q));
				return;
			}
			auto tags = PrepareText(text);
			const auto entities = TextWithEntities{
				tags.text,
				TextUtilities::ConvertTextTagsToEntities(tags.tags),
			};
			Api::EditTextMessage(
				item,
				entities,
				Data::WebPageDraft(),
				Api::SendOptions(),
				crl::guard(this, [=](mtpRequestId) {
					auto result = QJsonObject();
					result.insert(u"status"_q, u"edited"_q);
					done(Ok(result));
				}),
				crl::guard(this, [=](const QString &error, mtpRequestId) {
					done(Error(error));
				}),
				false);
		});
	});
}

void TelegramTools::deleteMessages(const Args &args, Done done) {
	const auto ids = ReadIds(args.value(u"message_ids"_q));
	const auto revoke = args.value(u"for_everyone"_q).toBool(true);
	if (ids.empty()) {
		done(Error(u"message_ids is empty."_q));
		return;
	}
	resolvePeer(args.value(u"chat_id"_q), [=](PeerData *peer, QString e) {
		if (!peer) {
			done(Error(e));
			return;
		}
		ensureMessages(peer, ids, [=](Items items) {
			auto list = MessageIdsList();
			for (const auto &item : items) {
				if (item->canDelete()) {
					list.push_back(item->fullId());
				}
			}
			if (list.empty()) {
				done(Error(u"No messages that you can delete."_q));
				return;
			}
			_session->data().histories().deleteMessages(list, revoke);
			_session->data().sendHistoryChangeNotifications();
			auto result = QJsonObject();
			result.insert(u"status"_q, u"deleted"_q);
			result.insert(u"count"_q, int(list.size()));
			done(Ok(result));
		});
	});
}

void TelegramTools::pinMessage(const Args &args, Done done) {
	const auto id = MsgId(
		int64(args.value(u"message_id"_q).toVariant().toLongLong()));
	const auto unpin = args.value(u"unpin"_q).toBool();
	const auto session = _session;
	resolvePeer(args.value(u"chat_id"_q), [=](PeerData *peer, QString e) {
		if (!peer) {
			done(Error(e));
			return;
		}
		using Flag = MTPmessages_UpdatePinnedMessage::Flag;
		_api.request(MTPmessages_UpdatePinnedMessage(
			MTP_flags(unpin ? Flag::f_unpin : Flag(0)),
			peer->input(),
			MTP_int(id.bare)
		)).done([=](const MTPUpdates &result) {
			session->api().applyUpdates(result);
			auto object = QJsonObject();
			object.insert(u"status"_q, unpin ? u"unpinned"_q : u"pinned"_q);
			done(Ok(object));
		}).fail([=](const MTP::Error &error) {
			done(Error(error.type()));
		}).send();
	});
}

void TelegramTools::markRead(const Args &args, Done done) {
	resolvePeer(args.value(u"chat_id"_q), [=](PeerData *peer, QString e) {
		if (!peer) {
			done(Error(e));
			return;
		}
		const auto history = _session->data().history(peer);
		_session->data().histories().readInbox(history);
		auto result = PeerJson(peer);
		result.insert(u"status"_q, u"read"_q);
		done(Ok(result));
	});
}

void TelegramTools::createGroup(const Args &args, Done done) {
	const auto title = args.value(u"title"_q).toString().trimmed();
	if (title.isEmpty()) {
		done(Error(u"title is required."_q));
		return;
	}
	auto inputs = QVector<MTPInputUser>();
	for (const auto &value : args.value(u"user_ids"_q).toArray()) {
		const auto bare = value.toVariant().toString().toULongLong();
		const auto peer = bare
			? _session->data().peerLoaded(PeerId(PeerIdHelper(bare)))
			: nullptr;
		if (const auto user = peer ? peer->asUser() : nullptr) {
			inputs.push_back(user->inputUser());
		}
	}
	if (inputs.isEmpty()) {
		done(Error(u"No valid user_ids, use chat_id values of users."_q));
		return;
	}
	const auto session = _session;
	_api.request(MTPmessages_CreateChat(
		MTP_flags(0),
		MTP_vector<MTPInputUser>(inputs),
		MTP_string(title),
		MTPint() // ttl_period
	)).done([=](const MTPmessages_InvitedUsers &result) {
		session->api().applyUpdates(result.data().vupdates());
		auto object = QJsonObject();
		object.insert(u"status"_q, u"created"_q);
		object.insert(u"title"_q, title);
		object.insert(
			u"missing_invitees"_q,
			int(result.data().vmissing_invitees().v.size()));
		done(Ok(object));
	}).fail([=](const MTP::Error &error) {
		done(Error(error.type()));
	}).send();
}

void TelegramTools::addMembers(
		const Args &args,
		Window::SessionController *window,
		Done done) {
	if (!window) {
		done(Error(u"No window to show the result."_q));
		return;
	}
	const auto show = window->uiShow();
	const auto userValues = args.value(u"user_ids"_q).toArray();
	resolvePeer(args.value(u"chat_id"_q), [=](PeerData *peer, QString e) {
		if (!peer) {
			done(Error(e));
			return;
		}
		auto users = std::vector<not_null<UserData*>>();
		for (const auto &value : userValues) {
			const auto bare = value.toVariant().toString().toULongLong();
			const auto member = bare
				? _session->data().peerLoaded(PeerId(PeerIdHelper(bare)))
				: nullptr;
			if (const auto user = member ? member->asUser() : nullptr) {
				users.push_back(user);
			}
		}
		if (users.empty()) {
			done(Error(u"No valid user_ids."_q));
			return;
		}
		_session->api().chatParticipants().add(
			show,
			peer,
			users,
			true,
			crl::guard(this, [=](bool success) {
				if (success) {
					auto result = PeerJson(peer);
					result.insert(u"status"_q, u"added"_q);
					done(Ok(result));
				} else {
					done(Error(u"Failed to add members."_q));
				}
			}));
	});
}

void TelegramTools::openChat(
		const Args &args,
		Window::SessionController *window,
		Done done) {
	const auto messageId = MsgId(
		int64(args.value(u"message_id"_q).toVariant().toLongLong()));
	const auto weak = base::make_weak(window);
	resolvePeer(args.value(u"chat_id"_q), [=](PeerData *peer, QString e) {
		if (!peer) {
			done(Error(e));
			return;
		}
		const auto strong = weak.get();
		if (!strong) {
			done(Error(u"No window."_q));
			return;
		}
		strong->showPeerHistory(
			peer,
			Window::SectionShow::Way::ClearStack,
			messageId ? messageId : ShowAtUnreadMsgId);
		auto result = PeerJson(peer);
		result.insert(u"status"_q, u"opened"_q);
		done(Ok(result));
	});
}

} // namespace AiAgent
