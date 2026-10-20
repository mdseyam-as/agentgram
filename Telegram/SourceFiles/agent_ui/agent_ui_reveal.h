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

// Covers `widget` with a snapshot of its current look that drifts down
// and fades out, so a content switch below it reads as a soft change.
void FadeOutSnapshot(not_null<QWidget*> widget);

// New messages of the open chat rise and fade into place.
void MessageAppeared(not_null<const void*> item, not_null<QWidget*> list);
[[nodiscard]] std::optional<float64> MessageAppearProgress(
	not_null<const void*> item);

} // namespace AgentUi
