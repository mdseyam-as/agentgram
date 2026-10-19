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
	Fn<void()> selected_changed;
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
	} else if (kind == u"location"_q) {
		return u"адрес"_q;
	} else if (kind == u"photo"_q) {
		return u"фото"_q;
	}
	return u"текст"_q;
}

[[nodiscard]] QString BlockType(const QJsonObject &block) {
	const auto type = block.value(u"type"_q).toString();
	return type.isEmpty() ? u"message"_q : type;
}

struct BlockKind {
	QString type;
	QString title;
	QString about;
	QString badge;
};

[[nodiscard]] const std::vector<BlockKind> &BlockKinds() {
	static const auto result = std::vector<BlockKind>{
		{ u"message"_q, u"💬 Сообщение"_q, u"текст, фото, кнопки"_q, QString() },
		{ u"condition"_q, u"⑂ Условие"_q, u"ответы, подписка, время"_q, u"⑂ условие"_q },
		{ u"ai"_q, u"✦ Ответ ИИ"_q, u"текст пишет нейросеть"_q, u"✦ ИИ"_q },
		{ u"action"_q, u"⚡ Действия"_q, u"баллы, метки, таймеры"_q, u"⚡ действия"_q },
		{ u"random"_q, u"🎲 Случайный выбор"_q, u"розыгрыш, A/B-тест"_q, u"🎲 случайно"_q },
		{ u"operator"_q, u"🎧 Чат с оператором"_q, u"живой чат с клиентом"_q, u"🎧 оператор"_q },
		{ u"catalog"_q, u"🛍 Каталог"_q, u"товары и цены"_q, u"🛍 каталог"_q },
		{ u"cart"_q, u"🧺 Корзина"_q, u"сумма и оформление"_q, u"🧺 корзина"_q },
		{ u"order"_q, u"📦 Заказ и оплата"_q, u"оплата звёздами, картой"_q, u"📦 заказ"_q },
	};
	return result;
}

[[nodiscard]] const BlockKind &KindOf(const QString &type) {
	for (const auto &kind : BlockKinds()) {
		if (kind.type == type) {
			return kind;
		}
	}
	return BlockKinds().front();
}

[[nodiscard]] bool IsShopBlock(const QString &type) {
	return (type == u"catalog"_q)
		|| (type == u"cart"_q)
		|| (type == u"order"_q);
}

