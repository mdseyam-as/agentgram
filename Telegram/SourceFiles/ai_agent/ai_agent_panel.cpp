/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ai_agent/ai_agent_panel.h"

#include "ai_agent/ai_agent_manager.h"
#include "ai_agent/ai_agent_markdown.h"
#include "ai_agent/ai_agent_settings_box.h"
#include "main/main_session.h"
#include "ui/effects/animations.h"
#include "ui/painter.h"
#include "ui/abstract_button.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/shadow.h"
#include "ui/widgets/rp_window.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/ui_utility.h"
#include "window/main_window.h"
#include "window/themes/window_theme.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_ai_agent.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"
#include "styles/style_window.h"

#include <QtGui/QGuiApplication>

namespace AiAgent {
namespace {

constexpr auto kSpinnerPeriod = crl::time(900);

struct Suggestion {
	QString label;
	QString prompt;
	bool sendImmediately = true;
};

[[nodiscard]] std::vector<Suggestion> Suggestions() {
	return {
		{
			u"Сводка непрочитанного"_q,
			u"Сделай краткую сводку непрочитанных сообщений по всем "
				"чатам: кто писал и что важно."_q,
		},
		{
			u"О чём этот чат?"_q,
			u"Кратко перескажи последние сообщения в открытом сейчас чате."_q,
		},
		{
			u"Подготовь ответ"_q,
			u"Прочитай последние сообщения в открытом чате и подготовь "
				"черновик ответа в поле ввода, не отправляй."_q,
		},
		{
			u"Найти сообщение"_q,
			u"Найди сообщения про "_q,
			false,
		},
		{
			u"Мои группы"_q,
			u"Покажи мои группы и каналы, где больше всего непрочитанных."_q,
		},
	};
}

class SendButton final : public Ui::AbstractButton {
public:
	explicit SendButton(QWidget *parent) : AbstractButton(parent) {
		resize(st::aiAgentSendSize, st::aiAgentSendSize);
		setCursor(style::cur_pointer);
	}

	void setState(bool busy, bool enabled) {
		if (_busy != busy || _enabled != enabled) {
			_busy = busy;
			_enabled = enabled;
			update();
		}
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		const auto active = _busy || _enabled;
		auto color = active
			? st::aiAgentAccent->c
			: st::aiAgentSendDisabledBg->c;
		if (!active) {
			color.setAlphaF(0.45);
		} else if (isOver()) {
			color = color.lighter(108);
		}
		p.setBrush(color);
		p.drawEllipse(rect());
		const auto &icon = _busy ? st::aiAgentStopIcon : st::aiAgentSendIcon;
		icon.paintInCenter(p, rect());
	}

	void onStateChanged(State was, StateChangeSource source) override {
		update();
	}

private:
	bool _busy = false;
	bool _enabled = false;

};

class EmptyState final : public Ui::RpWidget {
public:
	EmptyState(QWidget *parent, Fn<void(Suggestion)> chosen)
	: RpWidget(parent)
	, _title(this, u"Чем помочь?"_q, st::aiAgentEmptyTitle)
	, _text(
		this,
		u"Я вижу ваши чаты и могу читать, искать, пересказывать "
			"и отвечать в них. Действия, которые что-то меняют, "
			"я сначала покажу вам на подтверждение."_q,
		st::aiAgentEmptyText) {
		for (const auto &suggestion : Suggestions()) {
			const auto button = Ui::CreateChild<Ui::RoundButton>(
				this,
				rpl::single(suggestion.label),
				st::aiAgentSuggestion);
			button->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
			button->setClickedCallback([=] { chosen(suggestion); });
			_buttons.push_back(button);
		}
	}

protected:
	int resizeGetHeight(int newWidth) override {
		auto top = st::aiAgentEmptyTop;
		_iconTop = top;
		top += st::aiAgentEmptyIconSize + st::aiAgentEmptyTitleSkip;
		_title->resizeToWidth(newWidth);
		_title->moveToLeft(0, top, newWidth);
		top += _title->height() + st::aiAgentEmptyTextSkip;
		_text->resizeToWidth(newWidth);
		_text->moveToLeft(0, top, newWidth);
		top += _text->height() + st::aiAgentSuggestionsSkip;

		const auto skip = st::aiAgentSuggestionSkip;
		auto rows = std::vector<std::vector<Ui::RoundButton*>>(1);
		auto rowWidth = 0;
		for (const auto button : _buttons) {
			const auto width = button->width();
			if (!rows.back().empty() && rowWidth + skip + width > newWidth) {
				rows.emplace_back();
				rowWidth = 0;
			}
			rowWidth += (rows.back().empty() ? 0 : skip) + width;
			rows.back().push_back(button);
		}
		for (const auto &row : rows) {
			auto full = 0;
			for (const auto button : row) {
				full += (full ? skip : 0) + button->width();
			}
			auto left = std::max((newWidth - full) / 2, 0);
			for (const auto button : row) {
				button->moveToLeft(left, top, newWidth);
				left += button->width() + skip;
			}
			if (!row.empty()) {
				top += row.front()->height() + skip;
			}
		}
		return top;
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto size = st::aiAgentEmptyIconSize;
		const auto rect = QRect((width() - size) / 2, _iconTop, size, size);
		auto gradient = QLinearGradient(rect.topLeft(), rect.bottomRight());
		gradient.setColorAt(0., st::aiAgentAccent->c.lighter(125));
		gradient.setColorAt(1., st::aiAgentAccent->c);
		p.setPen(Qt::NoPen);
		p.setBrush(gradient);
		p.drawEllipse(rect);
		st::aiAgentEmptyIcon.paintInCenter(p, rect);
	}

private:
	object_ptr<Ui::FlatLabel> _title;
	object_ptr<Ui::FlatLabel> _text;
	std::vector<Ui::RoundButton*> _buttons;
	int _iconTop = 0;

};

} // namespace

class EntryWidget final : public Ui::RpWidget {
public:
	EntryWidget(QWidget *parent, not_null<Manager*> manager, uint64 id);

