/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace AiAgent {

enum class ApiFormat {
	OpenAIChat,
	OpenAIResponses,
	Anthropic,
};

enum class OAuthKind {
	None,
	ChatGPT,
	OpenRouter,
};

struct ProviderInfo {
	QString id;
	QString name;
	QString description;
	ApiFormat format = ApiFormat::OpenAIChat;
	QString defaultBaseUrl;
	QString keysUrl;
	QStringList models;
	OAuthKind oauth = OAuthKind::None;
	bool apiKey = true;
	bool keyOptional = false;
	bool editableBaseUrl = false;
};

[[nodiscard]] const std::vector<ProviderInfo> &Providers();
[[nodiscard]] const ProviderInfo &FindProvider(const QString &id);

struct ProviderSettings {
	QString apiKey;
	QString baseUrl;
	QString model;

	QString accessToken;
	QString refreshToken;
	QString accountId;
	QString accountEmail;
	int64 expiresAt = 0;

	[[nodiscard]] bool loggedIn() const {
		return !accessToken.isEmpty();
	}
};

struct Config {
	QString provider = u"chatgpt"_q;
	std::map<QString, ProviderSettings> providers;
	bool confirmActions = true;
	QString customInstructions;
	int maxSteps = 30;

	[[nodiscard]] ProviderSettings &settings(const QString &id);
	[[nodiscard]] const ProviderSettings *find(const QString &id) const;
	[[nodiscard]] QString baseUrl(const QString &id) const;
	[[nodiscard]] QString model(const QString &id) const;
	[[nodiscard]] bool ready(const QString &id) const;

	[[nodiscard]] QByteArray serialize() const;
	[[nodiscard]] static Config Deserialize(const QByteArray &data);
};

} // namespace AiAgent
