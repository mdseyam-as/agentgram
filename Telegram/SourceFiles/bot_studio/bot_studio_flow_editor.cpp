/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "bot_studio/bot_studio_flow_editor.h"

#include "bot_studio/bot_studio_flow.h"
#include "bot_studio/bot_studio_manager.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/layer_manager.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/ui_utility.h"
#include "styles/style_ai_agent.h"
#include "styles/style_bot_studio.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtGui/QPainterPath>

namespace BotStudio {

struct FlowState {
	QJsonObject flow;
	QString selected;
	bool dirty = false;
	Fn<void(bool rebuild)> changed;
};

namespace {

constexpr auto kSnap = 10;

// Block kind accents, the same in both themes.
const auto kStartColor = QColor(0x4f, 0xae, 0x4e);
const auto kInputColor = QColor(0xf0, 0xa0, 0x30);
const auto kNotifyColor = QColor(0xa3, 0x6f, 0xf0);
constexpr auto kMaxTextLines = 3;

[[nodiscard]] QColor WithAlpha(QColor color, int alpha) {
	color.setAlpha(alpha);
	return color;
}

[[nodiscard]] not_null<Ui::RoundButton*> MakeButton(
		not_null<QWidget*> parent,
		const QString &text,
		const style::RoundButton &st) {
	const auto result = Ui::CreateChild<Ui::RoundButton>(
		parent.get(),
		rpl::single(text),
		st);
	result->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
	return result;
}

[[nodiscard]] QJsonArray Buttons(const QJsonObject &block) {
	return block.value(u"buttons"_q).toArray();
}

[[nodiscard]] QJsonObject Input(const QJsonObject &block) {
	return block.value(u"input"_q).toObject();
}

[[nodiscard]] bool HasInput(const QJsonObject &block) {
	return Input(block).value(u"enabled"_q).toBool();
}

[[nodiscard]] QString KindName(const QString &kind) {
	if (kind == u"phone"_q) {
		return u"телефон"_q;
	} else if (kind == u"number"_q) {
		return u"число"_q;
	} else if (kind == u"email"_q) {
		return u"e-mail"_q;
	}
	return u"текст"_q;
}

[[nodiscard]] QStringList KnownVariables(const QJsonObject &flow) {
	auto result = QStringList{ u"first_name"_q, u"name"_q, u"username"_q };
	for (const auto &value : FlowBlocks(flow)) {
		const auto input = Input(value.toObject());
		const auto name = input.value(u"variable"_q).toString();
		if (input.value(u"enabled"_q).toBool()
			&& !name.isEmpty()
			&& !result.contains(name)) {
			result.push_back(name);
		}
	}
	return result;
}

struct NodeLayout {
	QRect rect;
	QRect textRect;
	std::vector<QRect> buttons;
	QRect footer;
	QPoint nextAnchor;
};

} // namespace

class FlowCanvas final : public Ui::RpWidget {
public:
	FlowCanvas(QWidget *parent, std::shared_ptr<FlowState> state);

