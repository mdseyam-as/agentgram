/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "bot_studio/bot_studio_window.h"

#include "ai_agent/ai_agent_markdown.h"
#include "ai_agent/ai_agent_panel.h"
#include "base/event_filter.h"
#include "bot_studio/bot_studio_flow.h"
#include "bot_studio/bot_studio_flow_editor.h"
#include "bot_studio/bot_studio_manager.h"
#include "main/main_session.h"
#include "ui/abstract_button.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/layer_manager.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/discrete_sliders.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/fields/password_input.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/rp_window.h"
#include "ui/widgets/scroll_area.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/ui_utility.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "window/window_session_controller_link_info.h"
#include "styles/style_ai_agent.h"
#include "styles/style_bot_studio.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"
#include "styles/style_window.h"

#include <QtCore/QJsonDocument>
#include <QtGui/QKeyEvent>

#include <array>
#include <QtCore/QRegularExpression>
#include <QtGui/QSyntaxHighlighter>
#include <QtGui/QTextCharFormat>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QTableWidget>

namespace BotStudio {
namespace {

[[nodiscard]] QFont CodeFont() {
	auto result = QFont(u"monospace"_q);
	result.setStyleHint(QFont::Monospace);
	result.setFixedPitch(true);
	result.setPixelSize(style::ConvertScale(st::botStudioCodeFontSize));
	return result;
}

[[nodiscard]] QString ColorName(const style::color &color) {
	return color->c.name();
}

[[nodiscard]] QColor StatusColor(BotStatus status) {
	switch (status) {
	case BotStatus::Running: return QColor(0x4f, 0xae, 0x4e);
	case BotStatus::Error: return st::attentionButtonFg->c;
	case BotStatus::Starting: return st::windowActiveTextFg->c;
	case BotStatus::Stopped: return st::botStudioStoppedFg->c;
	}
	return st::botStudioStoppedFg->c;
}

[[nodiscard]] QString BotInitial(const BotInfo &bot) {
	const auto name = bot.name.trimmed().isEmpty()
		? bot.username
		: bot.name.trimmed();
	return name.isEmpty() ? u"B"_q : name.left(1).toUpper();
}

void PaintAvatar(
		QPainter &p,
		QRect rect,
		const BotInfo &bot,
		bool inverted = false) {
	// Telegram-like userpic gradients, picked by the bot id.
	static const auto pairs = std::array<std::pair<QColor, QColor>, 7>{ {
		{ QColor(0xff, 0x88, 0x5e), QColor(0xff, 0x51, 0x6a) },
		{ QColor(0xff, 0xcd, 0x6a), QColor(0xff, 0xa8, 0x5c) },
		{ QColor(0x82, 0xb1, 0xff), QColor(0x66, 0x5f, 0xff) },
		{ QColor(0xa0, 0xde, 0x7e), QColor(0x54, 0xcb, 0x68) },
		{ QColor(0x53, 0xed, 0xd6), QColor(0x28, 0xc9, 0xb7) },
		{ QColor(0x72, 0xd5, 0xfd), QColor(0x2a, 0x9e, 0xf1) },
		{ QColor(0xe0, 0xa2, 0xf3), QColor(0xd6, 0x69, 0xed) },
	} };
	const auto &[top, bottom] = pairs[qHash(bot.id) % pairs.size()];
	auto hq = PainterHighQualityEnabler(p);
	if (inverted) {
		p.setPen(QPen(st::windowFgActive->c, 2));
	} else {
		p.setPen(Qt::NoPen);
	}
	auto gradient = QLinearGradient(rect.topLeft(), rect.bottomLeft());
	gradient.setColorAt(0., top);
	gradient.setColorAt(1., bottom);
	p.setBrush(gradient);
	p.drawEllipse(inverted ? rect.marginsRemoved({ 1, 1, 1, 1 }) : rect);
	auto font = st::botStudioRowNameFont->f;
	font.setPixelSize(rect.height() * 2 / 5);
	p.setFont(font);
	p.setPen(QColor(255, 255, 255));
	p.drawText(rect, Qt::AlignCenter, BotInitial(bot));
}

// Rounded status chip: colored dot and text on a tinted background.
int PaintStatusPill(
		QPainter &p,
		QPoint position,
		BotStatus status,
		const QString &text) {
	const auto font = style::font(st::botStudioStatusPillFont);
	const auto height = st::botStudioStatusPillHeight;
	const auto padding = st::botStudioStatusPillPadding;
	const auto dot = st::botStudioStatusDot;
	const auto width = padding + dot + 6 + font->width(text) + padding;
	const auto color = StatusColor(status);
	auto hq = PainterHighQualityEnabler(p);
	auto background = color;
	background.setAlpha(38);
	p.setPen(Qt::NoPen);
	p.setBrush(background);
	const auto rect = QRect(position, QSize(width, height));
	p.drawRoundedRect(rect, height / 2., height / 2.);
	p.setBrush(color);
	p.drawEllipse(QRect(
		rect.x() + padding,
		rect.y() + (height - dot) / 2,
		dot,
		dot));
	p.setFont(font);
	p.setPen(color);
	p.drawText(
		QRect(rect.x() + padding + dot + 6, rect.y(), width, height),
		Qt::AlignVCenter | Qt::AlignLeft,
		text);
	return width;
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

class CodeHighlighter final : public QSyntaxHighlighter {
public:
	explicit CodeHighlighter(QTextDocument *document)
	: QSyntaxHighlighter(document) {
	}

protected:
	void highlightBlock(const QString &text) override {
		static const auto keywords = QRegularExpression(
			u"\\b(await|async|break|case|catch|class|const|continue|"
			"default|delete|do|else|export|extends|false|finally|for|"
			"function|if|import|in|instanceof|let|new|null|of|return|"
			"static|super|switch|this|throw|true|try|typeof|undefined|"
			"var|void|while|yield)\\b"_q);
		static const auto globals = QRegularExpression(
			u"\\b(bot|ctx|db|ai|http|Keyboard|console|fetch)\\b"_q);
		static const auto numbers = QRegularExpression(
			u"\\b\\d+(\\.\\d+)?\\b"_q);
		static const auto strings = QRegularExpression(
			u"(\"(\\\\.|[^\"\\\\])*\"|'(\\\\.|[^'\\\\])*'|`(\\\\.|[^`\\\\])*`)"_q);
		static const auto comment = QRegularExpression(u"//[^\\n]*"_q);

		auto keyword = QTextCharFormat();
		keyword.setForeground(st::botStudioCodeKeywordFg->c);
		keyword.setFontWeight(QFont::DemiBold);
		auto global = QTextCharFormat();
		global.setForeground(st::botStudioCodeKeywordFg->c);
		auto number = QTextCharFormat();
		number.setForeground(st::botStudioCodeNumberFg->c);
		auto string = QTextCharFormat();
		string.setForeground(st::botStudioCodeStringFg->c);
		auto commentFormat = QTextCharFormat();
		commentFormat.setForeground(st::botStudioCodeCommentFg->c);
		commentFormat.setFontItalic(true);

		const auto apply = [&](
				const QRegularExpression &re,
				const QTextCharFormat &format) {
			auto i = re.globalMatch(text);
			while (i.hasNext()) {
				const auto match = i.next();
				setFormat(
					match.capturedStart(),
					match.capturedLength(),
					format);
			}
		};
		apply(keywords, keyword);
		apply(globals, global);
		apply(numbers, number);
		apply(strings, string);

		// Block comments, tracked between lines with the block state.
		auto start = 0;
		if (previousBlockState() != 1) {
			start = text.indexOf(u"/*"_q);
		}
		setCurrentBlockState(0);
		while (start >= 0) {
			const auto end = text.indexOf(u"*/"_q, start);
			auto length = 0;
			if (end < 0) {
				setCurrentBlockState(1);
				length = text.size() - start;
			} else {
				length = end - start + 2;
			}
			setFormat(start, length, commentFormat);
			start = text.indexOf(u"/*"_q, start + length);
		}
		auto i = comment.globalMatch(text);
		while (i.hasNext()) {
			const auto match = i.next();
			const auto from = match.capturedStart();
			// Skip "//" inside strings like 'https://...'.
			if (format(from).foreground().color()
				!= st::botStudioCodeStringFg->c) {
				setFormat(from, text.size() - from, commentFormat);
				break;
			}
		}
	}

};

[[nodiscard]] not_null<QPlainTextEdit*> CreateCodeView(
		not_null<QWidget*> parent,
		bool readOnly,
		bool highlight) {
	const auto result = Ui::CreateChild<QPlainTextEdit>(parent.get());
	result->setFrameShape(QFrame::NoFrame);
	result->setFont(CodeFont());
	result->setReadOnly(readOnly);
	result->setLineWrapMode(readOnly
		? QPlainTextEdit::WidgetWidth
		: QPlainTextEdit::NoWrap);
	result->setTabStopDistance(
		QFontMetricsF(CodeFont()).horizontalAdvance(u"  "_q));
	result->document()->setDocumentMargin(st::botStudioEditorPadding.left() * 2);
	const auto highlighter = highlight
		? new CodeHighlighter(result->document())
		: nullptr;
	const auto updatePalette = [=] {
		const auto handle = st::windowSubTextFg->c;
		auto hover = handle;
		hover.setAlpha(200);
		auto idle = handle;
		idle.setAlpha(110);
		result->setStyleSheet(uR"(
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle { background: %1; border-radius: 3px; min-height: 30px; min-width: 30px; }
QScrollBar::handle:hover { background: %2; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0px; height: 0px; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QAbstractScrollArea::corner { background: transparent; }
)"_q.arg(idle.name(QColor::HexArgb), hover.name(QColor::HexArgb)));
		auto palette = result->palette();
		palette.setColor(QPalette::Base, st::botStudioEditorBg->c);
		palette.setColor(QPalette::Text, st::windowFg->c);
		palette.setColor(QPalette::Highlight, st::windowBgActive->c);
		palette.setColor(QPalette::HighlightedText, st::windowFgActive->c);
		result->setPalette(palette);
		result->viewport()->setPalette(palette);
		if (highlighter) {
			highlighter->rehighlight();
		}
	};
	updatePalette();
	style::PaletteChanged(
	) | rpl::on_next(updatePalette, Ui::CreateChild<Ui::RpWidget>(
		result)->lifetime());
	return result;
}

[[nodiscard]] not_null<Ui::PasswordInput*> AddPasswordRow(
		not_null<Ui::GenericBox*> box,
		const QString &placeholder) {
	const auto wrap = box->addRow(object_ptr<Ui::RpWidget>(box));
	const auto field = Ui::CreateChild<Ui::PasswordInput>(
		wrap,
		st::defaultInputField,
		rpl::single(placeholder),
		QString());
	wrap->resize(wrap->width(), field->height());
	wrap->widthValue() | rpl::on_next([=](int width) {
		field->resize(width, field->height());
		field->moveToLeft(0, 0);
	}, wrap->lifetime());
	field->show();
	return field;
}

void SetEditorText(not_null<QPlainTextEdit*> editor, const QString &text) {
	// Keep undo history: replace the content through a cursor.
	auto cursor = editor->textCursor();
	cursor.beginEditBlock();
	cursor.select(QTextCursor::Document);
	cursor.insertText(text);
	cursor.endEditBlock();
	cursor.movePosition(QTextCursor::Start);
	editor->setTextCursor(cursor);
}

void DocsBox(not_null<Ui::GenericBox*> box) {
	box->setTitle(rpl::single(u"Справка Bot Studio"_q));
	box->setWidth(st::boxWideWidth + st::boxWideWidth / 2);
	box->setMaxHeight(st::boxMaxListHeight * 2);
	const auto label = box->addRow(
		object_ptr<Ui::FlatLabel>(box, QString(), st::boxLabel));
	label->setSelectable(true);
	label->setMarkedText(AiAgent::MarkdownToEntities(
		Runtime::Documentation()));
	box->addButton(rpl::single(u"Закрыть"_q), [=] { box->closeBox(); });
}

void TokenBox(
		not_null<Ui::GenericBox*> box,
		not_null<Studio*> studio,
		QString botId) {
	box->setTitle(rpl::single(u"Сменить токен"_q));
	box->setWidth(st::boxWideWidth);
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		u"Новый токен этого же бота из @BotFather (/token или /revoke)."_q,
		st::botStudioBoxLabel));
	const auto field = AddPasswordRow(box, u"Токен бота"_q);
	const auto error = box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		QString(),
		st::botStudioError));
	box->setFocusCallback([=] { field->setFocusFast(); });
	box->addButton(rpl::single(u"Сохранить"_q), [=] {
		error->setText(u"Проверяю токен…"_q);
		studio->setToken(botId, field->getLastText(), crl::guard(box, [=](
				QString text) {
			if (text.isEmpty()) {
				box->closeBox();
			} else {
				error->setText(text);
			}
		}));
	});
	box->addButton(rpl::single(u"Отмена"_q), [=] { box->closeBox(); });
}

