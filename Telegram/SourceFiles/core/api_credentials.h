/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Core {

struct ApiCredentials {
	int id = 0;
	QString hash;
};

// Values from AGENTGRAM_API_ID / AGENTGRAM_API_HASH or api_credentials.json
// beside tdata take priority over the ones compiled into the binary.
[[nodiscard]] ApiCredentials CurrentApiCredentials();
[[nodiscard]] bool UsingCustomApiCredentials();
[[nodiscard]] bool UsingTestApiCredentials();
[[nodiscard]] std::optional<ApiCredentials> SavedApiCredentials();
bool SaveApiCredentials(const ApiCredentials &credentials);
bool ClearSavedApiCredentials();

} // namespace Core