	void refresh();
	[[nodiscard]] QRect nodeRect(const QString &id) const;
	Fn<void(QPoint)> addRequest;
	Fn<void(QString, QPoint)> contextRequest;

protected:
	void paintEvent(QPaintEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;

private:
	[[nodiscard]] NodeLayout layout(const QJsonObject &block) const;
	[[nodiscard]] QString nodeAt(QPoint point) const;
	void paintNode(
		QPainter &p,
		const QJsonObject &block,
		const NodeLayout &layout);
	void paintArrow(QPainter &p, QPoint from, QRect target, bool dashed);

	const std::shared_ptr<FlowState> _state;
	QString _dragging;
	QPoint _dragStart;
	QPoint _dragOrigin;
	bool _moved = false;

};

FlowCanvas::FlowCanvas(QWidget *parent, std::shared_ptr<FlowState> state)
: RpWidget(parent)
, _state(std::move(state)) {
	setMouseTracking(true);
}

void FlowCanvas::refresh() {
	auto right = 0;
	auto bottom = 0;
	for (const auto &value : FlowBlocks(_state->flow)) {
		const auto rect = layout(value.toObject()).rect;
		right = std::max(right, rect.x() + rect.width());
		bottom = std::max(bottom, rect.y() + rect.height());
	}
	const auto padding = st::botStudioCanvasPadding;
	const auto parent = parentWidget();
	const auto minWidth = parent ? parent->width() : 0;
	const auto minHeight = parent ? parent->height() : 0;
	resize(
		std::max(right + padding * 4, minWidth),
		std::max(bottom + padding * 3, minHeight));
	update();
}

NodeLayout FlowCanvas::layout(const QJsonObject &block) const {
	auto result = NodeLayout();
	const auto &padding = st::botStudioNodePadding;
	const auto width = st::botStudioNodeWidth;
	const auto inner = width - padding.left() - padding.right();
	const auto x = st::botStudioCanvasPadding
		+ block.value(u"x"_q).toInt();
	const auto y = st::botStudioCanvasPadding
		+ block.value(u"y"_q).toInt();
	auto top = st::botStudioNodeStripe
		+ padding.top()
		+ style::font(st::botStudioNodeTitleFont)->height
		+ 6;

	const auto font = style::font(st::botStudioNodeTextFont);
	auto text = block.value(u"text"_q).toString().trimmed();
	if (!block.value(u"photo"_q).toString().trimmed().isEmpty()) {
		text = u"🖼 "_q + text;
	}
	if (text.isEmpty()) {
		text = u"Пустое сообщение"_q;
	}
	const auto bounding = QFontMetrics(font->f).boundingRect(
		QRect(0, 0, inner, font->height * kMaxTextLines),
		Qt::TextWordWrap,
		text);
	const auto textHeight = std::min(
		bounding.height(),
		font->height * kMaxTextLines);
	result.textRect = QRect(x + padding.left(), y + top, inner, textHeight);
	top += textHeight;

	const auto buttons = Buttons(block);
	if (!buttons.isEmpty()) {
		top += st::botStudioNodeButtonSkip * 2;
	}
	for (auto i = 0; i != buttons.size(); ++i) {
		result.buttons.push_back(QRect(
			x + padding.left(),
			y + top,
			inner,
			st::botStudioNodeButtonHeight));
		top += st::botStudioNodeButtonHeight + st::botStudioNodeButtonSkip;
	}
	const auto hasFooter = HasInput(block)
		|| !block.value(u"next"_q).toString().isEmpty();
	if (hasFooter) {
		top += st::botStudioNodeButtonSkip;
		result.footer = QRect(
			x + padding.left(),
			y + top,
			inner,
			style::font(st::botStudioNodeBadgeFont)->height + 4);
		top += result.footer.height();
	}
	top += padding.bottom();
	result.rect = QRect(x, y, width, top);
	result.nextAnchor = hasFooter
		? QPoint(x + width, result.footer.center().y())
		: QPoint(x + width, y + top / 2);
	return result;
}

QRect FlowCanvas::nodeRect(const QString &id) const {
	return layout(FlowBlock(_state->flow, id)).rect;
}

QString FlowCanvas::nodeAt(QPoint point) const {
	const auto blocks = FlowBlocks(_state->flow);
	for (auto i = blocks.size(); i != 0;) {
		const auto block = blocks[--i].toObject();
		if (layout(block).rect.contains(point)) {
			return block.value(u"id"_q).toString();
		}
	}
	return QString();
}

void FlowCanvas::paintArrow(
		QPainter &p,
		QPoint from,
		QRect target,
		bool dashed) {
	const auto to = QPoint(target.x(), target.y() + 22);
	auto path = QPainterPath(from);
	const auto dx = std::max(70, std::abs(to.x() - from.x()) / 2);
	if (to.x() > from.x()) {
		path.cubicTo(
			QPointF(from.x() + dx, from.y()),
			QPointF(to.x() - dx, to.y()),
			QPointF(to.x() - 6, to.y()));
	} else {
		// Back links go around: out to the right, in from the left.
		const auto loop = 90;
		path.cubicTo(
			QPointF(from.x() + loop, from.y()),
			QPointF(to.x() - loop, to.y()),
			QPointF(to.x() - 6, to.y()));
	}
	auto pen = QPen(
		WithAlpha(st::botStudioArrowFg->c, 150),
		st::botStudioArrowWidth);
	if (dashed) {
		pen.setDashPattern({ 3., 3. });
	}
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	p.drawPath(path);
	p.setPen(Qt::NoPen);
	p.setBrush(WithAlpha(st::botStudioArrowFg->c, 190));
	auto head = QPainterPath(QPointF(to.x(), to.y()));
	head.lineTo(to.x() - 9, to.y() - 5);
	head.lineTo(to.x() - 9, to.y() + 5);
	head.closeSubpath();
	p.drawPath(head);
}

void FlowCanvas::paintNode(
		QPainter &p,
		const QJsonObject &block,
		const NodeLayout &layout) {
	const auto id = block.value(u"id"_q).toString();
	const auto selected = (id == _state->selected);
	const auto isStart = (id == _state->flow.value(u"start"_q).toString());
	const auto rect = layout.rect;
	const auto radius = st::botStudioNodeRadius;

	// Soft shadow.
	p.setPen(Qt::NoPen);
	p.setBrush(WithAlpha(QColor(0, 0, 0), selected ? 46 : 26));
	p.drawRoundedRect(rect.translated(0, 3), radius, radius);

	p.setBrush(st::botStudioCardBg);
	p.setPen(selected
		? QPen(st::windowBgActive->c, 2)
		: QPen(WithAlpha(st::botStudioCardBorder->c, 120), 1));
	p.drawRoundedRect(rect, radius, radius);

	// Colored stripe on top shows the block kind.
	const auto stripe = isStart
		? kStartColor
		: HasInput(block)
		? kInputColor
		: block.value(u"notifyOwner"_q).toBool()
		? kNotifyColor
		: st::windowBgActive->c;
	p.save();
	auto clip = QPainterPath();
	clip.addRoundedRect(rect, radius, radius);
	p.setClipPath(clip);
	p.fillRect(
		QRect(rect.x(), rect.y(), rect.width(), st::botStudioNodeStripe),
		stripe);
	p.restore();

	const auto &padding = st::botStudioNodePadding;
	const auto inner = rect.width() - padding.left() - padding.right();
	const auto titleFont = style::font(st::botStudioNodeTitleFont);
	const auto badgeFont = style::font(st::botStudioNodeBadgeFont);
	const auto titleTop = rect.y() + st::botStudioNodeStripe + padding.top();

	// Badges on the right of the title.
	auto badges = QStringList();
	if (isStart) {
		badges.push_back(u"▶ старт"_q);
	}
	if (block.value(u"notifyOwner"_q).toBool()) {
		badges.push_back(u"📩"_q);
	}
	auto badgeRight = rect.x() + rect.width() - padding.right();
	p.setFont(badgeFont);
	for (const auto &badge : badges) {
		const auto w = badgeFont->width(badge) + 12;
		const auto badgeRect = QRect(
			badgeRight - w,
			titleTop,
			w,
			badgeFont->height + 2);
		p.setPen(Qt::NoPen);
		p.setBrush(WithAlpha(stripe, 40));
		p.drawRoundedRect(badgeRect, badgeRect.height() / 2., badgeRect.height() / 2.);
		p.setPen(stripe);
		p.drawText(badgeRect, Qt::AlignCenter, badge);
		badgeRight -= w + 4;
	}
	p.setFont(titleFont);
	p.setPen(st::windowBoldFg);
	const auto title = FlowBlockTitle(_state->flow, id);
	p.drawText(
		rect.x() + padding.left(),
		titleTop + titleFont->ascent,
		titleFont->elided(
			title,
			badgeRight - rect.x() - padding.left() - 4));

	// Text preview.
	const auto textFont = style::font(st::botStudioNodeTextFont);
	auto text = block.value(u"text"_q).toString().trimmed();
	if (!block.value(u"photo"_q).toString().trimmed().isEmpty()) {
		text = u"🖼 "_q + text;
	}
	p.setFont(textFont);
	p.setPen(text.isEmpty() ? st::attentionButtonFg : st::windowSubTextFg);
	if (text.isEmpty()) {
		text = u"Пустое сообщение"_q;
	}
	p.drawText(
		layout.textRect,
		Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop,
		text);

	// Buttons.
	const auto buttons = Buttons(block);
	const auto buttonFont = style::font(st::botStudioNodeButtonFont);
	p.setFont(buttonFont);
	for (auto i = 0; i != int(layout.buttons.size()); ++i) {
		const auto button = buttons[i].toObject();
		const auto r = layout.buttons[i];
		p.setPen(Qt::NoPen);
		p.setBrush(st::botStudioNodeButtonBg);
		p.drawRoundedRect(r, r.height() / 2., r.height() / 2.);
		const auto url = button.value(u"url"_q).toString();
		const auto target = button.value(u"goto"_q).toString();
		const auto suffix = !url.isEmpty()
			? u"  🔗"_q
			: target.isEmpty()
			? u"  ⚠"_q
			: QString();
		p.setPen(target.isEmpty() && url.isEmpty()
			? st::attentionButtonFg
			: st::windowActiveTextFg);
		p.drawText(
			r.marginsRemoved({ 10, 0, 10, 0 }),
			Qt::AlignVCenter | Qt::AlignHCenter,
			buttonFont->elided(
				button.value(u"text"_q).toString() + suffix,
				inner - 20));
	}

	// Footer: waiting for an answer or a follow-up block.
	if (!layout.footer.isEmpty()) {
		p.setFont(badgeFont);
		const auto input = Input(block);
		const auto footer = HasInput(block)
			? (u"✍ ждёт ответ: "_q
				+ KindName(input.value(u"kind"_q).toString())
				+ u" → {"_q
				+ input.value(u"variable"_q).toString()
				+ u"}"_q)
			: (u"⏬ затем: "_q
				+ FlowBlockTitle(
					_state->flow,
					block.value(u"next"_q).toString()));
		p.setPen(HasInput(block)
			? kInputColor
			: st::windowSubTextFg->c);
		p.drawText(
			layout.footer,
			Qt::AlignVCenter | Qt::AlignLeft,
			badgeFont->elided(footer, inner));
	}
}

void FlowCanvas::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto clip = e->rect();
	p.fillRect(clip, st::botStudioCanvasBg);