// A segmented "pill" switch for the page sections.
class PillTabs final : public Ui::RpWidget {
public:
	explicit PillTabs(QWidget *parent) : RpWidget(parent) {
		setMouseTracking(true);
		setCursor(style::cur_pointer);
	}

	void setSections(std::vector<QString> sections) {
		_sections = std::move(sections);
		const auto font = style::font(st::botStudioPillFont);
		auto width = st::botStudioPillSkip;
		_rects.clear();
		for (const auto &section : _sections) {
			const auto w = font->width(section) + st::botStudioPillPadding * 2;
			_rects.push_back(QRect(
				width,
				st::botStudioPillSkip,
				w,
				st::botStudioPillHeight - st::botStudioPillSkip * 2));
			width += w;
		}
		resize(width + st::botStudioPillSkip, st::botStudioPillHeight);
		update();
	}
	void setActive(int index) {
		if (_active != index) {
			_active = index;
			update();
		}
	}
	[[nodiscard]] int active() const {
		return _active;
	}
	[[nodiscard]] rpl::producer<int> activated() const {
		return _activated.events();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::botStudioPillBg);
		const auto radius = height() / 2.;
		p.drawRoundedRect(rect(), radius, radius);
		p.setFont(st::botStudioPillFont);
		for (auto i = 0; i != int(_rects.size()); ++i) {
			const auto r = _rects[i];
			const auto inner = r.height() / 2.;
			if (i == _active) {
				p.setPen(Qt::NoPen);
				p.setBrush(st::windowBgActive);
				p.drawRoundedRect(r, inner, inner);
				p.setPen(st::windowFgActive);
			} else {
				if (i == _over) {
					p.setPen(Qt::NoPen);
					p.setBrush(st::windowBgRipple);
					p.drawRoundedRect(r, inner, inner);
				}
				p.setPen(st::windowSubTextFg);
			}
			p.drawText(r, Qt::AlignCenter, _sections[i]);
		}
	}
	void mouseMoveEvent(QMouseEvent *e) override {
		const auto over = indexAt(e->pos());
		if (_over != over) {
			_over = over;
			update();
		}
	}
	void leaveEventHook(QEvent *e) override {
		_over = -1;
		update();
	}
	void mousePressEvent(QMouseEvent *e) override {
		const auto index = indexAt(e->pos());
		if (index >= 0 && index != _active) {
			setActive(index);
			_activated.fire_copy(index);
		}
	}

private:
	[[nodiscard]] int indexAt(QPoint point) const {
		for (auto i = 0; i != int(_rects.size()); ++i) {
			if (_rects[i].contains(point)) {
				return i;
			}
		}
		return -1;
	}

	std::vector<QString> _sections;
	std::vector<QRect> _rects;
	rpl::event_stream<int> _activated;
	int _active = 0;
	int _over = -1;

};

// A template card: big emoji, title and a short description.
class TemplateCard final : public Ui::AbstractButton {
public:
	TemplateCard(
		QWidget *parent,
		QString emoji,
		QString title,
		QString description)
	: AbstractButton(parent)
	, _emoji(std::move(emoji))
	, _title(std::move(title))
	, _description(std::move(description)) {
		setCursor(style::cur_pointer);
		resize(width(), st::botStudioTemplateCardHeight);
	}

	void setSelected(bool selected) {
		_selected = selected;
		update();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto r = rect().marginsRemoved({ 1, 1, 1, 1 });
		const auto radius = st::botStudioCardRadius;
		p.setBrush(isOver() ? st::windowBgOver : st::botStudioCardBg);
		auto border = _selected
			? st::windowBgActive->c
			: st::botStudioCardBorder->c;
		if (!_selected) {
			border.setAlpha(140);
		}
		p.setPen(QPen(border, _selected ? 2 : 1));
		p.drawRoundedRect(r, radius, radius);

		const auto left = 14;
		p.setFont(st::botStudioTemplateEmojiFont);
		p.setPen(st::windowFg);
		p.drawText(QRect(left, 12, 36, 32), Qt::AlignCenter, _emoji);
		const auto textLeft = left + 44;
		const auto textWidth = width() - textLeft - 12;
		const auto titleFont = style::font(st::botStudioTemplateTitleFont);
		p.setFont(titleFont);
		p.setPen(st::windowBoldFg);
		p.drawText(
			textLeft,
			16 + titleFont->ascent,
			titleFont->elided(_title, textWidth));
		p.setFont(st::botStudioTemplateTextFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			QRect(textLeft, 22 + titleFont->height, textWidth, height() - 30 - titleFont->height),
			Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop,
			_description);
	}
	void onStateChanged(State was, StateChangeSource source) override {
		update();
	}

private:
	const QString _emoji;
	const QString _title;
	const QString _description;
	bool _selected = false;

};

struct TemplateChoice {
	QString id;
	QString emoji;
	QString title;
	QString description;
	QJsonObject flow;
	QString code;
};

[[nodiscard]] std::vector<TemplateChoice> TemplateChoices() {
	auto result = std::vector<TemplateChoice>();
	for (const auto &entry : FlowTemplates()) {
		result.push_back({
			.id = entry.id,
			.emoji = entry.emoji,
			.title = entry.title,
			.description = entry.description,
			.flow = entry.flow,
		});
	}
	result.push_back({
		.id = u"code"_q,
		.emoji = u"💻"_q,
		.title = u"Код на JavaScript"_q,
		.description = u"Для разработчиков: полный контроль, любая логика, "
			"HTTP-запросы и база данных."_q,
		.code = Templates().front().code,
	});
	return result;
}

// Lays out cards in a grid that follows the container width.
[[nodiscard]] not_null<Ui::RpWidget*> AddTemplateGrid(
		not_null<Ui::RpWidget*> grid,
		int columns,
		Fn<void(QString)> chosen,
		std::shared_ptr<QString> selected) {
	const auto cards = std::make_shared<std::vector<
		std::pair<QString, not_null<TemplateCard*>>>>();
	for (const auto &choice : TemplateChoices()) {
		const auto card = Ui::CreateChild<TemplateCard>(
			grid.get(),
			choice.emoji,
			choice.title,
			choice.description);
		const auto id = choice.id;
		card->setClickedCallback([=] {
			if (selected) {
				*selected = id;
				for (const auto &[cardId, other] : *cards) {
					other->setSelected(cardId == id);
				}
			}
			chosen(id);
		});
		if (selected) {
			card->setSelected(*selected == id);
		}
		cards->push_back({ id, card });
	}
	grid->widthValue() | rpl::on_next([=](int width) {
		const auto skip = st::botStudioTemplateCardSkip;
		const auto cardWidth = (width - skip * (columns - 1)) / columns;
		const auto cardHeight = st::botStudioTemplateCardHeight;
		auto index = 0;
		for (const auto &[id, card] : *cards) {
			const auto column = index % columns;
			const auto row = index / columns;
			card->setGeometry(
				column * (cardWidth + skip),
				row * (cardHeight + skip),
				cardWidth,
				cardHeight);
			++index;
		}
		const auto rows = (int(cards->size()) + columns - 1) / columns;
		grid->resize(width, rows * cardHeight + std::max(rows - 1, 0) * skip);
	}, grid->lifetime());
	return grid;
}

