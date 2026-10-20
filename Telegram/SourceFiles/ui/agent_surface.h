/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/rect_part.h"

namespace Ui::AgentSurface {

// Glass surfaces of the AgentUI design. Widgets of the UI library fill
// their backgrounds through Fill(), the application installs the glass
// painter when AgentUI is on.
enum class Kind {
	ChatList,
	Folders,
	Header,
	Composer,
	Bar,
	Panel,
};

using Painter = Fn<bool(
	QPainter&,
	not_null<const QWidget*>,
	QRect,
	Kind,
	RectParts)>;

void SetPainter(Painter painter);

// Paints the glass of `kind`, or fills `fallback` without the glass.
void Fill(
	QPainter &p,
	not_null<const QWidget*> widget,
	QRect rect,
	const style::color &fallback,
	Kind kind,
	RectParts edges = RectPart::None);

} // namespace Ui::AgentSurface