	void refresh();

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;

private:
	void setupTool();
	void paintToolIcon(QPainter &p, QRect rect);

	const not_null<Manager*> _manager;
	const uint64 _id;
	Entry::Type _type = Entry::Type::User;
	Entry::ToolState _state = Entry::ToolState::Running;
	Ui::FlatLabel *_text = nullptr;
	Ui::FlatLabel *_details = nullptr;
	Ui::RoundButton *_approve = nullptr;
	Ui::RoundButton *_reject = nullptr;
	Ui::LinkButton *_always = nullptr;
	Ui::Animations::Basic _spinner;
	QRect _bubble;
	QRect _icon;
	QString _shownText;
	QString _shownTitle;
	QString _shownDetails;

};

EntryWidget::EntryWidget(
	QWidget *parent,
	not_null<Manager*> manager,
	uint64 id)
: RpWidget(parent)
, _manager(manager)
, _id(id)
, _spinner([=] { update(_icon); }) {
	const auto entry = _manager->entry(_id);
	Assert(entry != nullptr);
	_type = entry->type;
	switch (_type) {
	case Entry::Type::User:
		_text = Ui::CreateChild<Ui::FlatLabel>(this, st::aiAgentUserText);
		break;
	case Entry::Type::Assistant:
		_text = Ui::CreateChild<Ui::FlatLabel>(this, st::aiAgentMessageText);
		break;
	case Entry::Type::Error:
		_text = Ui::CreateChild<Ui::FlatLabel>(this, st::aiAgentErrorText);
		break;
	case Entry::Type::Tool:
		setupTool();
		break;
	}
	if (_type != Entry::Type::Tool) {
		_text->setSelectable(true);
		_text->setDoubleClickSelectsParagraph(true);
	}
	refresh();
}

void EntryWidget::setupTool() {
	_text = Ui::CreateChild<Ui::FlatLabel>(this, st::aiAgentToolTitle);
	_details = Ui::CreateChild<Ui::FlatLabel>(this, st::aiAgentToolDetails);
	_details->setSelectable(true);
	_approve = Ui::CreateChild<Ui::RoundButton>(
		this,
		rpl::single(u"Разрешить"_q),
		st::aiAgentApproveButton);
	_approve->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
	_reject = Ui::CreateChild<Ui::RoundButton>(
		this,
		rpl::single(u"Отклонить"_q),
		st::aiAgentRejectButton);
	_reject->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
	_always = Ui::CreateChild<Ui::LinkButton>(
		this,
		u"Разрешать всё без вопросов"_q,
		st::aiAgentAlwaysAllow);
	_approve->setClickedCallback([=] {
		_manager->resolveConfirmation(_id, true);
	});
	_reject->setClickedCallback([=] {
		_manager->resolveConfirmation(_id, false);
	});
	_always->setClickedCallback([=] {
		_manager->setAutoApprove(true);
	});
}

void EntryWidget::refresh() {
	const auto entry = _manager->entry(_id);
	if (!entry) {
		return;
	}
	auto relayout = false;
	if (_type == Entry::Type::Tool) {
		const auto confirm = (entry->toolState
			== Entry::ToolState::AwaitingConfirmation);
		if (_state != entry->toolState || _shownTitle.isEmpty()) {
			_state = entry->toolState;
			relayout = true;
		}
		if (_shownTitle != entry->title) {
			_shownTitle = entry->title;
			_text->setText(entry->title);
			relayout = true;
		}
		if (_shownDetails != entry->details) {
			_shownDetails = entry->details;
			_details->setText(entry->details);
			relayout = true;
		}
		_details->setVisible(!entry->details.isEmpty());
		_approve->setVisible(confirm);
		_reject->setVisible(confirm);
		_always->setVisible(confirm);
		if (_state == Entry::ToolState::Running) {
			if (!_spinner.animating()) {
				_spinner.start();
			}
		} else {
			_spinner.stop();
		}
	} else if (_shownText != entry->text) {
		_shownText = entry->text;
		if (_type == Entry::Type::Assistant) {
			_text->setMarkedText(MarkdownToEntities(entry->text));
		} else {
			_text->setText(entry->text);
		}
		relayout = true;
	}
	if (relayout && width() > 0) {
		resizeToWidth(width());
	}
	update();
}

int EntryWidget::resizeGetHeight(int newWidth) {
	if (_type == Entry::Type::Tool) {
		const auto &padding = st::aiAgentToolPadding;
		const auto iconSize = st::aiAgentToolIconSize;
		const auto left = padding.left() + iconSize + st::aiAgentToolIconSkip;
		const auto available = std::max(
			newWidth - left - padding.right(),
			st::aiAgentToolIconSize);
		const auto confirm = (_state
			== Entry::ToolState::AwaitingConfirmation);
		_text->setTextColorOverride(confirm
			? std::make_optional(st::windowBoldFg->c)
			: std::nullopt);
		_text->resizeToWidth(available);
		auto top = padding.top();
		_icon = QRect(
			padding.left(),
			top + std::max((_text->height() - iconSize) / 2, 0),
			iconSize,
			iconSize);
		_text->moveToLeft(left, top, newWidth);
		top += _text->height();
		if (!_details->isHidden()) {
			_details->resizeToWidth(available);
			top += st::aiAgentToolDetailsSkip;
			_details->moveToLeft(left, top, newWidth);
			top += _details->height();
		}
		if (confirm) {
			top += st::aiAgentToolButtonsSkip;
			_approve->moveToLeft(left, top, newWidth);
			_reject->moveToLeft(
				left + _approve->width() + st::aiAgentButtonSkip,
				top,
				newWidth);
			top += _approve->height() + st::aiAgentButtonSkip;
			_always->moveToLeft(left, top, newWidth);
			top += _always->height();
		}
		top += padding.bottom();
		_bubble = QRect(0, 0, newWidth, top);
		return top;
	}
	const auto &padding = st::aiAgentBubblePadding;
	const auto maxBubble = (_type == Entry::Type::User)
		? (newWidth * st::aiAgentUserMaxWidthPercent / 100)
		: newWidth;
	const auto textMax = std::max(
		maxBubble - padding.left() - padding.right(),
		st::aiAgentToolIconSize);
	_text->resizeToNaturalWidth(textMax);
	const auto bubbleWidth = (_type == Entry::Type::User)
		? (_text->width() + padding.left() + padding.right())
		: newWidth;
	const auto bubbleHeight = _text->height()
		+ padding.top()
		+ padding.bottom();
	const auto bubbleLeft = (_type == Entry::Type::User)
		? (newWidth - bubbleWidth)
		: 0;
	_bubble = QRect(bubbleLeft, 0, bubbleWidth, bubbleHeight);
	_text->moveToLeft(
		bubbleLeft + padding.left(),
		padding.top(),
		newWidth);
	return bubbleHeight;
}

void EntryWidget::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	if (_type == Entry::Type::Tool) {
		const auto confirm = (_state
			== Entry::ToolState::AwaitingConfirmation);
		const auto border = st::aiAgentToolBorder;
		auto pen = QPen(confirm
			? st::aiAgentToolConfirmBorderFg->c
			: st::aiAgentToolBorderFg->c);
		pen.setWidth(confirm ? border * 2 : border);
		p.setPen(pen);
		p.setBrush(confirm ? st::aiAgentAssistantBg->c : Qt::transparent);
		const auto inner = _bubble.marginsRemoved(
			QMargins(border, border, border, border));
		p.drawRoundedRect(
			inner,
			st::aiAgentToolRadius,
			st::aiAgentToolRadius);
		paintToolIcon(p, _icon);
		return;
	}
	const auto color = [&] {
		switch (_type) {
		case Entry::Type::User: return st::aiAgentUserBubbleBg->c;
		case Entry::Type::Error: return st::aiAgentErrorBg->c;
		default: return st::aiAgentAssistantBg->c;
		}
	}();
	p.setPen(Qt::NoPen);
	p.setBrush(color);
	p.drawRoundedRect(
		_bubble,
		st::aiAgentBubbleRadius,
		st::aiAgentBubbleRadius);
}