[[nodiscard]] QString AiCreatePrompt(const QString &idea) {
	return u"Создай мне Telegram-бота в Bot Studio. Идея: "_q
		+ idea
		+ u"\nЕсли у меня нет подходящего токена, создай нового бота через "
		"@BotFather (спроси у меня подтверждение имени и username). Собери "
		"бота визуальной схемой (flow) по справке bot_studio_docs, чтобы я "
		"мог менять его в конструкторе, запусти и проверь лог."_q;
}

void NewBotBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		not_null<Studio*> studio,
		QString preselected,
		Fn<void(QString)> created) {
	box->setTitle(rpl::single(u"Новый бот"_q));
	const auto width = st::boxWideWidth + st::boxWideWidth / 2;
	box->setWidth(width);
	box->setMaxHeight(st::boxMaxListHeight * 2);

	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		u"✦ Создать с ИИ-агентом"_q,
		st::botStudioBoxSection));
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		u"Опишите бота своими словами — агент сам зарегистрирует его в "
		"@BotFather, соберёт схему и запустит."_q,
		st::botStudioBoxLabel));
	const auto idea = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		Ui::InputField::Mode::MultiLine,
		rpl::single(u"Например: бот записи в барбершоп с выбором услуги"_q)));
	const auto aiButton = box->addRow(
		object_ptr<Ui::RoundButton>(
			box,
			rpl::single(u"✦ Создать с ИИ"_q),
			st::botStudioActionButton),
		style::al_left);
	aiButton->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
	aiButton->setClickedCallback([=] {
		const auto text = idea->getLastText().trimmed();
		if (text.isEmpty()) {
			idea->showError();
			return;
		}
		box->closeBox();
		AiAgent::ShowPanel(controller, AiCreatePrompt(text));
	});

	const auto padding = st::boxRowPadding;
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		u"Или выберите шаблон"_q,
		st::botStudioBoxSection),
		padding + QMargins(0, st::boxLittleSkip * 4, 0, st::boxLittleSkip));
	const auto selected = std::make_shared<QString>(preselected.isEmpty()
		? TemplateChoices().front().id
		: preselected);
	const auto grid = box->addRow(object_ptr<Ui::RpWidget>(box));
	AddTemplateGrid(grid, 2, [](QString) {}, selected);
	grid->resizeToWidth(width - padding.left() - padding.right());

	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		u"Токен бота"_q,
		st::botStudioBoxSection),
		padding + QMargins(0, st::boxLittleSkip * 4, 0, 0));
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		u"Откройте @BotFather, отправьте /newbot, придумайте имя и "
		"username — он пришлёт токен. Вставьте его сюда."_q,
		st::botStudioBoxLabel));
	const auto botFather = box->addRow(
		object_ptr<Ui::LinkButton>(box, u"Открыть @BotFather →"_q),
		style::al_left);
	botFather->setClickedCallback([=] {
		controller->showPeerByLink(Window::PeerByLinkInfo{
			.usernameOrId = u"BotFather"_q,
		});
		controller->window().activate();
	});
	const auto token = AddPasswordRow(box, u"123456789:AAH…"_q);
	const auto error = box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		QString(),
		st::botStudioError));
	box->setFocusCallback([=] { idea->setFocusFast(); });
	box->addButton(rpl::single(u"Создать бота"_q), [=] {
		const auto value = token->getLastText().trimmed();
		if (value.isEmpty()) {
			token->showError();
			error->setText(u"Вставьте токен от @BotFather."_q);
			return;
		}
		error->setText(u"Проверяю токен…"_q);
		auto flow = QJsonObject();
		auto code = QString();
		for (const auto &choice : TemplateChoices()) {
			if (choice.id == *selected) {
				flow = choice.flow;
				code = choice.code;
			}
		}
		studio->create(value, code, flow, crl::guard(box, [=](
				QString id,
				QString text) {
			if (text.isEmpty()) {
				studio->start(id);
				created(id);
				box->closeBox();
			} else {
				error->setText(text);
				if (!id.isEmpty()) {
					created(id);
				}
			}
		}));
	});
	box->addButton(rpl::single(u"Отмена"_q), [=] { box->closeBox(); });
}

class BotRow final : public Ui::AbstractButton {
public:
	BotRow(QWidget *parent, not_null<Studio*> studio, QString id)
	: AbstractButton(parent)
	, _studio(studio)
	, _id(std::move(id)) {
		resize(width(), st::botStudioRowHeight);
		setCursor(style::cur_pointer);
	}

	[[nodiscard]] const QString &id() const {
		return _id;
	}
	void setSelected(bool selected) {
		if (_selected != selected) {
			_selected = selected;
			update();
		}
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		const auto bot = _studio->bot(_id);
		if (!bot) {
			return;
		}
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto selected = _selected;
		if (selected || isOver()) {
			p.setPen(Qt::NoPen);
			p.setBrush(selected
				? st::botStudioRowBgActive
				: st::botStudioRowBgOver);
			p.drawRoundedRect(
				rect(),
				st::botStudioRowRadius,
				st::botStudioRowRadius);
		}
		const auto &padding = st::botStudioRowPadding;
		const auto size = st::botStudioAvatarSize;
		const auto avatar = QRect(
			padding.left(),
			(height() - size) / 2,
			size,
			size);
		PaintAvatar(p, avatar, *bot, selected);

		// Status dot on the avatar corner.
		const auto status = _studio->status(_id);
		const auto dot = st::botStudioStatusDot + 4;
		p.setPen(QPen(selected
			? st::botStudioRowBgActive->c
			: st::botStudioSidebarBg->c, 2));
		p.setBrush(StatusColor(status));
		p.drawEllipse(QRect(
			avatar.x() + avatar.width() - dot + 1,
			avatar.y() + avatar.height() - dot + 1,
			dot,
			dot));

		const auto left = avatar.x() + size + padding.left();
		const auto available = width() - left - padding.right();
		p.setPen(selected ? st::botStudioRowFgActive : st::windowBoldFg);
		const auto nameFont = style::font(st::botStudioRowNameFont);
		p.setFont(nameFont);
		const auto name = bot->name.isEmpty()
			? (u"@"_q + bot->username)
			: bot->name;
		p.drawText(
			left,
			padding.top() + nameFont->ascent + 2,
			nameFont->elided(name, available));

		const auto statusFont = style::font(st::botStudioRowStatusFont);
		p.setFont(statusFont);
		p.setPen(selected ? st::botStudioRowFgActive : st::windowSubTextFg);
		p.drawText(
			left,
			height() - padding.bottom() - statusFont->height
				+ statusFont->ascent,
			statusFont->elided(
				(bot->visual() ? u"🧩 "_q : u"💻 "_q)
					+ StatusText(status)
					+ u" · @"_q
					+ bot->username,
				available));
	}

	void onStateChanged(State was, StateChangeSource source) override {
		update();
	}

private:
	const not_null<Studio*> _studio;
	const QString _id;
	bool _selected = false;

};

class BotPage final : public Ui::RpWidget {
public:
	BotPage(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		not_null<Studio*> studio,
		not_null<Ui::LayerManager*> layers,
		QString id,
		Fn<void()> modeChanged);
	~BotPage();

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;

private:
	enum class Tab {
		Flow,
		Code,
		Logs,
		Audience,
		Data,
	};

	void setupHeader();
	void setupCode();
	void setupLogs();
	void setupData();
	void setupAudience();
	void refreshAudience();
	void exportLeads();
	void showBroadcast();
	void setupAiBar();
	void updateHeader();
	void updateGeometry();
	void showTab(Tab tab);
	void saveCode();
	void askAi();
	void showMenu();
	void appendLogs(bool reset);
	void refreshData();
	[[nodiscard]] bool codeDirty() const;
	void updateCodeHint();

	const not_null<Window::SessionController*> _controller;
	const not_null<Studio*> _studio;
	const not_null<Ui::LayerManager*> _layers;
	const QString _id;
	const Fn<void()> _modeChanged;
	const bool _visual = false;

	not_null<Ui::FlatLabel*> _name;
	not_null<Ui::FlatLabel*> _status;
	not_null<Ui::RoundButton*> _toggle;
	not_null<Ui::RoundButton*> _openChat;
	not_null<Ui::IconButton*> _menuButton;
	base::unique_qptr<Ui::PopupMenu> _menu;
	not_null<PillTabs*> _tabs;
	std::vector<Tab> _tabList;
	int _statusPillWidth = 0;

	FlowEditor *_flowEditor = nullptr;
	not_null<Ui::RpWidget*> _codeTab;
	not_null<Ui::RpWidget*> _logsTab;
	not_null<Ui::RpWidget*> _dataTab;
	not_null<Ui::RpWidget*> _audienceTab;

	Ui::RoundButton *_templates = nullptr;
	Ui::RoundButton *_docs = nullptr;
	Ui::RoundButton *_save = nullptr;
	Ui::RoundButton *_switchMode = nullptr;
	Ui::FlatLabel *_codeHint = nullptr;
	QPlainTextEdit *_editor = nullptr;

