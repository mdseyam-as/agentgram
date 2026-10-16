/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ai_agent/ai_agent_llm.h"
#include "base/weak_ptr.h"
#include "mtproto/sender.h"

class History;
class HistoryItem;
class PeerData;

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

namespace AiAgent {

struct ToolResult {
	QString text;
	bool error = false;
};

struct ToolPresentation {
	QString title;
	QString details;
	bool needsConfirmation = false;
};

class TelegramTools final : public base::has_weak_ptr {
public:
	explicit TelegramTools(not_null<Main::Session*> session);

	[[nodiscard]] std::vector<ToolDefinition> definitions() const;
	[[nodiscard]] ToolPresentation present(const ToolCall &call) const;

	void execute(
		const ToolCall &call,
		Window::SessionController *window,
		Fn<void(ToolResult)> done);

private:
	using Done = Fn<void(ToolResult)>;
	using Args = QJsonObject;

	void resolvePeer(
		const QJsonValue &value,
		Fn<void(PeerData*, QString)> done);
	void requestHistory(
		not_null<PeerData*> peer,
		int limit,
		int offsetId,
		Fn<void(std::vector<not_null<HistoryItem*>>, QString)> done);
	void ensureMessages(
		not_null<PeerData*> peer,
		std::vector<MsgId> ids,
		Fn<void(std::vector<not_null<HistoryItem*>>)> done);

	void getContext(Window::SessionController *window, Done done);
	void listChats(const Args &args, Done done);
	void findChats(const Args &args, Done done);
	void getChatInfo(const Args &args, Done done);
	void getMessages(const Args &args, Done done);
	void searchMessages(const Args &args, Done done);
	void getMembers(const Args &args, Done done);
	void listFolders(Done done);
	void unreadOverview(const Args &args, Done done);
	void sendMessage(const Args &args, Done done);
	void forwardMessages(const Args &args, Done done);
	void editMessage(const Args &args, Done done);
	void deleteMessages(const Args &args, Done done);
	void pinMessage(const Args &args, Done done);
	void markRead(const Args &args, Done done);
	void createGroup(const Args &args, Done done);
	void addMembers(
		const Args &args,
		Window::SessionController *window,
		Done done);
	void openChat(
		const Args &args,
		Window::SessionController *window,
		Done done);
	void setDraft(
		const Args &args,
		Window::SessionController *window,
		Done done);

	[[nodiscard]] QString peerTitle(const QJsonValue &value) const;

	const not_null<Main::Session*> _session;
	MTP::Sender _api;

};

} // namespace AiAgent