void EntryWidget::paintToolIcon(QPainter &p, QRect rect) {
	const auto line = std::max(rect.width() / 9, 1);
	const auto inner = rect.marginsRemoved({ line, line, line, line });
	switch (_state) {
	case Entry::ToolState::Running: {
		auto pen = QPen(st::aiAgentAccent->c);
		pen.setWidth(line);
		pen.setCapStyle(Qt::RoundCap);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		const auto progress = (crl::now() % kSpinnerPeriod)
			/ float64(kSpinnerPeriod);
		p.drawArc(inner, int(-progress * 360 * 16), 270 * 16);
	} break;
	case Entry::ToolState::Done: {
		p.setPen(Qt::NoPen);
		p.setBrush(st::aiAgentToolDoneFg->c);
		p.drawEllipse(rect);
		auto pen = QPen(st::windowBg->c);
		pen.setWidth(line);
		pen.setCapStyle(Qt::RoundCap);
		pen.setJoinStyle(Qt::RoundJoin);
		p.setPen(pen);
		const auto w = float64(rect.width());
		const auto x = float64(rect.x());
		const auto y = float64(rect.y());
		const QPointF points[] = {
			{ x + w * 0.28, y + w * 0.52 },
			{ x + w * 0.44, y + w * 0.67 },
			{ x + w * 0.72, y + w * 0.36 },
		};
		p.drawPolyline(points, 3);
	} break;
	case Entry::ToolState::Failed:
	case Entry::ToolState::Rejected: {
		p.setPen(Qt::NoPen);
		p.setBrush((_state == Entry::ToolState::Failed)
			? st::aiAgentToolFailFg->c
			: st::windowSubTextFg->c);
		p.drawEllipse(rect);
		auto pen = QPen(st::windowBg->c);
		pen.setWidth(line);
		pen.setCapStyle(Qt::RoundCap);
		p.setPen(pen);
		const auto shift = rect.width() * 0.3;
		const auto center = QPointF(rect.center()) + QPointF(0.5, 0.5);
		p.drawLine(
			center + QPointF(-shift, -shift),
			center + QPointF(shift, shift));
		p.drawLine(
			center + QPointF(shift, -shift),
			center + QPointF(-shift, shift));
	} break;
	case Entry::ToolState::AwaitingConfirmation: {
		p.setPen(Qt::NoPen);
		p.setBrush(st::aiAgentAccent->c);
		p.drawEllipse(rect);
		const auto dot = std::max(rect.width() / 7, 1);
		p.setBrush(st::aiAgentAccentFg->c);
		const auto center = QPointF(rect.center()) + QPointF(0.5, 0.5);
		const auto top = rect.height() * 0.22;
		p.drawRoundedRect(
			QRectF(center.x() - dot / 2., rect.y() + top, dot, rect.height() * 0.36),
			dot / 2.,
			dot / 2.);
		p.drawEllipse(QPointF(center.x(), rect.y() + rect.height() * 0.74), dot * 0.65, dot * 0.65);
	} break;
	}
}

