/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <map>

#include "ai_agent/ai_agent_config.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>

class QNetworkAccessManager;
class QNetworkReply;

namespace AiAgent {

struct ToolCall {
	QString id;
	QString name;
	QString arguments;
};

struct ChatMessage {
	enum class Role {
		User,
		Assistant,
		Tool,
	};
	Role role = Role::User;
	QString text;
	std::vector<ToolCall> toolCalls;
	QString toolCallId;
	QString toolName;
	bool toolError = false;

	// Raw Anthropic content blocks, replayed unchanged (thinking, fallback).
	QJsonArray anthropicContent;
};

struct ToolDefinition {
	QString name;
	QString description;
	QJsonObject parameters;
};

struct RequestSettings {
	QString providerId;
	ApiFormat format = ApiFormat::OpenAIChat;
	QString baseUrl;
	QString apiKey;
	QString model;
	QString accountId;
	QString sessionId;
};

struct RequestHandlers {
	Fn<void(QString)> textDelta;
	Fn<void(ChatMessage)> done;
	Fn<void(QString error, int status)> fail;
};

class LlmRequest final {
public:
	LlmRequest(
		not_null<QNetworkAccessManager*> network,
		RequestSettings settings,
		QString system,
		const std::vector<ChatMessage> &history,
		const std::vector<ToolDefinition> &tools,
		RequestHandlers handlers);
	~LlmRequest();

	void start();
	void cancel();

private:
	struct PendingCall {
		QString id;
		QString name;
		QString arguments;
	};
	struct PendingBlock {
		QJsonObject block;
		QString text;
		QString thinking;
		QString signature;
		QString json;
	};

	[[nodiscard]] QByteArray prepareBody() const;
	[[nodiscard]] QByteArray prepareOpenAIChat() const;
	[[nodiscard]] QByteArray prepareResponses() const;
	[[nodiscard]] QByteArray prepareAnthropic() const;

	void readyRead();
	void finished();
	void processEvent(const QByteArray &event, const QByteArray &data);
	void processOpenAIChat(const QJsonObject &data);
	void processResponses(const QByteArray &event, const QJsonObject &data);
	void processAnthropic(const QByteArray &event, const QJsonObject &data);
	void appendText(const QString &text);
	void complete();
	void failWith(const QString &error, int status = 0);

	const not_null<QNetworkAccessManager*> _network;
	const RequestSettings _settings;
	const QString _system;
	const std::vector<ChatMessage> _history;
	const std::vector<ToolDefinition> _tools;
	RequestHandlers _handlers;

	QPointer<QNetworkReply> _reply;
	QByteArray _buffer;
	QByteArray _errorBody;
	int _status = 0;
	bool _finished = false;

	QString _text;
	std::map<int, PendingCall> _calls;
	std::vector<ToolCall> _responseCalls;
	std::map<int, PendingBlock> _blocks;
	QString _stopReason;

};

struct ModelsHandlers {
	Fn<void(QStringList)> done;
	Fn<void(QString)> fail;
};

void FetchModels(
	not_null<QNetworkAccessManager*> network,
	const RequestSettings &settings,
	ModelsHandlers handlers);

[[nodiscard]] QString ErrorFromBody(const QByteArray &body, int status);

} // namespace AiAgent
