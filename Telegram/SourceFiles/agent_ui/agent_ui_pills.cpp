/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "agent_ui/agent_ui_pills.h"

#include "ui/painter.h"
#include "styles/palette.h"
#include "styles/style_basic.h"
#include "styles/style_dialogs.h"

namespace AgentUi {
namespace {

constexpr auto kMoveDuration = crl::time(240);
constexpr auto kFadeInDuration = crl::time(160);
constexpr auto kFadeOutDuration = crl::time(220);

[[nodiscard]] QRectF Interpolate(QRectF a, QRectF b, float64 t) {
	const auto mix = [&](float64 x, float64 y) {
		return x + (y - x) * t;
	};
	return QRectF(
		mix(a.x(), b.x()),
		mix(a.y(), b.y()),
		mix(a.width(), b.width()),
		mix(a.height(), b.height()));
}

[[nodiscard]] QColor Alpha(QColor color, float64 alpha) {
	color.setAlphaF(color.alphaF() * alpha);
	return color;
}

} // namespace

SlidingPill::SlidingPill(Fn<void(QRect)> repaint)
: _repaint(std::move(repaint)) {
}

QRectF SlidingPill::currentRect() const {
	return Interpolate(_from, _to, _move.value(1.));
}

float64 SlidingPill::currentOpacity() const {
	return _fade.value(_shown ? 1. : 0.);
}

QRect SlidingPill::paintRect() const {
	return currentRect().toAlignedRect().marginsAdded({ 2, 2, 2, 2 });
}

void SlidingPill::animationStep() {
	const auto now = paintRect();
	_repaint(_painted.united(now));
	_painted = now;
}

void SlidingPill::setTarget(std::optional<QRect> target) {
	if (!target || target->isEmpty()) {
		if (_shown) {
			_shown = false;
			_fade.start(
				[=] { animationStep(); },
				1.,
				0.,
				kFadeOutDuration,
				anim::easeOutCubic);
		}
		return;
	}
	const auto to = QRectF(*target);
	if (_shown && to == _to) {
		return;
	}
	const auto visible = currentOpacity() > 0.;
	if (!visible) {
		// Appear in place, no travel from a stale position.
		_move.stop();
		_from = _to = to;
		_shown = true;
		_fade.start(
			[=] { animationStep(); },
			0.,
			1.,
			kFadeInDuration,
			anim::easeOutCubic);
		animationStep();
		return;
	}
	_from = currentRect();
	_to = to;
	if (!_shown) {
		_shown = true;
		_fade.start(
			[=] { animationStep(); },
			currentOpacity(),
			1.,
			kFadeInDuration,
			anim::easeOutCubic);
	}
	_move.start(
		[=] { animationStep(); },
		0.,
		1.,
		kMoveDuration,
		anim::easeOutCubic);
}

void SlidingPill::stop() {
	_move.stop();
	_fade.stop();
	_shown = false;
	_painted = QRect();
}

void SlidingPill::paint(QPainter &p, bool active) const {
	const auto opacity = currentOpacity();
	if (opacity <= 0.) {
		return;
	}
	const auto rect = currentRect();
	const auto horizontal = std::min(
		float64(style::ConvertScale(8)),
		rect.width() / 8.);
	const auto vertical = float64(style::ConvertScale(2));
	const auto pill = rect.marginsRemoved({
		horizontal,
		vertical,
		horizontal,
		vertical,
	});
	const auto radius = std::min(
		float64(style::ConvertScale(14)),
		pill.height() / 2.);

	auto hq = PainterHighQualityEnabler(p);
	if (active) {
		p.setPen(QPen(
			Alpha(st::windowBgActive->c, 0.38 * opacity),
			style::ConvertScale(1.)));
		p.setBrush(Alpha(st::dialogsBgActive->c, opacity));
	} else {
		p.setPen(Qt::NoPen);
		p.setBrush(Alpha(st::dialogsBgOver->c, opacity));
	}
	p.drawRoundedRect(pill, radius, radius);
	if (active) {
		// Lit top edge of the glass pill.
		auto gradient = QLinearGradient(pill.topLeft(), pill.bottomLeft());
		gradient.setColorAt(0., QColor(255, 255, 255, int(28 * opacity)));
		gradient.setColorAt(0.5, QColor(255, 255, 255, 0));
		p.setPen(Qt::NoPen);
		p.setBrush(gradient);
		p.drawRoundedRect(pill, radius, radius);
	}
}

} // namespace AgentUi