Panel::Panel(
	QWidget *parent,
	not_null<Window::SessionController*> controller,
	bool separateWindow)
: RpWidget(parent)
, _controller(controller)
, _manager(Manager::Get(&controller->session()))
, _separateWindow(separateWindow)
, _header(this)
, _headerShadow(this)
, _leftShadow(this)
, _scroll(this)
, _composer(this)
, _composerShadow(this) {
	resize(st::aiAgentPanelWidth, height() ? height() : st::windowMinHeight);
	setupHeader();
	setupList();
	setupComposer();
	_leftShadow->setVisible(!_separateWindow);
	_manager->busyValue(
	) | rpl::on_next([=] {
		updateSendButton();
	}, lifetime());
	_manager->refreshModelsIfStale(_manager->config().provider);
	_manager->configChanges(
	) | rpl::on_next([=] {
		_manager->refreshModelsIfStale(_manager->config().provider);
	}, lifetime());
	Window::Theme::IsNightModeValue(
	) | rpl::on_next([=](bool night) {
		_theme->setIconOverride(
			night ? &st::aiAgentThemeDay.icon : nullptr,
			night ? &st::aiAgentThemeDay.iconOver : nullptr);
	}, lifetime());
}

Panel::~Panel() = default;

void Panel::setupHeader() {
	const auto header = _header.data();
	header->resize(width(), st::aiAgentHeaderHeight);
	_title = Ui::CreateChild<Ui::FlatLabel>(
		header,
		u"ИИ-агент"_q,
		st::aiAgentTitle);
	_subtitle = Ui::CreateChild<Ui::FlatLabel>(
		header,
		rpl::single(rpl::empty) | rpl::then(
			_manager->configChanges()
		) | rpl::map([=] {
			return _manager->statusText();
		}),
		st::aiAgentSubtitle);
	_title->setAttribute(Qt::WA_TransparentForMouseEvents);
	_subtitle->setAttribute(Qt::WA_TransparentForMouseEvents);
	const auto titleButton = Ui::CreateChild<Ui::AbstractButton>(header);
	titleButton->setClickedCallback([=] { showModelMenu(); });
	titleButton->setCursor(style::cur_pointer);

	_close = Ui::CreateChild<Ui::IconButton>(header, st::aiAgentClose);
	_popout = Ui::CreateChild<Ui::IconButton>(header, st::aiAgentPopout);
	_theme = Ui::CreateChild<Ui::IconButton>(header, st::aiAgentThemeNight);
	_settings = Ui::CreateChild<Ui::IconButton>(header, st::aiAgentSettings);
	_newChat = Ui::CreateChild<Ui::IconButton>(header, st::aiAgentNewChat);
	_close->setVisible(!_separateWindow);
	_popout->setVisible(!_separateWindow);

	_close->setClickedCallback([=] {
		if (_closeCallback) {
			_closeCallback();
		}
	});
	_popout->setClickedCallback([=] {
		if (_popoutCallback) {
			_popoutCallback();
		}
	});
	_settings->setClickedCallback([=] { showSettings(); });
	_newChat->setClickedCallback([=] {
		_manager->newConversation();
		focusInput();
	});
	_theme->setClickedCallback([=] {
		const auto toggle = [] {
			Window::Theme::ToggleNightMode();
			Window::Theme::KeepApplied();
		};
		Window::Theme::ToggleNightModeWithConfirmation(
			&_controller->window(),
			toggle);
	});
	_close->setAccessibleName(u"Закрыть"_q);
	_popout->setAccessibleName(u"Открыть в отдельном окне"_q);
	_theme->setAccessibleName(u"Тёмная / светлая тема"_q);
	_settings->setAccessibleName(u"Настройки ИИ"_q);
	_newChat->setAccessibleName(u"Новый диалог"_q);

	header->widthValue(
	) | rpl::on_next([=](int width) {
		const auto &padding = st::aiAgentHeaderPadding;
		const auto height = st::aiAgentHeaderHeight;
		auto right = width - padding.right();
		const auto place = [&](Ui::IconButton *button) {
			if (button->isHidden()) {
				return;
			}
			right -= button->width();
			button->moveToLeft(right, (height - button->height()) / 2);
		};
		place(_close);
		place(_popout);
		place(_settings);
		place(_theme);
		place(_newChat);
		const auto left = padding.left()
			+ st::aiAgentAvatarSize
			+ st::aiAgentTitleLeft;
		const auto available = std::max(right - left, 0);
		_title->resizeToNaturalWidth(available);
		_subtitle->resizeToNaturalWidth(available);
		_title->moveToLeft(left, st::aiAgentTitleTop);
		_subtitle->moveToLeft(left, st::aiAgentSubtitleTop);
		titleButton->setGeometry(
			padding.left(),
			0,
			std::max(right - padding.left(), 0),
			height);
	}, header->lifetime());

	header->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(header);
		auto hq = PainterHighQualityEnabler(p);
		const auto size = st::aiAgentAvatarSize;
		const auto rect = QRect(
			st::aiAgentHeaderPadding.left(),
			(st::aiAgentHeaderHeight - size) / 2,
			size,
			size);
		auto gradient = QLinearGradient(rect.topLeft(), rect.bottomRight());
		gradient.setColorAt(0., st::aiAgentAccent->c.lighter(125));
		gradient.setColorAt(1., st::aiAgentAccent->c);
		p.setPen(Qt::NoPen);
		p.setBrush(gradient);
		p.drawEllipse(rect);
		st::aiAgentAvatarIcon.paintInCenter(p, rect);
	}, header->lifetime());
}

