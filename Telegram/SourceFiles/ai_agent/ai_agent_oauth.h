/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ai_agent/ai_agent_config.h"

#include <QtCore/QPointer>

class QNetworkAccessManager;
class QTcpServer;
class QTcpSocket;

namespace AiAgent {

struct OAuthResult {
	QString apiKey;
	QString accessToken;
	QString refreshToken;
	QString accountId;
	QString accountEmail;
	int64 expiresAt = 0;
};

class OAuthFlow final {
public:
	OAuthFlow(
		not_null<QNetworkAccessManager*> network,
		OAuthKind kind,
		Fn<void(OAuthResult)> done,
		Fn<void(QString)> fail);
	~OAuthFlow();

	[[nodiscard]] QString start();

private:
	bool listen();
	void handleConnection(not_null<QTcpSocket*> socket);
	void handleRequest(not_null<QTcpSocket*> socket, const QByteArray &line);
	void exchange(const QString &code);
	void finishWith(std::optional<OAuthResult> result, const QString &error);

	const not_null<QNetworkAccessManager*> _network;
	const OAuthKind _kind;
	Fn<void(OAuthResult)> _done;
	Fn<void(QString)> _fail;

	std::vector<std::unique_ptr<QTcpServer>> _servers;
	QString _verifier;
	QString _state;
	QString _redirectUri;
	bool _exchanging = false;

};

void RefreshChatGPTToken(
	not_null<QNetworkAccessManager*> network,
	const QString &refreshToken,
	Fn<void(OAuthResult)> done,
	Fn<void(QString)> fail);

[[nodiscard]] bool ChatGPTTokenExpiring(const ProviderSettings &settings);

} // namespace AiAgent