	Ui::RpWidget *_aiBar = nullptr;
	Ui::InputField *_aiField = nullptr;
	Ui::RoundButton *_aiButton = nullptr;

	Ui::RoundButton *_clearLogs = nullptr;
	Ui::FlatLabel *_logsHint = nullptr;
	QPlainTextEdit *_logView = nullptr;
	size_t _logsShown = 0;

	Ui::RpWidget *_stats = nullptr;
	Ui::RoundButton *_broadcast = nullptr;
	Ui::RoundButton *_exportCsv = nullptr;
	Ui::RoundButton *_refreshAudience = nullptr;
	Ui::FlatLabel *_leadsTitle = nullptr;
	QTableWidget *_leadsTable = nullptr;
	AudienceStats _audienceStats;

	Ui::RoundButton *_refreshData = nullptr;
	Ui::RoundButton *_clearData = nullptr;
	QPlainTextEdit *_dataView = nullptr;

	QString _savedCode;
	QString _error;
	Tab _tab = Tab::Code;

};

BotPage::BotPage(
	QWidget *parent,
	not_null<Window::SessionController*> controller,
	not_null<Studio*> studio,
	not_null<Ui::LayerManager*> layers,
	QString id,
	Fn<void()> modeChanged)
: RpWidget(parent)
, _controller(controller)
, _studio(studio)
, _layers(layers)
, _id(std::move(id))
, _modeChanged(std::move(modeChanged))
, _visual(_studio->bot(_id) && _studio->bot(_id)->visual())
, _name(Ui::CreateChild<Ui::FlatLabel>(this, st::botStudioHeaderName))
, _status(Ui::CreateChild<Ui::FlatLabel>(this, st::botStudioHeaderStatus))
, _toggle(MakeButton(this, u"▶ Запустить"_q, st::botStudioActionButton))
, _openChat(MakeButton(this, u"Открыть в Telegram"_q, st::botStudioLightButton))
, _menuButton(Ui::CreateChild<Ui::IconButton>(this, st::botStudioMenuButton))
, _tabs(Ui::CreateChild<PillTabs>(this))
, _codeTab(Ui::CreateChild<Ui::RpWidget>(this))
, _logsTab(Ui::CreateChild<Ui::RpWidget>(this))
, _dataTab(Ui::CreateChild<Ui::RpWidget>(this))
, _audienceTab(Ui::CreateChild<Ui::RpWidget>(this)) {
	if (_visual) {
		_flowEditor = Ui::CreateChild<FlowEditor>(
			this,
			_studio,
			_layers,
			_id);
		_tabList = { Tab::Flow, Tab::Code, Tab::Logs, Tab::Audience, Tab::Data };
		_tabs->setSections({
			u"🧩 Конструктор"_q,
			u"</> Код"_q,
			u"📜 Логи"_q,
			u"👥 Аудитория"_q,
			u"🗄 Данные"_q,
		});
	} else {
		_tabList = { Tab::Code, Tab::Logs, Tab::Audience, Tab::Data };
		_tabs->setSections({
			u"</> Код"_q,
			u"📜 Логи"_q,
			u"👥 Аудитория"_q,
			u"🗄 Данные"_q,
		});
	}
	setupHeader();
	setupCode();
	setupLogs();
	setupData();
	setupAudience();
	setupAiBar();
	_tabs->activated(
	) | rpl::on_next([=](int index) {
		showTab(_tabList[index]);
	}, lifetime());
	showTab(_tabList.front());

	_studio->botUpdated(
	) | rpl::filter(rpl::mappers::_1 == _id) | rpl::on_next([=] {
		const auto bot = _studio->bot(_id);
		if (!bot) {
			return;
		} else if (bot->visual() != _visual) {
			if (_modeChanged) {
				crl::on_main(this, _modeChanged);
			}
			return;
		}
		updateHeader();
		if (bot->code != _savedCode) {
			if (_visual || !codeDirty()) {
				_savedCode = bot->code;
				SetEditorText(_editor, bot->code);
			} else {
				_savedCode = bot->code;
				updateCodeHint();
			}
		}
		if (_flowEditor) {
			_flowEditor->reload();
		}
	}, lifetime());
	_studio->logAdded(
	) | rpl::filter(rpl::mappers::_1 == _id) | rpl::on_next([=] {
		appendLogs(false);
		updateHeader();
	}, lifetime());
	updateHeader();
}

BotPage::~BotPage() {
	// Don't lose scheme edits when switching bots or closing the window.
	if (_flowEditor && _flowEditor->dirty()) {
		_flowEditor->save();
	}
}

void BotPage::setupHeader() {
	_toggle->setClickedCallback([=] {
		const auto status = _studio->status(_id);
		if (status == BotStatus::Running || status == BotStatus::Starting) {
			_studio->stop(_id);
		} else {
			if (_flowEditor && _flowEditor->dirty()) {
				_flowEditor->save();
			} else if (!_visual && codeDirty()) {
				saveCode();
			}
			_studio->start(_id);
		}
	});
	_openChat->setClickedCallback([=] {
		if (const auto bot = _studio->bot(_id)) {
			_controller->showPeerByLink(Window::PeerByLinkInfo{
				.usernameOrId = bot->username,
			});
			_controller->window().activate();
		}
	});
	_menuButton->setClickedCallback([=] { showMenu(); });
}

void BotPage::setupCode() {
	const auto tab = _codeTab.get();
	_docs = MakeButton(tab, u"Справка по API"_q, st::botStudioSmallButton);
	_codeHint = Ui::CreateChild<Ui::FlatLabel>(
		tab,
		QString(),
		st::botStudioHint);
	_editor = CreateCodeView(tab, _visual, true);
	if (const auto bot = _studio->bot(_id)) {
		_savedCode = bot->code;
		_editor->setPlainText(bot->code);
	}
	_docs->setClickedCallback([=] {
		_layers->showBox(Box(DocsBox));
	});
	if (_visual) {
		_switchMode = MakeButton(
			tab,
			u"Перейти на ручной код"_q,
			st::botStudioSmallButton);
		_switchMode->setClickedCallback([=] {
			_layers->showBox(Ui::MakeConfirmBox({
				.text = u"Бот перестанет быть схемой: вы будете менять его "
					"кодом, а вкладка «Конструктор» пропадёт. Продолжить?"_q,
				.confirmed = [=](Fn<void()> close) {
					close();
					if (_flowEditor && _flowEditor->dirty()) {
						_flowEditor->save();
					}
					_studio->convertToCode(_id);
				},
				.confirmText = u"Перейти на код"_q,
			}));
		});
		updateCodeHint();
		return;
	}
	_templates = MakeButton(tab, u"Шаблоны"_q, st::botStudioSmallButton);
	_save = MakeButton(tab, u"Сохранить и перезапустить"_q, st::botStudioSmallButton);
	_switchMode = MakeButton(
		tab,
		u"🧩 В конструктор"_q,
		st::botStudioSmallButton);
	_switchMode->setClickedCallback([=] {
		_layers->showBox(Ui::MakeConfirmBox({
			.text = u"Перевести бота в визуальный конструктор? Текущий код "
				"будет заменён простой схемой, которую можно собрать "
				"мышкой."_q,
			.confirmed = [=](Fn<void()> close) {
				close();
				_studio->setFlow(_id, EmptyFlow());
			},
			.confirmText = u"Открыть конструктор"_q,
		}));
	});
	QObject::connect(_editor, &QPlainTextEdit::textChanged, [=] {
		updateCodeHint();
	});
	base::install_event_filter(_editor, [=](not_null<QEvent*> e) {
		if (e->type() == QEvent::KeyPress) {
			const auto key = static_cast<QKeyEvent*>(e.get());
			if (key->matches(QKeySequence::Save)) {
				saveCode();
				return base::EventFilterResult::Cancel;
			}
		}
		return base::EventFilterResult::Continue;
	});
	_save->setClickedCallback([=] { saveCode(); });
	_templates->setClickedCallback([=] {
		_menu = base::make_unique_q<Ui::PopupMenu>(
			this,
			st::defaultPopupMenu);
		for (const auto &entry : Templates()) {
			const auto code = entry.code;
			_menu->addAction(entry.title, [=] {
				SetEditorText(_editor, code);
				_editor->setFocus();
			});
		}
		_menu->popup(QCursor::pos());
	});
	updateCodeHint();
}

void BotPage::setupAiBar() {
	_aiBar = Ui::CreateChild<Ui::RpWidget>(this);
	_aiField = Ui::CreateChild<Ui::InputField>(
		_aiBar,
		st::botStudioAiField,
		Ui::InputField::Mode::MultiLine,
		rpl::single(_visual
			? u"Опишите, что изменить в боте, — ИИ-агент перестроит схему"_q
			: u"Опишите, что изменить в боте, — ИИ-агент перепишет код"_q));
	_aiField->setSubmitSettings(Ui::InputField::SubmitSettings::Enter);
	_aiButton = MakeButton(_aiBar, u"✦ Сделать"_q, st::botStudioAiButton);
	_aiField->submits() | rpl::on_next([=] { askAi(); }, _aiField->lifetime());
	_aiButton->setClickedCallback([=] { askAi(); });
	_aiField->heightChanges(
	) | rpl::on_next([=] {
		updateGeometry();
	}, _aiField->lifetime());
	_aiBar->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(_aiBar);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(QPen(st::windowBgActive->c, 1.5));
		p.setBrush(st::aiAgentComposerBg);
		p.drawRoundedRect(
			QRectF(_aiBar->rect()).marginsRemoved({ 1., 1., 1., 1. }),
			st::botStudioAiBarRadius,
			st::botStudioAiBarRadius);
	}, _aiBar->lifetime());
}

