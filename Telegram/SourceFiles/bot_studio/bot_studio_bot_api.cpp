/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "bot_studio/bot_studio_bot_api.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace BotStudio {
namespace {

constexpr auto kPollTimeout = 25;
constexpr auto kCallTimeout = 60 * 1000;
constexpr auto kMaxRetryDelay = 30 * 1000;

[[nodiscard]] std::optional<ApiError> ParseError(
		const QJsonObject &root,
		int status,
		const QString &networkError) {
	if (root.value(u"ok"_q).toBool()) {
		return std::nullopt;
	}
	auto result = ApiError();
	result.code = root.contains(u"error_code"_q)
		? root.value(u"error_code"_q).toInt()
		: status;
	result.text = root.value(u"description"_q).toString();
	if (result.text.isEmpty()) {
		result.text = networkError.isEmpty()
			? u"HTTP %1"_q.arg(status)
			: networkError;
	}
	result.retryAfter = root.value(
		u"parameters"_q).toObject().value(u"retry_after"_q).toInt();
	return result;
}

} // namespace

BotApi::BotApi(not_null<QNetworkAccessManager*> network, QString token)
: _network(network)
, _token(std::move(token)) {
	_retryTimer.setSingleShot(true);
	QObject::connect(&_retryTimer, &QTimer::timeout, [=] { poll(); });
}

BotApi::~BotApi() {
	stopPolling();
	for (const auto &reply : base::take(_replies)) {
		if (reply) {
			reply->disconnect();
			reply->abort();
			reply->deleteLater();
		}
	}
}

QString BotApi::BaseUrl() {
	const auto custom = qEnvironmentVariable("AGENTGRAM_BOT_API_URL");
	return custom.isEmpty() ? u"https://api.telegram.org"_q : custom;
}

void BotApi::call(
		const QString &method,
		const QJsonObject &params,
		Done done) {
	call(
		method,
		QJsonDocument(params).toJson(QJsonDocument::Compact),
		std::move(done));
}

void BotApi::call(const QString &method, const QByteArray &params, Done done) {
	auto request = QNetworkRequest(
		QUrl(BaseUrl() + u"/bot"_q + _token + u"/"_q + method));
	request.setHeader(
		QNetworkRequest::ContentTypeHeader,
		u"application/json"_q);
	const auto timeout = (method == u"getUpdates"_q)
		? (kPollTimeout + 15) * 1000
		: kCallTimeout;
	request.setTransferTimeout(timeout);
	const auto reply = _network->post(
		request,
		params.isEmpty() ? QByteArray("{}") : params);
	_replies.erase(
		ranges::remove_if(_replies, [](const auto &r) { return !r; }),
		end(_replies));
	_replies.push_back(reply);
	if (method == u"getUpdates"_q) {
		_pollReply = reply;
	}
	QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
		reply->deleteLater();
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto body = reply->readAll();
		const auto networkError = (reply->error() == QNetworkReply::NoError)
			? QString()
			: reply->errorString();
		const auto document = QJsonDocument::fromJson(body);
		const auto root = document.object();
		if (!document.isObject()) {
			done({}, ApiError{
				.text = networkError.isEmpty()
					? u"Invalid response, HTTP %1"_q.arg(status)
					: networkError,
				.code = status,
			});
			return;
		}
		if (const auto error = ParseError(root, status, networkError)) {
			done({}, error);
			return;
		}
		const auto result = root.value(u"result"_q);
		const auto wrapped = QJsonDocument(QJsonArray{ result }).toJson(
			QJsonDocument::Compact);
		// Strip the [ and ] around the single value.
		done(wrapped.mid(1, wrapped.size() - 2), std::nullopt);
	});
}

void BotApi::startPolling(
		Fn<void(QByteArray update)> update,
		Fn<void(ApiError error)> error) {
	_update = std::move(update);
	_error = std::move(error);
	_polling = true;
	_failures = 0;
	poll();
}

void BotApi::stopPolling() {
	_polling = false;
	_retryTimer.stop();
	if (const auto reply = _pollReply.data()) {
		_pollReply = nullptr;
		reply->disconnect();
		reply->abort();
		reply->deleteLater();
	}
}

void BotApi::poll() {
	if (!_polling || _pollReply) {
		return;
	}
	auto params = QJsonObject();
	params.insert(u"timeout"_q, kPollTimeout);
	if (_offset) {
		params.insert(u"offset"_q, double(_offset));
	}
	const auto weak = base::make_weak(this);
	call(u"getUpdates"_q, params, [=](
			QByteArray result,
			std::optional<ApiError> error) {
		if (!weak) {
			return;
		}
		_pollReply = nullptr;
		if (!_polling) {
			return;
		} else if (error) {
			pollFailed(*error);
			return;
		}
		_failures = 0;
		const auto updates = QJsonDocument::fromJson(result).array();
		for (const auto &value : updates) {
			const auto update = value.toObject();
			const auto id = int64(update.value(u"update_id"_q).toDouble());
			_offset = std::max(_offset, id + 1);
		}
		for (const auto &value : updates) {
			if (!weak || !_polling) {
				return;
			}
			_update(QJsonDocument(value.toObject()).toJson(
				QJsonDocument::Compact));
		}
		if (weak && _polling) {
			poll();
		}
	});
}

void BotApi::pollFailed(ApiError error) {
	++_failures;
	const auto weak = base::make_weak(this);
	if (_error) {
		_error(error);
	}
	if (!weak || !_polling) {
		return;
	}
	const auto delay = error.retryAfter
		? error.retryAfter * 1000
		: std::min(kMaxRetryDelay, 1000 * (1 << std::min(_failures, 5)));
	_retryTimer.start(delay);
}

} // namespace BotStudio
