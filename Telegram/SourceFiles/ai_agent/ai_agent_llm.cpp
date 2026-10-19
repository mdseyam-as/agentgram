/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ai_agent/ai_agent_llm.h"

#include <QtCore/QJsonDocument>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace AiAgent {
namespace {

constexpr auto kAnthropicMaxTokens = 32000;
constexpr auto kTransferTimeout = 300 * 1000;
// The ChatGPT backend hides models whose minimal_client_version is newer
// than the version we report, so report one newer than any real release.
constexpr auto kCodexModelsClientVersion = "99.0.0";

[[nodiscard]] QJsonObject ParseObject(const QString &json) {
	const auto document = QJsonDocument::fromJson(json.toUtf8());
	return document.isObject() ? document.object() : QJsonObject();
}

[[nodiscard]] QString Serialize(const QJsonObject &object) {
	return QString::fromUtf8(
		QJsonDocument(object).toJson(QJsonDocument::Compact));
}

[[nodiscard]] bool SupportsServerFallback(const QString &model) {
	return model.startsWith(u"claude-opus-5"_q)
		|| model.startsWith(u"claude-fable-5"_q)
		|| (model == u"claude-sonnet-5-5"_q);
}

[[nodiscard]] QNetworkRequest PrepareRequest(
		const RequestSettings &settings,
		const QString &path) {
	auto request = QNetworkRequest(QUrl(settings.baseUrl + path));
	request.setTransferTimeout(kTransferTimeout);
	request.setHeader(
		QNetworkRequest::ContentTypeHeader,
		u"application/json"_q);
	request.setRawHeader("User-Agent", "Agentgram/1.0");
	switch (settings.format) {
	case ApiFormat::Anthropic:
		request.setRawHeader("x-api-key", settings.apiKey.toUtf8());
		request.setRawHeader("anthropic-version", "2023-06-01");
		break;
	case ApiFormat::OpenAIResponses:
		request.setRawHeader(
			"Authorization",
			"Bearer " + settings.apiKey.toUtf8());
		request.setRawHeader("OpenAI-Beta", "responses=experimental");
		request.setRawHeader("originator", "codex_cli_rs");
		if (!settings.accountId.isEmpty()) {
			request.setRawHeader(
				"chatgpt-account-id",
				settings.accountId.toUtf8());
		}
		if (!settings.sessionId.isEmpty()) {
			request.setRawHeader("session_id", settings.sessionId.toUtf8());
		}
		break;
	case ApiFormat::OpenAIChat:
		if (!settings.apiKey.isEmpty()) {
			request.setRawHeader(
				"Authorization",
				"Bearer " + settings.apiKey.toUtf8());
		}
		if (settings.providerId == u"openrouter"_q) {
			request.setRawHeader(
				"HTTP-Referer",
				"https://github.com/agentgram/agentgram");
			request.setRawHeader("X-Title", "Agentgram");
		}
		break;
	}
	return request;
}

[[nodiscard]] QJsonArray ToolsJson(
		const std::vector<ToolDefinition> &tools,
		ApiFormat format) {
	auto result = QJsonArray();
	for (const auto &tool : tools) {
		auto object = QJsonObject();
		switch (format) {
		case ApiFormat::OpenAIChat: {
			auto function = QJsonObject();
			function.insert(u"name"_q, tool.name);
			function.insert(u"description"_q, tool.description);
			function.insert(u"parameters"_q, tool.parameters);
			object.insert(u"type"_q, u"function"_q);
			object.insert(u"function"_q, function);
		} break;
		case ApiFormat::OpenAIResponses:
			object.insert(u"type"_q, u"function"_q);
			object.insert(u"name"_q, tool.name);
			object.insert(u"description"_q, tool.description);
			object.insert(u"parameters"_q, tool.parameters);
			object.insert(u"strict"_q, false);
			break;
		case ApiFormat::Anthropic:
			object.insert(u"name"_q, tool.name);
			object.insert(u"description"_q, tool.description);
			object.insert(u"input_schema"_q, tool.parameters);
			object.insert(u"eager_input_streaming"_q, true);
			break;
		}
		result.push_back(object);
	}
	return result;
}

} // namespace

