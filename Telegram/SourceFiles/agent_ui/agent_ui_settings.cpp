/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "agent_ui/agent_ui_settings.h"

#include "agent_ui/agent_ui.h"
#include "ui/effects/animations.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/vertical_list.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/palette.h"
#include "styles/style_basic.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtGui/QPainterPath>

namespace AgentUi {
namespace {

constexpr auto kHoverDuration = crl::time(180);
constexpr auto kSelectDuration = crl::time(260);

[[nodiscard]] int Px(float64 value) {
	return style::ConvertScale(int(base::SafeRound(value)));
}

[[nodiscard]] QColor Mix(QColor a, QColor b, float64 ratio) {
	const auto mix = [&](int x, int y) {
		return int(base::SafeRound(x + (y - x) * ratio));
	};
	return QColor(
		mix(a.red(), b.red()),
		mix(a.green(), b.green()),
		mix(a.blue(), b.blue()),
		mix(a.alpha(), b.alpha()));
}

[[nodiscard]] QColor Alpha(QColor color, float64 alpha) {
	color.setAlphaF(alpha);
	return color;
}

[[nodiscard]] float64 EaseOut(float64 t) {
	return 1. - std::pow(1. - t, 3);
}

// Mini chat window: list column, header, a few bubbles and a composer,
// drawn in the given colors. Both previews share the same layout, so
// the cards compare the looks and not the pictures.
struct PreviewColors {
	QColor listBg;
	QColor listEdge;
	QColor headerBg;
	QColor composerBg;
	QColor nameBar;
	QColor textBar;
	QColor activeBg;
	QColor activeEdge;
	QColor activeName;
	QColor activeText;
	QColor inBubble;
	QColor outBubble;
	QColor bubbleText;
	QColor outBubbleText;
	QColor accent;
	std::array<QColor, 4> userpics;
};

void PaintRows(
		QPainter &p,
		QRectF list,
		const PreviewColors &colors,
		bool pills) {
	const auto rowHeight = list.height() / 5.4;
	const auto top = list.y() + rowHeight * 0.55;
	const auto side = rowHeight * 0.62;
	for (auto i = 0; i != 4; ++i) {
		const auto row = QRectF(
			list.x(),
			top + i * rowHeight,
			list.width(),
			rowHeight);
		const auto active = (i == 1);
		if (active) {
			const auto pill = pills
				? row.marginsRemoved({ rowHeight * 0.16, 1.5, rowHeight * 0.16, 1.5 })
				: row;
			p.setPen(pills ? QPen(colors.activeEdge, 1.) : Qt::NoPen);
			p.setBrush(colors.activeBg);
			if (pills) {
				p.drawRoundedRect(pill, rowHeight * 0.26, rowHeight * 0.26);
			} else {
				p.drawRect(pill);
			}
			p.setPen(Qt::NoPen);
		}
		const auto userpic = QRectF(
			row.x() + rowHeight * 0.3,
			row.y() + (rowHeight - side) / 2.,
			side,
			side);
		p.setBrush(colors.userpics[i]);
		p.drawEllipse(userpic);
		const auto left = userpic.x() + side + rowHeight * 0.22;
		const auto width = row.x() + row.width() - left - rowHeight * 0.3;
		p.setBrush(active ? colors.activeName : colors.nameBar);
		p.drawRoundedRect(
			QRectF(left, row.y() + rowHeight * 0.3, width * (0.75 - i * 0.08), rowHeight * 0.13),
			rowHeight * 0.065,
			rowHeight * 0.065);
		p.setBrush(active ? colors.activeText : colors.textBar);
		p.drawRoundedRect(
			QRectF(left, row.y() + rowHeight * 0.56, width * (0.95 - i * 0.12), rowHeight * 0.11),
			rowHeight * 0.055,
			rowHeight * 0.055);
	}
}

void PaintBubbles(
		QPainter &p,
		QRectF chat,
		const PreviewColors &colors) {
	const auto unit = chat.height() / 10.;
	const auto radius = unit * 0.7;
	const auto paintBubble = [&](QRectF rect, bool out) {
		p.setBrush(out ? colors.outBubble : colors.inBubble);
		p.drawRoundedRect(rect, radius, radius);
		p.setBrush(out ? colors.outBubbleText : colors.bubbleText);
		const auto line = QRectF(
			rect.x() + unit * 0.5,
			rect.y() + rect.height() / 2. - unit * 0.12,
			rect.width() - unit,
			unit * 0.24);
		p.drawRoundedRect(line, unit * 0.12, unit * 0.12);
	};
	paintBubble(
		QRectF(chat.x() + unit * 0.6, chat.y() + unit * 1.0, chat.width() * 0.52, unit * 1.5),
		false);
	paintBubble(
		QRectF(chat.x() + chat.width() * 0.38, chat.y() + unit * 3.0, chat.width() * 0.56, unit * 1.5),
		true);
	paintBubble(
		QRectF(chat.x() + unit * 0.6, chat.y() + unit * 5.0, chat.width() * 0.4, unit * 1.5),
		false);
}

void PaintVanillaPreview(QPainter &p, QRectF rect) {
	const auto colors = PreviewColors{
		.listBg = QColor(255, 255, 255),
		.listEdge = QColor(0, 0, 0, 24),
		.headerBg = QColor(255, 255, 255),
		.composerBg = QColor(255, 255, 255),
		.nameBar = QColor(34, 34, 34, 200),
		.textBar = QColor(153, 153, 153, 170),
		.activeBg = QColor(0x41, 0x9f, 0xd9),
		.activeName = QColor(255, 255, 255, 235),
		.activeText = QColor(255, 255, 255, 170),
		.inBubble = QColor(255, 255, 255),
		.outBubble = QColor(0xef, 0xfd, 0xde),
		.bubbleText = QColor(0, 0, 0, 120),
		.outBubbleText = QColor(0x3d, 0x7a, 0x3a, 140),
		.accent = QColor(0x40, 0xa7, 0xe3),
		.userpics = {
			QColor(0xff, 0x84, 0x5e),
			QColor(0x5c, 0xaf, 0xfa),
			QColor(0x9a, 0xd1, 0x64),
			QColor(0xb6, 0x94, 0xf9),
		},
	};
	const auto listWidth = rect.width() * 0.38;
	const auto chat = QRectF(
		rect.x() + listWidth,
		rect.y(),
		rect.width() - listWidth,
		rect.height());

	auto wallpaper = QLinearGradient(chat.topLeft(), chat.bottomRight());
	wallpaper.setColorAt(0., QColor(0xd5, 0xd8, 0x8d));
	wallpaper.setColorAt(0.5, QColor(0x88, 0xb8, 0x84));
	wallpaper.setColorAt(1., QColor(0x6b, 0xa5, 0x87));
	p.fillRect(chat, wallpaper);

	const auto list = QRectF(rect.x(), rect.y(), listWidth, rect.height());
	p.fillRect(list, colors.listBg);
	p.fillRect(QRectF(list.right() - 1., list.y(), 1., list.height()), colors.listEdge);
	PaintRows(p, list, colors, false);

	const auto header = QRectF(chat.x(), chat.y(), chat.width(), rect.height() * 0.15);
	p.fillRect(header, colors.headerBg);
	p.fillRect(QRectF(header.x(), header.bottom() - 1., header.width(), 1.), colors.listEdge);
	p.setBrush(colors.nameBar);
	p.drawRoundedRect(
		QRectF(header.x() + header.height() * 0.4, header.y() + header.height() * 0.36, chat.width() * 0.3, header.height() * 0.26),
		header.height() * 0.13,
		header.height() * 0.13);

	const auto composer = QRectF(chat.x(), rect.bottom() - rect.height() * 0.15, chat.width(), rect.height() * 0.15);
	PaintBubbles(
		p,
		QRectF(chat.x(), header.bottom(), chat.width(), composer.y() - header.bottom()),
		colors);
	p.fillRect(composer, colors.composerBg);
	p.fillRect(QRectF(composer.x(), composer.y(), composer.width(), 1.), colors.listEdge);
	p.setBrush(colors.accent);
	const auto send = composer.height() * 0.42;
	p.drawEllipse(QRectF(
		composer.right() - composer.height() * 0.5 - send / 2.,
		composer.center().y() - send / 2.,
		send,
		send));
}

void PaintAgentPreview(QPainter &p, QRectF rect) {
	const auto accent = QColor(0x5c, 0xc8, 0xd8);
	const auto colors = PreviewColors{
		.listBg = QColor(12, 15, 20, 150),
		.listEdge = QColor(255, 255, 255, 26),
		.headerBg = QColor(12, 15, 20, 135),
		.composerBg = QColor(12, 15, 20, 145),
		.nameBar = QColor(238, 241, 245, 215),
		.textBar = QColor(136, 146, 163, 170),
		.activeBg = Alpha(accent, 0.18),
		.activeEdge = Alpha(accent, 0.38),
		.activeName = QColor(255, 255, 255, 240),
		.activeText = QColor(200, 230, 235, 180),
		.inBubble = QColor(27, 32, 41, 225),
		.outBubble = QColor(0x17, 0x43, 0x4b, 235),
		.bubbleText = QColor(233, 237, 242, 120),
		.outBubbleText = QColor(166, 228, 236, 150),
		.accent = accent,
		.userpics = {
			QColor(0xf0, 0x8a, 0x6c),
			QColor(0x5c, 0xc8, 0xd8),
			QColor(0x8f, 0xc9, 0x7a),
			QColor(0xa6, 0x95, 0xe8),
		},
	};

	// Backdrop: the AgentUI freeform gradient with two soft lights.
	auto backdrop = QLinearGradient(rect.topLeft(), rect.bottomRight());
	backdrop.setColorAt(0., QColor(0x0c, 0x14, 0x1b));
	backdrop.setColorAt(0.45, QColor(0x12, 0x30, 0x39));
	backdrop.setColorAt(1., QColor(0x1c, 0x1b, 0x33));
	p.fillRect(rect, backdrop);
	const auto light = [&](QPointF center, float64 radius, QColor color) {
		auto gradient = QRadialGradient(center, radius);
		gradient.setColorAt(0., color);
		gradient.setColorAt(1., Alpha(color, 0.));
		p.fillRect(rect, gradient);
	};
	light(
		QPointF(rect.x() + rect.width() * 0.22, rect.y() + rect.height() * 0.8),
		rect.width() * 0.45,
		QColor(0x2a, 0x8a, 0x96, 120));
	light(
		QPointF(rect.x() + rect.width() * 0.85, rect.y() + rect.height() * 0.15),
		rect.width() * 0.4,
		QColor(0x4a, 0x3a, 0x80, 110));

	const auto listWidth = rect.width() * 0.38;
	const auto list = QRectF(rect.x(), rect.y(), listWidth, rect.height());
	p.fillRect(list, colors.listBg);
	p.fillRect(QRectF(list.right() - 1., list.y(), 1., list.height()), colors.listEdge);
	PaintRows(p, list, colors, true);

	const auto chat = QRectF(
		rect.x() + listWidth,
		rect.y(),
		rect.width() - listWidth,
		rect.height());
	const auto header = QRectF(chat.x(), chat.y(), chat.width(), rect.height() * 0.15);
	p.fillRect(header, colors.headerBg);
	p.fillRect(QRectF(header.x(), header.bottom() - 1., header.width(), 1.), colors.listEdge);
	p.setBrush(colors.nameBar);
	p.drawRoundedRect(
		QRectF(header.x() + header.height() * 0.4, header.y() + header.height() * 0.36, chat.width() * 0.3, header.height() * 0.26),
		header.height() * 0.13,
		header.height() * 0.13);

	const auto composer = QRectF(chat.x(), rect.bottom() - rect.height() * 0.15, chat.width(), rect.height() * 0.15);
	PaintBubbles(
		p,
		QRectF(chat.x(), header.bottom(), chat.width(), composer.y() - header.bottom()),
		colors);
	p.fillRect(composer, colors.composerBg);
	p.fillRect(QRectF(composer.x(), composer.y(), composer.width(), 1.), colors.listEdge);
	p.setBrush(accent);
	const auto send = composer.height() * 0.42;
	p.drawEllipse(QRectF(
		composer.right() - composer.height() * 0.5 - send / 2.,
		composer.center().y() - send / 2.,
		send,
		send));
}

class DesignPicker final : public Ui::RpWidget {
public:
	DesignPicker(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	struct Card {
		Design design = Design::Vanilla;
		QString title;
		QString about;
		Ui::Animations::Simple hover;
		bool over = false;
	};

	[[nodiscard]] QRect cardRect(int index) const;
	[[nodiscard]] int cardAt(QPoint point) const;
	void setOver(int index);
	void select(Design design, QPoint globalOrigin);
	void paintCard(QPainter &p, int index);

	const not_null<Window::SessionController*> _controller;
	std::array<Card, 2> _cards;
	Design _selected = Design::Vanilla;
	Ui::Animations::Simple _selection;
	int _pressed = -1;
	int _previewHeight = 0;

};

DesignPicker::DesignPicker(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: RpWidget(parent)
, _controller(controller)
, _selected(CurrentDesign()) {
	_cards[0].design = Design::Vanilla;
	_cards[0].title = u"Vanilla"_q;
	_cards[0].about = u"Классика Telegram"_q;
	_cards[1].design = Design::AgentUI;
	_cards[1].title = u"AgentUI"_q;
	_cards[1].about = u"Стекло и анимации"_q;
	setMouseTracking(true);

	DesignValue(
	) | rpl::on_next([=](Design design) {
		if (_selected != design) {
			_selected = design;
			_selection.start(
				[=] { update(); },
				0.,
				1.,
				kSelectDuration,
				anim::easeOutCubic);
		}
		update();
	}, lifetime());
}

int DesignPicker::resizeGetHeight(int newWidth) {
	const auto padding = st::defaultSubsectionTitlePadding.left();
	const auto gap = Px(12);
	const auto cardWidth = std::max((newWidth - 2 * padding - gap) / 2, 1);
	_previewHeight = std::min(int(cardWidth * 0.64), Px(150));
	return Px(4) + Px(6) + _previewHeight + Px(52) + Px(4);
}

QRect DesignPicker::cardRect(int index) const {
	const auto padding = st::defaultSubsectionTitlePadding.left();
	const auto gap = Px(12);
	const auto cardWidth = std::max((width() - 2 * padding - gap) / 2, 1);
	return QRect(
		padding + index * (cardWidth + gap),
		Px(4),
		cardWidth,
		Px(6) + _previewHeight + Px(52));
}

int DesignPicker::cardAt(QPoint point) const {
	for (auto i = 0; i != int(_cards.size()); ++i) {
		if (cardRect(i).contains(point)) {
			return i;
		}
	}
	return -1;
}

void DesignPicker::setOver(int index) {
	for (auto i = 0; i != int(_cards.size()); ++i) {
		auto &card = _cards[i];
		const auto over = (i == index);
		if (card.over != over) {
			card.over = over;
			card.hover.start(
				[=] { update(cardRect(i).marginsAdded({ 4, 4, 4, 4 })); },
				over ? 0. : 1.,
				over ? 1. : 0.,
				kHoverDuration,
				anim::easeOutCubic);
		}
	}
	setCursor((index >= 0) ? style::cur_pointer : style::cur_default);
}

void DesignPicker::mouseMoveEvent(QMouseEvent *e) {
	setOver(cardAt(e->pos()));
}

void DesignPicker::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		_pressed = cardAt(e->pos());
	}
}

void DesignPicker::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = std::exchange(_pressed, -1);
	if (pressed >= 0 && pressed == cardAt(e->pos())) {
		select(_cards[pressed].design, e->globalPosition().toPoint());
	}
}

void DesignPicker::leaveEventHook(QEvent *e) {
	setOver(-1);
}

void DesignPicker::select(Design design, QPoint globalOrigin) {
	if (design == CurrentDesign()) {
		return;
	}
	SwitchDesign(&_controller->window(), design, globalOrigin);
}

void DesignPicker::paintCard(QPainter &p, int index) {
	const auto &card = _cards[index];
	const auto rect = QRectF(cardRect(index));
	const auto hover = EaseOut(card.hover.value(card.over ? 1. : 0.));
	const auto shown = _selection.value(1.);
	const auto selected = (card.design == _selected)
		? shown
		: (1. - shown);
	const auto radius = float64(Px(14));

	const auto bg = st::windowBg->c;
	const auto fg = st::windowFg->c;
	const auto accent = st::windowBgActive->c;

	// Card body.
	p.setPen(Qt::NoPen);
	p.setBrush(Mix(Mix(bg, fg, 0.035), fg, 0.03 * hover));
	p.drawRoundedRect(rect, radius, radius);

	// Preview, lifted a little on hover.
	const auto previewRect = QRectF(
		rect.x() + Px(6),
		rect.y() + Px(6),
		rect.width() - 2 * Px(6),
		_previewHeight);
	{
		p.save();
		auto clip = QPainterPath();
		clip.addRoundedRect(previewRect, radius - Px(4), radius - Px(4));
		p.setClipPath(clip);
		const auto scale = 1. + 0.035 * hover;
		const auto center = previewRect.center();
		p.translate(center);
		p.scale(scale, scale);
		p.translate(-center);
		if (card.design == Design::Vanilla) {
			PaintVanillaPreview(p, previewRect);
		} else {
			PaintAgentPreview(p, previewRect);
		}
		p.restore();
	}

	// Title and description.
	const auto textLeft = int(rect.x()) + Px(14);
	const auto textWidth = int(rect.width()) - 2 * Px(14);
	const auto titleTop = int(previewRect.y() + previewRect.height()) + Px(9);
	p.setFont(st::semiboldFont);
	p.setPen(st::windowBoldFg);
	p.drawText(
		QRect(textLeft, titleTop, textWidth, st::semiboldFont->height),
		Qt::AlignLeft | Qt::AlignVCenter,
		card.title);
	p.setFont(st::normalFont);
	p.setPen(st::windowSubTextFg);
	p.drawText(
		QRect(
			textLeft,
			titleTop + st::semiboldFont->height + Px(2),
			textWidth,
			st::normalFont->height),
		Qt::AlignLeft | Qt::AlignVCenter,
		st::normalFont->elided(card.about, textWidth));

	// Selection badge in the corner of the preview.
	const auto markSize = float64(Px(22));
	const auto mark = QRectF(
		previewRect.x() + previewRect.width() - Px(8) - markSize,
		previewRect.y() + Px(8),
		markSize,
		markSize);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0, 0, 0, int(70 * (1. - selected))));
	p.drawEllipse(mark);
	p.setBrush(Qt::NoBrush);
	p.setPen(QPen(
		Mix(QColor(255, 255, 255, 220), accent, selected),
		Px(2) / 1.));
	p.drawEllipse(mark.marginsRemoved({ 1., 1., 1., 1. }));
	if (selected > 0.) {
		const auto inner = mark.width() * (0.5 + 0.5 * EaseOut(selected));
		const auto filled = QRectF(
			mark.center().x() - inner / 2.,
			mark.center().y() - inner / 2.,
			inner,
			inner);
		p.setPen(Qt::NoPen);
		p.setBrush(Alpha(accent, selected));
		p.drawEllipse(filled);

		auto check = QPainterPath();
		const auto c = mark.center();
		const auto u = mark.width() / 20.;
		check.moveTo(c.x() - 4.6 * u, c.y() + 0.2 * u);
		check.lineTo(c.x() - 1.4 * u, c.y() + 3.3 * u);
		check.lineTo(c.x() + 4.8 * u, c.y() - 3.2 * u);
		p.setBrush(Qt::NoBrush);
		auto pen = QPen(Alpha(st::activeButtonFg->c, selected), 1.8 * u);
		pen.setCapStyle(Qt::RoundCap);
		pen.setJoinStyle(Qt::RoundJoin);
		p.setPen(pen);
		p.drawPath(check);
	}

	// Ring around the chosen card.
	if (selected > 0.) {
		auto pen = QPen(Alpha(accent, selected), Px(2));
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(rect.marginsRemoved({ 1., 1., 1., 1. }), radius - 1, radius - 1);
	} else if (hover > 0.) {
		p.setPen(QPen(Alpha(fg, 0.12 * hover), 1.));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(rect.marginsRemoved({ .5, .5, .5, .5 }), radius, radius);
	}
}

void DesignPicker::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	for (auto i = 0; i != int(_cards.size()); ++i) {
		paintCard(p, i);
	}
}

} // namespace

void SetupDesignSettings(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container) {
	Ui::AddSkip(container, st::settingsPrivacySkip);
	Ui::AddSubsectionTitle(container, rpl::single(u"Дизайн интерфейса"_q));
	container->add(object_ptr<DesignPicker>(container, controller));
	Ui::AddSkip(container, Px(6));
	Ui::AddDividerText(
		container,
		rpl::single(u"AgentUI — матовое стекло поверх обоев, мягкое "
			"движение и аккуратная типографика. Светлый и тёмный варианты "
			"переключаются ночным режимом."_q));
}

} // namespace AgentUi
