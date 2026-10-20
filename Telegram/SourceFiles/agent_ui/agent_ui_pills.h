/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/effects/animations.h"

namespace AgentUi {

// A rounded highlight that glides between list rows instead of jumping:
// it slides to a new target, fades in where nothing was highlighted and
// fades out in place when the highlight goes away.
class SlidingPill final {
public:
	explicit SlidingPill(Fn<void(QRect)> repaint);

	void setTarget(std::optional<QRect> target);
	void paint(QPainter &p, bool active) const;
	void stop();

private:
	[[nodiscard]] QRectF currentRect() const;
	[[nodiscard]] float64 currentOpacity() const;
	[[nodiscard]] QRect paintRect() const;
	void animationStep();

	Fn<void(QRect)> _repaint;
	QRectF _from;
	QRectF _to;
	bool _shown = false;
	Ui::Animations::Simple _move;
	Ui::Animations::Simple _fade;
	QRect _painted;

};

} // namespace AgentUi
