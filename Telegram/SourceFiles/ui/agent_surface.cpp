/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ui/agent_surface.h"

namespace Ui::AgentSurface {
namespace {

Painter &Instance() {
	static auto result = Painter();
	return result;
}

} // namespace

void SetPainter(Painter painter) {
	Instance() = std::move(painter);
}

void Fill(
		QPainter &p,
		not_null<const QWidget*> widget,
		QRect rect,
		const style::color &fallback,
		Kind kind,
		RectParts edges) {
	const auto &painter = Instance();
	if (!painter || !painter(p, widget, rect, kind, edges)) {
		p.fillRect(rect, fallback);
	}
}

} // namespace Ui::AgentSurface
