/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/flat_map.h"
#include "base/flat_set.h"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "bot_studio/bot_studio_runtime.h"

#include <deque>

class QNetworkAccessManager;

namespace Main {
class Session;
} // namespace Main

namespace BotStudio {

class BotApi;

enum class BotStatus {
	Stopped,
	Starting,
	Running,
	Error,
};

struct BotInfo {
	QString id;
	QString name;
	QString username;
	int64 botId = 0;
	QString token;
	QString code;
	// Visual constructor scheme as compact JSON, empty for code bots.
	QString flow;
	bool enabled = false;
	int64 created = 0;
	int64 updated = 0;

	[[nodiscard]] bool visual() const {
		return !flow.isEmpty();
	}
};

struct LogEntry {
	QDateTime when;
	LogLevel level = LogLevel::Info;
	QString text;
};

struct Template {
	QString id;
	QString title;
	QString description;
	QString code;
};

[[nodiscard]] const std::vector<Template> &Templates();

// All bots of one Telegram account. Bots with enabled == true run while
// the account is open in the app.
class Studio final : public base::has_weak_ptr {
public:
	explicit Studio(not_null<Main::Session*> session);
	~Studio();

	[[nodiscard]] static not_null<Studio*> Get(
		not_null<Main::Session*> session);

	[[nodiscard]] Main::Session &session() const {
		return *_session;
	}

	[[nodiscard]] const std::vector<BotInfo> &bots() const {
		return _bots;
	}
	[[nodiscard]] const BotInfo *bot(const QString &id) const;
	// By id, @username or name.
	[[nodiscard]] const BotInfo *find(const QString &query) const;
	[[nodiscard]] BotStatus status(const QString &id) const;
	[[nodiscard]] QString statusError(const QString &id) const;
	[[nodiscard]] int errorCount(const QString &id) const;
	[[nodiscard]] const std::deque<LogEntry> &logs(const QString &id) const;
	void clearLogs(const QString &id);
	[[nodiscard]] QByteArray storage(const QString &id) const;
	void clearStorage(const QString &id);

	[[nodiscard]] rpl::producer<> botsChanged() const;
	[[nodiscard]] rpl::producer<QString> botUpdated() const;
	[[nodiscard]] rpl::producer<QString> logAdded() const;

	// Validates the token with getMe, done(id, error).
	void create(
		const QString &token,
		const QString &code,
		const QJsonObject &flow,
		Fn<void(QString id, QString error)> done);
	void setToken(
		const QString &id,
		const QString &token,
		Fn<void(QString error)> done);
	// Saves the code and restarts a running bot. Returns a syntax error,
	// in that case a running bot keeps the old code.
	QString setCode(const QString &id, const QString &code);
	// Saves a visual scheme, generates its code and restarts the bot.
	QString setFlow(const QString &id, const QJsonObject &flow);
	[[nodiscard]] QJsonObject flow(const QString &id) const;
	// Keeps the generated code and drops the scheme.
	void convertToCode(const QString &id);
	void start(const QString &id);
	void stop(const QString &id);
	void restart(const QString &id);
	void remove(const QString &id);

	// Waits until a starting bot is running or failed.
	void whenStarted(const QString &id, Fn<void()> callback);

private:
	struct Instance;

	QString applyCode(const QString &id, const QString &code);
	void load();
	void save();
	void launch(const QString &id);
	void shutdown(const QString &id);
	void setStatus(
		const QString &id,
		BotStatus status,
		const QString &error = QString());
	void addLog(const QString &id, LogLevel level, const QString &text);
	void storageChanged(const QString &id, QByteArray data);
	void saveStorage();
	[[nodiscard]] QByteArray loadStorage(const QString &id) const;
	[[nodiscard]] RuntimeHost makeHost(const QString &id);
	[[nodiscard]] BotInfo *botMutable(const QString &id);
	[[nodiscard]] BotApi *instanceApi(const QString &id) const;

	const not_null<Main::Session*> _session;
	const std::unique_ptr<QNetworkAccessManager> _network;
	std::vector<BotInfo> _bots;
	base::flat_map<QString, std::unique_ptr<Instance>> _instances;
	base::flat_map<QString, std::deque<LogEntry>> _logs;
	base::flat_map<QString, int> _errors;
	base::flat_map<QString, QByteArray> _storage;
	base::flat_set<QString> _storageDirty;
	base::flat_map<QString, std::vector<Fn<void()>>> _startWaiters;
	base::Timer _storageTimer;

	rpl::event_stream<> _botsChanged;
	rpl::event_stream<QString> _botUpdated;
	rpl::event_stream<QString> _logAdded;

};

[[nodiscard]] QString StatusText(BotStatus status);

} // namespace BotStudio
