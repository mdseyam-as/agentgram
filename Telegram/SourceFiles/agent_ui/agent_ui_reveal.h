/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace AgentUi {

// Freezes the current look of `window`, calls `apply` below that frozen
// picture and then opens a growing circle from the global point
// `origin`, revealing the new look through it.
void RunReveal(
	not_null<QWidget*> window,
	QPoint origin,
	Fn<void()> apply);

} // namespace AgentUi