void Panel::setupList() {
	_list = _scroll->setOwnedWidget(object_ptr<Ui::VerticalLayout>(_scroll.data()));
	Ui::SetStickyBottomScroll(_scroll.data(), _list.data());
	rebuildEntries();

	_manager->entryAdded(
	) | rpl::on_next([=](uint64 id) {
		addEntryWidget(id);
		refreshEmptyState();
		scrollToBottom();
	}, lifetime());
	_manager->entryUpdated(
	) | rpl::on_next([=](uint64 id) {
		updateEntryWidget(id);
	}, lifetime());
	_manager->entriesReset(
	) | rpl::on_next([=] {
		rebuildEntries();
	}, lifetime());
}

void Panel::rebuildEntries() {
	_entryWidgets.clear();
	_empty = nullptr;
	_list->clear();
	for (const auto &entry : _manager->entries()) {
		addEntryWidget(entry.id);
	}
	refreshEmptyState();
	_list->resizeToWidth(_scroll->width());
	scrollToBottom();
}

void Panel::refreshEmptyState() {
	const auto empty = _manager->entries().empty();
	if (empty && !_empty) {
		_empty = _list->insert(0, object_ptr<EmptyState>(
			_list.data(),
			[=](Suggestion suggestion) {
				if (suggestion.sendImmediately) {
					sendPrompt(suggestion.prompt);
				} else {
					_field->setText(suggestion.prompt);
					_field->setFocusFast();
					_field->setCursorPosition(suggestion.prompt.size());
				}
			}),
			st::aiAgentListPadding);
		_list->resizeToWidth(_scroll->width());
	} else if (!empty && _empty) {
		delete base::take(_empty);
	}
}