void BotPage::setupLogs() {
	const auto tab = _logsTab.get();
	_clearLogs = MakeButton(tab, u"Очистить"_q, st::botStudioSmallButton);
	_logsHint = Ui::CreateChild<Ui::FlatLabel>(
		tab,
		u"Сообщения пользователей, console.log и ошибки — в реальном "
			"времени."_q,
		st::botStudioHint);
	_logView = CreateCodeView(tab, true, false);
	_logView->setMaximumBlockCount(1000);
	_clearLogs->setClickedCallback([=] {
		_studio->clearLogs(_id);
		appendLogs(true);
	});
	appendLogs(true);
}

void BotPage::setupData() {
	const auto tab = _dataTab.get();
	_refreshData = MakeButton(tab, u"Обновить"_q, st::botStudioSmallButton);
	_clearData = MakeButton(tab, u"Очистить данные"_q, st::botStudioSmallButton);
	_dataView = CreateCodeView(tab, true, false);
	_refreshData->setClickedCallback([=] { refreshData(); });
	_clearData->setClickedCallback([=] {
		_layers->showBox(Ui::MakeConfirmBox({
			.text = u"Удалить все данные, которые сохранил бот: ответы "
				"пользователей, сессии, историю ИИ?"_q,
			.confirmed = [=](Fn<void()> close) {
				_studio->clearStorage(_id);
				refreshData();
				close();
			},
			.confirmText = u"Удалить"_q,
			.confirmStyle = &st::attentionBoxButton,
		}));
	});
}

void BotPage::refreshData() {
	const auto raw = _studio->storage(_id);
	const auto document = QJsonDocument::fromJson(raw);
	_dataView->setPlainText(document.isNull()
		? QString::fromUtf8(raw)
		: QString::fromUtf8(document.toJson(QJsonDocument::Indented)));
}

void BotPage::appendLogs(bool reset) {
	const auto &logs = _studio->logs(_id);
	if (reset || _logsShown > logs.size()) {
		_logView->clear();
		_logsShown = 0;
	}
	const auto escape = [](const QString &text) {
		return text.toHtmlEscaped().replace('\n', u"<br>"_q);
	};
	for (auto i = _logsShown; i < logs.size(); ++i) {
		const auto &entry = logs[i];
		const auto color = [&] {
			switch (entry.level) {
			case LogLevel::Error: return ColorName(st::botStudioErrorFg);
			case LogLevel::Warning: return ColorName(st::botStudioCodeNumberFg);
			case LogLevel::Incoming: return ColorName(st::windowActiveTextFg);
			case LogLevel::System: return ColorName(st::windowSubTextFg);
			case LogLevel::Info: return ColorName(st::windowFg);
			}
			return ColorName(st::windowFg);
		}();
		const auto prefix = (entry.level == LogLevel::Incoming)
			? u"→ "_q
			: (entry.level == LogLevel::Error)
			? u"✖ "_q
			: (entry.level == LogLevel::Warning)
			? u"⚠ "_q
			: QString();
		_logView->appendHtml(u"<span style=\"color:%1\">%2</span> "
			"<span style=\"color:%3\">%4</span>"_q.arg(
				ColorName(st::windowSubTextFg),
				entry.when.toString(u"HH:mm:ss"_q),
				color,
				escape(prefix + entry.text)));
	}
	_logsShown = logs.size();
}

bool BotPage::codeDirty() const {
	return !_visual && _editor && (_editor->toPlainText() != _savedCode);
}

void BotPage::updateCodeHint() {
	if (!_codeHint) {
		return;
	}
	if (_visual) {
		_codeHint->setTextColorOverride(std::nullopt);
		_codeHint->setText(u"Код создаётся из схемы автоматически"_q);
	} else if (!_error.isEmpty()) {
		static const auto line = QRegularExpression(u"bot\\.js:(\\d+)"_q);
		const auto match = line.match(_error);
		_codeHint->setTextColorOverride(st::botStudioErrorFg->c);
		_codeHint->setText((match.hasMatch()
			? (u"Строка %1: "_q.arg(match.captured(1)))
			: QString()) + _error.section('\n', 0, 0));
	} else {
		_codeHint->setTextColorOverride(std::nullopt);
		_codeHint->setText(codeDirty()
			? u"Есть несохранённые изменения · Ctrl+S"_q
			: u"Работает прямо в Agentgram, без деплоя"_q);
	}
	updateGeometry();
}

void BotPage::saveCode() {
	if (_visual) {
		return;
	}
	const auto code = _editor->toPlainText();
	_savedCode = code;
	_error = _studio->setCode(_id, code);
	updateCodeHint();
}

void BotPage::askAi() {
	const auto text = _aiField->getLastText().trimmed();
	const auto bot = _studio->bot(_id);
	if (text.isEmpty() || !bot) {
		_aiField->setFocus();
		return;
	}
	if (_flowEditor && _flowEditor->dirty()) {
		_flowEditor->save();
	} else if (codeDirty()) {
		saveCode();
	}
	_aiField->clear();
	AiAgent::ShowPanel(_controller, _visual
		? u"Bot Studio, бот @%1 (bot: %2) — визуальная схема. Задача: %3\n"
			"Посмотри текущую схему (bot_studio_get), при необходимости "
			"справку (bot_studio_docs), измени схему через "
			"bot_studio_set_flow и проверь лог."_q.arg(
				bot->username,
				bot->id,
				text)
		: u"Bot Studio, бот @%1 (bot: %2). Задача: %3\nПосмотри текущий "
			"код (bot_studio_get), при необходимости справку "
			"(bot_studio_docs), обнови код через bot_studio_set_code и "
			"проверь лог."_q.arg(
				bot->username,
				bot->id,
				text));
}

void BotPage::setupAudience() {
	const auto tab = _audienceTab.get();
	_broadcast = MakeButton(tab, u"📣 Рассылка"_q, st::botStudioActionButton);
	_exportCsv = MakeButton(tab, u"⬇ Заявки в CSV"_q, st::botStudioLightButton);
	_refreshAudience = MakeButton(tab, u"Обновить"_q, st::botStudioLightButton);
	_broadcast->setClickedCallback([=] { showBroadcast(); });
	_exportCsv->setClickedCallback([=] { exportLeads(); });
	_refreshAudience->setClickedCallback([=] { refreshAudience(); });

	_stats = Ui::CreateChild<Ui::RpWidget>(tab);
	_stats->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(_stats);
		auto hq = PainterHighQualityEnabler(p);
		const auto items = std::vector<std::pair<QString, int>>{
			{ u"👥 Пользователи"_q, _audienceStats.users },
			{ u"🔥 Активны за 7 дней"_q, _audienceStats.active },
			{ u"🗂 Заявки"_q, _audienceStats.leads },
			{ u"🚫 Заблокировали"_q, _audienceStats.blocked },
		};
		const auto skip = 10;
		const auto count = int(items.size());
		const auto width = (_stats->width() - skip * (count - 1)) / count;
		const auto height = _stats->height();
		const auto numberFont = style::font(st::botStudioEmptyTitle.style.font);
		const auto labelFont = style::font(st::botStudioRowStatusFont);
		for (auto i = 0; i != count; ++i) {
			const auto r = QRect(i * (width + skip), 0, width, height);
			p.setPen(Qt::NoPen);
			p.setBrush(st::botStudioEditorBg);
			p.drawRoundedRect(r, st::botStudioCardRadius, st::botStudioCardRadius);
			p.setFont(numberFont);
			p.setPen(st::windowBoldFg);
			p.drawText(
				r.marginsRemoved({ 16, 12, 16, 0 }),
				Qt::AlignLeft | Qt::AlignTop,
				QString::number(items[i].second));
			p.setFont(labelFont);
			p.setPen(st::windowSubTextFg);
			p.drawText(
				r.marginsRemoved({ 16, 0, 16, 12 }),
				Qt::AlignLeft | Qt::AlignBottom,
				items[i].first);
		}
	}, _stats->lifetime());

	_leadsTitle = Ui::CreateChild<Ui::FlatLabel>(
		tab,
		u"Заявки"_q,
		st::botStudioInspectorSection);
	_leadsTable = Ui::CreateChild<QTableWidget>(tab);
	_leadsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
	_leadsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
	_leadsTable->verticalHeader()->setVisible(false);
	_leadsTable->horizontalHeader()->setStretchLastSection(true);
	_leadsTable->setFrameShape(QFrame::NoFrame);
	_leadsTable->setShowGrid(false);
	_leadsTable->setWordWrap(false);
	const auto updatePalette = [=] {
		const auto bg = st::botStudioEditorBg->c.name();
		const auto fg = st::windowFg->c.name();
		const auto sub = st::windowSubTextFg->c.name();
		const auto line = st::windowBg->c.name();
		const auto active = st::windowBgActive->c.name();
		_leadsTable->setStyleSheet(uR"(
QTableWidget { background: %1; color: %2; border-radius: 10px; gridline-color: %4; }
QTableWidget::item { padding: 6px 10px; border-bottom: 1px solid %4; }
QTableWidget::item:selected { background: %5; color: white; }
QHeaderView::section { background: %1; color: %3; border: none; border-bottom: 1px solid %4; padding: 8px 10px; font-weight: 600; }
QTableCornerButton::section { background: %1; border: none; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle { background: %3; border-radius: 3px; min-height: 30px; min-width: 30px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0px; height: 0px; }
)"_q.arg(bg, fg, sub, line, active));
	};
	updatePalette();
	style::PaletteChanged(
	) | rpl::on_next(updatePalette, _leadsTitle->lifetime());
}

