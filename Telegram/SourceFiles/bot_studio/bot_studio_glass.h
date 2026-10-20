/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "agent_ui/agent_ui.h"
#include "agent_ui/agent_ui_glass.h"

namespace BotStudio {

// With AgentUI on, the studio window is matte glass: the window paints
// the glass, inner elements (cards, pills, fields, nodes) are translucent
// tints over it with a thin edge. Vanilla keeps the palette colors.

[[nodiscard]] inline bool Glass() {
	return AgentUi::Active();
}

[[nodiscard]] inline QColor InnerFill(const style::color &vanilla) {
	return Glass() ? AgentUi::FieldTint() : vanilla->c;
}

[[nodiscard]] inline QColor InnerHover(const style::color &vanilla) {
	return !Glass()
		? vanilla->c
		: AgentUi::Dark()
		? QColor(255, 255, 255, 30)
		: QColor(255, 255, 255, 235);
}

[[nodiscard]] inline QColor InnerEdge(QColor vanilla) {
	return Glass() ? AgentUi::FieldEdge() : vanilla;
}

// Opaque fill for small details that must hide what is under them,
// like port holes or a ring around a status dot.
[[nodiscard]] inline QColor SolidFill(
		const style::color &vanilla,
		AgentUi::Surface surface = AgentUi::Surface::Panel) {
	if (!Glass()) {
		return vanilla->c;
	}
	auto result = AgentUi::GlassTint(surface);
	result.setAlpha(255);
	return result;
}

} // namespace BotStudio
