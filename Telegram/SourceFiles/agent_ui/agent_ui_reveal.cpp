/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "agent_ui/agent_ui_reveal.h"

#include "ui/effects/animations.h"
#include "ui/painter.h"
#include "base/call_delayed.h"
#include "ui/rp_widget.h"
#include "ui/ui_utility.h"

#include <QtCore/QPointer>
#include <QtGui/QPainterPath>

namespace AgentUi {
namespace {

constexpr auto kDuration = crl::time(720);
// The heavy repaint after a palette change happens under the frozen
// picture, the circle starts growing once it is done.
constexpr auto kStartDelay = crl::time(60);

[[nodiscard]] float64 EaseInOutCubic(float64 t) {
	return (t < 0.5)
		? (4. * t * t * t)
		: (1. - std::pow(-2. * t + 2., 3) / 2.);
}

class RevealOverlay final : public Ui::RpWidget {
public:
	RevealOverlay(not_null<QWidget*> parent, QImage frozen, QPoint center);

	void start();

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	[[nodiscard]] float64 maxRadius() const;

	QImage _frozen;
	QPoint _center;
	Ui::Animations::Simple _animation;

};

RevealOverlay::RevealOverlay(
	not_null<QWidget*> parent,
	QImage frozen,
	QPoint center)
: RpWidget(parent)
, _frozen(std::move(frozen))
, _center(center) {
	setAttribute(Qt::WA_TransparentForMouseEvents);
	setGeometry(parent->rect());
}

float64 RevealOverlay::maxRadius() const {
	auto result = 0.;
	for (const auto corner : {
			rect().topLeft(),
			rect().topRight(),
			rect().bottomLeft(),
			rect().bottomRight() }) {
		const auto dx = float64(corner.x() - _center.x());
		const auto dy = float64(corner.y() - _center.y());
		result = std::max(result, std::sqrt(dx * dx + dy * dy));
	}
	return result + 2.;
}

void RevealOverlay::start() {
	_animation.start([=](float64 value) {
		update();
		if (value >= 1.) {
			crl::on_main(this, [=] { delete this; });
		}
	}, 0., 1., kDuration, anim::linear);
}

void RevealOverlay::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	const auto progress = EaseInOutCubic(_animation.value(0.));
	if (progress >= 1.) {
		return;
	}
	const auto radius = maxRadius() * progress;
	const auto center = QPointF(_center);

	auto hq = PainterHighQualityEnabler(p);
	auto path = QPainterPath();
	path.setFillRule(Qt::OddEvenFill);
	path.addRect(QRectF(rect()));
	if (radius > 0.) {
		path.addEllipse(center, radius, radius);
	}
	auto brush = QBrush(_frozen);
	const auto ratio = _frozen.devicePixelRatio();
	brush.setTransform(QTransform::fromScale(1. / ratio, 1. / ratio));
	p.setPen(Qt::NoPen);
	p.setBrush(brush);
	p.drawPath(path);

	if (radius > 0.) {
		// A soft lit rim on the edge of the opening, fading out.
		const auto fade = 1. - progress;
		const auto width = style::ConvertScale(18.);
		auto gradient = QRadialGradient(center, radius + width / 2.);
		const auto inner = std::clamp(
			(radius - width / 2.) / (radius + width / 2.),
			0.,
			1.);
		gradient.setColorAt(0., QColor(255, 255, 255, 0));
		gradient.setColorAt(inner, QColor(255, 255, 255, 0));
		gradient.setColorAt(
			std::clamp((radius) / (radius + width / 2.), inner, 1.),
			QColor(255, 255, 255, int(70 * fade)));
		gradient.setColorAt(1., QColor(255, 255, 255, 0));
		p.setBrush(gradient);
		p.drawEllipse(center, radius + width / 2., radius + width / 2.);
	}
}

} // namespace

void RunReveal(
		not_null<QWidget*> window,
		QPoint origin,
		Fn<void()> apply) {
	if (anim::Disabled() || window->isHidden() || window->size().isEmpty()) {
		apply();
		return;
	}
	auto frozen = Ui::GrabWidgetToImage(window);
	const auto overlay = new RevealOverlay(
		window,
		std::move(frozen),
		window->mapFromGlobal(origin));
	overlay->show();
	overlay->raise();

	apply();

	const auto weak = QPointer<QWidget>(overlay);
	overlay->raise();
	base::call_delayed(kStartDelay, overlay, [=] {
		if (const auto strong = static_cast<RevealOverlay*>(weak.data())) {
			strong->raise();
			strong->start();
		}
	});
}

} // namespace AgentUi