QString ErrorFromBody(const QByteArray &body, int status) {
	const auto document = QJsonDocument::fromJson(body);
	auto message = QString();
	if (document.isObject()) {
		const auto root = document.object();
		const auto error = root.value(u"error"_q);
		if (error.isObject()) {
			message = error.toObject().value(u"message"_q).toString();
			if (message.isEmpty()) {
				message = error.toObject().value(u"type"_q).toString();
			}
		} else if (error.isString()) {
			message = error.toString();
		}
		if (message.isEmpty()) {
			message = root.value(u"detail"_q).toString();
		}
		if (message.isEmpty()) {
			message = root.value(u"message"_q).toString();
		}
	}
	if (message.isEmpty()) {
		message = QString::fromUtf8(body.left(500)).trimmed();
	}
	if (status > 0) {
		message = u"HTTP %1: %2"_q.arg(status).arg(message);
	}
	return message;
}

LlmRequest::LlmRequest(
	not_null<QNetworkAccessManager*> network,
	RequestSettings settings,
	QString system,
	const std::vector<ChatMessage> &history,
	const std::vector<ToolDefinition> &tools,
	RequestHandlers handlers)
: _network(network)
, _settings(std::move(settings))
, _system(std::move(system))
, _history(history)
, _tools(tools)
, _handlers(std::move(handlers)) {
}

LlmRequest::~LlmRequest() {
	cancel();
}

void LlmRequest::cancel() {
	_finished = true;
	if (const auto reply = _reply.data()) {
		_reply = nullptr;
		reply->disconnect();
		reply->abort();
		reply->deleteLater();
	}
}

QByteArray LlmRequest::prepareBody() const {
	switch (_settings.format) {
	case ApiFormat::OpenAIChat: return prepareOpenAIChat();
	case ApiFormat::OpenAIResponses: return prepareResponses();
	case ApiFormat::Anthropic: return prepareAnthropic();
	}
	return QByteArray();
}

