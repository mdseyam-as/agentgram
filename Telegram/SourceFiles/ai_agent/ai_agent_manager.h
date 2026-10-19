/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <deque>

#include "ai_agent/ai_agent_config.h"
#include "ai_agent/ai_agent_llm.h"
#include "base/flat_set.h"
#include "base/weak_ptr.h"

class QNetworkAccessManager;

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

namespace AiAgent {

class TelegramTools;
class OAuthFlow;

struct Entry {
	enum class Type {
		User,
		Assistant,
		Tool,
		Error,
	};
	enum class ToolState {
		Running,
		AwaitingConfirmation,
		Done,
		Failed,
		Rejected,
	};
	uint64 id = 0;
	Type type = Type::User;
	QString text;
	QString title;
	QString details;
	ToolState toolState = ToolState::Running;
	bool streaming = false;
};

class Manager final : public base::has_weak_ptr {
public:
	explicit Manager(not_null<Main::Session*> session);
	~Manager();

	[[nodiscard]] static not_null<Manager*> Get(
		not_null<Main::Session*> session);

	[[nodiscard]] Main::Session &session() const {
		return *_session;
	}
	[[nodiscard]] not_null<QNetworkAccessManager*> network() const;

	[[nodiscard]] const Config &config() const {
		return _config;
	}
	void updateConfig(Fn<void(Config&)> change);
	[[nodiscard]] rpl::producer<> configChanges() const;
	[[nodiscard]] QString statusText() const;

	[[nodiscard]] const std::vector<Entry> &entries() const {
		return _entries;
	}
	[[nodiscard]] const Entry *entry(uint64 id) const;
	[[nodiscard]] rpl::producer<uint64> entryAdded() const;
	[[nodiscard]] rpl::producer<uint64> entryUpdated() const;
	[[nodiscard]] rpl::producer<> entriesReset() const;

	[[nodiscard]] bool busy() const;
	[[nodiscard]] rpl::producer<bool> busyValue() const;

	void send(const QString &text, Window::SessionController *window);
	void stop();
	void newConversation();
	void resolveConfirmation(uint64 entryId, bool approved);
	void setAutoApprove(bool enabled);

	void startOAuth(
		const QString &providerId,
		Fn<void(QString url)> started,
		Fn<void(QString error)> finished);
	void cancelOAuth();
	void logout(const QString &providerId);
	void fetchModels(
		const QString &providerId,
		Fn<void(QStringList models, QString error)> done);
	void refreshModelsIfStale(const QString &providerId);

private:
	struct PendingTool {
		ToolCall call;
		uint64 entryId = 0;
	};

	void load();
	void save();
	[[nodiscard]] QString systemPrompt() const;
	[[nodiscard]] RequestSettings requestSettings() const;
	void trimHistory();

	void step();
	void stepWithFreshToken();
	void requestFinished(ChatMessage message);
	void requestFailed(const QString &error, int status);
	void runNextTool();
	void executeTool(PendingTool tool);
	void toolFinished(const PendingTool &tool, const QString &text, bool error);
	void finish();

	uint64 addEntry(Entry entry);
	void updateEntry(uint64 id, Fn<void(Entry&)> change);
	void addError(const QString &text);

	const not_null<Main::Session*> _session;
	const std::unique_ptr<QNetworkAccessManager> _network;
	const std::unique_ptr<TelegramTools> _tools;
	Config _config;
	rpl::event_stream<> _configChanges;

	std::vector<ChatMessage> _history;
	std::vector<Entry> _entries;
	uint64 _entryIdCounter = 0;
	rpl::event_stream<uint64> _entryAdded;
	rpl::event_stream<uint64> _entryUpdated;
	rpl::event_stream<> _entriesReset;

	rpl::variable<bool> _busy = false;
	std::unique_ptr<LlmRequest> _request;
	uint64 _streamingEntryId = 0;
	std::deque<PendingTool> _pendingTools;
	std::optional<PendingTool> _awaiting;
	int _steps = 0;
	bool _triedRefresh = false;
	QString _conversationId;
	base::weak_ptr<Window::SessionController> _window;

	std::unique_ptr<OAuthFlow> _oauth;
	base::flat_set<QString> _modelsLoading;

};

} // namespace AiAgent