[[nodiscard]] bool HasShop(const QJsonObject &flow) {
	for (const auto &value : FlowBlocks(flow)) {
		if (IsShopBlock(BlockType(value.toObject()))) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] QStringList KnownVariables(const QJsonObject &flow) {
	auto result = QStringList{ u"first_name"_q, u"name"_q, u"username"_q };
	const auto add = [&](const QString &name) {
		if (!name.isEmpty() && !result.contains(name)) {
			result.push_back(name);
		}
	};
	for (const auto &value : FlowBlocks(flow)) {
		const auto block = value.toObject();
		const auto input = Input(block);
		if (input.value(u"enabled"_q).toBool()) {
			add(input.value(u"variable"_q).toString());
		}
		add(block.value(u"saveChoice"_q).toString());
		const auto type = BlockType(block);
		if (type == u"ai"_q) {
			add(block.value(u"variable"_q).toString());
		} else if (type == u"order"_q) {
			add(u"order_id"_q);
			add(u"order_total"_q);
			add(u"order_items"_q);
		}
		for (const auto &action : block.value(u"actions"_q).toArray()) {
			const auto object = action.toObject();
			const auto kind = object.value(u"kind"_q).toString();
			if (kind == u"set"_q
				|| kind == u"increment"_q
				|| kind == u"calc"_q
				|| kind == u"request"_q) {
				add(object.value(u"variable"_q).toString());
			}
		}
	}
	return result;
}

[[nodiscard]] QString ShopCurrency(const QJsonObject &flow) {
	const auto currency = flow.value(u"shop"_q).toObject().value(
		u"currency"_q).toString();
	return currency.isEmpty() ? u"RUB"_q : currency;
}

[[nodiscard]] QString ProductTitle(
		const QJsonObject &flow,
		const QString &key) {
	for (const auto &value : flow.value(u"products"_q).toArray()) {
		const auto product = value.toObject();
		if (FlowProductKey(product) == key) {
			return product.value(u"title"_q).toString();
		}
	}
	return QString();
}

[[nodiscard]] QStringList ProductCategories(const QJsonObject &flow) {
	auto result = QStringList();
	for (const auto &value : flow.value(u"products"_q).toArray()) {
		const auto category = value.toObject().value(
			u"category"_q).toString().trimmed();
		if (!category.isEmpty() && !result.contains(category)) {
			result.push_back(category);
		}
	}
	return result;
}

// One line inside a node: a button, a condition or an action. A row with a
// target draws an arrow to that block.
struct NodeRow {
	QString text;
	QString target;
	bool warning = false;
	bool link = false;
	bool button = false;
};

struct NodeLayout {
	QRect rect;
	QRect textRect;
	std::vector<QRect> buttons;
	QRect footer;
	QPoint nextAnchor;
};

[[nodiscard]] QString OperatorText(const QString &op) {
	if (op == u"neq"_q) {
		return u"≠"_q;
	} else if (op == u"contains"_q) {
		return u"содержит"_q;
	} else if (op == u"gt"_q) {
		return u">"_q;
	} else if (op == u"lt"_q) {
		return u"<"_q;
	} else if (op == u"filled"_q) {
		return u"заполнено"_q;
	} else if (op == u"empty"_q) {
		return u"пусто"_q;
	} else if (op == u"member"_q) {
		return u"подписан на канал"_q;
	} else if (op == u"tag"_q) {
		return u"есть метка"_q;
	} else if (op == u"hours"_q) {
		return u"рабочее время"_q;
	}
	return u"="_q;
}

// Conditions that don't look at an answer.
[[nodiscard]] bool OperatorWithoutVariable(const QString &op) {
	return (op == u"member"_q) || (op == u"tag"_q) || (op == u"hours"_q);
}

[[nodiscard]] QString UnitText(const QString &unit) {
	return (unit == u"hour"_q)
		? u"ч"_q
		: (unit == u"day"_q)
		? u"дн"_q
		: u"мин"_q;
}

[[nodiscard]] QString ActionTitle(const QString &kind) {
	if (kind == u"set"_q) {
		return u"✎ Записать значение"_q;
	} else if (kind == u"increment"_q) {
		return u"➕ Прибавить к числу"_q;
	} else if (kind == u"calc"_q) {
		return u"🧮 Посчитать по формуле"_q;
	} else if (kind == u"clear"_q) {
		return u"🧹 Очистить ответы"_q;
	} else if (kind == u"notify"_q) {
		return u"📩 Написать мне"_q;
	} else if (kind == u"send"_q) {
		return u"📢 Отправить в чат или канал"_q;
	} else if (kind == u"webhook"_q) {
		return u"🌐 Отправить данные на сайт"_q;
	} else if (kind == u"request"_q) {
		return u"🔗 Запрос к API и сохранить ответ"_q;
	} else if (kind == u"save_lead"_q) {
		return u"🗂 Сохранить заявку в таблицу"_q;
	} else if (kind == u"tag"_q) {
		return u"🏷 Добавить метку"_q;
	} else if (kind == u"untag"_q) {
		return u"🏷 Снять метку"_q;
	} else if (kind == u"remind"_q) {
		return u"⏰ Напомнить позже"_q;
	} else if (kind == u"cancel_reminders"_q) {
		return u"🔕 Отменить напоминания"_q;
	} else if (kind == u"add_to_cart"_q) {
		return u"🛒 Положить товар в корзину"_q;
	} else if (kind == u"clear_cart"_q) {
		return u"🧺 Очистить корзину"_q;
	}
	return kind;
}

[[nodiscard]] QString ActionText(
		const QJsonObject &flow,
		const QJsonObject &action) {
	const auto kind = action.value(u"kind"_q).toString();
	const auto variable = action.value(u"variable"_q).toString();
	const auto value = action.value(u"value"_q).toString();
	if (kind == u"set"_q) {
		return u"✎ {%1} = %2"_q.arg(variable, value);
	} else if (kind == u"increment"_q) {
		return u"➕ {%1} + %2"_q.arg(variable, value.isEmpty() ? u"1"_q : value);
	} else if (kind == u"calc"_q) {
		return u"🧮 {%1} = %2"_q.arg(variable, value);
	} else if (kind == u"send"_q) {
		return u"📢 В чат "_q + action.value(u"chat"_q).toString();
	} else if (kind == u"webhook"_q) {
		return u"🌐 Вебхук "_q + QUrl(action.value(u"url"_q).toString()).host();
	} else if (kind == u"request"_q) {
		return u"🔗 "_q
			+ QUrl(action.value(u"url"_q).toString()).host()
			+ (variable.isEmpty() ? QString() : (u" → {"_q + variable + '}'));
	} else if (kind == u"tag"_q) {
		return u"🏷 + "_q + value;
	} else if (kind == u"untag"_q) {
		return u"🏷 − "_q + value;
	} else if (kind == u"remind"_q) {
		return u"⏰ через %1 %2"_q.arg(
			value.isEmpty() ? u"1"_q : value,
			UnitText(action.value(u"unit"_q).toString()));
	} else if (kind == u"add_to_cart"_q) {
		const auto title = ProductTitle(
			flow,
			action.value(u"product"_q).toString());
		return u"🛒 "_q + (title.isEmpty() ? u"⚠ выберите товар"_q : title);
	}
	return ActionTitle(kind);
}

[[nodiscard]] std::vector<NodeRow> ButtonRows(const QJsonObject &block) {
	auto result = std::vector<NodeRow>();
	for (const auto &value : Buttons(block)) {
		const auto button = value.toObject();
		const auto url = button.value(u"url"_q).toString();
		const auto target = button.value(u"goto"_q).toString();
		result.push_back({
			button.value(u"text"_q).toString(),
			target,
			url.isEmpty() && target.isEmpty() && block.value(
				u"saveChoice"_q).toString().isEmpty(),
			!url.isEmpty(),
			true,
		});
	}
	return result;
}

[[nodiscard]] QString PaymentText(const QString &payment) {
	return (payment == u"stars"_q)
		? u"⭐ Оплата звёздами Telegram"_q
		: (payment == u"card"_q)
		? u"💳 Оплата картой"_q
		: u"💵 Оплата при получении"_q;
}

[[nodiscard]] std::vector<NodeRow> BlockRows(
		const QJsonObject &flow,
		const QJsonObject &block) {
	auto result = std::vector<NodeRow>();
	const auto type = BlockType(block);
	if (type == u"condition"_q) {
		for (const auto &value : block.value(u"conditions"_q).toArray()) {
			const auto condition = value.toObject();
			const auto op = condition.value(u"op"_q).toString();
			auto text = OperatorWithoutVariable(op)
				? (u"если "_q + OperatorText(op))
				: (u"если {"_q
					+ condition.value(u"variable"_q).toString()
					+ u"} "_q
					+ OperatorText(op));
			if (op != u"filled"_q && op != u"empty"_q) {
				text += ' ' + condition.value(u"value"_q).toString();
			}
			const auto target = condition.value(u"goto"_q).toString();
			result.push_back({ text, target, target.isEmpty() });
		}
		const auto otherwise = block.value(u"else"_q).toString();
		result.push_back({ u"иначе"_q, otherwise, false });
		return result;
	} else if (type == u"random"_q) {
		const auto branches = block.value(u"branches"_q).toArray();
		auto total = 0.;
		for (const auto &value : branches) {
			total += std::max(value.toObject().value(u"weight"_q).toDouble(1.), 0.);
		}
		for (const auto &value : branches) {
			const auto branch = value.toObject();
			const auto weight = std::max(branch.value(u"weight"_q).toDouble(1.), 0.);
			const auto target = branch.value(u"goto"_q).toString();
			result.push_back({
				u"🎲 %1%"_q.arg(total > 0 ? qRound(weight * 100. / total) : 0),
				target,
				target.isEmpty(),
			});
		}
		return result;
	} else if (type == u"action"_q) {
		for (const auto &value : block.value(u"actions"_q).toArray()) {
			const auto action = value.toObject();
			const auto remind = (action.value(u"kind"_q).toString()
				== u"remind"_q);
			const auto target = remind
				? action.value(u"goto"_q).toString()
				: QString();
			result.push_back({
				ActionText(flow, action),
				target,
				remind && target.isEmpty(),
			});
		}
		return result;
	} else if (type == u"catalog"_q) {
		const auto category = block.value(u"category"_q).toString().trimmed();
		auto count = 0;
		for (const auto &value : flow.value(u"products"_q).toArray()) {
			if (category.isEmpty()
				|| value.toObject().value(u"category"_q).toString().trimmed()
					== category) {
				++count;
			}
		}
		result.push_back({
			(category.isEmpty() ? u"📦 Товаров: %1"_q : (category + u" · %1"_q)).arg(count),
			QString(),
			count == 0,
		});
		const auto cart = block.value(u"cart"_q).toString();
		if (!cart.isEmpty()) {
			result.push_back({ u"🧺 Корзина"_q, cart });
		}
	} else if (type == u"cart"_q) {
		const auto checkout = block.value(u"checkout"_q).toString();
		result.push_back({ u"✅ Оформить заказ"_q, checkout, checkout.isEmpty() });
	} else if (type == u"order"_q) {
		const auto payment = block.value(u"payment"_q).toString();
		result.push_back({ PaymentText(payment) });
		if (payment == u"stars"_q || payment == u"card"_q) {
			result.push_back({ u"✅ После оплаты"_q, block.value(u"paid"_q).toString() });
		}
	} else if (type == u"operator"_q) {
		const auto chat = block.value(u"chat"_q).toString().trimmed();
		result.push_back({ chat.isEmpty()
			? u"🎧 Вопросы приходят вам"_q
			: (u"🎧 Вопросы в "_q + chat) });
	}
	const auto buttons = ButtonRows(block);
	result.insert(end(result), begin(buttons), end(buttons));
	return result;
}

[[nodiscard]] QString BlockPreview(const QJsonObject &block) {
	const auto type = BlockType(block);
	const auto text = block.value(u"text"_q).toString().trimmed();
	if (type == u"condition"_q) {
		return u"Проверяет и выбирает ветку"_q;
	} else if (type == u"random"_q) {
		return u"Выбирает ветку случайно"_q;
	} else if (type == u"action"_q) {
		return block.value(u"actions"_q).toArray().isEmpty()
			? u"Добавьте действия"_q
			: QString();
	} else if (type == u"ai"_q) {
		const auto prompt = block.value(u"prompt"_q).toString().trimmed();
		return u"✦ "_q + (prompt.isEmpty()
			? u"Отвечает ИИ на последнее сообщение"_q
			: prompt);
	} else if (type == u"catalog"_q) {
		return text.isEmpty() ? u"Товары с ценами и кнопкой «В корзину»"_q : text;
	} else if (type == u"cart"_q) {
		return text.isEmpty() ? u"Товары в корзине и сумма"_q : text;
	} else if (type == u"order"_q) {
		return text.isEmpty() ? u"Создаёт заказ из корзины"_q : text;
	} else if (type == u"operator"_q) {
		return text.isEmpty() ? u"Клиент пишет — вы отвечаете"_q : text;
	}
	auto result = text;
	const auto media = block.value(u"media"_q).toObject();
	const auto kind = media.value(u"kind"_q).toString();
	if (!media.value(u"url"_q).toString().isEmpty()
		|| !block.value(u"photo"_q).toString().trimmed().isEmpty()) {
		result = ((kind == u"video"_q)
			? u"🎬 "_q
			: (kind == u"document"_q)
			? u"📎 "_q
			: (kind == u"audio"_q)
			? u"🎵 "_q
			: u"🖼 "_q) + result;
	}
	return result;
}

[[nodiscard]] QString NextTarget(const QJsonObject &block) {
	const auto type = BlockType(block);
	if (type == u"condition"_q
		|| type == u"random"_q
		|| type == u"catalog"_q
		|| type == u"cart"_q) {
		return QString();
	} else if (type == u"order"_q) {
		const auto payment = block.value(u"payment"_q).toString();
		return (payment == u"stars"_q || payment == u"card"_q)
			? QString()
			: block.value(u"next"_q).toString();
	} else if (type == u"message"_q && HasInput(block)) {
		return Input(block).value(u"next"_q).toString();
	}
	return block.value(u"next"_q).toString();
}

[[nodiscard]] QColor BlockColor(const QJsonObject &block, bool isStart) {
	const auto type = BlockType(block);
	if (isStart) {
		return kStartColor;
	} else if (type == u"condition"_q) {
		return QColor(0x2a, 0xb3, 0xc7);
	} else if (type == u"action"_q) {
		return QColor(0x8a, 0x8f, 0x99);
	} else if (type == u"ai"_q) {
		return QColor(0x66, 0x5f, 0xff);
	} else if (type == u"random"_q) {
		return QColor(0xd9, 0x4f, 0x9e);
	} else if (type == u"operator"_q) {
		return QColor(0x1f, 0xa3, 0x8a);
	} else if (type == u"catalog"_q) {
		return QColor(0xf0, 0x62, 0x3e);
	} else if (type == u"cart"_q) {
		return QColor(0xc2, 0x47, 0x7a);
	} else if (type == u"order"_q) {
		return QColor(0xd4, 0xa0, 0x17);
	} else if (HasInput(block)) {
		return kInputColor;
	} else if (block.value(u"notifyOwner"_q).toBool()) {
		return kNotifyColor;
	}
	return st::windowBgActive->c;
}

// Sets fields a block of this type needs, keeps the rest.
void ApplyTypeDefaults(QJsonObject &block, const QString &type) {
	if (type == u"message"_q) {
		block.remove(u"type"_q);
	} else {
		block.insert(u"type"_q, type);
	}
	const auto setDefault = [&](const QString &key, QJsonValue value) {
		if (!block.contains(key)) {
			block.insert(key, value);
		}
	};
	if (type == u"condition"_q) {
		setDefault(u"conditions"_q, QJsonArray{ QJsonObject{
			{ u"variable"_q, u"ответ"_q },
			{ u"op"_q, u"eq"_q },
			{ u"value"_q, QString() },
		} });
	} else if (type == u"ai"_q) {
		setDefault(u"prompt"_q, u"{last_message}"_q);
	} else if (type == u"action"_q) {
		setDefault(u"actions"_q, QJsonArray());
	} else if (type == u"random"_q) {
		setDefault(u"branches"_q, QJsonArray{
			QJsonObject{ { u"weight"_q, 50 } },
			QJsonObject{ { u"weight"_q, 50 } },
		});
	} else if (type == u"catalog"_q) {
		setDefault(u"text"_q, u"🛍 Выберите категорию:"_q);
	} else if (type == u"cart"_q) {
		setDefault(u"text"_q, u"🧺 Ваша корзина:"_q);
	} else if (type == u"order"_q) {
		setDefault(u"payment"_q, u"none"_q);
		if (block.value(u"text"_q).toString().isEmpty()) {
			block.insert(u"text"_q, u"🎉 Заказ №{order_id} на {order_total} "
				"принят! Мы скоро свяжемся с вами."_q);
		}
	} else if (type == u"operator"_q) {
		if (block.value(u"text"_q).toString().isEmpty()) {
			block.insert(u"text"_q, u"💬 Напишите ваш вопрос — оператор "
				"ответит прямо здесь."_q);
		}
	}
}

[[nodiscard]] QString DefaultTitle(const QString &type, int number) {
	const auto title = KindOf(type).title;
	return u"%1 %2"_q.arg(title.mid(title.indexOf(' ') + 1)).arg(number);
}

} // namespace