	// Dotted grid.
	const auto step = 22;
	p.setPen(Qt::NoPen);
	p.setBrush(WithAlpha(st::botStudioCanvasDot->c, 45));
	for (auto x = (clip.x() / step) * step; x < clip.right(); x += step) {
		for (auto y = (clip.y() / step) * step; y < clip.bottom(); y += step) {
			p.drawEllipse(QPointF(x, y), 1., 1.);
		}
	}

	const auto blocks = FlowBlocks(_state->flow);
	auto layouts = base::flat_map<QString, NodeLayout>();
	for (const auto &value : blocks) {
		const auto block = value.toObject();
		layouts.emplace(block.value(u"id"_q).toString(), layout(block));
	}
	const auto targetRect = [&](const QString &id) -> std::optional<QRect> {
		const auto i = layouts.find(id);
		return (i != end(layouts))
			? std::make_optional(i->second.rect)
			: std::nullopt;
	};
	for (const auto &value : blocks) {
		const auto block = value.toObject();
		const auto &l = layouts[block.value(u"id"_q).toString()];
		const auto buttons = Buttons(block);
		for (auto i = 0; i != int(l.buttons.size()); ++i) {
			const auto target = buttons[i].toObject().value(
				u"goto"_q).toString();
			if (const auto rect = targetRect(target)) {
				const auto r = l.buttons[i];
				paintArrow(
					p,
					QPoint(l.rect.x() + l.rect.width(), r.center().y()),
					*rect,
					false);
			}
		}
		const auto next = HasInput(block)
			? Input(block).value(u"next"_q).toString()
			: block.value(u"next"_q).toString();
		if (const auto rect = targetRect(next)) {
			paintArrow(p, l.nextAnchor, *rect, true);
		}
	}
	for (const auto &value : blocks) {
		const auto block = value.toObject();
		paintNode(p, block, layouts[block.value(u"id"_q).toString()]);
	}
	if (blocks.size() == 1) {
		p.setFont(st::botStudioNodeTextFont);
		p.setPen(st::windowSubTextFg);
		const auto first = layouts.begin()->second.rect;
		p.drawText(
			first.x(),
			first.y() + first.height() + 28,
			u"Двойной щелчок по пустому месту — новое сообщение"_q);
	}
}

void FlowCanvas::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto id = nodeAt(e->pos());
	if (id != _state->selected) {
		_state->selected = id;
		_state->changed(true);
	}
	if (!id.isEmpty()) {
		const auto block = FlowBlock(_state->flow, id);
		_dragging = id;
		_dragStart = e->pos();
		_dragOrigin = QPoint(
			block.value(u"x"_q).toInt(),
			block.value(u"y"_q).toInt());
		_moved = false;
	}
	update();
}

void FlowCanvas::mouseMoveEvent(QMouseEvent *e) {
	if (_dragging.isEmpty()) {
		setCursor(nodeAt(e->pos()).isEmpty()
			? style::cur_default
			: style::cur_pointer);
		return;
	}
	const auto delta = e->pos() - _dragStart;
	if (!_moved && delta.manhattanLength() < 4) {
		return;
	}
	_moved = true;
	setCursor(style::cur_sizeall);
	auto block = FlowBlock(_state->flow, _dragging);
	const auto snap = [](int value) {
		return std::max(0, (value / kSnap) * kSnap);
	};
	block.insert(u"x"_q, snap(_dragOrigin.x() + delta.x()));
	block.insert(u"y"_q, snap(_dragOrigin.y() + delta.y()));
	SetFlowBlock(_state->flow, block);
	update();
}

void FlowCanvas::mouseReleaseEvent(QMouseEvent *e) {
	if (!_dragging.isEmpty() && _moved) {
		_state->changed(false);
		refresh();
	}
	_dragging = QString();
	_moved = false;
	setCursor(style::cur_default);
}

void FlowCanvas::mouseDoubleClickEvent(QMouseEvent *e) {
	if (nodeAt(e->pos()).isEmpty() && addRequest) {
		addRequest(e->pos() - QPoint(
			st::botStudioCanvasPadding,
			st::botStudioCanvasPadding));
	}
}

void FlowCanvas::contextMenuEvent(QContextMenuEvent *e) {
	const auto id = nodeAt(e->pos());
	if (!id.isEmpty() && contextRequest) {
		_state->selected = id;
		_state->changed(true);
		contextRequest(id, e->globalPos());
	}
}

FlowEditor::FlowEditor(
	QWidget *parent,
	not_null<Studio*> studio,
	not_null<Ui::LayerManager*> layers,
	QString botId)
: RpWidget(parent)
, _studio(studio)
, _layers(layers)
, _botId(std::move(botId))
, _state(std::make_shared<FlowState>())
, _add(MakeButton(this, u"＋ Сообщение"_q, st::botStudioSmallButton))
, _settings(MakeButton(this, u"⚙ Команды и ответы"_q, st::botStudioSmallButton))
, _save(MakeButton(this, u"Сохранить и запустить"_q, st::botStudioActionButton))
, _status(Ui::CreateChild<Ui::FlatLabel>(this, QString(), st::botStudioHint))
, _canvasScroll(Ui::CreateChild<Ui::ScrollArea>(this, st::defaultSolidScroll))
, _canvas(_canvasScroll->setOwnedWidget(
	object_ptr<FlowCanvas>(_canvasScroll, _state)))
