/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "agent_ui/agent_ui_reveal.h"

#include "ui/effects/animations.h"
#include "ui/painter.h"
#include "base/call_delayed.h"
#include "base/flat_map.h"
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

class FadeOverlay final : public Ui::RpWidget {
public:
	FadeOverlay(
		not_null<QWidget*> parent,
		QRect geometry,
		QPixmap snapshot)
	: RpWidget(parent)
	, _snapshot(std::move(snapshot)) {
		setAttribute(Qt::WA_TransparentForMouseEvents);
		setGeometry(geometry);
	}

	void start() {
		_animation.start([=](float64 value) {
			raise();
			update();
			if (value >= 1.) {
				crl::on_main(this, [=] { delete this; });
			}
		}, 0., 1., kFadeDuration, anim::linear);
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		const auto progress = _animation.value(0.);
		if (progress >= 1.) {
			return;
		}
		const auto eased = 1. - std::pow(1. - progress, 3);
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		p.setOpacity(1. - progress);
		const auto shift = style::ConvertScale(14.) * eased;
		const auto scale = 1. - 0.015 * eased;
		const auto center = QPointF(rect().center());
		p.translate(center.x(), center.y() + shift);
		p.scale(scale, scale);
		p.translate(-center.x(), -center.y());
		p.drawPixmap(0, 0, _snapshot);
	}

private:
	static constexpr auto kFadeDuration = crl::time(280);

	QPixmap _snapshot;
	Ui::Animations::Simple _animation;

};

} // namespace

namespace {

constexpr auto kAppearDuration = crl::time(340);

struct Appearing {
	base::flat_map<const void*, crl::time> started;
	std::vector<QPointer<QWidget>> lists;
	Ui::Animations::Basic animation;
};

[[nodiscard]] Appearing &AppearingState() {
	static auto result = Appearing();
	return result;
}

} // namespace

void MessageAppeared(not_null<const void*> item, not_null<QWidget*> list) {
	if (anim::Disabled()) {
		return;
	}
	auto &state = AppearingState();
	state.started[item.get()] = crl::now();
	if (!ranges::contains(state.lists, list.get(), &QPointer<QWidget>::data)) {
		state.lists.push_back(list.get());
	}
	if (!state.animation.animating()) {
		state.animation.init([] {
			auto &state = AppearingState();
			const auto now = crl::now();
			for (auto i = state.started.begin(); i != state.started.end();) {
				if (now - i->second >= kAppearDuration) {
					i = state.started.erase(i);
				} else {
					++i;
				}
			}
			for (const auto &list : state.lists) {
				if (list) {
					list->update();
				}
			}
			if (state.started.empty()) {
				state.lists.clear();
				state.animation.stop();
			}
		});
		state.animation.start();
	}
}

std::optional<float64> MessageAppearProgress(not_null<const void*> item) {
	const auto &state = AppearingState();
	const auto i = state.started.find(item.get());
	if (i == state.started.end()) {
		return std::nullopt;
	}
	const auto progress = (crl::now() - i->second) / float64(kAppearDuration);
	return (progress < 1.) ? std::make_optional(std::max(progress, 0.)) : std::nullopt;
}

void FadeOutSnapshot(not_null<QWidget*> widget) {
	if (anim::Disabled() || widget->isHidden() || widget->size().isEmpty()) {
		return;
	}
	// A sibling above the widget: the widget may hide its own children
	// while switching the content.
	const auto parent = widget->parentWidget();
	const auto overlay = parent
		? new FadeOverlay(parent, widget->geometry(), Ui::GrabWidget(widget))
		: new FadeOverlay(widget, widget->rect(), Ui::GrabWidget(widget));
	overlay->show();
	overlay->raise();
	overlay->start();
}

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