void Panel::addEntryWidget(uint64 id) {
	if (_entryWidgets.contains(id) || !_manager->entry(id)) {
		return;
	}
	const auto &padding = st::aiAgentListPadding;
	const auto first = _entryWidgets.empty();
	const auto widget = _list->add(
		object_ptr<EntryWidget>(_list.data(), _manager, id),
		QMargins(
			padding.left(),
			first ? padding.top() : st::aiAgentEntrySkip,
			padding.right(),
			0));
	_entryWidgets.emplace(id, widget);
	widget->show();
	_list->resizeToWidth(_scroll->width());
}

void Panel::updateEntryWidget(uint64 id) {
	const auto i = _entryWidgets.find(id);
	if (i != end(_entryWidgets)) {
		i->second->refresh();
	}
}

void Panel::scrollToBottom() {
	crl::on_main(this, [=] {
		_scroll->scrollToY(_scroll->scrollTopMax());
	});
}

void Panel::setupComposer() {
	const auto composer = _composer.data();
	_field = Ui::CreateChild<Ui::InputField>(
		composer,
		st::aiAgentField,
		Ui::InputField::Mode::MultiLine,
		rpl::single(u"Спросите или поручите что-нибудь…"_q));
	_field->setSubmitSettings(Ui::InputField::SubmitSettings::Enter);
	const auto send = Ui::CreateChild<SendButton>(composer);
	_send = send;
	_hint = Ui::CreateChild<Ui::FlatLabel>(
		composer,
		u"Enter — отправить, Shift+Enter — новая строка"_q,
		st::aiAgentHint);

	_field->submits(
	) | rpl::on_next([=] {
		submit();
	}, _field->lifetime());
	_field->changes(
	) | rpl::on_next([=] {
		updateSendButton();
	}, _field->lifetime());
	_field->heightChanges(
	) | rpl::on_next([=] {
		updateControlsGeometry();
	}, _field->lifetime());
	_send->setClickedCallback([=] {
		if (_manager->busy()) {
			_manager->stop();
		} else {
			submit();
		}
	});

	composer->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(composer);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::aiAgentComposerBg);
		const auto &padding = st::aiAgentComposerPadding;
		const auto rect = QRect(
			padding.left(),
			padding.top(),
			composer->width() - padding.left() - padding.right(),
			_field->height());
		p.drawRoundedRect(
			rect,
			st::aiAgentComposerRadius,
			st::aiAgentComposerRadius);
	}, composer->lifetime());
	updateSendButton();
}

void Panel::updateSendButton() {
	if (!_send) {
		return;
	}
	const auto has = !_field->getLastText().trimmed().isEmpty();
	static_cast<SendButton*>(_send)->setState(_manager->busy(), has);
}