, _inspectorScroll(Ui::CreateChild<Ui::ScrollArea>(
	this,
	st::defaultSolidScroll)) {
	_state->flow = NormalizeFlow(_studio->flow(_botId));
	_state->selected = _state->flow.value(u"start"_q).toString();
	_state->changed = [=](bool rebuild) {
		markChanged(rebuild);
	};
	_canvas->addRequest = [=](QPoint position) {
		addBlock(position);
	};
	_canvas->contextRequest = [=](QString id, QPoint global) {
		const auto menu = Ui::CreateChild<Ui::PopupMenu>(
			this,
			st::defaultPopupMenu);
		menu->setDestroyedCallback([] {});
		menu->addAction(u"★ Сделать стартовым"_q, [=] {
			_state->flow.insert(u"start"_q, id);
			markChanged(true);
		});
		menu->addAction(u"⧉ Дублировать"_q, [=] {
			auto copy = FlowBlock(_state->flow, id);
			copy.insert(u"id"_q, NewFlowBlockId(_state->flow));
			copy.insert(u"title"_q, copy.value(u"title"_q).toString()
				+ u" (копия)"_q);
			copy.insert(u"x"_q, copy.value(u"x"_q).toInt() + 30);
			copy.insert(u"y"_q, copy.value(u"y"_q).toInt() + 30);
			SetFlowBlock(_state->flow, copy);
			_state->selected = copy.value(u"id"_q).toString();
			markChanged(true);
		});
		menu->addAction(u"🗑 Удалить"_q, [=] {
			if (FlowBlocks(_state->flow).size() < 2) {
				return;
			}
			RemoveFlowBlock(_state->flow, id);
			_state->selected = _state->flow.value(u"start"_q).toString();
			markChanged(true);
		});
		menu->popup(global);
	};
	setupToolbar();
	_inspectorScroll->paintRequest(
	) | rpl::on_next([=](QRect clip) {
		auto p = QPainter(_inspectorScroll);
		p.fillRect(clip, st::botStudioInspectorBg);
	}, _inspectorScroll->lifetime());
	rebuildInspector();
	updateStatus();
}

FlowEditor::~FlowEditor() = default;

bool FlowEditor::dirty() const {
	return _state->dirty;
}

void FlowEditor::setupToolbar() {
	_add->setClickedCallback([=] {
		// Put the new block to the right of the selected one.
		auto position = QPoint(40, 40);
		const auto selected = FlowBlock(_state->flow, _state->selected);
		if (!selected.isEmpty()) {
			position = QPoint(
				selected.value(u"x"_q).toInt() + 320,
				selected.value(u"y"_q).toInt());
		}
		addBlock(position);
	});
	_settings->setClickedCallback([=] { showSettings(); });
	_save->setClickedCallback([=] { save(); });
}

void FlowEditor::addBlock(QPoint position, bool select) {
	const auto id = NewFlowBlockId(_state->flow);
	SetFlowBlock(_state->flow, QJsonObject{
		{ u"id"_q, id },
		{ u"title"_q, u"Сообщение %1"_q.arg(FlowBlocks(_state->flow).size() + 1) },
		{ u"text"_q, QString() },
		{ u"x"_q, std::max(0, (position.x() / kSnap) * kSnap) },
		{ u"y"_q, std::max(0, (position.y() / kSnap) * kSnap) },
	});
	if (select) {
		_state->selected = id;
	}
	markChanged(true);
	const auto rect = _canvas->nodeRect(id);
	_canvasScroll->scrollToX(rect.x() - 40, rect.x() + rect.width() + 40);
	_canvasScroll->scrollToY(rect.y() - 40, rect.y() + rect.height() + 40);
}

void FlowEditor::markChanged(bool rebuild) {
	_state->dirty = true;
	_canvas->refresh();
	if (rebuild) {
		rebuildInspector();
	}
	updateStatus();
}

void FlowEditor::save() {
	const auto error = _studio->setFlow(_botId, _state->flow);
	_state->dirty = false;
	if (error.isEmpty()) {
		_studio->start(_botId);
	}
	updateStatus();
}

void FlowEditor::reload() {
	if (_state->dirty) {
		return;
	}
	const auto flow = NormalizeFlow(_studio->flow(_botId));
	if (flow == _state->flow) {
		return;
	}
	_state->flow = flow;
	if (FlowBlockIndex(flow, _state->selected) < 0) {
		_state->selected = flow.value(u"start"_q).toString();
	}
	_canvas->refresh();
	rebuildInspector();
	updateStatus();
}

void FlowEditor::updateStatus() {
	const auto problems = ValidateFlow(_state->flow);
	if (!problems.isEmpty()) {
		_status->setTextColorOverride(st::attentionButtonFg->c);
		_status->setText((problems.size() > 1)
			? u"%1 (и ещё %2)"_q.arg(problems.front()).arg(problems.size() - 1)
			: problems.front());
	} else {
		_status->setTextColorOverride(std::nullopt);
		_status->setText(_state->dirty
			? u"Есть несохранённые изменения"_q
			: u"Все изменения сохранены"_q);
	}
	updateGeometry();
}