class FlowCanvas final : public Ui::RpWidget {
public:
	FlowCanvas(QWidget *parent, std::shared_ptr<FlowState> state);

	void refresh();
	void setViewport(QSize size) {
		_viewport = size;
		refresh();
	}
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
	QSize _viewport;
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
	const auto minWidth = _viewport.width();
	const auto minHeight = _viewport.height();
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
	const auto preview = BlockPreview(block);
	if (!preview.isEmpty() || BlockType(block) == u"message"_q) {
		const auto text = preview.isEmpty()
			? u"Пустое сообщение"_q
			: preview;
		const auto bounding = QFontMetrics(font->f).boundingRect(
			QRect(0, 0, inner, font->height * kMaxTextLines),
			Qt::TextWordWrap,
			text);
		const auto textHeight = std::min(
			bounding.height(),
			font->height * kMaxTextLines);
		result.textRect = QRect(x + padding.left(), y + top, inner, textHeight);
		top += textHeight;
	}

	const auto rows = BlockRows(_state->flow, block);
	if (!rows.empty()) {
		top += st::botStudioNodeButtonSkip * 2;
	}
	for (auto i = 0; i != int(rows.size()); ++i) {
		result.buttons.push_back(QRect(
			x + padding.left(),
			y + top,
			inner,
			st::botStudioNodeButtonHeight));
		top += st::botStudioNodeButtonHeight + st::botStudioNodeButtonSkip;
	}
	const auto hasFooter = (BlockType(block) == u"message"_q && HasInput(block))
		|| !NextTarget(block).isEmpty()
		|| block.value(u"delay"_q).toDouble() > 0;
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
	const auto back = (to.x() <= from.x());
	auto pen = QPen(
		WithAlpha(st::botStudioArrowFg->c, back ? 70 : 150),
		st::botStudioArrowWidth);
	if (dashed) {
		pen.setDashPattern({ 3., 3. });
	}
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	p.drawPath(path);
	p.setPen(Qt::NoPen);
	p.setBrush(WithAlpha(st::botStudioArrowFg->c, back ? 90 : 190));
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
	const auto type = BlockType(block);
	const auto selected = (id == _state->selected);
	const auto isStart = (id == _state->flow.value(u"start"_q).toString());
	const auto rect = layout.rect;
	const auto radius = st::botStudioNodeRadius;
	const auto stripe = BlockColor(block, isStart);

	// Soft shadow.
	p.setPen(Qt::NoPen);
	p.setBrush(WithAlpha(QColor(0, 0, 0), selected ? 46 : 26));
	p.drawRoundedRect(rect.translated(0, 3), radius, radius);

	p.setBrush(st::botStudioCardBg);
	p.setPen(selected
		? QPen(st::windowBgActive->c, 2)
		: QPen(WithAlpha(st::botStudioCardBorder->c, 120), 1));
	p.drawRoundedRect(rect, radius, radius);

	p.save();
	auto clip = QPainterPath();
	clip.addRoundedRect(rect, radius, radius);
	p.setClipPath(clip);
	p.fillRect(
		QRect(rect.x(), rect.y(), rect.width(), st::botStudioNodeStripe),
		stripe);
	if (type != u"message"_q) {
		// Tinted body tells service blocks from messages.
		p.fillRect(
			rect.marginsRemoved({ 0, st::botStudioNodeStripe, 0, 0 }),
			WithAlpha(stripe, 18));
	}
	p.restore();

	const auto &padding = st::botStudioNodePadding;
	const auto inner = rect.width() - padding.left() - padding.right();
	const auto titleFont = style::font(st::botStudioNodeTitleFont);
	const auto badgeFont = style::font(st::botStudioNodeBadgeFont);
	const auto titleTop = rect.y() + st::botStudioNodeStripe + padding.top();

	auto badges = QStringList();
	if (isStart) {
		badges.push_back(u"▶ старт"_q);
	}
	if (const auto &badge = KindOf(type).badge; !badge.isEmpty()) {
		badges.push_back(badge);
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
	p.drawText(
		rect.x() + padding.left(),
		titleTop + titleFont->ascent,
		titleFont->elided(
			FlowBlockTitle(_state->flow, id),
			badgeRight - rect.x() - padding.left() - 4));

	if (!layout.textRect.isEmpty()) {
		auto text = BlockPreview(block);
		p.setFont(st::botStudioNodeTextFont);
		p.setPen(text.isEmpty() ? st::attentionButtonFg : st::windowSubTextFg);
		if (text.isEmpty()) {
			text = u"Пустое сообщение"_q;
		}
		p.drawText(
			layout.textRect,
			Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop,
			text);
	}

	const auto rows = BlockRows(_state->flow, block);
	const auto buttonFont = style::font(st::botStudioNodeButtonFont);
	p.setFont(buttonFont);
	for (auto i = 0; i != int(layout.buttons.size()); ++i) {
		const auto &row = rows[i];
		const auto r = layout.buttons[i];
		p.setPen(Qt::NoPen);
		p.setBrush(st::botStudioNodeButtonBg);
		p.drawRoundedRect(r, r.height() / 2., r.height() / 2.);
		const auto suffix = row.link
			? u"  🔗"_q
			: row.warning
			? u"  ⚠"_q
			: QString();
		p.setPen(row.warning
			? st::attentionButtonFg->c
			: (row.button || !row.target.isEmpty())
			? st::windowActiveTextFg->c
			: st::windowFg->c);
		p.drawText(
			r.marginsRemoved({ 10, 0, 10, 0 }),
			Qt::AlignVCenter
				| (row.button ? Qt::AlignHCenter : Qt::AlignLeft),
			buttonFont->elided(row.text + suffix, inner - 20));
	}

	if (!layout.footer.isEmpty()) {
		p.setFont(badgeFont);
		auto parts = QStringList();
		const auto delay = block.value(u"delay"_q).toDouble();
		if (delay > 0) {
			parts.push_back(u"⏱ %1 с"_q.arg(delay));
		}
		if (type == u"message"_q && HasInput(block)) {
			const auto input = Input(block);
			parts.push_back(u"✍ ждёт: "_q
				+ KindName(input.value(u"kind"_q).toString())
				+ u" → {"_q
				+ input.value(u"variable"_q).toString()
				+ u"}"_q);
		} else if (!NextTarget(block).isEmpty()) {
			parts.push_back(u"⏬ затем: "_q
				+ FlowBlockTitle(_state->flow, NextTarget(block)));
		}
		p.setPen((type == u"message"_q && HasInput(block))
			? kInputColor
			: st::windowSubTextFg->c);
		p.drawText(
			layout.footer,
			Qt::AlignVCenter | Qt::AlignLeft,
			badgeFont->elided(parts.join(u"  "_q), inner));
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
		const auto rows = BlockRows(_state->flow, block);
		for (auto i = 0; i != int(l.buttons.size()); ++i) {
			if (const auto rect = targetRect(rows[i].target)) {
				const auto r = l.buttons[i];
				paintArrow(
					p,
					QPoint(l.rect.x() + l.rect.width(), r.center().y()),
					*rect,
					false);
			}
		}
		if (const auto rect = targetRect(NextTarget(block))) {
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
			u"Двойной щелчок по пустому месту — новый блок"_q);
	}
}

void FlowCanvas::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto id = nodeAt(e->pos());
	if (id != _state->selected) {
		_state->selected = id;
		_state->selected_changed();
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
		_state->selected_changed();
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
, _add(MakeButton(this, u"＋ Блок"_q, st::botStudioSmallButton))
, _settings(MakeButton(this, u"⚙ Команды и ответы"_q, st::botStudioSmallButton))
, _products(MakeButton(this, u"🛍 Товары"_q, st::botStudioSmallButton))
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
	_state->selected_changed = [=] {
		_canvas->update();
		rebuildInspector();
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
		const auto menu = Ui::CreateChild<Ui::PopupMenu>(
			this,
			st::defaultPopupMenu);
		for (const auto &kind : BlockKinds()) {
			if (kind.type == u"catalog"_q) {
				menu->addSeparator();
			}
			const auto type = kind.type;
			menu->addAction(
				kind.title + u" — "_q + kind.about,
				[=] { addBlock(position, true, type); });
		}
		menu->popup(QCursor::pos());
	});
	_settings->setClickedCallback([=] { showSettings(); });
	_products->setClickedCallback([=] { showProducts(); });
	_save->setClickedCallback([=] { save(); });
}