void Panel::submit() {
	if (_manager->busy()) {
		return;
	}
	const auto text = _field->getLastText().trimmed();
	if (text.isEmpty()) {
		return;
	}
	_field->clear();
	_manager->send(text, _controller);
	_stickToBottom = true;
	scrollToBottom();
}

void Panel::sendPrompt(const QString &text) {
	if (_manager->busy() || text.trimmed().isEmpty()) {
		return;
	}
	_manager->send(text, _controller);
	scrollToBottom();
}

void Panel::showModelMenu() {
	const auto &config = _manager->config();
	const auto provider = config.provider;
	const auto current = config.model(provider);
	_modelMenu = base::make_unique_q<Ui::PopupMenu>(
		this,
		st::defaultPopupMenu);
	const auto menu = _modelMenu.get();
	constexpr auto kMaxMenuModels = 30;
	auto models = config.modelChoices(provider);
	if (models.size() > kMaxMenuModels) {
		models = models.mid(0, kMaxMenuModels);
		if (!models.contains(current)) {
			models.push_front(current);
		}
	}
	for (const auto &model : models) {
		const auto text = (model == current)
			? (QString(QChar(0x2713)) + u"  "_q + model)
			: model;
		menu->addAction(text, [=] {
			_manager->updateConfig([&](Config &config) {
				config.settings(provider).model = model;
			});
		});
	}
	menu->addSeparator();
	menu->addAction(u"Обновить список моделей"_q, [=] {
		_manager->fetchModels(provider, [](QStringList, QString) {});
	});
	menu->addAction(u"Настройки ИИ…"_q, [=] { showSettings(); });
	menu->popup(QCursor::pos());
}

void Panel::showSettings() {
	_controller->show(Box(SettingsBox, _manager));
}

void Panel::setCloseCallback(Fn<void()> callback) {
	_closeCallback = std::move(callback);
}

void Panel::setPopoutCallback(Fn<void()> callback) {
	_popoutCallback = std::move(callback);
}

void Panel::focusInput() {
	if (_field) {
		_field->setFocusFast();
	}
}

void Panel::focusInEvent(QFocusEvent *e) {
	focusInput();
}

void Panel::resizeEvent(QResizeEvent *e) {
	updateControlsGeometry();
}

void Panel::updateControlsGeometry() {
	const auto shadow = _separateWindow ? 0 : st::lineWidth;
	const auto inner = width() - shadow;
	_leftShadow->setGeometry(0, 0, shadow, height());
	_header->setGeometry(shadow, 0, inner, st::aiAgentHeaderHeight);
	_headerShadow->setGeometry(
		shadow,
		st::aiAgentHeaderHeight,
		inner,
		st::lineWidth);

	const auto &padding = st::aiAgentComposerPadding;
	const auto sendSkip = st::aiAgentSendSkip;
	const auto fieldWidth = inner
		- padding.left()
		- padding.right()
		- st::aiAgentSendSize
		- sendSkip * 2;
	_field->resizeToWidth(std::max(fieldWidth, st::aiAgentSendSize));
	const auto fieldHeight = _field->height();
	_hint->resizeToNaturalWidth(inner - padding.left() - padding.right());
	const auto composerHeight = padding.top()
		+ fieldHeight
		+ st::aiAgentHintTop
		+ _hint->height()
		+ padding.bottom();
	_composer->setGeometry(
		shadow,
		height() - composerHeight,
		inner,
		composerHeight);
	_field->moveToLeft(padding.left(), padding.top());
	_send->moveToLeft(
		inner - padding.right() - sendSkip - st::aiAgentSendSize,
		padding.top() + fieldHeight - sendSkip - st::aiAgentSendSize);
	_hint->moveToLeft(
		padding.left() + st::aiAgentField.textMargins.left(),
		padding.top() + fieldHeight + st::aiAgentHintTop);
	_composerShadow->setGeometry(
		shadow,
		_composer->y() - st::lineWidth,
		inner,
		st::lineWidth);

	const auto scrollTop = st::aiAgentHeaderHeight + st::lineWidth;
	const auto wasAtBottom = (_scroll->scrollTop() >= _scroll->scrollTopMax());
	_scroll->setGeometry(
		shadow,
		scrollTop,
		inner,
		std::max(_composer->y() - st::lineWidth - scrollTop, 0));
	_list->resizeToWidth(inner);
	if (wasAtBottom) {
		_scroll->scrollToY(_scroll->scrollTopMax());
	}
}

void Panel::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	p.fillRect(e->rect(), st::aiAgentPanelBg);
}