void FlowEditor::rebuildInspector() {
	const auto id = _state->selected;
	_inspected = id;
	auto content = object_ptr<Ui::VerticalLayout>(_inspectorScroll);
	const auto layout = content.data();
	_inspector = layout;
	const auto &padding = st::botStudioInspectorPadding;
	const auto row = [&](object_ptr<Ui::RpWidget> widget, int top = 0) {
		return layout->add(
			std::move(widget),
			QMargins(padding.left(), top, padding.right(), 0));
	};
	const auto section = [&](const QString &text, int top = 14) {
		row(object_ptr<Ui::FlatLabel>(
			layout,
			text,
			st::botStudioInspectorSection), top);
	};
	const auto hint = [&](const QString &text, int top = 4) {
		return static_cast<Ui::FlatLabel*>(row(object_ptr<Ui::FlatLabel>(
			layout,
			text,
			st::botStudioInspectorLabel), top));
	};
	const auto update = [=](Fn<void(QJsonObject&)> change, bool rebuild) {
		auto block = FlowBlock(_state->flow, id);
		if (block.isEmpty()) {
			return;
		}
		change(block);
		SetFlowBlock(_state->flow, block);
		if (rebuild) {
			crl::on_main(this, [=] { markChanged(true); });
		} else {
			markChanged(false);
		}
	};
	const auto field = [&](
			const QString &placeholder,
			const QString &value,
			bool multiline,
			Fn<void(QString)> changed) {
		const auto result = static_cast<Ui::InputField*>(row(
			object_ptr<Ui::InputField>(
				layout,
				multiline ? st::botStudioFieldMulti : st::botStudioField,
				multiline
					? Ui::InputField::Mode::MultiLine
					: Ui::InputField::Mode::SingleLine,
				rpl::single(placeholder),
				value)));
		result->changes() | rpl::on_next([=] {
			changed(result->getLastText());
		}, result->lifetime());
		return result;
	};
	// A "→ target" chooser: blocks, a new block and optional extra items.
	const auto target = [=](
			not_null<QWidget*> parent,
			const QString &current,
			bool allowNone,
			Fn<void(QString)> chosen,
			Fn<void()> chooseUrl) {
		const auto label = current.isEmpty()
			? (allowNone ? u"нет"_q : u"⚠ выберите, куда ведёт"_q)
			: (u"→ "_q + FlowBlockTitle(_state->flow, current));
		const auto button = MakeButton(
			parent,
			label,
			st::botStudioTargetButton);
		button->setClickedCallback([=] {
			const auto menu = Ui::CreateChild<Ui::PopupMenu>(
				this,
				st::defaultPopupMenu);
			for (const auto &value : FlowBlocks(_state->flow)) {
				const auto targetId = value.toObject().value(
					u"id"_q).toString();
				const auto title = FlowBlockTitle(_state->flow, targetId);
				menu->addAction(
					(targetId == current ? u"✓ "_q : u"→ "_q) + title,
					[=] { chosen(targetId); });
			}
			menu->addSeparator();
			menu->addAction(u"＋ Новое сообщение"_q, [=] {
				const auto source = FlowBlock(_state->flow, id);
				const auto newId = NewFlowBlockId(_state->flow);
				SetFlowBlock(_state->flow, QJsonObject{
					{ u"id"_q, newId },
					{ u"title"_q, u"Сообщение %1"_q.arg(
						FlowBlocks(_state->flow).size() + 1) },
					{ u"text"_q, QString() },
					{ u"x"_q, source.value(u"x"_q).toInt() + 320 },
					{ u"y"_q, source.value(u"y"_q).toInt()
						+ 60 * (Buttons(source).size()) },
				});
				chosen(newId);
			});
			if (chooseUrl) {
				menu->addAction(u"🔗 Открыть ссылку"_q, chooseUrl);
			}
			if (allowNone) {
				menu->addAction(u"✕ Ничего"_q, [=] { chosen(QString()); });
			}
			menu->popup(QCursor::pos());
		});
		return button;
	};

	const auto block = FlowBlock(_state->flow, id);
	if (block.isEmpty()) {
		section(u"Схема бота"_q, 18);
		hint(u"Выберите сообщение на схеме, чтобы изменить его, или "
			"нажмите «＋ Сообщение».\n\nСообщения связываются кнопками: "
			"кнопка ведёт к другому сообщению. Блок может ждать ответ "
			"пользователя (имя, телефон) и присылать вам заявку."_q);
	} else {
		const auto isStart = (id == _state->flow.value(u"start"_q).toString());
		section(isStart
			? u"▶ Стартовое сообщение"_q
			: u"Сообщение"_q, 16);
		hint(isStart
			? u"Его бот показывает по команде /start."_q
			: u"Показывается, когда пользователь нажал ведущую сюда "
				"кнопку."_q);
		field(
			u"Название (видно только вам)"_q,
			block.value(u"title"_q).toString(),
			false,
			[=](QString text) {
				update([&](QJsonObject &b) { b.insert(u"title"_q, text); }, false);
			});
		field(
			u"Текст сообщения"_q,
			block.value(u"text"_q).toString(),
			true,
			[=](QString text) {
				update([&](QJsonObject &b) { b.insert(u"text"_q, text); }, false);
			});
		auto variables = QStringList();
		for (const auto &name : KnownVariables(_state->flow)) {
			variables.push_back(u"{"_q + name + u"}"_q);
		}
		hint(u"Подстановки: "_q + variables.join(u", "_q)
			+ u". Например: «Привет, {first_name}!»"_q);
		field(
			u"Картинка: ссылка https://… (необязательно)"_q,
			block.value(u"photo"_q).toString(),
			false,
			[=](QString text) {
				update([&](QJsonObject &b) {
					b.insert(u"photo"_q, text.trimmed());
				}, false);
			});

		// Buttons.
		section(u"Кнопки"_q);
		const auto buttons = Buttons(block);
		for (auto i = 0; i != buttons.size(); ++i) {
			const auto button = buttons[i].toObject();
			const auto line = row(object_ptr<Ui::RpWidget>(layout), 6);
			const auto text = Ui::CreateChild<Ui::InputField>(
				line,
				st::botStudioField,
				rpl::single(u"Текст кнопки %1"_q.arg(i + 1)),
				button.value(u"text"_q).toString());
			const auto url = button.value(u"url"_q).toString();
			const auto isUrl = button.contains(u"url"_q);
			const auto setButton = [=](Fn<void(QJsonObject&)> change, bool rebuild) {
				update([&](QJsonObject &b) {
					auto list = Buttons(b);
					if (i < list.size()) {
						auto object = list[i].toObject();
						change(object);
						list[i] = object;
						b.insert(u"buttons"_q, list);
					}
				}, rebuild);
			};
			text->changes() | rpl::on_next([=] {
				const auto value = text->getLastText();
				setButton([&](QJsonObject &o) {
					o.insert(u"text"_q, value);
				}, false);
			}, text->lifetime());
			const auto remove = Ui::CreateChild<Ui::IconButton>(
				line,
				st::botStudioRemoveButton);
			remove->setClickedCallback([=] {
				update([&](QJsonObject &b) {
					auto list = Buttons(b);
					if (i < list.size()) {
						list.removeAt(i);
					}
					b.insert(u"buttons"_q, list);
				}, true);
			});
			Ui::RpWidget *second = nullptr;
			if (isUrl) {
				const auto link = Ui::CreateChild<Ui::InputField>(
					line,
					st::botStudioField,
					rpl::single(u"Ссылка https://…"_q),
					url);
				link->changes() | rpl::on_next([=] {
					const auto value = link->getLastText().trimmed();
					setButton([&](QJsonObject &o) {
						o.insert(u"url"_q, value);
					}, false);
				}, link->lifetime());
				second = link;
			} else {
				second = target(
					line,
					button.value(u"goto"_q).toString(),
					false,
					[=](QString chosen) {
						setButton([&](QJsonObject &o) {
							o.remove(u"url"_q);
							o.insert(u"goto"_q, chosen);
						}, true);
					},
					[=] {
						setButton([&](QJsonObject &o) {
							o.remove(u"goto"_q);
							o.insert(u"url"_q, u"https://"_q);
						}, true);
					});
			}
			line->widthValue() | rpl::on_next([=](int width) {
				const auto removeWidth = remove->width();
				text->resize(width - removeWidth - 4, text->height());
				text->moveToLeft(0, 0);
				remove->moveToLeft(width - removeWidth, 14);
				second->resizeToWidth(isUrl
					? (width - removeWidth - 4)
					: second->width());
				second->moveToLeft(0, text->height() + 2);
				line->resize(width, text->height() + 2 + second->height() + 4);
			}, line->lifetime());
		}
		const auto addButton = row(object_ptr<Ui::LinkButton>(
			layout,
			u"＋ Добавить кнопку"_q), 8);
		static_cast<Ui::LinkButton*>(addButton)->setClickedCallback([=] {
			update([&](QJsonObject &b) {
				auto list = Buttons(b);
				list.push_back(QJsonObject{
					{ u"text"_q, u"Кнопка %1"_q.arg(list.size() + 1) },
				});
				b.insert(u"buttons"_q, list);
			}, true);
		});

		// Waiting for an answer.
		section(u"Ответ пользователя"_q, 18);
		const auto input = Input(block);
		const auto wait = static_cast<Ui::Checkbox*>(row(
			object_ptr<Ui::Checkbox>(
				layout,
				u"Ждать ответ и сохранить его"_q,
				input.value(u"enabled"_q).toBool(),
				st::botStudioCheckbox,
				st::defaultToggle), 8));
		wait->checkedChanges() | rpl::on_next([=](bool checked) {
			update([&](QJsonObject &b) {
				auto in = Input(b);
				in.insert(u"enabled"_q, checked);
				if (in.value(u"variable"_q).toString().isEmpty()) {
					in.insert(u"variable"_q, u"ответ"_q);
				}
				if (in.value(u"kind"_q).toString().isEmpty()) {
					in.insert(u"kind"_q, u"text"_q);
				}
				b.insert(u"input"_q, in);
			}, true);
		}, wait->lifetime());
		if (input.value(u"enabled"_q).toBool()) {
			field(
				u"Сохранить ответ как (например: имя, телефон)"_q,
				input.value(u"variable"_q).toString(),
				false,
				[=](QString text) {
					update([&](QJsonObject &b) {
						auto in = Input(b);
						in.insert(
							u"variable"_q,
							text.trimmed().replace(' ', '_'));
						b.insert(u"input"_q, in);
					}, false);
				});
			const auto kinds = row(object_ptr<Ui::RpWidget>(layout), 8);
			auto left = 0;
			auto height = 0;
			const auto kind = input.value(u"kind"_q).toString();
			for (const auto &[value, title] : std::vector<std::pair<QString, QString>>{
					{ u"text"_q, u"Текст"_q },
					{ u"phone"_q, u"📱 Телефон"_q },
					{ u"number"_q, u"Число"_q },
					{ u"email"_q, u"E-mail"_q } }) {
				const auto chip = MakeButton(
					kinds,
					title,
					(kind == value || (kind.isEmpty() && value == u"text"_q))
						? st::botStudioTemplateButtonActive
						: st::botStudioTemplateButton);
				chip->moveToLeft(left, 0);
				left += chip->width() + 6;
				height = chip->height();
				const auto chosen = value;
				chip->setClickedCallback([=] {
					update([&](QJsonObject &b) {
						auto in = Input(b);
						in.insert(u"kind"_q, chosen);
						b.insert(u"input"_q, in);
					}, true);
				});
			}
			kinds->resize(left, height);
			const auto after = row(object_ptr<Ui::RpWidget>(layout), 10);
			const auto afterLabel = Ui::CreateChild<Ui::FlatLabel>(
				after,
				u"После ответа:"_q,
				st::botStudioInspectorLabel);
			const auto afterTarget = target(
				after,
				input.value(u"next"_q).toString(),
				true,
				[=](QString chosen) {
					update([&](QJsonObject &b) {
						auto in = Input(b);
						in.insert(u"next"_q, chosen);
						b.insert(u"input"_q, in);
					}, true);
				},
				nullptr);
			afterLabel->moveToLeft(0, (afterTarget->height() - afterLabel->height()) / 2);
			afterTarget->moveToLeft(afterLabel->width() + 8, 0);
			after->resize(
				afterLabel->width() + 8 + afterTarget->width(),
				afterTarget->height());
		} else if (Buttons(block).isEmpty()) {
			const auto after = row(object_ptr<Ui::RpWidget>(layout), 10);
			const auto afterLabel = Ui::CreateChild<Ui::FlatLabel>(
				after,
				u"Затем сразу показать:"_q,
				st::botStudioInspectorLabel);
			const auto afterTarget = target(
				after,
				block.value(u"next"_q).toString(),
				true,
				[=](QString chosen) {
					update([&](QJsonObject &b) {
						if (chosen.isEmpty()) {
							b.remove(u"next"_q);
						} else {
							b.insert(u"next"_q, chosen);
						}
					}, true);
				},
				nullptr);
			afterLabel->moveToLeft(0, (afterTarget->height() - afterLabel->height()) / 2);
			afterTarget->moveToLeft(afterLabel->width() + 8, 0);
			after->resize(
				afterLabel->width() + 8 + afterTarget->width(),
				afterTarget->height());
		}

		section(u"Заявки"_q, 18);
		const auto notify = static_cast<Ui::Checkbox*>(row(
			object_ptr<Ui::Checkbox>(
				layout,
				u"Присылать мне все ответы клиента, когда он дошёл до "
					"этого сообщения"_q,
				block.value(u"notifyOwner"_q).toBool(),
				st::botStudioCheckbox,
				st::defaultToggle), 8));
		notify->checkedChanges() | rpl::on_next([=](bool checked) {
			update([&](QJsonObject &b) {
				b.insert(u"notifyOwner"_q, checked);
			}, true);
		}, notify->lifetime());
		hint(u"Чтобы бот мог вам писать, один раз откройте его в "
			"Telegram и нажмите /start."_q);

		const auto actions = row(object_ptr<Ui::RpWidget>(layout), 18);
		auto left = 0;
		auto height = 0;
		if (!isStart) {
			const auto makeStart = MakeButton(
				actions,
				u"★ Сделать стартовым"_q,
				st::botStudioSmallButton);
			makeStart->setClickedCallback([=] {
				_state->flow.insert(u"start"_q, id);
				markChanged(true);
			});
			left += makeStart->width() + 8;
			height = makeStart->height();
		}
		if (FlowBlocks(_state->flow).size() > 1) {
			const auto remove = MakeButton(
				actions,
				u"Удалить"_q,
				st::botStudioDangerButton);
			remove->moveToLeft(left, 0);
			remove->setClickedCallback([=] {
				RemoveFlowBlock(_state->flow, id);
				_state->selected = _state->flow.value(u"start"_q).toString();
				markChanged(true);
			});
			left += remove->width();
			height = std::max(height, remove->height());
		}
		actions->resize(left, height);
	}
	layout->add(object_ptr<Ui::RpWidget>(layout), QMargins(0, 0, 0, 24))
		->resize(1, 24);

	const auto scrollTop = _inspectorScroll->scrollTop();
	_inspectorScroll->setOwnedWidget(std::move(content));
	layout->resizeToWidth(_inspectorScroll->width());
	if (_inspected == id) {
		_inspectorScroll->scrollToY(scrollTop);
	}
	_canvas->update();
}

