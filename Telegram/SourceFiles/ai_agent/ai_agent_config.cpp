/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ai_agent/ai_agent_config.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace AiAgent {
namespace {

[[nodiscard]] std::vector<ProviderInfo> PrepareProviders() {
	auto result = std::vector<ProviderInfo>();
	const auto add = [&](ProviderInfo info) {
		result.push_back(std::move(info));
	};
	add({
		.id = u"chatgpt"_q,
		.name = u"ChatGPT (OAuth)"_q,
		.description = u"Вход через аккаунт ChatGPT Plus / Pro, "
			"используется ваша подписка."_q,
		.format = ApiFormat::OpenAIResponses,
		.defaultBaseUrl = u"https://chatgpt.com/backend-api/codex"_q,
		.keysUrl = QString(),
		.models = {
			u"gpt-6.1-sol"_q,
			u"gpt-6-astra"_q,
			u"gpt-6-sol"_q,
			u"gpt-6-luna"_q,
			u"gpt-5.6-sol"_q,
			u"gpt-5.3-codex"_q,
		},
		.oauth = OAuthKind::ChatGPT,
		.apiKey = false,
	});
	add({
		.id = u"openai"_q,
		.name = u"OpenAI API"_q,
		.description = u"Ключ API платформы OpenAI."_q,
		.format = ApiFormat::OpenAIChat,
		.defaultBaseUrl = u"https://api.openai.com/v1"_q,
		.keysUrl = u"https://platform.openai.com/api-keys"_q,
		.models = {
			u"gpt-6.1-sol"_q,
			u"gpt-6-astra"_q,
			u"gpt-6-sol"_q,
			u"gpt-6-luna"_q,
			u"gpt-5.6-terra"_q,
		},
	});
	add({
		.id = u"anthropic"_q,
		.name = u"Anthropic Claude"_q,
		.description = u"Ключ API Anthropic (console.anthropic.com)."_q,
		.format = ApiFormat::Anthropic,
		.defaultBaseUrl = u"https://api.anthropic.com"_q,
		.keysUrl = u"https://console.anthropic.com/settings/keys"_q,
		.models = {
			u"claude-opus-5-5"_q,
			u"claude-sonnet-5-5"_q,
			u"claude-haiku-5-5"_q,
			u"claude-fable-5-1"_q,
		},
	});
	add({
		.id = u"openrouter"_q,
		.name = u"OpenRouter"_q,
		.description = u"Сотни моделей по одному ключу. Можно войти "
			"через OAuth или вставить ключ."_q,
		.format = ApiFormat::OpenAIChat,
		.defaultBaseUrl = u"https://openrouter.ai/api/v1"_q,
		.keysUrl = u"https://openrouter.ai/keys"_q,
		.models = {
			u"openai/gpt-6.1-sol"_q,
			u"anthropic/claude-opus-5.5"_q,
			u"anthropic/claude-sonnet-5.5"_q,
			u"google/gemini-4-argon"_q,
			u"x-ai/grok-4.7"_q,
			u"deepseek/deepseek-v4.1-flash"_q,
			u"moonshotai/kimi-k3"_q,
			u"qwen/qwen3.8-max-prime"_q,
		},
		.oauth = OAuthKind::OpenRouter,
	});
	add({
		.id = u"gemini"_q,
		.name = u"Google Gemini"_q,
		.description = u"Ключ Google AI Studio."_q,
		.format = ApiFormat::OpenAIChat,
		.defaultBaseUrl
			= u"https://generativelanguage.googleapis.com/v1beta/openai"_q,
		.keysUrl = u"https://aistudio.google.com/apikey"_q,
		.models = {
			u"gemini-4-argon"_q,
			u"gemini-3.8-flash"_q,
			u"gemini-3.5-flash-lite"_q,
		},
	});
	add({
		.id = u"deepseek"_q,
		.name = u"DeepSeek"_q,
		.description = u"Ключ API DeepSeek."_q,
		.format = ApiFormat::OpenAIChat,
		.defaultBaseUrl = u"https://api.deepseek.com/v1"_q,
		.keysUrl = u"https://platform.deepseek.com/api_keys"_q,
		.models = { u"deepseek-chat"_q, u"deepseek-reasoner"_q },
	});
	add({
		.id = u"xai"_q,
		.name = u"xAI Grok"_q,
		.description = u"Ключ API xAI."_q,
		.format = ApiFormat::OpenAIChat,
		.defaultBaseUrl = u"https://api.x.ai/v1"_q,
		.keysUrl = u"https://console.x.ai"_q,
		.models = {
			u"grok-4.7"_q,
			u"grok-4.6"_q,
		},
	});
	add({
		.id = u"mistral"_q,
		.name = u"Mistral AI"_q,
		.description = u"Ключ API Mistral."_q,
		.format = ApiFormat::OpenAIChat,
		.defaultBaseUrl = u"https://api.mistral.ai/v1"_q,
		.keysUrl = u"https://console.mistral.ai/api-keys"_q,
		.models = { u"mistral-large-latest"_q, u"mistral-medium-latest"_q },
	});
	add({
		.id = u"groq"_q,
		.name = u"Groq"_q,
		.description = u"Быстрый инференс открытых моделей."_q,
		.format = ApiFormat::OpenAIChat,
		.defaultBaseUrl = u"https://api.groq.com/openai/v1"_q,
		.keysUrl = u"https://console.groq.com/keys"_q,
		.models = {
			u"llama-3.3-70b-versatile"_q,
			u"moonshotai/kimi-k2-instruct"_q,
		},
	});
	add({
		.id = u"ollama"_q,
		.name = u"Ollama (локально)"_q,
		.description = u"Локальные модели через Ollama или LM Studio."_q,
		.format = ApiFormat::OpenAIChat,
		.defaultBaseUrl = u"http://localhost:11434/v1"_q,
		.models = { u"qwen3"_q, u"llama3.1"_q },
		.keyOptional = true,
		.editableBaseUrl = true,
	});
	add({
		.id = u"custom"_q,
		.name = u"Свой OpenAI-совместимый"_q,
		.description = u"Любой сервер с API /chat/completions."_q,
		.format = ApiFormat::OpenAIChat,
		.defaultBaseUrl = u"https://"_q,
		.keyOptional = true,
		.editableBaseUrl = true,
	});
	return result;
}

} // namespace

const std::vector<ProviderInfo> &Providers() {
	static const auto result = PrepareProviders();
	return result;
}

const ProviderInfo &FindProvider(const QString &id) {
	const auto &list = Providers();
	const auto i = ranges::find(list, id, &ProviderInfo::id);
	return (i != end(list)) ? *i : list.front();
}

ProviderSettings &Config::settings(const QString &id) {
	return providers[id];
}

const ProviderSettings *Config::find(const QString &id) const {
	const auto i = providers.find(id);
	return (i != end(providers)) ? &i->second : nullptr;
}

QString Config::baseUrl(const QString &id) const {
	const auto settings = find(id);
	const auto custom = settings ? settings->baseUrl.trimmed() : QString();
	auto result = custom.isEmpty()
		? FindProvider(id).defaultBaseUrl
		: custom;
	while (result.endsWith('/')) {
		result.chop(1);
	}
	return result;
}

QString Config::model(const QString &id) const {
	const auto settings = find(id);
	const auto custom = settings ? settings->model.trimmed() : QString();
	if (!custom.isEmpty()) {
		return custom;
	}
	const auto &models = FindProvider(id).models;
	return models.isEmpty() ? QString() : models.front();
}

bool Config::ready(const QString &id) const {
	const auto &info = FindProvider(id);
	const auto settings = find(id);
	if (model(id).isEmpty()) {
		return false;
	} else if (info.oauth == OAuthKind::ChatGPT) {
		return settings && settings->loggedIn();
	} else if (info.keyOptional) {
		return true;
	}
	return settings && !settings->apiKey.isEmpty();
}

QStringList Config::modelChoices(const QString &id) const {
	const auto settings = find(id);
	auto result = (settings && !settings->models.isEmpty())
		? settings->models
		: FindProvider(id).models;
	const auto current = model(id);
	if (!current.isEmpty() && !result.contains(current)) {
		result.push_front(current);
	}
	return result;
}

bool Config::modelsStale(const QString &id) const {
	constexpr auto kModelsLifetime = 12 * 3600;
	const auto settings = find(id);
	return !settings
		|| settings->models.isEmpty()
		|| (QDateTime::currentSecsSinceEpoch() - settings->modelsUpdatedAt
			> kModelsLifetime);
}

QByteArray Config::serialize() const {
	auto list = QJsonObject();
	for (const auto &[id, settings] : providers) {
		auto object = QJsonObject();
		const auto put = [&](const char *key, const QString &value) {
			if (!value.isEmpty()) {
				object.insert(QString::fromLatin1(key), value);
			}
		};
		put("api_key", settings.apiKey);
		put("base_url", settings.baseUrl);
		put("model", settings.model);
		put("access_token", settings.accessToken);
		put("refresh_token", settings.refreshToken);
		put("account_id", settings.accountId);
		put("account_email", settings.accountEmail);
		if (settings.expiresAt) {
			object.insert(u"expires_at"_q, double(settings.expiresAt));
		}
		if (!settings.models.isEmpty()) {
			object.insert(
				u"models"_q,
				QJsonArray::fromStringList(settings.models));
			object.insert(
				u"models_updated_at"_q,
				double(settings.modelsUpdatedAt));
		}
		list.insert(id, object);
	}
	auto root = QJsonObject();
	root.insert(u"version"_q, 1);
	root.insert(u"provider"_q, provider);
	root.insert(u"confirm_actions"_q, confirmActions);
	root.insert(u"custom_instructions"_q, customInstructions);
	root.insert(u"max_steps"_q, maxSteps);
	root.insert(u"providers"_q, list);
	return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

Config Config::Deserialize(const QByteArray &data) {
	auto result = Config();
	const auto root = QJsonDocument::fromJson(data).object();
	if (root.isEmpty()) {
		return result;
	}
	const auto provider = root.value(u"provider"_q).toString();
	if (!provider.isEmpty()) {
		result.provider = FindProvider(provider).id;
	}
	result.confirmActions = root.value(u"confirm_actions"_q).toBool(true);
	result.customInstructions = root.value(
		u"custom_instructions"_q).toString();
	result.maxSteps = std::clamp(
		root.value(u"max_steps"_q).toInt(30),
		4,
		100);
	const auto list = root.value(u"providers"_q).toObject();
	for (auto i = list.begin(); i != list.end(); ++i) {
		const auto object = i.value().toObject();
		const auto get = [&](const char *key) {
			return object.value(QString::fromLatin1(key)).toString();
		};
		auto &settings = result.providers[i.key()];
		settings.apiKey = get("api_key");
		settings.baseUrl = get("base_url");
		settings.model = get("model");
		settings.accessToken = get("access_token");
		settings.refreshToken = get("refresh_token");
		settings.accountId = get("account_id");
		settings.accountEmail = get("account_email");
		settings.expiresAt = int64(
			object.value(u"expires_at"_q).toDouble());
		for (const auto &model : object.value(u"models"_q).toArray()) {
			const auto name = model.toString();
			if (!name.isEmpty()) {
				settings.models.push_back(name);
			}
		}
		settings.modelsUpdatedAt = int64(
			object.value(u"models_updated_at"_q).toDouble());
	}
	return result;
}

} // namespace AiAgent
