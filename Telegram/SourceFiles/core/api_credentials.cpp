/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/api_credentials.h"

#include "settings.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>

#include <mutex>

namespace Core {
namespace {

constexpr auto kTestApiId = 17349;

[[nodiscard]] QString CredentialsPath() {
	return cWorkingDir() + u"api_credentials.json"_q;
}

[[nodiscard]] std::optional<ApiCredentials> ReadFromEnvironment() {
	const auto id = qEnvironmentVariableIntValue("AGENTGRAM_API_ID");
	const auto hash = qEnvironmentVariable("AGENTGRAM_API_HASH").trimmed();
	if (id > 0 && !hash.isEmpty()) {
		return ApiCredentials{ id, hash };
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<ApiCredentials> ReadFromFile() {
	auto file = QFile(CredentialsPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return std::nullopt;
	}
	const auto object = QJsonDocument::fromJson(file.readAll()).object();
	const auto id = object.value(u"api_id"_q).toVariant().toInt();
	const auto hash = object.value(u"api_hash"_q).toString().trimmed();
	if (id > 0 && !hash.isEmpty()) {
		return ApiCredentials{ id, hash };
	}
	return std::nullopt;
}

struct State {
	ApiCredentials current;
	bool custom = false;
};

[[nodiscard]] const State &Resolved() {
	static auto state = State();
	static auto once = std::once_flag();
	std::call_once(once, [] {
		if (const auto env = ReadFromEnvironment()) {
			state = { *env, true };
		} else if (const auto file = ReadFromFile()) {
			state = { *file, true };
		} else {
			state = {
				ApiCredentials{ int(BuiltinApiId), QString(BuiltinApiHash) },
				false,
			};
		}
	});
	return state;
}

} // namespace

ApiCredentials CurrentApiCredentials() {
	return Resolved().current;
}

bool UsingCustomApiCredentials() {
	return Resolved().custom;
}

bool UsingTestApiCredentials() {
	return (Resolved().current.id == kTestApiId);
}

std::optional<ApiCredentials> SavedApiCredentials() {
	return ReadFromFile();
}

bool SaveApiCredentials(const ApiCredentials &credentials) {
	auto object = QJsonObject();
	object.insert(u"api_id"_q, credentials.id);
	object.insert(u"api_hash"_q, credentials.hash);
	auto file = QSaveFile(CredentialsPath());
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
	file.write(QJsonDocument(object).toJson());
	return file.commit();
}

bool ClearSavedApiCredentials() {
	return !QFile::exists(CredentialsPath())
		|| QFile::remove(CredentialsPath());
}

} // namespace Core

int ApiId() {
	return Core::CurrentApiCredentials().id;
}

QString ApiHash() {
	return Core::CurrentApiCredentials().hash;
}
