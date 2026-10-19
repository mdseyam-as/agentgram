/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/weak_ptr.h"

#include <QtCore/QJsonObject>

class QTimer;

struct JSRuntime;
struct JSContext;

namespace BotStudio {

enum class LogLevel {
	Info,
	Warning,
	Error,
	Incoming,
	System,
};

struct HttpResponse {
	int status = 0;
	QByteArray body;
	QJsonObject headers;
	QString error;
};

struct RuntimeHost {
	// Bot API call, params and result are JSON.
	Fn<void(
		QString method,
		QByteArray params,
		Fn<void(QByteArray result, QString error)> done)> call;
	// {system, prompt, history: [{role, text}]} -> answer text.
	Fn<void(
		QJsonObject request,
		Fn<void(QString text, QString error)> done)> ai;
	// {url, method, headers, body}.
	Fn<void(QJsonObject request, Fn<void(HttpResponse)> done)> http;
	Fn<void(LogLevel level, QString text)> log;
	Fn<void(QByteArray storage)> storageSave;
};

struct RuntimeDescriptor {
	RuntimeHost host;
	QByteArray storage;
	QByteArray me;
	int64 ownerId = 0;
};

// One bot script in its own QuickJS runtime. Everything a script can do
// goes through RuntimeHost, the engine has no file or process access.
class Runtime final : public base::has_weak_ptr {
public:
	explicit Runtime(RuntimeDescriptor descriptor);
	~Runtime();

	// Returns an error text or an empty string.
	[[nodiscard]] QString load(const QString &code);
	void dispatch(const QByteArray &update);
	// {text, photo?, buttons?, html?} to every private chat of the bot.
	void broadcast(const QByteArray &request);

	// Compiles without running, returns an error text or an empty string.
	[[nodiscard]] static QString Check(const QString &code);

	[[nodiscard]] static QString Documentation();

private:
	struct Pending;
	struct Rejected;
	struct Native;
	friend struct Native;

	void enter();
	void drain();
	void reportException(JSContext *ctx, const QString &where);
	uint64 createPromise(void *result);
	void settle(uint64 id, bool success, QByteArray json, QString error);
	void settleString(uint64 id, QString text);
	void callGlobal(const char *name, const QByteArray &argument);
	void callTimer(int id);

	RuntimeHost _host;
	JSRuntime *_rt = nullptr;
	JSContext *_ctx = nullptr;
	std::map<uint64, std::unique_ptr<Pending>> _pending;
	std::map<int, std::unique_ptr<QTimer>> _timers;
	std::vector<Rejected> _rejections;
	QByteArray _storage;
	QByteArray _me;
	int64 _ownerId = 0;
	crl::time _deadline = 0;
	uint64 _promiseId = 0;
	bool _destroying = false;

};

} // namespace BotStudio