QByteArray LlmRequest::prepareOpenAIChat() const {
	auto messages = QJsonArray();
	auto system = QJsonObject();
	system.insert(u"role"_q, u"system"_q);
	system.insert(u"content"_q, _system);
	messages.push_back(system);
	for (const auto &message : _history) {
		auto object = QJsonObject();
		switch (message.role) {
		case ChatMessage::Role::User:
			object.insert(u"role"_q, u"user"_q);
			object.insert(u"content"_q, message.text);
			break;
		case ChatMessage::Role::Assistant: {
			object.insert(u"role"_q, u"assistant"_q);
			object.insert(u"content"_q, message.text);
			if (!message.toolCalls.empty()) {
				auto calls = QJsonArray();
				for (const auto &call : message.toolCalls) {
					auto function = QJsonObject();
					function.insert(u"name"_q, call.name);
					function.insert(u"arguments"_q, call.arguments);
					auto entry = QJsonObject();
					entry.insert(u"id"_q, call.id);
					entry.insert(u"type"_q, u"function"_q);
					entry.insert(u"function"_q, function);
					calls.push_back(entry);
				}
				object.insert(u"tool_calls"_q, calls);
			}
		} break;
		case ChatMessage::Role::Tool:
			object.insert(u"role"_q, u"tool"_q);
			object.insert(u"tool_call_id"_q, message.toolCallId);
			object.insert(u"content"_q, message.text);
			break;
		}
		messages.push_back(object);
	}
	auto root = QJsonObject();
	root.insert(u"model"_q, _settings.model);
	root.insert(u"messages"_q, messages);
	root.insert(u"stream"_q, true);
	if (!_tools.empty()) {
		root.insert(u"tools"_q, ToolsJson(_tools, _settings.format));
		root.insert(u"tool_choice"_q, u"auto"_q);
	}
	return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QByteArray LlmRequest::prepareResponses() const {
	auto input = QJsonArray();
	for (const auto &message : _history) {
		switch (message.role) {
		case ChatMessage::Role::User: {
			auto content = QJsonObject();
			content.insert(u"type"_q, u"input_text"_q);
			content.insert(u"text"_q, message.text);
			auto object = QJsonObject();
			object.insert(u"type"_q, u"message"_q);
			object.insert(u"role"_q, u"user"_q);
			object.insert(u"content"_q, QJsonArray{ content });
			input.push_back(object);
		} break;
		case ChatMessage::Role::Assistant: {
			if (!message.text.isEmpty()) {
				auto content = QJsonObject();
				content.insert(u"type"_q, u"output_text"_q);
				content.insert(u"text"_q, message.text);
				auto object = QJsonObject();
				object.insert(u"type"_q, u"message"_q);
				object.insert(u"role"_q, u"assistant"_q);
				object.insert(u"content"_q, QJsonArray{ content });
				input.push_back(object);
			}
			for (const auto &call : message.toolCalls) {
				auto object = QJsonObject();
				object.insert(u"type"_q, u"function_call"_q);
				object.insert(u"call_id"_q, call.id);
				object.insert(u"name"_q, call.name);
				object.insert(u"arguments"_q, call.arguments);
				input.push_back(object);
			}
		} break;
		case ChatMessage::Role::Tool: {
			auto object = QJsonObject();
			object.insert(u"type"_q, u"function_call_output"_q);
			object.insert(u"call_id"_q, message.toolCallId);
			object.insert(u"output"_q, message.text);
			input.push_back(object);
		} break;
		}
	}
	auto reasoning = QJsonObject();
	reasoning.insert(u"effort"_q, u"medium"_q);
	auto root = QJsonObject();
	root.insert(u"model"_q, _settings.model);
	root.insert(u"instructions"_q, _system);
	root.insert(u"input"_q, input);
	root.insert(u"tools"_q, ToolsJson(_tools, _settings.format));
	root.insert(u"tool_choice"_q, u"auto"_q);
	root.insert(u"parallel_tool_calls"_q, false);
	root.insert(u"reasoning"_q, reasoning);
	root.insert(u"store"_q, false);
	root.insert(u"stream"_q, true);
	return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QByteArray LlmRequest::prepareAnthropic() const {
	auto messages = QJsonArray();
	auto pendingResults = QJsonArray();
	const auto flushResults = [&] {
		if (pendingResults.isEmpty()) {
			return;
		}
		auto object = QJsonObject();
		object.insert(u"role"_q, u"user"_q);
		object.insert(u"content"_q, pendingResults);
		messages.push_back(object);
		pendingResults = QJsonArray();
	};
	for (const auto &message : _history) {
		if (message.role == ChatMessage::Role::Tool) {
			auto block = QJsonObject();
			block.insert(u"type"_q, u"tool_result"_q);
			block.insert(u"tool_use_id"_q, message.toolCallId);
			block.insert(u"content"_q, message.text);
			if (message.toolError) {
				block.insert(u"is_error"_q, true);
			}
			pendingResults.push_back(block);
			continue;
		}
		flushResults();
		auto object = QJsonObject();
		if (message.role == ChatMessage::Role::User) {
			object.insert(u"role"_q, u"user"_q);
			object.insert(u"content"_q, message.text);
		} else {
			object.insert(u"role"_q, u"assistant"_q);
			auto content = message.anthropicContent;
			if (content.isEmpty()) {
				if (!message.text.isEmpty()) {
					auto text = QJsonObject();
					text.insert(u"type"_q, u"text"_q);
					text.insert(u"text"_q, message.text);
					content.push_back(text);
				}
				for (const auto &call : message.toolCalls) {
					auto use = QJsonObject();
					use.insert(u"type"_q, u"tool_use"_q);
					use.insert(u"id"_q, call.id);
					use.insert(u"name"_q, call.name);
					use.insert(u"input"_q, ParseObject(call.arguments));
					content.push_back(use);
				}
			}
			if (content.isEmpty()) {
				continue;
			}
			object.insert(u"content"_q, content);
		}
		messages.push_back(object);
	}
	flushResults();

	auto root = QJsonObject();
	root.insert(u"model"_q, _settings.model);
	root.insert(u"max_tokens"_q, kAnthropicMaxTokens);
	root.insert(u"system"_q, _system);
	root.insert(u"messages"_q, messages);
	root.insert(u"stream"_q, true);
	if (!_tools.empty()) {
		root.insert(u"tools"_q, ToolsJson(_tools, _settings.format));
	}
	if (SupportsServerFallback(_settings.model)
		&& _settings.baseUrl.startsWith(u"https://api.anthropic.com"_q)) {
		root.insert(u"fallbacks"_q, u"default"_q);
	}
	return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

void LlmRequest::start() {
	const auto path = [&] {
		switch (_settings.format) {
		case ApiFormat::OpenAIChat: return u"/chat/completions"_q;
		case ApiFormat::OpenAIResponses: return u"/responses"_q;
		case ApiFormat::Anthropic: return u"/v1/messages"_q;
		}
		return QString();
	}();
	auto request = PrepareRequest(_settings, path);
	request.setRawHeader("Accept", "text/event-stream");
	if (_settings.format == ApiFormat::Anthropic
		&& SupportsServerFallback(_settings.model)
		&& _settings.baseUrl.startsWith(u"https://api.anthropic.com"_q)) {
		request.setRawHeader(
			"anthropic-beta",
			"server-side-fallback-2026-07-01");
	}
	const auto reply = _network->post(request, prepareBody());
	_reply = reply;
	QObject::connect(reply, &QNetworkReply::readyRead, reply, [=] {
		readyRead();
	});
	QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
		finished();
	});
}

void LlmRequest::readyRead() {
	if (!_reply) {
		return;
	}
	if (!_status) {
		_status = _reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
	}
	const auto chunk = _reply->readAll();
	if (_status >= 400) {
		_errorBody += chunk;
		return;
	}
	_buffer += chunk;
	_buffer.replace("\r\n", "\n");
	while (true) {
		const auto index = _buffer.indexOf("\n\n");
		if (index < 0) {
			break;
		}
		const auto raw = _buffer.left(index);
		_buffer.remove(0, index + 2);
		auto event = QByteArray();
		auto data = QByteArray();
		for (const auto &line : raw.split('\n')) {
			if (line.startsWith("event:")) {
				event = line.mid(6).trimmed();
			} else if (line.startsWith("data:")) {
				if (!data.isEmpty()) {
					data += '\n';
				}
				data += line.mid(5).trimmed();
			}
		}
		if (!data.isEmpty()) {
			processEvent(event, data);
		}
		if (_finished) {
			return;
		}
	}
}

void LlmRequest::finished() {
	const auto reply = _reply.data();
	if (!reply || _finished) {
		return;
	}
	if (!_status) {
		_status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
	}
	const auto rest = reply->readAll();
	if (_status >= 400) {
		_errorBody += rest;
	} else if (!rest.isEmpty()) {
		_buffer += rest;
	}
	if (!_buffer.trimmed().isEmpty() && _status < 400) {
		_buffer += "\n\n";
		readyRead();
		if (_finished) {
			return;
		}
	}
	const auto error = reply->error();
	_reply = nullptr;
	reply->deleteLater();
	if (_status >= 400) {
		failWith(ErrorFromBody(_errorBody, _status), _status);
	} else if (error != QNetworkReply::NoError) {
		failWith(reply->errorString(), _status);
	} else {
		complete();
	}
}

void LlmRequest::processEvent(
		const QByteArray &event,
		const QByteArray &data) {
	if (data == "[DONE]") {
		complete();
		return;
	}
	const auto document = QJsonDocument::fromJson(data);
	if (!document.isObject()) {
		return;
	}
	const auto object = document.object();
	switch (_settings.format) {
	case ApiFormat::OpenAIChat: processOpenAIChat(object); break;
	case ApiFormat::OpenAIResponses: processResponses(event, object); break;
	case ApiFormat::Anthropic: processAnthropic(event, object); break;
	}
}

void LlmRequest::processOpenAIChat(const QJsonObject &data) {
	if (data.contains(u"error"_q)) {
		failWith(ErrorFromBody(QJsonDocument(data).toJson(), 0));
		return;
	}
	const auto choices = data.value(u"choices"_q).toArray();
	if (choices.isEmpty()) {
		return;
	}
	const auto choice = choices.first().toObject();
	const auto delta = choice.value(u"delta"_q).toObject();
	const auto content = delta.value(u"content"_q);
	if (content.isString()) {
		appendText(content.toString());
	}
	for (const auto &value : delta.value(u"tool_calls"_q).toArray()) {
		const auto call = value.toObject();
		const auto index = call.value(u"index"_q).toInt(int(_calls.size()));
		auto &pending = _calls[index];
		const auto id = call.value(u"id"_q).toString();
		if (!id.isEmpty()) {
			pending.id = id;
		}
		const auto function = call.value(u"function"_q).toObject();
		const auto name = function.value(u"name"_q).toString();
		if (!name.isEmpty()) {
			pending.name = name;
		}
		pending.arguments += function.value(u"arguments"_q).toString();
	}
	const auto reason = choice.value(u"finish_reason"_q).toString();
	if (!reason.isEmpty()) {
		_stopReason = reason;
	}
}

void LlmRequest::processResponses(
		const QByteArray &event,
		const QJsonObject &data) {
	const auto type = data.value(u"type"_q).toString().isEmpty()
		? QString::fromUtf8(event)
		: data.value(u"type"_q).toString();
	if (type == u"response.output_text.delta"_q) {
		appendText(data.value(u"delta"_q).toString());
	} else if (type == u"response.output_item.done"_q) {
		const auto item = data.value(u"item"_q).toObject();
		if (item.value(u"type"_q).toString() == u"function_call"_q) {
			_responseCalls.push_back({
				.id = item.value(u"call_id"_q).toString(),
				.name = item.value(u"name"_q).toString(),
				.arguments = item.value(u"arguments"_q).toString(),
			});
		}
	} else if (type == u"response.completed"_q) {
		complete();
	} else if (type == u"response.failed"_q
		|| type == u"response.incomplete"_q) {
		const auto response = data.value(u"response"_q).toObject();
		const auto error = response.value(u"error"_q).toObject();
		const auto details = response.value(
			u"incomplete_details"_q).toObject();
		auto message = error.value(u"message"_q).toString();
		if (message.isEmpty()) {
			message = details.value(u"reason"_q).toString();
		}
		failWith(message.isEmpty() ? type : message);
	} else if (type == u"error"_q) {
		auto message = data.value(u"message"_q).toString();
		if (message.isEmpty()) {
			message = data.value(u"error"_q).toObject().value(
				u"message"_q).toString();
		}
		failWith(message.isEmpty() ? u"Unknown error"_q : message);
	}
}

void LlmRequest::processAnthropic(
		const QByteArray &event,
		const QJsonObject &data) {
	const auto type = data.value(u"type"_q).toString();
	const auto index = data.value(u"index"_q).toInt();
	if (type == u"content_block_start"_q) {
		auto &block = _blocks[index];
		block.block = data.value(u"content_block"_q).toObject();
		block.text = block.block.value(u"text"_q).toString();
		block.thinking = block.block.value(u"thinking"_q).toString();
		block.signature = block.block.value(u"signature"_q).toString();
	} else if (type == u"content_block_delta"_q) {
		auto &block = _blocks[index];
		const auto delta = data.value(u"delta"_q).toObject();
		const auto kind = delta.value(u"type"_q).toString();
		if (kind == u"text_delta"_q) {
			const auto text = delta.value(u"text"_q).toString();
			block.text += text;
			appendText(text);
		} else if (kind == u"input_json_delta"_q) {
			block.json += delta.value(u"partial_json"_q).toString();
		} else if (kind == u"thinking_delta"_q) {
			block.thinking += delta.value(u"thinking"_q).toString();
		} else if (kind == u"signature_delta"_q) {
			block.signature += delta.value(u"signature"_q).toString();
		}
	} else if (type == u"message_delta"_q) {
		const auto delta = data.value(u"delta"_q).toObject();
		const auto reason = delta.value(u"stop_reason"_q).toString();
		if (!reason.isEmpty()) {
			_stopReason = reason;
		}
	} else if (type == u"message_stop"_q) {
		complete();
	} else if (type == u"error"_q) {
		const auto error = data.value(u"error"_q).toObject();
		failWith(error.value(u"message"_q).toString());
	}
	Q_UNUSED(event);
}

void LlmRequest::appendText(const QString &text) {
	if (text.isEmpty()) {
		return;
	}
	_text += text;
	if (_handlers.textDelta) {
		_handlers.textDelta(text);
	}
}

void LlmRequest::complete() {
	if (_finished) {
		return;
	}
	_finished = true;
	auto result = ChatMessage{ .role = ChatMessage::Role::Assistant };
	result.text = _text;
	switch (_settings.format) {
	case ApiFormat::OpenAIChat:
		for (const auto &[index, call] : _calls) {
			if (call.name.isEmpty()) {
				continue;
			}
			result.toolCalls.push_back({
				.id = call.id.isEmpty()
					? u"call_%1"_q.arg(index)
					: call.id,
				.name = call.name,
				.arguments = call.arguments.isEmpty()
					? u"{}"_q
					: call.arguments,
			});
		}
		break;
	case ApiFormat::OpenAIResponses:
		result.toolCalls = _responseCalls;
		break;
	case ApiFormat::Anthropic:
		for (auto &[index, pending] : _blocks) {
			auto block = pending.block;
			const auto type = block.value(u"type"_q).toString();
			if (type == u"text"_q) {
				block.insert(u"text"_q, pending.text);
			} else if (type == u"thinking"_q) {
				block.insert(u"thinking"_q, pending.thinking);
				block.insert(u"signature"_q, pending.signature);
			} else if (type == u"tool_use"_q) {
				const auto input = pending.json.isEmpty()
					? block.value(u"input"_q).toObject()
					: ParseObject(pending.json);
				block.insert(u"input"_q, input);
				result.toolCalls.push_back({
					.id = block.value(u"id"_q).toString(),
					.name = block.value(u"name"_q).toString(),
					.arguments = (pending.json.isEmpty() || !input.isEmpty())
						? Serialize(input)
						: pending.json,
				});
			}
			result.anthropicContent.push_back(block);
		}
		if (_stopReason == u"refusal"_q) {
			_finished = false;
			failWith(u"Модель отказалась отвечать на этот запрос."_q);
			return;
		}
		break;
	}
	if (_reply) {
		const auto reply = _reply.data();
		_reply = nullptr;
		reply->disconnect();
		reply->deleteLater();
	}
	if (const auto done = base::take(_handlers.done)) {
		done(std::move(result));
	}
}

void LlmRequest::failWith(const QString &error, int status) {
	if (_finished) {
		return;
	}
	_finished = true;
	if (_reply) {
		const auto reply = _reply.data();
		_reply = nullptr;
		reply->disconnect();
		reply->abort();
		reply->deleteLater();
	}
	if (const auto fail = base::take(_handlers.fail)) {
		fail(error, status);
	}
}

QStringList ParseModels(const QByteArray &body) {
	struct Model {
		QString id;
		int64 created = 0;
		int priority = 0;
		int index = 0;
		bool hidden = false;
	};
	const auto document = QJsonDocument::fromJson(body);
	auto list = QJsonArray();
	if (document.isArray()) {
		list = document.array();
	} else {
		const auto root = document.object();
		list = root.value(u"data"_q).toArray();
		if (list.isEmpty()) {
			list = root.value(u"models"_q).toArray();
		}
	}
	static const auto skip = QStringList{
		u"embed"_q, u"tts"_q, u"whisper"_q, u"dall-e"_q, u"davinci"_q,
		u"babbage"_q, u"moderation"_q, u"transcribe"_q, u"-image"_q,
		u"image-"_q, u"imagen"_q, u"veo-"_q, u"sora"_q, u"-audio"_q,
		u"audio-"_q, u"realtime"_q, u"aqa"_q, u"rerank"_q, u"guard"_q,
	};
	auto models = std::vector<Model>();
	auto hasPriority = false;
	auto hasCreated = false;
	for (const auto &value : list) {
		auto model = Model{ .index = int(models.size()) };
		if (value.isString()) {
			model.id = value.toString();
		} else {
			const auto object = value.toObject();
			// Models hidden from the Codex picker still work, list them last.
			const auto visibility = object.value(u"visibility"_q).toString();
			model.hidden = (visibility == u"hide"_q)
				|| (visibility == u"hidden"_q)
				|| (visibility == u"none"_q);
			for (const auto key : { "id", "slug", "model", "name" }) {
				model.id = object.value(QString::fromLatin1(key)).toString();
				if (!model.id.isEmpty()) {
					break;
				}
			}
			model.created = int64(object.value(u"created"_q).toDouble());
			if (object.contains(u"priority"_q)) {
				model.priority = object.value(u"priority"_q).toInt();
				hasPriority = true;
			}
		}
		if (model.id.startsWith(u"models/"_q)) {
			model.id = model.id.mid(7);
		}
		const auto lower = model.id.toLower();
		if (model.id.isEmpty()
			|| ranges::any_of(skip, [&](const QString &part) {
				return lower.contains(part);
			})) {
			continue;
		}
		hasCreated = hasCreated || (model.created > 0);
		models.push_back(std::move(model));
	}
	if (hasPriority) {
		ranges::stable_sort(models, std::less<>(), &Model::priority);
	} else if (hasCreated) {
		ranges::stable_sort(models, std::greater<>(), &Model::created);
	}
	ranges::stable_partition(models, [](const Model &model) {
		return !model.hidden;
	});
	auto result = QStringList();
	for (const auto &model : models) {
		if (!result.contains(model.id)) {
			result.push_back(model.id);
		}
	}
	return result;
}

void FetchModels(
		not_null<QNetworkAccessManager*> network,
		const RequestSettings &settings,
		ModelsHandlers handlers) {
	const auto path = (settings.format == ApiFormat::Anthropic)
		? u"/v1/models?limit=1000"_q
		: (settings.format == ApiFormat::OpenAIResponses)
		? (u"/models?client_version="_q
			+ QString::fromLatin1(kCodexModelsClientVersion))
		: u"/models"_q;
	auto request = PrepareRequest(settings, path);
	request.setRawHeader("Accept", "application/json");
	const auto reply = network->get(request);
	QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
		reply->deleteLater();
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto body = reply->readAll();
		if (status >= 400 || reply->error() != QNetworkReply::NoError) {
			if (handlers.fail) {
				handlers.fail(body.isEmpty()
					? reply->errorString()
					: ErrorFromBody(body, status));
			}
			return;
		}
		const auto result = ParseModels(body);
		if (result.isEmpty()) {
			if (handlers.fail) {
				handlers.fail(u"Сервер вернул пустой список моделей."_q);
			}
		} else if (handlers.done) {
			handlers.done(result);
		}
	});
}

} // namespace AiAgent
