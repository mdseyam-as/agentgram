/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ai_agent/ai_agent_oauth.h"

#include "ai_agent/ai_agent_llm.h"
#include "base/random.h"
#include "base/unixtime.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

namespace AiAgent {
namespace {

// Public PKCE client used by the open-source Codex CLI for ChatGPT sign-in.
constexpr auto kChatGPTClientId = "app_EMoamEEZ73f0CkXaXp7hrann";
constexpr auto kChatGPTIssuer = "https://auth.openai.com";
constexpr auto kChatGPTPort = 1455;
constexpr auto kOpenRouterPort = 3000;
constexpr auto kRefreshBeforeExpiry = 5 * 60;

[[nodiscard]] QByteArray Base64Url(const QByteArray &data) {
	return data.toBase64(
		QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

[[nodiscard]] QString RandomToken(int size) {
	auto bytes = QByteArray(size, Qt::Uninitialized);
	base::RandomFill(bytes.data(), bytes.size());
	return QString::fromLatin1(Base64Url(bytes));
}

[[nodiscard]] QJsonObject JwtClaims(const QString &token) {
	const auto parts = token.split('.');
	if (parts.size() < 2) {
		return {};
	}
	const auto payload = QByteArray::fromBase64(
		parts[1].toLatin1(),
		QByteArray::Base64UrlEncoding);
	return QJsonDocument::fromJson(payload).object();
}

void FillFromTokens(OAuthResult &result, const QJsonObject &response) {
	result.accessToken = response.value(u"access_token"_q).toString();
	const auto refresh = response.value(u"refresh_token"_q).toString();
	if (!refresh.isEmpty()) {
		result.refreshToken = refresh;
	}
	const auto expiresIn = response.value(u"expires_in"_q).toInt();
	const auto idToken = response.value(u"id_token"_q).toString();
	const auto claims = JwtClaims(idToken.isEmpty()
		? result.accessToken
		: idToken);
	const auto access = JwtClaims(result.accessToken);
	const auto auth = claims.value(u"https://api.openai.com/auth"_q)
		.toObject();
	const auto accessAuth = access.value(u"https://api.openai.com/auth"_q)
		.toObject();
	result.accountId = auth.value(u"chatgpt_account_id"_q).toString();
	if (result.accountId.isEmpty()) {
		result.accountId = accessAuth.value(
			u"chatgpt_account_id"_q).toString();
	}
	result.accountEmail = claims.value(u"email"_q).toString();
	if (result.accountEmail.isEmpty()) {
		result.accountEmail = access.value(
			u"https://api.openai.com/profile"_q
		).toObject().value(u"email"_q).toString();
	}
	const auto exp = int64(access.value(u"exp"_q).toDouble());
	result.expiresAt = (expiresIn > 0)
		? (base::unixtime::now() + expiresIn)
		: exp;
}

[[nodiscard]] QByteArray SuccessPage(bool success, const QString &text) {
	const auto title = success
		? u"Готово!"_q
		: u"Не получилось"_q;
	const auto html = uR"(<!doctype html><html><head><meta charset="utf-8">
<title>Agentgram</title><style>
body{font-family:system-ui,sans-serif;background:#0f1720;color:#e8eef5;
display:flex;align-items:center;justify-content:center;height:100vh;margin:0}
.card{background:#17212b;padding:40px 48px;border-radius:20px;
text-align:center;max-width:420px;box-shadow:0 10px 40px #0006}
h1{margin:0 0 12px;font-size:26px}p{margin:0;color:#9fb0c2;line-height:1.5}
</style></head><body><div class="card"><h1>%1</h1><p>%2</p></div>
</body></html>)"_q.arg(title.toHtmlEscaped(), text.toHtmlEscaped());
	const auto body = html.toUtf8();
	return "HTTP/1.1 200 OK\r\n"
		"Content-Type: text/html; charset=utf-8\r\n"
		"Connection: close\r\n"
		"Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n"
		+ body;
}

} // namespace

OAuthFlow::OAuthFlow(
	not_null<QNetworkAccessManager*> network,
	OAuthKind kind,
	Fn<void(OAuthResult)> done,
	Fn<void(QString)> fail)
: _network(network)
, _kind(kind)
, _done(std::move(done))
, _fail(std::move(fail)) {
}

OAuthFlow::~OAuthFlow() {
	for (const auto &server : _servers) {
		server->disconnect();
		server->close();
	}
}

bool OAuthFlow::listen() {
	const auto port = (_kind == OAuthKind::ChatGPT)
		? kChatGPTPort
		: kOpenRouterPort;
	for (const auto &address : {
			QHostAddress(QHostAddress::LocalHost),
			QHostAddress(QHostAddress::LocalHostIPv6) }) {
		auto server = std::make_unique<QTcpServer>();
		if (!server->listen(address, port)) {
			continue;
		}
		const auto raw = server.get();
		QObject::connect(raw, &QTcpServer::newConnection, raw, [=] {
			while (const auto socket = raw->nextPendingConnection()) {
				handleConnection(socket);
			}
		});
		_servers.push_back(std::move(server));
	}
	return !_servers.empty();
}

QString OAuthFlow::start() {
	if (!listen()) {
		return QString();
	}
	_verifier = RandomToken(64);
	_state = RandomToken(32);
	const auto challenge = QString::fromLatin1(Base64Url(
		QCryptographicHash::hash(
			_verifier.toLatin1(),
			QCryptographicHash::Sha256)));
	auto query = QUrlQuery();
	if (_kind == OAuthKind::ChatGPT) {
		_redirectUri = u"http://localhost:%1/auth/callback"_q.arg(
			kChatGPTPort);
		query.addQueryItem(u"response_type"_q, u"code"_q);
		query.addQueryItem(
			u"client_id"_q,
			QString::fromLatin1(kChatGPTClientId));
		query.addQueryItem(u"redirect_uri"_q, _redirectUri);
		query.addQueryItem(
			u"scope"_q,
			u"openid profile email offline_access"_q);
		query.addQueryItem(u"code_challenge"_q, challenge);
		query.addQueryItem(u"code_challenge_method"_q, u"S256"_q);
		query.addQueryItem(u"id_token_add_organizations"_q, u"true"_q);
		query.addQueryItem(u"codex_cli_simplified_flow"_q, u"true"_q);
		query.addQueryItem(u"state"_q, _state);
		query.addQueryItem(u"originator"_q, u"codex_cli_rs"_q);
		auto url = QUrl(QString::fromLatin1(kChatGPTIssuer)
			+ u"/oauth/authorize"_q);
		url.setQuery(query);
		return url.toString(QUrl::FullyEncoded);
	}
	_redirectUri = u"http://localhost:%1/callback"_q.arg(kOpenRouterPort);
	query.addQueryItem(u"callback_url"_q, _redirectUri);
	query.addQueryItem(u"code_challenge"_q, challenge);
	query.addQueryItem(u"code_challenge_method"_q, u"S256"_q);
	auto url = QUrl(u"https://openrouter.ai/auth"_q);
	url.setQuery(query);
	return url.toString(QUrl::FullyEncoded);
}

void OAuthFlow::handleConnection(not_null<QTcpSocket*> socket) {
	const auto buffer = std::make_shared<QByteArray>();
	const auto handled = std::make_shared<bool>(false);
	QObject::connect(socket, &QTcpSocket::disconnected, socket, [=] {
		socket->deleteLater();
	});
	QObject::connect(socket, &QTcpSocket::readyRead, socket, [=] {
		if (*handled) {
			socket->readAll();
			return;
		}
		*buffer += socket->readAll();
		const auto end = buffer->indexOf("\r\n");
		if (end < 0) {
			if (buffer->size() > 16 * 1024) {
				socket->abort();
			}
			return;
		}
		const auto line = buffer->left(end);
		*handled = true;
		handleRequest(socket, line);
	});
}

void OAuthFlow::handleRequest(
		not_null<QTcpSocket*> socket,
		const QByteArray &line) {
	const auto parts = line.split(' ');
	const auto target = (parts.size() >= 2) ? parts[1] : QByteArray();
	const auto url = QUrl(QString::fromLatin1(target));
	const auto expectedPath = (_kind == OAuthKind::ChatGPT)
		? u"/auth/callback"_q
		: u"/callback"_q;
	if (url.path() != expectedPath) {
		socket->write("HTTP/1.1 404 Not Found\r\n"
			"Connection: close\r\nContent-Length: 0\r\n\r\n");
		socket->disconnectFromHost();
		return;
	}
	const auto query = QUrlQuery(url);
	const auto code = query.queryItemValue(u"code"_q);
	const auto state = query.queryItemValue(u"state"_q);
	const auto error = query.queryItemValue(u"error"_q);
	auto problem = QString();
	if (!error.isEmpty()) {
		problem = query.queryItemValue(
			u"error_description"_q,
			QUrl::FullyDecoded);
		if (problem.isEmpty()) {
			problem = error;
		}
	} else if (_kind == OAuthKind::ChatGPT && state != _state) {
		problem = u"Неверный параметр state."_q;
	} else if (code.isEmpty()) {
		problem = u"Сервер не вернул код авторизации."_q;
	}
	socket->write(SuccessPage(
		problem.isEmpty(),
		problem.isEmpty()
			? u"Вход выполнен. Вернитесь в Agentgram, "
				"вкладку браузера можно закрыть."_q
			: problem));
	socket->disconnectFromHost();
	if (!problem.isEmpty()) {
		finishWith(std::nullopt, problem);
	} else if (!_exchanging) {
		_exchanging = true;
		exchange(code);
	}
}

void OAuthFlow::exchange(const QString &code) {
	auto request = QNetworkRequest();
	auto body = QByteArray();
	if (_kind == OAuthKind::ChatGPT) {
		request.setUrl(QUrl(QString::fromLatin1(kChatGPTIssuer)
			+ u"/oauth/token"_q));
		request.setHeader(
			QNetworkRequest::ContentTypeHeader,
			u"application/x-www-form-urlencoded"_q);
		auto form = QUrlQuery();
		form.addQueryItem(u"grant_type"_q, u"authorization_code"_q);
		form.addQueryItem(
			u"client_id"_q,
			QString::fromLatin1(kChatGPTClientId));
		form.addQueryItem(u"code"_q, code);
		form.addQueryItem(u"redirect_uri"_q, _redirectUri);
		form.addQueryItem(u"code_verifier"_q, _verifier);
		body = form.toString(QUrl::FullyEncoded).toUtf8();
	} else {
		request.setUrl(QUrl(u"https://openrouter.ai/api/v1/auth/keys"_q));
		request.setHeader(
			QNetworkRequest::ContentTypeHeader,
			u"application/json"_q);
		auto object = QJsonObject();
		object.insert(u"code"_q, code);
		object.insert(u"code_verifier"_q, _verifier);
		object.insert(u"code_challenge_method"_q, u"S256"_q);
		body = QJsonDocument(object).toJson(QJsonDocument::Compact);
	}
	const auto reply = _network->post(request, body);
	const auto kind = _kind;
	QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
		reply->deleteLater();
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto data = reply->readAll();
		if (status >= 400 || reply->error() != QNetworkReply::NoError) {
			finishWith(std::nullopt, data.isEmpty()
				? reply->errorString()
				: ErrorFromBody(data, status));
			return;
		}
		const auto object = QJsonDocument::fromJson(data).object();
		auto result = OAuthResult();
		if (kind == OAuthKind::ChatGPT) {
			FillFromTokens(result, object);
			if (result.accessToken.isEmpty()) {
				finishWith(std::nullopt, u"Пустой ответ сервера."_q);
				return;
			}
		} else {
			result.apiKey = object.value(u"key"_q).toString();
			if (result.apiKey.isEmpty()) {
				finishWith(std::nullopt, u"OpenRouter не выдал ключ."_q);
				return;
			}
		}
		finishWith(result, QString());
	});
}

void OAuthFlow::finishWith(
		std::optional<OAuthResult> result,
		const QString &error) {
	for (const auto &server : _servers) {
		server->close();
	}
	if (result) {
		if (const auto done = base::take(_done)) {
			_fail = nullptr;
			done(std::move(*result));
		}
	} else if (const auto fail = base::take(_fail)) {
		_done = nullptr;
		fail(error);
	}
}

void RefreshChatGPTToken(
		not_null<QNetworkAccessManager*> network,
		const QString &refreshToken,
		Fn<void(OAuthResult)> done,
		Fn<void(QString)> fail) {
	auto request = QNetworkRequest(QUrl(
		QString::fromLatin1(kChatGPTIssuer) + u"/oauth/token"_q));
	request.setHeader(
		QNetworkRequest::ContentTypeHeader,
		u"application/json"_q);
	auto object = QJsonObject();
	object.insert(u"client_id"_q, QString::fromLatin1(kChatGPTClientId));
	object.insert(u"grant_type"_q, u"refresh_token"_q);
	object.insert(u"refresh_token"_q, refreshToken);
	object.insert(u"scope"_q, u"openid profile email"_q);
	const auto reply = network->post(
		request,
		QJsonDocument(object).toJson(QJsonDocument::Compact));
	QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
		reply->deleteLater();
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto data = reply->readAll();
		if (status >= 400 || reply->error() != QNetworkReply::NoError) {
			fail(data.isEmpty()
				? reply->errorString()
				: ErrorFromBody(data, status));
			return;
		}
		auto result = OAuthResult{ .refreshToken = refreshToken };
		FillFromTokens(result, QJsonDocument::fromJson(data).object());
		if (result.accessToken.isEmpty()) {
			fail(u"Не удалось обновить токен ChatGPT."_q);
		} else {
			done(std::move(result));
		}
	});
}

bool ChatGPTTokenExpiring(const ProviderSettings &settings) {
	return settings.expiresAt > 0
		&& (settings.expiresAt - kRefreshBeforeExpiry
			< base::unixtime::now());
}

} // namespace AiAgent