void FlowEditor::showSettings() {
	const auto state = _state;
	const auto draft = std::make_shared<QJsonObject>(state->flow);
	_layers->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Команды и ответы"_q));
		box->setWidth(st::boxWideWidth + st::boxWideWidth / 3);
		const auto content = box->verticalLayout();
		const auto rebuild = std::make_shared<Fn<void()>>();
		const auto holder = box->addRow(object_ptr<Ui::VerticalLayout>(box));
		const auto chooser = [=](
				not_null<QWidget*> parent,
				const QString &current,
				Fn<void(QString)> chosen) {
			const auto button = MakeButton(
				parent,
				current.isEmpty()
					? u"⚠ выберите сообщение"_q
					: (u"→ "_q + FlowBlockTitle(*draft, current)),
				st::botStudioTargetButton);
			button->setClickedCallback([=] {
				const auto menu = Ui::CreateChild<Ui::PopupMenu>(
					box.get(),
					st::defaultPopupMenu);
				for (const auto &value : FlowBlocks(*draft)) {
					const auto targetId = value.toObject().value(
						u"id"_q).toString();
					menu->addAction(
						u"→ "_q + FlowBlockTitle(*draft, targetId),
						[=] { chosen(targetId); });
				}
				menu->popup(QCursor::pos());
			});
			return button;
		};
		*rebuild = [=] {
			while (holder->count()) {
				delete holder->widgetAt(0);
			}
			const auto addLabel = [&](const QString &text, const style::FlatLabel &st, int top) {
				holder->add(
					object_ptr<Ui::FlatLabel>(holder, text, st),
					QMargins(0, top, 0, 4));
			};
			const auto chips = [&](
					std::vector<std::pair<QString, QString>> options,
					const QString &current,
					Fn<void(QString)> chosen) {
				const auto line = holder->add(
					object_ptr<Ui::RpWidget>(holder),
					QMargins(0, 4, 0, 4));
				auto left = 0;
				auto height = 0;
				for (const auto &[value, title] : options) {
					const auto chip = MakeButton(
						line,
						title,
						(value == current)
							? st::botStudioTemplateButtonActive
							: st::botStudioTemplateButton);
					chip->moveToLeft(left, 0);
					left += chip->width() + 6;
					height = chip->height();
					const auto v = value;
					chip->setClickedCallback([=] {
						chosen(v);
						crl::on_main(box, [=] { (*rebuild)(); });
					});
				}
				line->resize(left, height);
			};

			addLabel(u"Где показывать кнопки"_q, st::botStudioInspectorSection, 4);
			chips({
				{ u"inline"_q, u"Под сообщением"_q },
				{ u"keyboard"_q, u"Вместо клавиатуры"_q },
			}, draft->value(u"buttonsStyle"_q).toString(), [=](QString v) {
				draft->insert(u"buttonsStyle"_q, v);
			});

			addLabel(u"Если бот не понял сообщение"_q, st::botStudioInspectorSection, 14);
			auto fallback = draft->value(u"fallback"_q).toObject();
			const auto mode = fallback.value(u"mode"_q).toString();
			chips({
				{ u"start"_q, u"Показать старт"_q },
				{ u"block"_q, u"Показать сообщение"_q },
				{ u"text"_q, u"Ответить текстом"_q },
				{ u"ai"_q, u"✦ Ответить через ИИ"_q },
			}, mode.isEmpty() ? u"start"_q : mode, [=](QString v) {
				auto f = draft->value(u"fallback"_q).toObject();
				f.insert(u"mode"_q, v);
				draft->insert(u"fallback"_q, f);
			});
			const auto fallbackField = [&](
					const QString &key,
					const QString &placeholder) {
				const auto value = fallback.value(key).toString();
				const auto input = holder->add(
					object_ptr<Ui::InputField>(
						holder,
						st::botStudioFieldMulti,
						Ui::InputField::Mode::MultiLine,
						rpl::single(placeholder),
						value),
					QMargins(0, 0, 0, 4));
				input->changes() | rpl::on_next([=] {
					auto f = draft->value(u"fallback"_q).toObject();
					f.insert(key, input->getLastText());
					draft->insert(u"fallback"_q, f);
				}, input->lifetime());
			};
			if (mode == u"block"_q) {
				const auto line = holder->add(
					object_ptr<Ui::RpWidget>(holder),
					QMargins(0, 4, 0, 4));
				const auto button = chooser(
					line,
					fallback.value(u"goto"_q).toString(),
					[=](QString v) {
						auto f = draft->value(u"fallback"_q).toObject();
						f.insert(u"goto"_q, v);
						draft->insert(u"fallback"_q, f);
						crl::on_main(box, [=] { (*rebuild)(); });
					});
				line->resize(button->width(), button->height());
			} else if (mode == u"text"_q) {
				fallbackField(u"text"_q, u"Текст ответа"_q);
			} else if (mode == u"ai"_q) {
				fallbackField(
					u"prompt"_q,
					u"Кто такой бот и как отвечать (роль для ИИ)"_q);
				addLabel(
					u"Используется ИИ, настроенный в Agentgram."_q,
					st::botStudioInspectorLabel,
					0);
			}

			addLabel(u"Команды меню (/команда)"_q, st::botStudioInspectorSection, 14);
			const auto commands = draft->value(u"commands"_q).toArray();
			for (auto i = 0; i != commands.size(); ++i) {
				const auto command = commands[i].toObject();
				const auto line = holder->add(
					object_ptr<Ui::RpWidget>(holder),
					QMargins(0, 2, 0, 2));
				const auto name = Ui::CreateChild<Ui::InputField>(
					line,
					st::botStudioField,
					rpl::single(u"команда"_q),
					command.value(u"command"_q).toString());
				const auto about = Ui::CreateChild<Ui::InputField>(
					line,
					st::botStudioField,
					rpl::single(u"описание"_q),
					command.value(u"description"_q).toString());
				const auto set = [=](const QString &key, const QString &value) {
					auto list = draft->value(u"commands"_q).toArray();
					if (i < list.size()) {
						auto object = list[i].toObject();
						object.insert(key, value);
						list[i] = object;
						draft->insert(u"commands"_q, list);
					}
				};
				name->changes() | rpl::on_next([=] {
					set(u"command"_q, name->getLastText().trimmed()
						.remove('/').toLower());
				}, name->lifetime());
				about->changes() | rpl::on_next([=] {
					set(u"description"_q, about->getLastText());
				}, about->lifetime());
				const auto to = chooser(
					line,
					command.value(u"goto"_q).toString(),
					[=](QString v) {
						set(u"goto"_q, v);
						crl::on_main(box, [=] { (*rebuild)(); });
					});
				const auto remove = Ui::CreateChild<Ui::IconButton>(
					line,
					st::botStudioRemoveButton);
				remove->setClickedCallback([=] {
					auto list = draft->value(u"commands"_q).toArray();
					if (i < list.size()) {
						list.removeAt(i);
					}
					draft->insert(u"commands"_q, list);
					crl::on_main(box, [=] { (*rebuild)(); });
				});
				line->widthValue() | rpl::on_next([=](int width) {
					const auto w = (width - remove->width() - 16) / 2;
					name->resize(w * 2 / 3, name->height());
					about->resize(w + w / 3, about->height());
					name->moveToLeft(0, 0);
					about->moveToLeft(name->width() + 8, 0);
					remove->moveToLeft(width - remove->width(), 14);
					to->moveToLeft(0, name->height() + 2);
					line->resize(width, name->height() + to->height() + 6);
				}, line->lifetime());
			}
			const auto addCommand = holder->add(
				object_ptr<Ui::LinkButton>(holder, u"＋ Добавить команду"_q),
				QMargins(0, 4, 0, 4));
			addCommand->setClickedCallback([=] {
				auto list = draft->value(u"commands"_q).toArray();
				list.push_back(QJsonObject{
					{ u"command"_q, u"help"_q },
					{ u"description"_q, u"Помощь"_q },
					{ u"goto"_q, draft->value(u"start"_q).toString() },
				});
				draft->insert(u"commands"_q, list);
				crl::on_main(box, [=] { (*rebuild)(); });
			});

			addLabel(u"Ключевые слова"_q, st::botStudioInspectorSection, 14);
			addLabel(
				u"Если сообщение содержит одно из слов (через запятую) — "
					"показать сообщение."_q,
				st::botStudioInspectorLabel,
				0);
			const auto keywords = draft->value(u"keywords"_q).toArray();
			for (auto i = 0; i != keywords.size(); ++i) {
				const auto keyword = keywords[i].toObject();
				const auto line = holder->add(
					object_ptr<Ui::RpWidget>(holder),
					QMargins(0, 2, 0, 2));
				const auto words = Ui::CreateChild<Ui::InputField>(
					line,
					st::botStudioField,
					rpl::single(u"слова через запятую"_q),
					keyword.value(u"words"_q).toString());
				words->changes() | rpl::on_next([=] {
					auto list = draft->value(u"keywords"_q).toArray();
					if (i < list.size()) {
						auto object = list[i].toObject();
						object.insert(u"words"_q, words->getLastText());
						list[i] = object;
						draft->insert(u"keywords"_q, list);
					}
				}, words->lifetime());
				const auto to = chooser(
					line,
					keyword.value(u"goto"_q).toString(),
					[=](QString v) {
						auto list = draft->value(u"keywords"_q).toArray();
						if (i < list.size()) {
							auto object = list[i].toObject();
							object.insert(u"goto"_q, v);
							list[i] = object;
							draft->insert(u"keywords"_q, list);
						}
						crl::on_main(box, [=] { (*rebuild)(); });
					});
				const auto remove = Ui::CreateChild<Ui::IconButton>(
					line,
					st::botStudioRemoveButton);
				remove->setClickedCallback([=] {
					auto list = draft->value(u"keywords"_q).toArray();
					if (i < list.size()) {
						list.removeAt(i);
					}
					draft->insert(u"keywords"_q, list);
					crl::on_main(box, [=] { (*rebuild)(); });
				});
				line->widthValue() | rpl::on_next([=](int width) {
					words->resize(width - remove->width() - 8, words->height());
					words->moveToLeft(0, 0);
					remove->moveToLeft(width - remove->width(), 14);
					to->moveToLeft(0, words->height() + 2);
					line->resize(width, words->height() + to->height() + 6);
				}, line->lifetime());
			}
			const auto addKeyword = holder->add(
				object_ptr<Ui::LinkButton>(holder, u"＋ Добавить слова"_q),
				QMargins(0, 4, 0, 4));
			addKeyword->setClickedCallback([=] {
				auto list = draft->value(u"keywords"_q).toArray();
				list.push_back(QJsonObject{
					{ u"words"_q, QString() },
					{ u"goto"_q, draft->value(u"start"_q).toString() },
				});
				draft->insert(u"keywords"_q, list);
				crl::on_main(box, [=] { (*rebuild)(); });
			});
			holder->resizeToWidth(content->width()
				- st::boxRowPadding.left()
				- st::boxRowPadding.right());
		};
		(*rebuild)();
		box->addButton(rpl::single(u"Готово"_q), [=] {
			state->flow = *draft;
			markChanged(true);
			box->closeBox();
		});
		box->addButton(rpl::single(u"Отмена"_q), [=] { box->closeBox(); });
	}));
}