void BotPage::refreshAudience() {
	_audienceStats = _studio->audience(_id);
	_stats->update();
	const auto leads = _studio->leads(_id);
	auto keys = QStringList();
	for (const auto &value : leads) {
		const auto answers = value.toObject().value(u"answers"_q).toObject();
		for (const auto &key : answers.keys()) {
			if (!keys.contains(key)) {
				keys.push_back(key);
			}
		}
	}
	auto headers = QStringList{
		u"Дата"_q,
		u"Имя"_q,
		u"Username"_q,
		u"Шаг"_q,
	};
	headers += keys;
	_leadsTable->clear();
	_leadsTable->setColumnCount(headers.size());
	_leadsTable->setHorizontalHeaderLabels(headers);
	_leadsTable->setRowCount(leads.size());
	for (auto i = 0; i != leads.size(); ++i) {
		// Newest first.
		const auto lead = leads[leads.size() - 1 - i].toObject();
		const auto date = QDateTime::fromString(
			lead.value(u"date"_q).toString(),
			Qt::ISODateWithMs).toLocalTime();
		auto values = QStringList{
			date.isValid()
				? date.toString(u"dd.MM.yyyy HH:mm"_q)
				: lead.value(u"date"_q).toString(),
			lead.value(u"name"_q).toString(),
			lead.value(u"username"_q).toString(),
			lead.value(u"step"_q).toString(),
		};
		const auto answers = lead.value(u"answers"_q).toObject();
		for (const auto &key : keys) {
			values.push_back(answers.value(key).toVariant().toString());
		}
		for (auto j = 0; j != values.size(); ++j) {
			_leadsTable->setItem(i, j, new QTableWidgetItem(values[j]));
		}
	}
	_leadsTable->resizeColumnsToContents();
	_leadsTitle->setText(leads.isEmpty()
		? u"Заявок пока нет. Включите «Присылать мне ответы клиента» в "
			"сообщении или добавьте действие «Сохранить заявку»."_q
		: u"Заявки (%1)"_q.arg(leads.size()));
	updateGeometry();
}

void BotPage::exportLeads() {
	const auto leads = _studio->leads(_id);
	if (leads.isEmpty()) {
		_layers->showBox(Ui::MakeInformBox(u"Заявок пока нет."_q));
		return;
	}
	const auto bot = _studio->bot(_id);
	const auto path = QFileDialog::getSaveFileName(
		window(),
		u"Сохранить заявки"_q,
		u"leads_%1_%2.csv"_q.arg(
			bot ? bot->username : u"bot"_q,
			QDate::currentDate().toString(u"yyyy-MM-dd"_q)),
		u"CSV (*.csv)"_q);
	if (path.isEmpty()) {
		return;
	}
	auto keys = QStringList();
	for (const auto &value : leads) {
		for (const auto &key : value.toObject().value(u"answers"_q).toObject().keys()) {
			if (!keys.contains(key)) {
				keys.push_back(key);
			}
		}
	}
	const auto escape = [](QString value) {
		value.replace('"', u"\"\""_q);
		return '"' + value + '"';
	};
	auto lines = QStringList();
	auto header = QStringList{ u"Дата"_q, u"Имя"_q, u"Username"_q, u"ID"_q, u"Шаг"_q };
	header += keys;
	auto headerCells = QStringList();
	for (const auto &cell : header) {
		headerCells.push_back(escape(cell));
	}
	lines.push_back(headerCells.join(';'));
	for (const auto &value : leads) {
		const auto lead = value.toObject();
		const auto answers = lead.value(u"answers"_q).toObject();
		auto row = QStringList{
			lead.value(u"date"_q).toString(),
			lead.value(u"name"_q).toString(),
			lead.value(u"username"_q).toString(),
			lead.value(u"user_id"_q).toVariant().toString(),
			lead.value(u"step"_q).toString(),
		};
		for (const auto &key : keys) {
			row.push_back(answers.value(key).toVariant().toString());
		}
		auto escaped = QStringList();
		for (const auto &cell : row) {
			escaped.push_back(escape(cell));
		}
		lines.push_back(escaped.join(';'));
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		_layers->showBox(Ui::MakeInformBox(u"Не удалось сохранить файл."_q));
		return;
	}
	// BOM: Excel opens UTF-8 CSV correctly only with it.
	file.write("\xEF\xBB\xBF");
	file.write(lines.join(u"\r\n"_q).toUtf8());
	file.close();
	_layers->showBox(Ui::MakeInformBox(
		u"Сохранено заявок: %1\n%2"_q.arg(leads.size()).arg(path)));
}

void BotPage::showBroadcast() {
	const auto stats = _studio->audience(_id);
	_layers->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Рассылка"_q));
		box->setWidth(st::boxWideWidth);
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Сообщение получат все, кто писал боту: %1 чел. (кроме "
				"заблокировавших). Используйте рассылку бережно — "
				"Telegram ограничивает спам."_q.arg(
					std::max(stats.users - stats.blocked, 0)),
			st::botStudioBoxLabel));
		const auto text = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			rpl::single(u"Текст рассылки"_q)));
		const auto photo = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Картинка: ссылка https://… (необязательно)"_q)));
		const auto error = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			QString(),
			st::botStudioError));
		box->setFocusCallback([=] { text->setFocusFast(); });
		box->addButton(rpl::single(u"Отправить"_q), [=] {
			const auto value = text->getLastText().trimmed();
			if (value.isEmpty()) {
				text->showError();
				return;
			}
			const auto result = _studio->broadcast(
				_id,
				value,
				photo->getLastText());
			if (result.isEmpty()) {
				box->closeBox();
				showTab(Tab::Logs);
			} else {
				error->setText(result);
			}
		});
		box->addButton(rpl::single(u"Отмена"_q), [=] { box->closeBox(); });
	}));
}

void BotPage::showMenu() {
	_menu = base::make_unique_q<Ui::PopupMenu>(this, st::defaultPopupMenu);
	_menu->addAction(u"Сменить токен…"_q, [=] {
		_layers->showBox(Box(TokenBox, _studio, _id));
	});
	_menu->addAction(u"Очистить логи"_q, [=] {
		_studio->clearLogs(_id);
		appendLogs(true);
	});
	_menu->addAction(u"Удалить бота из Bot Studio"_q, [=] {
		const auto bot = _studio->bot(_id);
		if (!bot) {
			return;
		}
		_layers->showBox(Ui::MakeConfirmBox({
			.text = u"Удалить @%1 из Bot Studio вместе со схемой, кодом и "
				"данными? Сам бот в Telegram останется."_q.arg(
					bot->username),
			.confirmed = [=](Fn<void()> close) {
				close();
				_studio->remove(_id);
			},
			.confirmText = u"Удалить"_q,
			.confirmStyle = &st::attentionBoxButton,
		}));
	});
	_menu->popup(QCursor::pos());
}

void BotPage::updateHeader() {
	const auto bot = _studio->bot(_id);
	if (!bot) {
		return;
	}
	_name->setText(bot->name.isEmpty() ? (u"@"_q + bot->username) : bot->name);
	const auto status = _studio->status(_id);
	auto text = u"@"_q + bot->username;
	_audienceStats = _studio->audience(_id);
	if (_audienceStats.users > 0) {
		text += u" · 👥 %1"_q.arg(_audienceStats.users);
	}
	const auto error = _studio->statusError(_id);
	if (status == BotStatus::Error && !error.isEmpty()) {
		text += u" · "_q + error;
	}
	const auto errors = _studio->errorCount(_id);
	if (errors > 0 && status == BotStatus::Running) {
		text += u" · ошибок в логе: %1"_q.arg(errors);
	}
	_status->setText(text);
	const auto font = style::font(st::botStudioStatusPillFont);
	_statusPillWidth = st::botStudioStatusPillPadding * 2
		+ st::botStudioStatusDot
		+ 6
		+ font->width(StatusText(status));
	const auto running = (status == BotStatus::Running)
		|| (status == BotStatus::Starting);
	_toggle->setText(rpl::single(running
		? u"■ Остановить"_q
		: u"▶ Запустить"_q));
	updateGeometry();
	update();
}

void BotPage::showTab(Tab tab) {
	_tab = tab;
	const auto index = int(ranges::find(_tabList, tab) - begin(_tabList));
	_tabs->setActive(index);
	if (_flowEditor) {
		_flowEditor->setVisible(tab == Tab::Flow);
	}
	_codeTab->setVisible(tab == Tab::Code);
	_logsTab->setVisible(tab == Tab::Logs);
	_dataTab->setVisible(tab == Tab::Data);
	_audienceTab->setVisible(tab == Tab::Audience);
	_aiBar->setVisible(tab == Tab::Flow || tab == Tab::Code);
	if (tab == Tab::Logs) {
		appendLogs(false);
	} else if (tab == Tab::Data) {
		refreshData();
	} else if (tab == Tab::Audience) {
		refreshAudience();
	} else if (tab == Tab::Code && _visual) {
		if (const auto bot = _studio->bot(_id); bot && bot->code != _editor->toPlainText()) {
			SetEditorText(_editor, bot->code);
		}
	}
	updateGeometry();
}

void BotPage::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	p.fillRect(e->rect(), st::windowBg);
	const auto bot = _studio->bot(_id);
	if (!bot) {
		return;
	}
	const auto &padding = st::botStudioContentPadding;
	const auto size = st::botStudioHeaderAvatarSize;
	PaintAvatar(
		p,
		QRect(
			padding.left(),
			padding.top() + (st::botStudioHeaderHeight - size) / 2,
			size,
			size),
		*bot);
	PaintStatusPill(
		p,
		QPoint(
			_name->x(),
			_status->y() + (_status->height() - st::botStudioStatusPillHeight) / 2),
		_studio->status(_id),
		StatusText(_studio->status(_id)));
}

void BotPage::resizeEvent(QResizeEvent *e) {
	updateGeometry();
}

