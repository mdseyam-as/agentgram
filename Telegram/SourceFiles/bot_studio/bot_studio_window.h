/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Window {
class SessionController;
} // namespace Window

namespace BotStudio {

// Opens (or raises) the Bot Studio window of the controller's account.
void ShowWindow(
	not_null<Window::SessionController*> controller,
	const QString &botId = QString());

} // namespace BotStudio