void FlowEditor::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	p.fillRect(e->rect(), st::windowBg);
}

void FlowEditor::resizeEvent(QResizeEvent *e) {
	updateGeometry();
}

void FlowEditor::updateGeometry() {
	const auto width = this->width();
	auto left = 0;
	_add->moveToLeft(left, 0);
	left += _add->width() + st::botStudioButtonSkip;
	_settings->moveToLeft(left, 0);
	left += _settings->width() + st::botStudioButtonSkip * 2;
	_save->moveToLeft(width - _save->width(), 0);
	const auto statusWidth = std::max(
		width - left - _save->width() - st::botStudioButtonSkip * 2,
		60);
	_status->resizeToWidth(statusWidth);
	_status->moveToLeft(left, (_save->height() - _status->height()) / 2);

	const auto top = _save->height() + st::botStudioToolbarSkip;
	const auto height = std::max(this->height() - top, 100);
	const auto inspectorWidth = std::min(
		st::botStudioInspectorWidth,
		width / 2);
	_canvasScroll->setGeometry(
		0,
		top,
		width - inspectorWidth - st::botStudioButtonSkip,
		height);
	_inspectorScroll->setGeometry(
		width - inspectorWidth,
		top,
		inspectorWidth,
		height);
	if (_inspector) {
		_inspector->resizeToWidth(inspectorWidth);
	}
	_canvas->refresh();
}

} // namespace BotStudio