void FlowEditor::addBlock(QPoint position, bool select, QString type) {
	const auto id = NewFlowBlockId(_state->flow);
	const auto number = FlowBlocks(_state->flow).size() + 1;
	auto block = QJsonObject{
		{ u"id"_q, id },
		{ u"x"_q, std::max(0, (position.x() / kSnap) * kSnap) },
		{ u"y"_q, std::max(0, (position.y() / kSnap) * kSnap) },
	};
	if (type.isEmpty()) {
		type = u"message"_q;
	}
	block.insert(u"title"_q, DefaultTitle(type, number));
	if (type == u"message"_q) {
		block.insert(u"text"_q, QString());
	}
	ApplyTypeDefaults(block, type);
	// Move down until the new card doesn't cover another one.
	const auto blocks = FlowBlocks(_state->flow);
	for (auto attempt = 0; attempt != 60; ++attempt) {
		const auto x = block.value(u"x"_q).toInt();
		const auto y = block.value(u"y"_q).toInt();
		const auto wanted = QRect(x, y, st::botStudioNodeWidth, 150);
		auto busy = false;
		for (const auto &value : blocks) {
			const auto other = value.toObject();
			const auto rect = _canvas->nodeRect(
				other.value(u"id"_q).toString()).translated(
					-st::botStudioCanvasPadding,
					-st::botStudioCanvasPadding);
			if (rect.marginsAdded({ 10, 10, 10, 10 }).intersects(wanted)) {
				busy = true;
				block.insert(u"y"_q, rect.y() + rect.height() + 30);
				break;
			}
		}
		if (!busy) {
			break;
		}
	}
	SetFlowBlock(_state->flow, block);
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
	const auto sameBlock = (_inspected == id);
	_inspected = id;
	auto content = object_ptr<Ui::VerticalLayout>(_inspectorScroll);
	const auto layout = content.data();
	_inspector = layout;
	const auto &padding = st::botStudioInspectorPadding;
	const auto maxWidth = st::botStudioInspectorWidth
		- padding.left()
		- padding.right();
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
	// A wrapped line of choice chips.
	const auto chips = [&](
			std::vector<std::pair<QString, QString>> options,
			const QString &current,
			Fn<void(QString)> chosen,
			int top = 6) {
		const auto line = row(object_ptr<Ui::RpWidget>(layout), top);
		auto left = 0;
		auto y = 0;
		auto height = 0;
		for (const auto &[value, title] : options) {
			const auto chip = MakeButton(
				line,
				title,
				(value == current)
					? st::botStudioTemplateButtonActive
					: st::botStudioTemplateButton);
			if (left && left + chip->width() > maxWidth) {
				left = 0;
				y += chip->height() + 6;
			}
			chip->moveToLeft(left, y);
			left += chip->width() + 6;
			height = y + chip->height();
			const auto v = value;
			chip->setClickedCallback([=] { chosen(v); });
		}
		line->resize(maxWidth, height);
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
	// "Label: → target" line.
	const auto labeledTarget = [&](
			const QString &label,
			const QString &current,
			Fn<void(QString)> chosen) {
		const auto after = row(object_ptr<Ui::RpWidget>(layout), 10);
		const auto afterLabel = Ui::CreateChild<Ui::FlatLabel>(
			after,
			label,
			st::botStudioInspectorLabel);
		const auto afterTarget = target(
			after,
			current,
			true,
			std::move(chosen),
			nullptr);
		afterLabel->moveToLeft(
			0,
			(afterTarget->height() - afterLabel->height()) / 2);
		afterTarget->moveToLeft(afterLabel->width() + 8, 0);
		after->resize(
			afterLabel->width() + 8 + afterTarget->width(),
			afterTarget->height());
	};
	const auto setKey = [=](const QString &key) {
		return [=](QString chosen) {
			update([&](QJsonObject &b) {
				if (chosen.isEmpty()) {
					b.remove(key);
				} else {
					b.insert(key, chosen);
				}
			}, true);
		};
	};
	const auto textField = [&](
			const QString &key,
			const QString &placeholder,
			bool multiline) {
		const auto block = FlowBlock(_state->flow, id);
		return field(
			placeholder,
			block.value(key).toString(),
			multiline,
			[=](QString text) {
				update([&](QJsonObject &b) { b.insert(key, text); }, false);
			});
	};

	const auto block = FlowBlock(_state->flow, id);
	if (block.isEmpty()) {
		section(u"Схема бота"_q, 18);
		hint(u"Выберите блок на схеме, чтобы изменить его, или нажмите "
			"«＋ Блок».\n\nСообщения связываются кнопками: кнопка ведёт к "
			"другому блоку. Для магазина добавьте товары в «🛍 Товары» и "
			"блоки «Каталог», «Корзина» и «Заказ и оплата»."_q);
	} else {
		const auto isStart = (id == _state->flow.value(u"start"_q).toString());
		const auto type = BlockType(block);
		const auto &kind = KindOf(type);
		section(isStart ? (u"▶ Старт · "_q + kind.title) : kind.title, 16);
		hint((type == u"condition"_q)
			? u"Проверяет ответы, подписку на канал, метки или время и ведёт "
				"в нужную ветку. Пользователь этот блок не видит."_q
			: (type == u"action"_q)
			? u"Выполняет действия по порядку: значения, баллы, метки, "
				"напоминания, уведомления, запросы к сайтам."_q
			: (type == u"ai"_q)
			? u"Бот отвечает текстом, который пишет ИИ по вашему заданию."_q
			: (type == u"random"_q)
			? u"Случайно выбирает одну из веток: розыгрыши, призы, A/B-тесты."_q
			: (type == u"operator"_q)
			? u"Живой чат: клиент пишет боту, вы отвечаете ему из Telegram."_q
			: (type == u"catalog"_q)
			? u"Карточки товаров с фото, ценой и кнопкой «В корзину»."_q
			: (type == u"cart"_q)
			? u"Показывает корзину: можно менять количество и оформить заказ."_q
			: (type == u"order"_q)
			? u"Создаёт заказ из корзины, принимает оплату и присылает заказ "
				"вам с кнопками статусов."_q
			: isStart
			? u"Его бот показывает по команде /start."_q
			: u"Показывается, когда пользователь нажал ведущую сюда "
				"кнопку."_q);

		// Block type.
		{
			const auto line = row(object_ptr<Ui::RpWidget>(layout), 10);
			const auto label = Ui::CreateChild<Ui::FlatLabel>(
				line,
				u"Тип блока:"_q,
				st::botStudioInspectorLabel);
			const auto button = MakeButton(
				line,
				kind.title + u"  ▾"_q,
				st::botStudioTargetButton);
			button->setClickedCallback([=] {
				const auto menu = Ui::CreateChild<Ui::PopupMenu>(
					this,
					st::defaultPopupMenu);
				for (const auto &entry : BlockKinds()) {
					const auto chosen = entry.type;
					menu->addAction(
						(chosen == type ? u"✓ "_q : QString())
							+ entry.title
							+ u" — "_q
							+ entry.about,
						[=] {
							update([&](QJsonObject &b) {
								ApplyTypeDefaults(b, chosen);
							}, true);
						});
				}
				menu->popup(QCursor::pos());
			});
			label->moveToLeft(0, (button->height() - label->height()) / 2);
			button->moveToLeft(label->width() + 8, 0);
			line->resize(label->width() + 8 + button->width(), button->height());
		}

		field(
			u"Название (видно только вам)"_q,
			block.value(u"title"_q).toString(),
			false,
			[=](QString text) {
				update([&](QJsonObject &b) { b.insert(u"title"_q, text); }, false);
			});
		auto variables = QStringList();
		for (const auto &name : KnownVariables(_state->flow)) {
			variables.push_back(u"{"_q + name + u"}"_q);
		}
		const auto variablesHint = u"Подстановки: "_q
			+ variables.join(u", "_q)
			+ u", {last_message}, {date}, {time}"_q
			+ (HasShop(_state->flow)
				? u", {cart}, {cart_total}, {cart_count}, {my_orders}."_q
				: u"."_q);

		// Buttons of a block: text and where it leads.
		const auto buttonsEditor = [&](const QString &title) {
			section(title);
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
						!block.value(u"saveChoice"_q).toString().isEmpty(),
						[=](QString chosen) {
							setButton([&](QJsonObject &o) {
								o.remove(u"url"_q);
								if (chosen.isEmpty()) {
									o.remove(u"goto"_q);
								} else {
									o.insert(u"goto"_q, chosen);
								}
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
		};
		const auto productsButton = [&] {
			const auto count = _state->flow.value(u"products"_q).toArray().size();
			const auto button = row(object_ptr<Ui::RoundButton>(
				layout,
				rpl::single(count
					? u"🛍 Товары (%1) — изменить"_q.arg(count)
					: u"🛍 Добавить товары"_q),
				st::botStudioSmallButton), 10);
			const auto round = static_cast<Ui::RoundButton*>(button);
			round->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
			round->setClickedCallback([=] { showProducts(); });
		};

		if (type == u"message"_q) {
			textField(u"text"_q, u"Текст сообщения"_q, true);
			hint(variablesHint + u" Например: «Привет, {first_name}!»"_q);

			// Media attachment.
			section(u"Вложение"_q);
			auto media = block.value(u"media"_q).toObject();
			if (media.isEmpty() && !block.value(u"photo"_q).toString().isEmpty()) {
				media = QJsonObject{
					{ u"kind"_q, u"photo"_q },
					{ u"url"_q, block.value(u"photo"_q).toString() },
				};
			}
			chips({
				{ u"photo"_q, u"🖼 Фото"_q },
				{ u"video"_q, u"🎬 Видео"_q },
				{ u"document"_q, u"📎 Файл"_q },
				{ u"audio"_q, u"🎵 Аудио"_q },
			}, media.value(u"kind"_q).toString().isEmpty()
				? u"photo"_q
				: media.value(u"kind"_q).toString(), [=](QString chosen) {
				update([&](QJsonObject &b) {
					auto m = b.value(u"media"_q).toObject();
					if (m.isEmpty() && !b.value(u"photo"_q).toString().isEmpty()) {
						m.insert(u"url"_q, b.value(u"photo"_q).toString());
					}
					m.insert(u"kind"_q, chosen);
					b.remove(u"photo"_q);
					b.insert(u"media"_q, m);
				}, true);
			});
			field(
				u"Ссылка https://… на файл (необязательно)"_q,
				media.value(u"url"_q).toString(),
				false,
				[=](QString text) {
					update([&](QJsonObject &b) {
						auto m = b.value(u"media"_q).toObject();
						if (!m.contains(u"kind"_q)) {
							m.insert(u"kind"_q, u"photo"_q);
						}
						m.insert(u"url"_q, text.trimmed());
						b.remove(u"photo"_q);
						b.insert(u"media"_q, m);
					}, false);
				});

			buttonsEditor(u"Кнопки"_q);
			if (!Buttons(block).isEmpty()) {
				field(
					u"Запомнить нажатую кнопку как (необязательно)"_q,
					block.value(u"saveChoice"_q).toString(),
					false,
					[=](QString text) {
						update([&](QJsonObject &b) {
							const auto name = text.trimmed()
								.remove('{').remove('}').replace(' ', '_');
							if (name.isEmpty()) {
								b.remove(u"saveChoice"_q);
							} else {
								b.insert(u"saveChoice"_q, name);
							}
						}, false);
					});
				hint(u"Текст нажатой кнопки сохранится в переменную — "
					"удобно для выбора услуги, размера или времени."_q);
			}

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
				const auto kind = input.value(u"kind"_q).toString();
				chips({
					{ u"text"_q, u"Текст"_q },
					{ u"phone"_q, u"📱 Телефон"_q },
					{ u"number"_q, u"Число"_q },
					{ u"email"_q, u"E-mail"_q },
					{ u"location"_q, u"📍 Адрес"_q },
					{ u"photo"_q, u"📷 Фото"_q },
				}, kind.isEmpty() ? u"text"_q : kind, [=](QString chosen) {
					update([&](QJsonObject &b) {
						auto in = Input(b);
						in.insert(u"kind"_q, chosen);
						b.insert(u"input"_q, in);
					}, true);
				}, 8);
				if (kind == u"location"_q) {
					hint(u"Бот покажет кнопку «📍 Отправить геопозицию», "
						"адрес можно и написать."_q);
				} else if (kind == u"photo"_q) {
					hint(u"Фото придёт вам вместе с заявкой."_q);
				}
				labeledTarget(
					u"После ответа:"_q,
					input.value(u"next"_q).toString(),
					[=](QString chosen) {
						update([&](QJsonObject &b) {
							auto in = Input(b);
							in.insert(u"next"_q, chosen);
							b.insert(u"input"_q, in);
						}, true);
					});
			} else if (Buttons(block).isEmpty()) {
				labeledTarget(
					u"Затем сразу показать:"_q,
					block.value(u"next"_q).toString(),
					setKey(u"next"_q));
			}

			section(u"Заявки"_q, 18);
			const auto notify = static_cast<Ui::Checkbox*>(row(
				object_ptr<Ui::Checkbox>(
					layout,
					u"Присылать мне ответы клиента"_q,
					block.value(u"notifyOwner"_q).toBool(),
					st::botStudioCheckbox,
					st::defaultToggle), 8));
			notify->checkedChanges() | rpl::on_next([=](bool checked) {
				update([&](QJsonObject &b) {
					b.insert(u"notifyOwner"_q, checked);
				}, true);
			}, notify->lifetime());
			hint(u"Когда клиент дойдёт до этого сообщения, бот пришлёт вам в "
				"Telegram всё, что он ответил. Чтобы бот мог вам писать, один "
				"раз откройте его и нажмите /start."_q);

		} else if (type == u"condition"_q) {
			section(u"Проверки по порядку"_q);
			hint(u"Первая выполненная проверка ведёт в свою ветку."_q);
			const auto conditions = block.value(u"conditions"_q).toArray();
			for (auto i = 0; i != conditions.size(); ++i) {
				const auto condition = conditions[i].toObject();
				const auto setCondition = [=](Fn<void(QJsonObject&)> change, bool rebuild) {
					update([&](QJsonObject &b) {
						auto list = b.value(u"conditions"_q).toArray();
						if (i < list.size()) {
							auto object = list[i].toObject();
							change(object);
							list[i] = object;
							b.insert(u"conditions"_q, list);
						}
					}, rebuild);
				};
				const auto op = condition.value(u"op"_q).toString().isEmpty()
					? u"eq"_q
					: condition.value(u"op"_q).toString();
				const auto noVariable = OperatorWithoutVariable(op);
				const auto line = row(object_ptr<Ui::RpWidget>(layout), 8);
				const auto variable = Ui::CreateChild<Ui::InputField>(
					line,
					st::botStudioField,
					rpl::single(u"Ответ (переменная)"_q),
					condition.value(u"variable"_q).toString());
				variable->changes() | rpl::on_next([=] {
					const auto value = variable->getLastText().trimmed().remove('{').remove('}');
					setCondition([&](QJsonObject &o) {
						o.insert(u"variable"_q, value);
					}, false);
				}, variable->lifetime());
				variable->setVisible(!noVariable);
				const auto opButton = MakeButton(
					line,
					OperatorText(op) + u" ▾"_q,
					st::botStudioTargetButton);
				opButton->setClickedCallback([=] {
					const auto menu = Ui::CreateChild<Ui::PopupMenu>(
						this,
						st::defaultPopupMenu);
					for (const auto &[value, title] : std::vector<std::pair<QString, QString>>{
							{ u"eq"_q, u"= равно"_q },
							{ u"neq"_q, u"≠ не равно"_q },
							{ u"contains"_q, u"содержит"_q },
							{ u"gt"_q, u"> больше (число)"_q },
							{ u"lt"_q, u"< меньше (число)"_q },
							{ u"filled"_q, u"заполнено"_q },
							{ u"empty"_q, u"пусто"_q },
							{ u"member"_q, u"📢 подписан на канал"_q },
							{ u"tag"_q, u"🏷 есть метка"_q },
							{ u"hours"_q, u"🕘 сейчас рабочее время"_q } }) {
						const auto chosen = value;
						menu->addAction(title, [=] {
							setCondition([&](QJsonObject &o) {
								o.insert(u"op"_q, chosen);
								if (chosen == u"hours"_q
									&& o.value(u"value"_q).toString().isEmpty()) {
									o.insert(u"value"_q, u"пн-пт 9:00-18:00"_q);
								}
							}, true);
						});
					}
					menu->popup(QCursor::pos());
				});
				const auto value = Ui::CreateChild<Ui::InputField>(
					line,
					st::botStudioField,
					rpl::single((op == u"member"_q)
						? u"@канал или id"_q
						: (op == u"tag"_q)
						? u"Метка, например: vip"_q
						: (op == u"hours"_q)
						? u"пн-пт 9:00-18:00"_q
						: u"Значение"_q),
					condition.value(u"value"_q).toString());
				value->changes() | rpl::on_next([=] {
					const auto text = value->getLastText();
					setCondition([&](QJsonObject &o) {
						o.insert(u"value"_q, text);
					}, false);
				}, value->lifetime());
				value->setVisible(op != u"filled"_q && op != u"empty"_q);
				const auto to = target(
					line,
					condition.value(u"goto"_q).toString(),
					false,
					[=](QString chosen) {
						setCondition([&](QJsonObject &o) {
							o.insert(u"goto"_q, chosen);
						}, true);
					},
					nullptr);
				const auto remove = Ui::CreateChild<Ui::IconButton>(
					line,
					st::botStudioRemoveButton);
				remove->setClickedCallback([=] {
					update([&](QJsonObject &b) {
						auto list = b.value(u"conditions"_q).toArray();
						if (i < list.size()) {
							list.removeAt(i);
						}
						b.insert(u"conditions"_q, list);
					}, true);
				});
				line->widthValue() | rpl::on_next([=](int width) {
					const auto removeWidth = remove->width();
					if (noVariable) {
						opButton->moveToLeft(0, 18);
						const auto left = opButton->width() + 6;
						value->resize(width - removeWidth - left - 4, value->height());
						value->moveToLeft(left, 0);
					} else {
						const auto fieldWidth = (width - removeWidth - opButton->width() - 16) / 2;
						variable->resize(fieldWidth, variable->height());
						variable->moveToLeft(0, 0);
						opButton->moveToLeft(fieldWidth + 6, 18);
						value->resize(fieldWidth, value->height());
						value->moveToLeft(fieldWidth + 12 + opButton->width(), 0);
					}
					remove->moveToLeft(width - removeWidth, 14);
					to->moveToLeft(0, variable->height() + 2);
					line->resize(width, variable->height() + 2 + to->height() + 4);
				}, line->lifetime());
			}
			const auto addCondition = row(object_ptr<Ui::LinkButton>(
				layout,
				u"＋ Добавить проверку"_q), 8);
			static_cast<Ui::LinkButton*>(addCondition)->setClickedCallback([=] {
				update([&](QJsonObject &b) {
					auto list = b.value(u"conditions"_q).toArray();
					const auto known = KnownVariables(_state->flow);
					list.push_back(QJsonObject{
						{ u"variable"_q, known.size() > 3 ? known.back() : u"ответ"_q },
						{ u"op"_q, u"eq"_q },
						{ u"value"_q, QString() },
					});
					b.insert(u"conditions"_q, list);
				}, true);
			});
			hint(variablesHint
				+ u"\n«Подписан на канал»: добавьте бота администратором "
				"канала. «Рабочее время» — по часам этого компьютера."_q, 8);
			labeledTarget(
				u"Иначе:"_q,
				block.value(u"else"_q).toString(),
				setKey(u"else"_q));
		} else if (type == u"random"_q) {
			section(u"Варианты"_q);
			hint(u"Вес — как часто выпадает ветка: 50 и 50 — поровну, "
				"10 и 90 — редко и часто."_q);
			const auto branches = block.value(u"branches"_q).toArray();
			for (auto i = 0; i != branches.size(); ++i) {
				const auto branch = branches[i].toObject();
				const auto setBranch = [=](Fn<void(QJsonObject&)> change, bool rebuild) {
					update([&](QJsonObject &b) {
						auto list = b.value(u"branches"_q).toArray();
						if (i < list.size()) {
							auto object = list[i].toObject();
							change(object);
							list[i] = object;
							b.insert(u"branches"_q, list);
						}
					}, rebuild);
				};
				const auto line = row(object_ptr<Ui::RpWidget>(layout), 6);
				const auto weight = Ui::CreateChild<Ui::InputField>(
					line,
					st::botStudioField,
					rpl::single(u"Вес"_q),
					QString::number(branch.value(u"weight"_q).toDouble(1.)));
				weight->changes() | rpl::on_next([=] {
					const auto value = weight->getLastText().trimmed()
						.replace(',', '.').toDouble();
					setBranch([&](QJsonObject &o) {
						o.insert(u"weight"_q, std::max(value, 0.));
					}, false);
				}, weight->lifetime());
				const auto to = target(
					line,
					branch.value(u"goto"_q).toString(),
					false,
					[=](QString chosen) {
						setBranch([&](QJsonObject &o) {
							o.insert(u"goto"_q, chosen);
						}, true);
					},
					nullptr);
				const auto remove = Ui::CreateChild<Ui::IconButton>(
					line,
					st::botStudioRemoveButton);
				remove->setClickedCallback([=] {
					update([&](QJsonObject &b) {
						auto list = b.value(u"branches"_q).toArray();
						if (i < list.size()) {
							list.removeAt(i);
						}
						b.insert(u"branches"_q, list);
					}, true);
				});
				line->widthValue() | rpl::on_next([=](int width) {
					weight->resize(70, weight->height());
					weight->moveToLeft(0, 0);
					to->moveToLeft(80, 18);
					remove->moveToLeft(width - remove->width(), 14);
					line->resize(width, weight->height() + 4);
				}, line->lifetime());
			}
			const auto addBranch = row(object_ptr<Ui::LinkButton>(
				layout,
				u"＋ Добавить вариант"_q), 8);
			static_cast<Ui::LinkButton*>(addBranch)->setClickedCallback([=] {
				update([&](QJsonObject &b) {
					auto list = b.value(u"branches"_q).toArray();
					list.push_back(QJsonObject{ { u"weight"_q, 50 } });
					b.insert(u"branches"_q, list);
				}, true);
			});
		} else if (type == u"ai"_q) {
			section(u"Задание для ИИ"_q);
			textField(
				u"prompt"_q,
				u"Что написать (например: «Ответь на вопрос {last_message}»)"_q,
				true);
			textField(
				u"system"_q,
				u"Роль ИИ (необязательно): «Ты вежливый консультант салона»"_q,
				true);
			hint(variablesHint + u" Используется ИИ, настроенный в "
				"Agentgram."_q);
			field(
				u"Сохранить ответ ИИ как (необязательно)"_q,
				block.value(u"variable"_q).toString(),
				false,
				[=](QString text) {
					update([&](QJsonObject &b) {
						b.insert(u"variable"_q, text.trimmed().replace(' ', '_'));
					}, false);
				});
			const auto send = static_cast<Ui::Checkbox*>(row(
				object_ptr<Ui::Checkbox>(
					layout,
					u"Отправить ответ пользователю"_q,
					block.value(u"send"_q).toBool(true),
					st::botStudioCheckbox,
					st::defaultToggle), 10));
			send->checkedChanges() | rpl::on_next([=](bool checked) {
				update([&](QJsonObject &b) {
					b.insert(u"send"_q, checked);
				}, false);
			}, send->lifetime());
			labeledTarget(
				u"Затем показать:"_q,
				block.value(u"next"_q).toString(),
				setKey(u"next"_q));
		} else if (type == u"action"_q) {
			section(u"Действия по порядку"_q);
			const auto actions = block.value(u"actions"_q).toArray();
			for (auto i = 0; i != actions.size(); ++i) {
				const auto action = actions[i].toObject();
				const auto kind = action.value(u"kind"_q).toString();
				const auto setAction = [=](
						const QString &key,
						QJsonValue value,
						bool rebuild) {
					update([&](QJsonObject &b) {
						auto list = b.value(u"actions"_q).toArray();
						if (i < list.size()) {
							auto object = list[i].toObject();
							object.insert(key, value);
							list[i] = object;
							b.insert(u"actions"_q, list);
						}
					}, rebuild);
				};
				const auto head = row(object_ptr<Ui::RpWidget>(layout), 12);
				const auto label = Ui::CreateChild<Ui::FlatLabel>(
					head,
					ActionTitle(kind),
					st::botStudioInspectorSection);
				const auto remove = Ui::CreateChild<Ui::IconButton>(
					head,
					st::botStudioRemoveButton);
				remove->setClickedCallback([=] {
					update([&](QJsonObject &b) {
						auto list = b.value(u"actions"_q).toArray();
						if (i < list.size()) {
							list.removeAt(i);
						}
						b.insert(u"actions"_q, list);
					}, true);
				});
				head->widthValue() | rpl::on_next([=](int width) {
					label->resizeToWidth(width - remove->width() - 8);
					label->moveToLeft(0, (remove->height() - label->height()) / 2);
					remove->moveToLeft(width - remove->width(), 0);
					head->resize(width, remove->height());
				}, head->lifetime());
				const auto actionField = [&](
						const QString &key,
						const QString &placeholder,
						bool multiline) {
					field(placeholder, action.value(key).toVariant().toString(), multiline, [=](QString text) {
						setAction(key, (key == u"variable"_q)
							? text.trimmed().remove('{').remove('}').replace(' ', '_')
							: text, false);
					});
				};
				if (kind == u"set"_q || kind == u"increment"_q) {
					actionField(u"variable"_q, u"Переменная, например: баллы"_q, false);
					actionField(
						u"value"_q,
						(kind == u"set"_q)
							? u"Значение (можно с {подстановками})"_q
							: u"Сколько прибавить (по умолчанию 1)"_q,
						false);
				} else if (kind == u"calc"_q) {
					actionField(u"variable"_q, u"Куда записать результат: сумма"_q, false);
					actionField(u"value"_q, u"Формула: {кол} * 250 + 300"_q, false);
					hint(u"Числа, + − * / %, скобки и {переменные}."_q);
				} else if (kind == u"tag"_q || kind == u"untag"_q) {
					actionField(u"value"_q, u"Метка, например: vip, купил, курс"_q, false);
					hint(u"По меткам работают условия и рассылка только нужным "
						"людям во вкладке «Аудитория»."_q);
				} else if (kind == u"remind"_q) {
					actionField(u"value"_q, u"Через сколько: 30"_q, false);
					chips({
						{ u"min"_q, u"минут"_q },
						{ u"hour"_q, u"часов"_q },
						{ u"day"_q, u"дней"_q },
					}, action.value(u"unit"_q).toString().isEmpty()
						? u"min"_q
						: action.value(u"unit"_q).toString(), [=](QString chosen) {
						setAction(u"unit"_q, chosen, true);
					});
					labeledTarget(
						u"Показать:"_q,
						action.value(u"goto"_q).toString(),
						[=](QString chosen) { setAction(u"goto"_q, chosen, true); });
					hint(u"Бот сам напишет пользователю позже. Если Agentgram в "
						"это время закрыт, напоминание придёт, как только вы его "
						"откроете."_q);
				} else if (kind == u"cancel_reminders"_q) {
					hint(u"Отменяет все запланированные напоминания этого "
						"пользователя."_q);
				} else if (kind == u"add_to_cart"_q) {
					const auto line = row(object_ptr<Ui::RpWidget>(layout), 8);
					const auto current = action.value(u"product"_q).toString();
					const auto title = ProductTitle(_state->flow, current);
					const auto button = MakeButton(
						line,
						(title.isEmpty() ? u"⚠ выберите товар"_q : title) + u"  ▾"_q,
						st::botStudioTargetButton);
					button->setClickedCallback([=] {
						const auto menu = Ui::CreateChild<Ui::PopupMenu>(
							this,
							st::defaultPopupMenu);
						for (const auto &value : _state->flow.value(u"products"_q).toArray()) {
							const auto product = value.toObject();
							const auto key = FlowProductKey(product);
							menu->addAction(
								(key == current ? u"✓ "_q : QString())
									+ product.value(u"title"_q).toString(),
								[=] { setAction(u"product"_q, key, true); });
						}
						menu->addSeparator();
						menu->addAction(u"🛍 Изменить товары…"_q, [=] {
							showProducts();
						});
						menu->popup(QCursor::pos());
					});
					line->resize(button->width(), button->height());
					actionField(u"value"_q, u"Количество (по умолчанию 1)"_q, false);
				} else if (kind == u"request"_q) {
					actionField(u"url"_q, u"Адрес https://api…/?q={ответ}"_q, false);
					chips({
						{ u"GET"_q, u"GET"_q },
						{ u"POST"_q, u"POST с ответами"_q },
					}, action.value(u"method"_q).toString().isEmpty()
						? u"GET"_q
						: action.value(u"method"_q).toString(), [=](QString chosen) {
						setAction(u"method"_q, chosen, true);
					});
					actionField(u"variable"_q, u"Сохранить ответ как: курс"_q, false);
					actionField(u"path"_q, u"Поле из JSON, например: data.price"_q, false);
					hint(u"Подходит для курса валют, погоды, статуса заказа на "
						"сайте. Без поля сохраняется весь ответ."_q);
				} else if (kind == u"notify"_q) {
					actionField(
						u"text"_q,
						u"Текст для вас (пусто — все ответы клиента)"_q,
						true);
				} else if (kind == u"send"_q) {
					actionField(u"chat"_q, u"Чат: @channel или числовой id"_q, false);
					actionField(
						u"text"_q,
						u"Текст (пусто — все ответы клиента)"_q,
						true);
					hint(u"Добавьте бота в группу или администратором в канал."_q);
				} else if (kind == u"webhook"_q) {
					actionField(u"url"_q, u"Адрес https://… (Make, Zapier, Google Apps Script)"_q, false);
					hint(u"Бот отправит POST с JSON: пользователь и все его ответы."_q);
				} else if (kind == u"save_lead"_q) {
					hint(u"Ответы клиента попадут во вкладку «Аудитория» → "
						"«Заявки», оттуда их можно выгрузить в CSV."_q);
				}
			}
			const auto addAction = row(object_ptr<Ui::LinkButton>(
				layout,
				u"＋ Добавить действие"_q), 12);
			static_cast<Ui::LinkButton*>(addAction)->setClickedCallback([=] {
				const auto menu = Ui::CreateChild<Ui::PopupMenu>(
					this,
					st::defaultPopupMenu);
				const auto groups = std::vector<std::vector<QString>>{
					{ u"set"_q, u"increment"_q, u"calc"_q, u"clear"_q },
					{ u"tag"_q, u"untag"_q, u"remind"_q, u"cancel_reminders"_q },
					{ u"notify"_q, u"send"_q, u"save_lead"_q, u"webhook"_q, u"request"_q },
					{ u"add_to_cart"_q, u"clear_cart"_q },
				};
				for (const auto &group : groups) {
					if (!menu->actions().empty()) {
						menu->addSeparator();
					}
					for (const auto &value : group) {
						const auto chosen = value;
						menu->addAction(ActionTitle(chosen), [=] {
							update([&](QJsonObject &b) {
								auto list = b.value(u"actions"_q).toArray();
								auto object = QJsonObject{ { u"kind"_q, chosen } };
								if (chosen == u"increment"_q) {
									object.insert(u"value"_q, u"1"_q);
								} else if (chosen == u"remind"_q) {
									object.insert(u"value"_q, u"30"_q);
									object.insert(u"unit"_q, u"min"_q);
								}
								list.push_back(object);
								b.insert(u"actions"_q, list);
							}, true);
						});
					}
				}
				menu->popup(QCursor::pos());
			});
			labeledTarget(
				u"Затем показать:"_q,
				block.value(u"next"_q).toString(),
				setKey(u"next"_q));
		} else if (type == u"catalog"_q) {
			textField(u"text"_q, u"Заголовок: «🛍 Выберите категорию:»"_q, true);
			productsButton();
			const auto categories = ProductCategories(_state->flow);
			if (!categories.isEmpty()) {
				section(u"Что показывать"_q);
				auto options = std::vector<std::pair<QString, QString>>{
					{ QString(), u"Все категории"_q },
				};
				for (const auto &category : categories) {
					options.emplace_back(category, category);
				}
				chips(options, block.value(u"category"_q).toString(), [=](QString chosen) {
					update([&](QJsonObject &b) {
						if (chosen.isEmpty()) {
							b.remove(u"category"_q);
						} else {
							b.insert(u"category"_q, chosen);
						}
					}, true);
				});
				hint((categories.size() > 1 && block.value(u"category"_q).toString().isEmpty())
					? u"Сначала бот покажет кнопки категорий, затем карточки "
						"товаров с листанием ◀️ ▶️."_q
					: u"Бот покажет карточки товаров с листанием ◀️ ▶️."_q);
			}
			labeledTarget(
				u"Кнопка «🧺 Корзина»:"_q,
				block.value(u"cart"_q).toString(),
				setKey(u"cart"_q));
			hint(u"Если не выбрано — ведёт в первый блок «Корзина»."_q);
			buttonsEditor(u"Дополнительные кнопки"_q);
		} else if (type == u"cart"_q) {
			textField(u"text"_q, u"Заголовок: «🧺 Ваша корзина:»"_q, false);
			textField(u"emptyText"_q, u"Если корзина пуста: «🧺 Корзина пуста.»"_q, false);
			labeledTarget(
				u"«Оформить заказ»:"_q,
				block.value(u"checkout"_q).toString(),
				setKey(u"checkout"_q));
			hint(u"Обычно ведёт к вопросам «Как вас зовут?», «Телефон», "
				"«Адрес» и затем к блоку «Заказ и оплата»."_q);
			labeledTarget(
				u"Кнопка «В каталог»:"_q,
				block.value(u"catalog"_q).toString(),
				setKey(u"catalog"_q));
			hint(u"Доставка, бесплатная доставка от суммы, минимальный заказ "
				"и валюта — в «🛍 Товары»."_q);
			productsButton();
			buttonsEditor(u"Дополнительные кнопки"_q);
		} else if (type == u"order"_q) {
			section(u"Оплата"_q);
			const auto payment = block.value(u"payment"_q).toString();
			chips({
				{ u"none"_q, u"💵 При получении"_q },
				{ u"stars"_q, u"⭐ Звёзды Telegram"_q },
				{ u"card"_q, u"💳 Картой"_q },
			}, (payment == u"stars"_q || payment == u"card"_q)
				? payment
				: u"none"_q, [=](QString chosen) {
				update([&](QJsonObject &b) {
					b.insert(u"payment"_q, chosen);
				}, true);
			});
			hint((payment == u"stars"_q)
				? u"Покупатель оплатит звёздами прямо в Telegram — без "
					"договоров и платёжных систем. Цены в ⭐ или курс "
					"звезды задаются в «🛍 Товары»."_q
				: (payment == u"card"_q)
				? u"Подключите оплату в @BotFather → Bot Settings → Payments "
					"и вставьте платёжный токен в «🛍 Товары»."_q
				: u"Заказ сразу придёт вам в Telegram с кнопками статусов, "
					"покупатель получит уведомление о каждом статусе."_q);
			section(u"Сообщение покупателю"_q);
			textField(u"text"_q, u"После оформления заказа"_q, true);
			hint(u"{order_id} — номер заказа, {order_total} — сумма, "
				"{order_items} — состав. "_q + variablesHint);
			if (payment == u"stars"_q || payment == u"card"_q) {
				labeledTarget(
					u"После оплаты:"_q,
					block.value(u"paid"_q).toString(),
					setKey(u"paid"_q));
			} else {
				labeledTarget(
					u"Затем показать:"_q,
					block.value(u"next"_q).toString(),
					setKey(u"next"_q));
			}
			labeledTarget(
				u"Если корзина пуста:"_q,
				block.value(u"empty"_q).toString(),
				setKey(u"empty"_q));
			productsButton();
			buttonsEditor(u"Кнопки под сообщением"_q);
		} else if (type == u"operator"_q) {
			textField(u"text"_q, u"Сообщение клиенту"_q, true);
			textField(
				u"chat"_q,
				u"Куда слать вопросы: пусто — вам, или @группа / id"_q,
				false);
			textField(
				u"endText"_q,
				u"Кнопка завершения: «✖️ Завершить диалог»"_q,
				false);
			hint(u"Сообщения клиента придут вам от бота. Ответьте на них "
				"(reply) — бот перешлёт ответ клиенту: текст, фото, файлы, "
				"голосовые. Чтобы бот мог вам писать, откройте его и нажмите "
				"/start. Команда /start у клиента тоже завершает диалог."_q);
			labeledTarget(
				u"После завершения:"_q,
				block.value(u"next"_q).toString(),
				setKey(u"next"_q));
		}

		// Pause before the block.
		section(u"Пауза"_q, 18);
		field(
			u"Подождать секунд перед этим блоком (0–30)"_q,
			block.value(u"delay"_q).toDouble() > 0
				? QString::number(block.value(u"delay"_q).toDouble())
				: QString(),
			false,
			[=](QString text) {
				const auto value = std::clamp(
					text.trimmed().replace(',', '.').toDouble(),
					0.,
					30.);
				update([&](QJsonObject &b) {
					if (value > 0) {
						b.insert(u"delay"_q, value);
					} else {
						b.remove(u"delay"_q);
					}
				}, false);
			});

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
	if (sameBlock) {
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
				// Wrap the chips to the box width.
				const auto maxWidth = st::boxWideWidth
					+ st::boxWideWidth / 3
					- st::boxRowPadding.left()
					- st::boxRowPadding.right();
				auto left = 0;
				auto top = 0;
				auto height = 0;
				auto width = 0;
				for (const auto &[value, title] : options) {
					const auto chip = MakeButton(
						line,
						title,
						(value == current)
							? st::botStudioTemplateButtonActive
							: st::botStudioTemplateButton);
					if (left && left + chip->width() > maxWidth) {
						left = 0;
						top += chip->height() + 6;
					}
					chip->moveToLeft(left, top);
					left += chip->width() + 6;
					width = std::max(width, left);
					height = top + chip->height();
					const auto v = value;
					chip->setClickedCallback([=] {
						chosen(v);
						crl::on_main(box, [=] { (*rebuild)(); });
					});
				}
				line->resize(width, height);
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

void FlowEditor::showProducts() {
	const auto state = _state;
	const auto draft = std::make_shared<QJsonObject>(state->flow);
	_layers->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Товары и магазин"_q));
		const auto boxWidth = st::boxWideWidth + st::boxWideWidth / 3;
		box->setWidth(boxWidth);
		box->setMaxHeight(st::boxMaxListHeight * 2);
		const auto innerWidth = boxWidth
			- st::boxRowPadding.left()
			- st::boxRowPadding.right();
		const auto content = box->verticalLayout();
		const auto rebuild = std::make_shared<Fn<void()>>();
		const auto holder = box->addRow(object_ptr<Ui::VerticalLayout>(box));
		const auto number = [](const QString &text) {
			return text.trimmed().replace(',', '.').remove(' ').toDouble();
		};
		const auto shopValue = [=](const QString &key) {
			const auto value = draft->value(u"shop"_q).toObject().value(key);
			return value.isDouble()
				? (value.toDouble() > 0 ? QString::number(value.toDouble()) : QString())
				: value.toString();
		};
		const auto setShop = [=](const QString &key, QJsonValue value) {
			auto shop = draft->value(u"shop"_q).toObject();
			if ((value.isDouble() && value.toDouble() <= 0.)
				|| (value.isString() && value.toString().trimmed().isEmpty())) {
				shop.remove(key);
			} else {
				shop.insert(key, value);
			}
			draft->insert(u"shop"_q, shop);
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
			const auto makeField = [&](
					not_null<QWidget*> parent,
					const QString &placeholder,
					const QString &value,
					Fn<void(QString)> changed) {
				const auto result = Ui::CreateChild<Ui::InputField>(
					parent.get(),
					st::botStudioField,
					rpl::single(placeholder),
					value);
				result->changes() | rpl::on_next([=] {
					changed(result->getLastText());
				}, result->lifetime());
				return result;
			};
			// Several fields in one line with the given width shares.
			const auto fieldsLine = [&](
					std::vector<std::tuple<QString, QString, Fn<void(QString)>, int>> fields,
					Fn<void()> remove = nullptr) {
				const auto line = holder->add(
					object_ptr<Ui::RpWidget>(holder),
					QMargins(0, 2, 0, 2));
				auto widgets = std::vector<std::pair<Ui::InputField*, int>>();
				for (const auto &[placeholder, value, changed, share] : fields) {
					widgets.emplace_back(
						makeField(line, placeholder, value, changed),
						share);
				}
				Ui::IconButton *removeButton = nullptr;
				if (remove) {
					removeButton = Ui::CreateChild<Ui::IconButton>(
						line,
						st::botStudioRemoveButton);
					removeButton->setClickedCallback(remove);
				}
				line->widthValue() | rpl::on_next([=](int width) {
					const auto skip = 8;
					const auto available = width
						- (removeButton ? removeButton->width() + skip : 0)
						- skip * (int(widgets.size()) - 1);
					auto total = 0;
					for (const auto &[widget, share] : widgets) {
						total += share;
					}
					auto left = 0;
					auto height = 0;
					for (const auto &[widget, share] : widgets) {
						const auto w = available * share / std::max(total, 1);
						widget->resize(w, widget->height());
						widget->moveToLeft(left, 0);
						left += w + skip;
						height = std::max(height, widget->height());
					}
					if (removeButton) {
						removeButton->moveToLeft(width - removeButton->width(), 14);
					}
					line->resize(width, height);
				}, line->lifetime());
				line->resizeToWidth(innerWidth);
			};

			addLabel(u"Магазин"_q, st::botStudioInspectorSection, 4);
			addLabel(u"Валюта цен"_q, st::botStudioInspectorLabel, 4);
			const auto currency = ShopCurrency(*draft);
			{
				const auto line = holder->add(
					object_ptr<Ui::RpWidget>(holder),
					QMargins(0, 2, 0, 6));
				auto left = 0;
				auto top = 0;
				auto height = 0;
				for (const auto &[value, title] : std::vector<std::pair<QString, QString>>{
						{ u"RUB"_q, u"₽ RUB"_q },
						{ u"USD"_q, u"$ USD"_q },
						{ u"EUR"_q, u"€ EUR"_q },
						{ u"KZT"_q, u"₸ KZT"_q },
						{ u"UAH"_q, u"₴ UAH"_q },
						{ u"BYN"_q, u"Br BYN"_q },
						{ u"UZS"_q, u"сум UZS"_q },
						{ u"XTR"_q, u"⭐ Звёзды"_q } }) {
					const auto chip = MakeButton(
						line,
						title,
						(value == currency)
							? st::botStudioTemplateButtonActive
							: st::botStudioTemplateButton);
					if (left && left + chip->width() > innerWidth) {
						left = 0;
						top += chip->height() + 6;
					}
					chip->moveToLeft(left, top);
					left += chip->width() + 6;
					height = top + chip->height();
					const auto v = value;
					chip->setClickedCallback([=] {
						setShop(u"currency"_q, v);
						crl::on_main(box, [=] { (*rebuild)(); });
					});
				}
				line->resize(innerWidth, height);
			}
			fieldsLine({
				{ u"Доставка"_q, shopValue(u"delivery"_q), [=](QString v) { setShop(u"delivery"_q, number(v)); }, 1 },
				{ u"Бесплатно от"_q, shopValue(u"freeFrom"_q), [=](QString v) { setShop(u"freeFrom"_q, number(v)); }, 1 },
				{ u"Мин. заказ"_q, shopValue(u"minOrder"_q), [=](QString v) { setShop(u"minOrder"_q, number(v)); }, 1 },
			});
			fieldsLine({
				{ u"Куда слать заказы: пусто — вам, или @группа"_q, shopValue(u"ordersChat"_q), [=](QString v) { setShop(u"ordersChat"_q, v.trimmed()); }, 1 },
			});
			addLabel(
				u"Заказы приходят с кнопками статусов: «Принят», «В пути», "
				"«Выполнен», «Отменён» — покупатель получает уведомление. "
				"Команда /admin в боте показывает вам статистику."_q,
				st::botStudioInspectorLabel,
				0);

			addLabel(u"Онлайн-оплата"_q, st::botStudioInspectorSection, 12);
			if (currency != u"XTR"_q) {
				fieldsLine({
					{ u"Цена одной звезды в вашей валюте, например 1.8"_q, shopValue(u"starsRate"_q), [=](QString v) { setShop(u"starsRate"_q, number(v)); }, 1 },
				});
			}
			fieldsLine({
				{ u"Платёжный токен для карт из @BotFather"_q, shopValue(u"providerToken"_q), [=](QString v) { setShop(u"providerToken"_q, v.trimmed()); }, 1 },
			});
			addLabel(
				u"Звёзды Telegram работают сразу, без договоров. Для карт "
				"подключите ЮKassa, Stripe или другого провайдера в "
				"@BotFather. Способ оплаты выбирается в блоке «Заказ и "
				"оплата»."_q,
				st::botStudioInspectorLabel,
				0);

			const auto products = draft->value(u"products"_q).toArray();
			addLabel(
				u"Товары (%1)"_q.arg(products.size()),
				st::botStudioInspectorSection,
				14);
			addLabel(
				u"Фото — ссылка https://… на картинку или file_id. Товары с "
				"одинаковой категорией покажутся в одном разделе каталога."_q,
				st::botStudioInspectorLabel,
				0);
			for (auto i = 0; i != products.size(); ++i) {
				const auto product = products[i].toObject();
				const auto set = [=](const QString &key, QJsonValue value) {
					auto list = draft->value(u"products"_q).toArray();
					if (i < list.size()) {
						auto object = list[i].toObject();
						if ((value.isString() && value.toString().isEmpty())) {
							object.remove(key);
						} else {
							object.insert(key, value);
						}
						list[i] = object;
						draft->insert(u"products"_q, list);
					}
				};
				const auto price = product.value(u"price"_q).toDouble();
				fieldsLine({
					{ u"Товар %1: название"_q.arg(i + 1), product.value(u"title"_q).toString(), [=](QString v) { set(u"title"_q, v.trimmed()); }, 3 },
					{ u"Цена"_q, price > 0 ? QString::number(price) : QString(), [=](QString v) { set(u"price"_q, number(v)); }, 1 },
				}, [=] {
					auto list = draft->value(u"products"_q).toArray();
					if (i < list.size()) {
						list.removeAt(i);
					}
					draft->insert(u"products"_q, list);
					crl::on_main(box, [=] { (*rebuild)(); });
				});
				fieldsLine({
					{ u"Категория"_q, product.value(u"category"_q).toString(), [=](QString v) { set(u"category"_q, v.trimmed()); }, 1 },
					{ u"Фото: https://…"_q, product.value(u"photo"_q).toString(), [=](QString v) { set(u"photo"_q, v.trimmed()); }, 2 },
				});
				fieldsLine({
					{ u"Описание"_q, product.value(u"description"_q).toString(), [=](QString v) { set(u"description"_q, v); }, 1 },
				});
				holder->add(
					object_ptr<Ui::RpWidget>(holder),
					QMargins(0, 0, 0, 10))->resize(1, 1);
			}
			const auto addProduct = holder->add(
				object_ptr<Ui::LinkButton>(holder, u"＋ Добавить товар"_q),
				QMargins(0, 4, 0, 4));
			addProduct->setClickedCallback([=] {
				auto list = draft->value(u"products"_q).toArray();
				auto used = QStringList();
				for (const auto &value : list) {
					used.push_back(FlowProductKey(value.toObject()));
				}
				auto n = list.size() + 1;
				while (used.contains(u"p%1"_q.arg(n))) {
					++n;
				}
				list.push_back(QJsonObject{
					{ u"id"_q, u"p%1"_q.arg(n) },
					{ u"title"_q, u"Новый товар"_q },
					{ u"price"_q, 100 },
				});
				draft->insert(u"products"_q, list);
				crl::on_main(box, [=] { (*rebuild)(); });
			});
			holder->resizeToWidth(innerWidth);
		};
		(*rebuild)();
		box->addButton(rpl::single(u"Готово"_q), [=] {
			auto products = draft->value(u"products"_q).toArray();
			for (auto i = products.size(); i != 0;) {
				if (products[--i].toObject().value(u"title"_q).toString().isEmpty()) {
					products.removeAt(i);
				}
			}
			draft->insert(u"products"_q, products);
			// Keep block edits made meanwhile, take only shop data.
			state->flow.insert(u"products"_q, products);
			state->flow.insert(u"shop"_q, draft->value(u"shop"_q));
			markChanged(true);
			box->closeBox();
		});
		box->addButton(rpl::single(u"Отмена"_q), [=] { box->closeBox(); });
		(void)content;
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
	left += _settings->width() + st::botStudioButtonSkip;
	_products->moveToLeft(left, 0);
	left += _products->width() + st::botStudioButtonSkip * 2;
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
	_canvas->setViewport(_canvasScroll->size());
}

} // namespace BotStudio