void BotPage::updateGeometry() {
	const auto &padding = st::botStudioContentPadding;
	const auto width = this->width();
	const auto inner = width - padding.left() - padding.right();
	if (inner <= 0 || !_aiBar) {
		return;
	}
	const auto headerTop = padding.top();
	const auto headerHeight = st::botStudioHeaderHeight;
	const auto avatar = st::botStudioHeaderAvatarSize;
	auto right = width - padding.right();
	_menuButton->moveToLeft(
		right - _menuButton->width(),
		headerTop + (headerHeight - _menuButton->height()) / 2);
	right -= _menuButton->width() + st::botStudioButtonSkip;
	_openChat->moveToLeft(
		right - _openChat->width(),
		headerTop + (headerHeight - _openChat->height()) / 2);
	right -= _openChat->width() + st::botStudioButtonSkip;
	_toggle->moveToLeft(
		right - _toggle->width(),
		headerTop + (headerHeight - _toggle->height()) / 2);
	right -= _toggle->width() + st::botStudioButtonSkip * 2;

	const auto textLeft = padding.left() + avatar + 14;
	const auto textWidth = std::max(right - textLeft, 80);
	_name->resizeToWidth(textWidth);
	const auto pillSkip = _statusPillWidth + 8;
	_status->resizeToWidth(std::max(textWidth - pillSkip, 40));
	const auto lineHeight = std::max(
		_status->height(),
		st::botStudioStatusPillHeight);
	const auto textHeight = _name->height() + 4 + lineHeight;
	const auto textTop = headerTop + (headerHeight - textHeight) / 2;
	_name->moveToLeft(textLeft, textTop);
	_status->moveToLeft(
		textLeft + pillSkip,
		textTop + _name->height() + 4 + (lineHeight - _status->height()) / 2);

	const auto tabsTop = headerTop + headerHeight + st::botStudioTabsSkip;
	_tabs->moveToLeft(padding.left(), tabsTop);

	const auto contentTop = tabsTop + _tabs->height() + st::botStudioTabsSkip + 4;
	auto contentBottom = height() - padding.bottom();
	if (_aiBar->isVisible()) {
		const auto fieldHeight = _aiField->height();
		const auto barHeight = std::max(fieldHeight, _aiButton->height() + 8);
		_aiBar->setGeometry(
			padding.left(),
			contentBottom - barHeight,
			inner,
			barHeight);
		const auto buttonLeft = inner - _aiButton->width() - 4;
		_aiButton->moveToLeft(buttonLeft, (barHeight - _aiButton->height()) / 2);
		_aiField->resize(buttonLeft - 4, fieldHeight);
		_aiField->moveToLeft(0, (barHeight - fieldHeight) / 2);
		contentBottom -= barHeight + st::botStudioAiBarSkip;
	}
	const auto contentHeight = std::max(contentBottom - contentTop, 60);
	const auto content = QRect(padding.left(), contentTop, inner, contentHeight);
	if (_flowEditor) {
		_flowEditor->setGeometry(content);
	}
	for (const auto tab : { _codeTab.get(), _logsTab.get(), _dataTab.get(), _audienceTab.get() }) {
		tab->setGeometry(content);
	}
	// Audience tab.
	{
		auto left = 0;
		for (const auto button : { _broadcast, _exportCsv, _refreshAudience }) {
			button->moveToLeft(left, 0);
			left += button->width() + st::botStudioButtonSkip;
		}
		const auto statsTop = _broadcast->height() + st::botStudioToolbarSkip;
		_stats->setGeometry(0, statsTop, inner, 84);
		const auto titleTop = statsTop + _stats->height() + 14;
		_leadsTitle->resizeToWidth(inner);
		_leadsTitle->moveToLeft(0, titleTop);
		const auto tableTop = titleTop + _leadsTitle->height() + 8;
		_leadsTable->setGeometry(
			0,
			tableTop,
			inner,
			std::max(contentHeight - tableTop, 40));
	}

	// Code tab.
	{
		auto left = 0;
		for (const auto button : { _templates, _docs, _save, _switchMode }) {
			if (button) {
				button->moveToLeft(left, 0);
				left += button->width() + st::botStudioButtonSkip;
			}
		}
		left += st::botStudioButtonSkip;
		_codeHint->resizeToWidth(std::max(inner - left, 50));
		_codeHint->moveToLeft(
			left,
			(_docs->height() - _codeHint->height()) / 2);
		const auto editorTop = _docs->height() + st::botStudioToolbarSkip;
		_editor->setGeometry(
			0,
			editorTop,
			inner,
			std::max(contentHeight - editorTop, 40));
	}
	// Logs tab.
	{
		_clearLogs->moveToLeft(0, 0);
		const auto left = _clearLogs->width() + st::botStudioButtonSkip * 2;
		_logsHint->resizeToWidth(std::max(inner - left, 50));
		_logsHint->moveToLeft(
			left,
			(_clearLogs->height() - _logsHint->height()) / 2);
		const auto top = _clearLogs->height() + st::botStudioToolbarSkip;
		_logView->setGeometry(0, top, inner, std::max(contentHeight - top, 40));
	}
	// Data tab.
	{
		_refreshData->moveToLeft(0, 0);
		_clearData->moveToLeft(
			_refreshData->width() + st::botStudioButtonSkip,
			0);
		const auto top = _refreshData->height() + st::botStudioToolbarSkip;
		_dataView->setGeometry(0, top, inner, std::max(contentHeight - top, 40));
	}
}

class EmptyPage final : public Ui::RpWidget {
public:
	EmptyPage(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		Fn<void(QString)> create);

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;

private:
	const not_null<Ui::FlatLabel*> _title;
	const not_null<Ui::FlatLabel*> _text;
	const not_null<Ui::RpWidget*> _aiBar;
	const not_null<Ui::InputField*> _idea;
	const not_null<Ui::RoundButton*> _aiButton;
	const not_null<Ui::FlatLabel*> _templatesTitle;
	const not_null<Ui::RpWidget*> _grid;
	int _iconTop = 0;

};

EmptyPage::EmptyPage(
	QWidget *parent,
	not_null<Window::SessionController*> controller,
	Fn<void(QString)> create)
: RpWidget(parent)
, _title(Ui::CreateChild<Ui::FlatLabel>(
	this,
	u"Создайте своего Telegram-бота"_q,
	st::botStudioEmptyTitle))
, _text(Ui::CreateChild<Ui::FlatLabel>(
	this,
	u"Без программирования и серверов: соберите бота из блоков "
		"мышкой или просто опишите его — ИИ-агент сделает всё сам. Бот "
		"работает прямо в Agentgram."_q,
	st::botStudioEmptyText))
, _aiBar(Ui::CreateChild<Ui::RpWidget>(this))
, _idea(Ui::CreateChild<Ui::InputField>(
	_aiBar.get(),
	st::botStudioAiField,
	Ui::InputField::Mode::MultiLine,
	rpl::single(u"Опишите бота: например, «запись на маникюр с выбором "
		"времени»"_q)))
, _aiButton(MakeButton(_aiBar, u"✦ Создать с ИИ"_q, st::botStudioAiButton))
, _templatesTitle(Ui::CreateChild<Ui::FlatLabel>(
	this,
	u"Или начните с шаблона"_q,
	st::botStudioInspectorSection))
, _grid(Ui::CreateChild<Ui::RpWidget>(this)) {
	const auto submit = [=] {
		const auto text = _idea->getLastText().trimmed();
		if (text.isEmpty()) {
			_idea->setFocus();
			return;
		}
		_idea->clear();
		AiAgent::ShowPanel(controller, AiCreatePrompt(text));
	};
	_idea->setSubmitSettings(Ui::InputField::SubmitSettings::Enter);
	_idea->submits() | rpl::on_next(submit, _idea->lifetime());
	_aiButton->setClickedCallback(submit);
	_idea->heightChanges() | rpl::on_next([=] {
		resizeEvent(nullptr);
	}, _idea->lifetime());
	_aiBar->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(_aiBar);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(QPen(st::windowBgActive->c, 1.5));
		p.setBrush(st::aiAgentComposerBg);
		p.drawRoundedRect(
			QRectF(_aiBar->rect()).marginsRemoved({ 1., 1., 1., 1. }),
			st::botStudioAiBarRadius,
			st::botStudioAiBarRadius);
	}, _aiBar->lifetime());
	AddTemplateGrid(_grid, 3, create, nullptr);
}

void EmptyPage::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	p.fillRect(e->rect(), st::windowBg);
	auto hq = PainterHighQualityEnabler(p);
	const auto size = st::botStudioEmptyIconSize;
	const auto rect = QRect((width() - size) / 2, _iconTop, size, size);
	auto gradient = QLinearGradient(rect.topLeft(), rect.bottomRight());
	gradient.setColorAt(0., QColor(0x72, 0xd5, 0xfd));
	gradient.setColorAt(1., QColor(0x66, 0x5f, 0xff));
	p.setPen(Qt::NoPen);
	p.setBrush(gradient);
	p.drawEllipse(rect);
	st::botStudioEmptyIcon.paintInCenter(p, rect);
}

