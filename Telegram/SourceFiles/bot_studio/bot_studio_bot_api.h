/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/weak_ptr.h"

#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>

class QNetworkAccessManager;
class QNetworkReply;

namespace BotStudio {

struct ApiError {
	QString text;
	int code = 0;
	int retryAfter = 0;
};

// Telegram Bot API client: plain HTTPS calls plus a getUpdates long
// polling loop, so a bot runs right inside the app without a server.
class BotApi final : public base::has_weak_ptr {
public:
	using Done = Fn<void(QByteArray result, std::optional<ApiError> error)>;

	BotApi(not_null<QNetworkAccessManager*> network, QString token);
	~BotApi();

	[[nodiscard]] static QString BaseUrl();

	// params is a JSON object, result is the JSON of the "result" field.
	void call(const QString &method, const QByteArray &params, Done done);
	void call(const QString &method, const QJsonObject &params, Done done);

	void startPolling(
		Fn<void(QByteArray update)> update,
		Fn<void(ApiError error)> error);
	void stopPolling();
	[[nodiscard]] bool polling() const {
		return _polling;
	}

private:
	void poll();
	void pollFailed(ApiError error);

	const not_null<QNetworkAccessManager*> _network;
	const QString _token;
	std::vector<QPointer<QNetworkReply>> _replies;
	QPointer<QNetworkReply> _pollReply;
	Fn<void(QByteArray)> _update;
	Fn<void(ApiError)> _error;
	QTimer _retryTimer;
	int64 _offset = 0;
	int _failures = 0;
	bool _polling = false;

};

} // namespace BotStudio