namespace {

struct ControllerState {
	QPointer<Panel> docked;
	std::unique_ptr<Ui::RpWindow> window;
	QPointer<Panel> windowPanel;
	rpl::variable<bool> shown = false;
};

[[nodiscard]] not_null<ControllerState*> StateFor(
		not_null<Window::SessionController*> controller) {
	static auto states = base::flat_map<
		not_null<Window::SessionController*>,
		std::unique_ptr<ControllerState>>();
	const auto i = states.find(controller);
	if (i != end(states)) {
		return i->second.get();
	}
	const auto result = states.emplace(
		controller,
		std::make_unique<ControllerState>()).first->second.get();
	controller->lifetime().add([=] {
		states.remove(controller);
	});
	return result;
}

void UpdateShown(not_null<ControllerState*> state) {
	state->shown = (state->docked != nullptr)
		|| (state->window != nullptr);
}

void CloseDocked(not_null<Window::SessionController*> controller) {
	const auto state = StateFor(controller);
	if (state->docked) {
		controller->window().showRightColumn(nullptr);
		state->docked = nullptr;
		UpdateShown(state);
	}
}

void ShowSeparate(
		not_null<Window::SessionController*> controller,
		const QString &prompt) {
	const auto state = StateFor(controller);
	CloseDocked(controller);
	if (!state->window) {
		state->window = std::make_unique<Ui::RpWindow>();
		const auto window = state->window.get();
		window->setWindowTitle(u"Agentgram — ИИ-агент"_q);
		window->setMinimumSize(QSize(
			st::aiAgentPanelWidth - st::aiAgentPanelWidth / 4,
			st::windowMinHeight));
		const auto panel = Ui::CreateChild<Panel>(
			window->body().get(),
			controller,
			true);
		state->windowPanel = panel;
		window->body()->sizeValue(
		) | rpl::on_next([=](QSize size) {
			panel->setGeometry(QRect(QPoint(), size));
		}, panel->lifetime());
		window->setAttribute(Qt::WA_DeleteOnClose, false);
		window->events(
		) | rpl::filter([](not_null<QEvent*> e) {
			return (e->type() == QEvent::Close);
		}) | rpl::on_next([=] {
			crl::on_main(controller, [=] {
				state->window = nullptr;
				UpdateShown(state);
			});
		}, window->lifetime());
		const auto main = controller->widget()->geometry();
		window->setGeometry(QRect(
			main.x() + main.width() + st::lineWidth,
			main.y(),
			st::aiAgentPanelWidth + st::aiAgentPanelWidth / 4,
			main.height()));
		window->show();
	}
	state->window->raise();
	state->window->activateWindow();
	if (const auto panel = state->windowPanel.data()) {
		panel->focusInput();
		if (!prompt.isEmpty()) {
			panel->sendPrompt(prompt);
		}
	}
	UpdateShown(state);
}

} // namespace

void ShowPanel(
		not_null<Window::SessionController*> controller,
		const QString &prompt) {
	const auto state = StateFor(controller);
	if (state->window) {
		ShowSeparate(controller, prompt);
		return;
	}
	if (!state->docked) {
		auto panel = object_ptr<Panel>(
			controller->widget().get(),
			controller,
			false);
		const auto raw = panel.data();
		raw->setCloseCallback([=] {
			CloseDocked(controller);
		});
		raw->setPopoutCallback([=] {
			ShowSeparate(controller, QString());
		});
		raw->lifetime().add([=] {
			if (state->docked.data() == raw) {
				state->docked = nullptr;
				crl::on_main(controller, [=] {
					UpdateShown(state);
				});
			}
		});
		state->docked = raw;
		controller->window().showRightColumn(std::move(panel));
	}
	UpdateShown(state);
	if (const auto panel = state->docked.data()) {
		panel->focusInput();
		if (!prompt.isEmpty()) {
			panel->sendPrompt(prompt);
		}
	}
}

void TogglePanel(
		not_null<Window::SessionController*> controller,
		const QString &prompt) {
	const auto state = StateFor(controller);
	if (state->docked && prompt.isEmpty()) {
		CloseDocked(controller);
	} else if (state->window && prompt.isEmpty()) {
		state->window->raise();
		state->window->activateWindow();
	} else {
		ShowPanel(controller, prompt);
	}
}

bool PanelShown(not_null<Window::SessionController*> controller) {
	return StateFor(controller)->shown.current();
}

rpl::producer<bool> PanelShownValue(
		not_null<Window::SessionController*> controller) {
	return StateFor(controller)->shown.value();
}

} // namespace AiAgent