void EmptyPage::resizeEvent(QResizeEvent *e) {
	const auto contentWidth = std::min(width() - 64, 640);
	const auto left = (width() - contentWidth) / 2;
	_title->resizeToWidth(contentWidth);
	_text->resizeToWidth(std::min(contentWidth, 520));
	const auto fieldHeight = _idea->height();
	const auto barHeight = std::max(fieldHeight, _aiButton->height() + 8);
	_grid->resizeToWidth(contentWidth);
	const auto total = st::botStudioEmptyIconSize
		+ st::aiAgentEmptyTitleSkip
		+ _title->height()
		+ st::aiAgentEmptyTextSkip
		+ _text->height()
		+ 20
		+ barHeight
		+ 28
		+ _templatesTitle->height()
		+ 10
		+ _grid->height();
	auto top = std::max((height() - total) / 2, 16);
	_iconTop = top;
	top += st::botStudioEmptyIconSize + st::aiAgentEmptyTitleSkip;
	_title->moveToLeft(left, top);
	top += _title->height() + st::aiAgentEmptyTextSkip;
	_text->moveToLeft((width() - _text->width()) / 2, top);
	top += _text->height() + 20;
	_aiBar->setGeometry(left, top, contentWidth, barHeight);
	const auto buttonLeft = contentWidth - _aiButton->width() - 4;
	_aiButton->moveToLeft(buttonLeft, (barHeight - _aiButton->height()) / 2);
	_idea->resize(buttonLeft - 4, fieldHeight);
	_idea->moveToLeft(0, (barHeight - fieldHeight) / 2);
	top += barHeight + 28;
	_templatesTitle->resizeToWidth(contentWidth);
	_templatesTitle->moveToLeft(left, top);
	top += _templatesTitle->height() + 10;
	_grid->moveToLeft(left, top);
	update();
}

class StudioWidget final : public Ui::RpWidget {
public:
	StudioWidget(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		not_null<Ui::LayerManager*> layers);

	void selectBot(const QString &id);
	void createBot(const QString &preselected = QString());

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;

private:
	void rebuildList();
	void showPage(bool force = false);
	void updateGeometry();

	const not_null<Window::SessionController*> _controller;
	const not_null<Studio*> _studio;
	const not_null<Ui::LayerManager*> _layers;
	const not_null<Ui::FlatLabel*> _title;
	const not_null<Ui::FlatLabel*> _subtitle;
	const not_null<Ui::RoundButton*> _newBot;
	const not_null<Ui::ScrollArea*> _scroll;
	const not_null<Ui::VerticalLayout*> _list;
	std::vector<not_null<BotRow*>> _rows;
	QString _selected;
	base::unique_qptr<Ui::RpWidget> _page;
	QString _pageId;

};

StudioWidget::StudioWidget(
	QWidget *parent,
	not_null<Window::SessionController*> controller,
	not_null<Ui::LayerManager*> layers)
: RpWidget(parent)
, _controller(controller)
, _studio(Studio::Get(&controller->session()))
, _layers(layers)
, _title(Ui::CreateChild<Ui::FlatLabel>(
	this,
	u"Bot Studio"_q,
	st::botStudioTitle))
, _subtitle(Ui::CreateChild<Ui::FlatLabel>(
	this,
	u"Конструктор Telegram-ботов"_q,
	st::botStudioSubtitle))
, _newBot(MakeButton(this, u"＋ Новый бот"_q, st::botStudioNewButton))
, _scroll(Ui::CreateChild<Ui::ScrollArea>(this, st::defaultSolidScroll))
, _list(_scroll->setOwnedWidget(object_ptr<Ui::VerticalLayout>(_scroll))) {
	_newBot->setClickedCallback([=] { createBot(); });
	_studio->botsChanged(
	) | rpl::on_next([=] {
		rebuildList();
		if (!_studio->bot(_selected)) {
			_selected = _studio->bots().empty()
				? QString()
				: _studio->bots().front().id;
		}
		showPage();
	}, lifetime());
	_studio->botUpdated(
	) | rpl::on_next([=](const QString &id) {
		for (const auto row : _rows) {
			if (row->id() == id) {
				row->update();
			}
		}
	}, lifetime());
	rebuildList();
	if (!_studio->bots().empty()) {
		_selected = _studio->bots().front().id;
	}
	showPage();
}

void StudioWidget::selectBot(const QString &id) {
	if (id.isEmpty() || !_studio->bot(id)) {
		return;
	}
	_selected = id;
	showPage();
}

void StudioWidget::createBot(const QString &preselected) {
	const auto weak = QPointer<StudioWidget>(this);
	_layers->showBox(Box(
		NewBotBox,
		_controller,
		_studio,
		preselected,
		[=](QString id) {
			if (const auto strong = weak.data()) {
				strong->selectBot(id);
			}
		}));
}

void StudioWidget::rebuildList() {
	while (_list->count()) {
		delete _list->widgetAt(0);
	}
	_rows.clear();
	for (const auto &bot : _studio->bots()) {
		const auto row = _list->add(
			object_ptr<BotRow>(_list, _studio, bot.id),
			QMargins(0, 0, 0, 4));
		row->setSelected(bot.id == _selected);
		const auto id = bot.id;
		row->setClickedCallback([=] { selectBot(id); });
		_rows.push_back(row);
	}
	_list->resizeToWidth(_scroll->width());
}

void StudioWidget::showPage(bool force) {
	const auto id = _studio->bot(_selected) ? _selected : QString();
	if (_page && _pageId == id && !force) {
		return;
	}
	_pageId = id;
	for (const auto row : _rows) {
		row->setSelected(row->id() == id);
	}
	_page = nullptr;
	if (id.isEmpty()) {
		const auto weak = QPointer<StudioWidget>(this);
		_page = base::make_unique_q<EmptyPage>(this, _controller, [=](
				QString templateId) {
			if (const auto strong = weak.data()) {
				strong->createBot(templateId);
			}
		});
	} else {
		const auto weak = QPointer<StudioWidget>(this);
		_page = base::make_unique_q<BotPage>(
			this,
			_controller,
			_studio,
			_layers,
			id,
			[=] {
				if (const auto strong = weak.data()) {
					strong->showPage(true);
				}
			});
	}
	_page->show();
	updateGeometry();
}

void StudioWidget::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	p.fillRect(
		QRect(0, 0, st::botStudioSidebarWidth, height()),
		st::botStudioSidebarBg);
	auto hq = PainterHighQualityEnabler(p);
	const auto &padding = st::botStudioSidebarPadding;
	const auto size = st::botStudioLogoSize;
	const auto logo = QRect(
		padding.left(),
		padding.top() + (_title->height() + _subtitle->height() + 2 - size) / 2,
		size,
		size);
	auto gradient = QLinearGradient(logo.topLeft(), logo.bottomRight());
	gradient.setColorAt(0., QColor(0x72, 0xd5, 0xfd));
	gradient.setColorAt(1., QColor(0x66, 0x5f, 0xff));
	p.setPen(Qt::NoPen);
	p.setBrush(gradient);
	p.drawRoundedRect(logo, size / 3., size / 3.);
	st::aiAgentAvatarIcon.paintInCenter(p, logo);
}

void StudioWidget::resizeEvent(QResizeEvent *e) {
	updateGeometry();
}

void StudioWidget::updateGeometry() {
	const auto &padding = st::botStudioSidebarPadding;
	const auto sidebar = st::botStudioSidebarWidth;
	const auto inner = sidebar - padding.left() - padding.right();
	const auto textLeft = padding.left() + st::botStudioLogoSize + 10;
	_title->resizeToWidth(sidebar - textLeft - padding.right());
	_title->moveToLeft(textLeft, padding.top());
	_subtitle->resizeToWidth(sidebar - textLeft - padding.right());
	_subtitle->moveToLeft(textLeft, _title->y() + _title->height() + 2);
	_newBot->resizeToWidth(inner);
	_newBot->moveToLeft(
		padding.left(),
		_subtitle->y() + _subtitle->height() + st::botStudioNewSkip + 4);
	const auto listTop = _newBot->y()
		+ _newBot->height()
		+ st::botStudioListTop;
	_scroll->setGeometry(
		padding.left() / 2,
		listTop,
		sidebar - padding.left(),
		height() - listTop - padding.bottom());
	_list->resizeToWidth(_scroll->width());
	if (_page) {
		_page->setGeometry(sidebar, 0, width() - sidebar, height());
	}
	update();
}

struct WindowState {
	std::unique_ptr<Ui::RpWindow> window;
	std::unique_ptr<Ui::LayerManager> layers;
	QPointer<StudioWidget> widget;
};

} // namespace

void ShowWindow(
		not_null<Window::SessionController*> controller,
		const QString &botId) {
	static auto states = base::flat_map<
		not_null<Main::Session*>,
		std::unique_ptr<WindowState>>();
	const auto session = &controller->session();
	auto i = states.find(session);
	if (i == end(states)) {
		i = states.emplace(session, std::make_unique<WindowState>()).first;
		session->lifetime().add([=] {
			states.remove(session);
		});
	}
	const auto state = i->second.get();
	if (!state->window) {
		state->window = std::make_unique<Ui::RpWindow>();
		const auto window = state->window.get();
		window->setWindowTitle(u"Agentgram — Bot Studio"_q);
		window->setMinimumSize(st::botStudioWindowMinSize);
		window->setAttribute(Qt::WA_DeleteOnClose, false);
		state->layers = std::make_unique<Ui::LayerManager>(window->body());
		const auto widget = Ui::CreateChild<StudioWidget>(
			window->body().get(),
			controller,
			state->layers.get());
		state->widget = widget;
		window->body()->sizeValue(
		) | rpl::on_next([=](QSize size) {
			widget->setGeometry(QRect(QPoint(), size));
		}, widget->lifetime());
		window->events(
		) | rpl::filter([](not_null<QEvent*> e) {
			return (e->type() == QEvent::Close);
		}) | rpl::on_next([=] {
			crl::on_main(session, [=] {
				state->widget = nullptr;
				state->layers = nullptr;
				state->window = nullptr;
			});
		}, window->lifetime());
		const auto main = controller->widget()->geometry();
		const auto size = st::botStudioWindowSize;
		window->setGeometry(QRect(
			main.x() + (main.width() - size.width()) / 2,
			main.y() + (main.height() - size.height()) / 2,
			size.width(),
			size.height()));
		window->show();
	}
	if (const auto widget = state->widget.data()) {
		widget->selectBot(botId);
	}
	state->window->raise();
	state->window->activateWindow();
}

} // namespace BotStudio
