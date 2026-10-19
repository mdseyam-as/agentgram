/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "bot_studio/bot_studio_flow_editor.h"

#include "bot_studio/bot_studio_flow.h"
#include "bot_studio/bot_studio_manager.h"
#include "base/event_filter.h"
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
#include <QtGui/QWheelEvent>
#include <QtWidgets/QTextEdit>

#include <deque>

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

} // namespace

// Where a connection is stored in the scheme.
enum class LinkField {
	Main, // next / input.next / paid, by the block type
	Button,
	Condition,
	Else,
	Branch,
	Cart,
	Checkout,
	Catalog,
	Remind,
};

struct LinkRef {
	QString key;
	LinkField field = LinkField::Main;
	int index = 0;

	friend inline bool operator==(const LinkRef &a, const LinkRef &b) {
		return (a.key == b.key) && (a.field == b.field) && (a.index == b.index);
	}
};

namespace {

// One line inside a node: a button, a condition or an action. A row with
// a port can be connected to another node.
struct NodeRow {
	QString text;
	QString target;
	bool warning = false;
	bool link = false;
	bool button = false;
	bool port = false;
	LinkField field = LinkField::Main;
	int index = 0;
};

struct PortHit {
	LinkRef ref;
	QPointF pos;
	QString target;
	QColor color;
};

struct NodeLayout {
	QRect rect;
	QRect textRect;
	std::vector<QRect> rows;
	QRect footer;
	QPointF input;
	bool hasInput = false;
	std::vector<PortHit> ports;
};

// Trigger nodes: "#cmd:N" (commands[N]), "#kw:N" (keywords[N]),
// "#fallback". Blocks are keyed by their id.
[[nodiscard]] bool IsTrigger(const QString &key) {
	return key.startsWith('#');
}

[[nodiscard]] std::pair<QString, int> ParseTrigger(const QString &key) {
	if (key.startsWith(u"#cmd:"_q)) {
		return { u"commands"_q, key.mid(5).toInt() };
	} else if (key.startsWith(u"#kw:"_q)) {
		return { u"keywords"_q, key.mid(4).toInt() };
	}
	return { u"fallback"_q, -1 };
}

[[nodiscard]] QJsonObject TriggerObject(
		const QJsonObject &flow,
		const QString &key) {
	const auto [list, index] = ParseTrigger(key);
	if (index < 0) {
		return flow.value(list).toObject();
	}
	const auto array = flow.value(list).toArray();
	return (index < array.size()) ? array[index].toObject() : QJsonObject();
}

void SetTriggerObject(
		QJsonObject &flow,
		const QString &key,
		const QJsonObject &object) {
	const auto [list, index] = ParseTrigger(key);
	if (index < 0) {
		flow.insert(list, object);
		return;
	}
	auto array = flow.value(list).toArray();
	if (index < array.size()) {
		array[index] = object;
		flow.insert(list, array);
	}
}

void RemoveTrigger(QJsonObject &flow, const QString &key) {
	const auto [list, index] = ParseTrigger(key);
	if (index < 0) {
		auto object = flow.value(list).toObject();
		object.insert(u"mode"_q, u"start"_q);
		object.remove(u"goto"_q);
		flow.insert(list, object);
		return;
	}
	auto array = flow.value(list).toArray();
	if (index < array.size()) {
		array.removeAt(index);
		flow.insert(list, array);
	}
}

[[nodiscard]] std::vector<QString> NodeKeys(const QJsonObject &flow) {
	auto result = std::vector<QString>();
	const auto commands = flow.value(u"commands"_q).toArray().size();
	for (auto i = 0; i != commands; ++i) {
		result.push_back(u"#cmd:%1"_q.arg(i));
	}
	const auto keywords = flow.value(u"keywords"_q).toArray().size();
	for (auto i = 0; i != keywords; ++i) {
		result.push_back(u"#kw:%1"_q.arg(i));
	}
	result.push_back(u"#fallback"_q);
	for (const auto &value : FlowBlocks(flow)) {
		result.push_back(value.toObject().value(u"id"_q).toString());
	}
	return result;
}

[[nodiscard]] QJsonObject NodeObject(
		const QJsonObject &flow,
		const QString &key) {
	return IsTrigger(key) ? TriggerObject(flow, key) : FlowBlock(flow, key);
}

[[nodiscard]] QPoint NodePosition(
		const QJsonObject &flow,
		const QString &key) {
	const auto object = NodeObject(flow, key);
	return QPoint(
		object.value(u"x"_q).toInt(),
		object.value(u"y"_q).toInt());
}

void SetNodePosition(QJsonObject &flow, const QString &key, QPoint point) {
	auto object = NodeObject(flow, key);
	if (object.isEmpty() && !IsTrigger(key)) {
		return;
	}
	object.insert(u"x"_q, point.x());
	object.insert(u"y"_q, point.y());
	if (IsTrigger(key)) {
		SetTriggerObject(flow, key, object);
	} else {
		SetFlowBlock(flow, object);
	}
}

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
	const auto buttons = Buttons(block);
	for (auto i = 0; i != buttons.size(); ++i) {
		const auto button = buttons[i].toObject();
		const auto url = button.value(u"url"_q).toString();
		const auto target = button.value(u"goto"_q).toString();
		auto row = NodeRow{
			.text = button.value(u"text"_q).toString(),
			.target = target,
			.warning = url.isEmpty() && target.isEmpty() && block.value(
				u"saveChoice"_q).toString().isEmpty(),
			.link = !url.isEmpty(),
			.button = true,
			.port = url.isEmpty(),
			.field = LinkField::Button,
			.index = i,
		};
		result.push_back(row);
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
	const auto portRow = [&](
			QString text,
			QString target,
			LinkField field,
			int index = 0,
			bool required = false) {
		result.push_back({
			.text = std::move(text),
			.target = target,
			.warning = required && target.isEmpty(),
			.port = true,
			.field = field,
			.index = index,
		});
	};
	if (type == u"condition"_q) {
		const auto conditions = block.value(u"conditions"_q).toArray();
		for (auto i = 0; i != conditions.size(); ++i) {
			const auto condition = conditions[i].toObject();
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
			portRow(text, condition.value(u"goto"_q).toString(), LinkField::Condition, i, true);
		}
		portRow(u"иначе"_q, block.value(u"else"_q).toString(), LinkField::Else);
		return result;
	} else if (type == u"random"_q) {
		const auto branches = block.value(u"branches"_q).toArray();
		auto total = 0.;
		for (const auto &value : branches) {
			total += std::max(value.toObject().value(u"weight"_q).toDouble(1.), 0.);
		}
		for (auto i = 0; i != branches.size(); ++i) {
			const auto branch = branches[i].toObject();
			const auto weight = std::max(branch.value(u"weight"_q).toDouble(1.), 0.);
			portRow(
				u"🎲 %1%"_q.arg(total > 0 ? qRound(weight * 100. / total) : 0),
				branch.value(u"goto"_q).toString(),
				LinkField::Branch,
				i,
				true);
		}
		return result;
	} else if (type == u"action"_q) {
		const auto actions = block.value(u"actions"_q).toArray();
		for (auto i = 0; i != actions.size(); ++i) {
			const auto action = actions[i].toObject();
			if (action.value(u"kind"_q).toString() == u"remind"_q) {
				portRow(
					ActionText(flow, action),
					action.value(u"goto"_q).toString(),
					LinkField::Remind,
					i,
					true);
			} else {
				result.push_back({ .text = ActionText(flow, action) });
			}
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
			.text = (category.isEmpty()
				? u"📦 Товаров: %1"_q
				: (category + u" · %1"_q)).arg(count),
			.warning = (count == 0),
		});
		portRow(u"🧺 Кнопка корзины"_q, block.value(u"cart"_q).toString(), LinkField::Cart);
	} else if (type == u"cart"_q) {
		portRow(
			u"✅ Оформить заказ"_q,
			block.value(u"checkout"_q).toString(),
			LinkField::Checkout,
			0,
			true);
		portRow(u"🛍 В каталог"_q, block.value(u"catalog"_q).toString(), LinkField::Catalog);
	} else if (type == u"order"_q) {
		result.push_back({ .text = PaymentText(block.value(u"payment"_q).toString()) });
	} else if (type == u"operator"_q) {
		const auto chat = block.value(u"chat"_q).toString().trimmed();
		result.push_back({ .text = chat.isEmpty()
			? u"🎧 Вопросы приходят вам"_q
			: (u"🎧 Вопросы в "_q + chat) });
	}
	const auto buttons = ButtonRows(block);
	result.insert(end(result), begin(buttons), end(buttons));
	return result;
}

// The main output on the right of the header: what is shown next.
[[nodiscard]] bool HasMainOutput(const QJsonObject &block) {
	const auto type = BlockType(block);
	if (type == u"message"_q || type == u"ai"_q) {
		if (type == u"message"_q && HasInput(block)) {
			return true;
		}
		// With buttons "then" is rare, show it only when it is used.
		for (const auto &value : Buttons(block)) {
			if (value.toObject().value(u"url"_q).toString().isEmpty()) {
				return !block.value(u"next"_q).toString().isEmpty();
			}
		}
		return true;
	}
	return (type == u"action"_q)
		|| (type == u"operator"_q)
		|| (type == u"order"_q);
}

[[nodiscard]] QString MainTarget(const QJsonObject &block) {
	const auto type = BlockType(block);
	if (type == u"message"_q && HasInput(block)) {
		return Input(block).value(u"next"_q).toString();
	} else if (type == u"order"_q) {
		const auto payment = block.value(u"payment"_q).toString();
		return (payment == u"stars"_q || payment == u"card"_q)
			? block.value(u"paid"_q).toString()
			: block.value(u"next"_q).toString();
	}
	return block.value(u"next"_q).toString();
}

void SetMainTarget(QJsonObject &block, const QString &target) {
	const auto type = BlockType(block);
	auto key = u"next"_q;
	if (type == u"message"_q && HasInput(block)) {
		auto input = Input(block);
		input.insert(u"next"_q, target);
		block.insert(u"input"_q, input);
		return;
	} else if (type == u"order"_q) {
		const auto payment = block.value(u"payment"_q).toString();
		if (payment == u"stars"_q || payment == u"card"_q) {
			key = u"paid"_q;
		}
	}
	if (target.isEmpty()) {
		block.remove(key);
	} else {
		block.insert(key, target);
	}
}

[[nodiscard]] QString TriggerTarget(
		const QJsonObject &flow,
		const QString &key) {
	const auto object = TriggerObject(flow, key);
	if (key == u"#fallback"_q) {
		const auto mode = object.value(u"mode"_q).toString();
		return (mode == u"block"_q)
			? object.value(u"goto"_q).toString()
			: (mode.isEmpty() || mode == u"start"_q)
			? flow.value(u"start"_q).toString()
			: QString();
	}
	return object.value(u"goto"_q).toString();
}

[[nodiscard]] bool TriggerHasOutput(
		const QJsonObject &flow,
		const QString &key) {
	if (key != u"#fallback"_q) {
		return true;
	}
	const auto mode = TriggerObject(flow, key).value(u"mode"_q).toString();
	return mode.isEmpty() || mode == u"start"_q || mode == u"block"_q;
}

[[nodiscard]] QString TriggerTitle(
		const QJsonObject &flow,
		const QString &key) {
	const auto object = TriggerObject(flow, key);
	if (key.startsWith(u"#cmd:"_q)) {
		return u"/"_q + object.value(u"command"_q).toString();
	} else if (key.startsWith(u"#kw:"_q)) {
		const auto words = object.value(u"words"_q).toString().trimmed();
		return words.isEmpty() ? u"Ключевые слова"_q : words;
	}
	return u"Любое другое сообщение"_q;
}

[[nodiscard]] QString TriggerPreview(
		const QJsonObject &flow,
		const QString &key) {
	const auto object = TriggerObject(flow, key);
	if (key.startsWith(u"#cmd:"_q)) {
		return object.value(u"description"_q).toString();
	} else if (key.startsWith(u"#kw:"_q)) {
		return u"Сообщение содержит одно из слов"_q;
	}
	const auto mode = object.value(u"mode"_q).toString();
	return (mode == u"ai"_q)
		? u"✦ Отвечает ИИ"_q
		: (mode == u"text"_q)
		? (u"💬 "_q + object.value(u"text"_q).toString())
		: u"Если бот не понял сообщение"_q;
}

[[nodiscard]] QString NodeTitle(const QJsonObject &flow, const QString &key) {
	return IsTrigger(key) ? TriggerTitle(flow, key) : FlowBlockTitle(flow, key);
}

void SetLink(
		QJsonObject &flow,
		const LinkRef &ref,
		const QString &target) {
	if (IsTrigger(ref.key)) {
		auto object = TriggerObject(flow, ref.key);
		if (ref.key == u"#fallback"_q) {
			if (target.isEmpty()) {
				object.insert(u"mode"_q, u"start"_q);
				object.remove(u"goto"_q);
			} else {
				object.insert(u"mode"_q, u"block"_q);
				object.insert(u"goto"_q, target);
			}
		} else if (target.isEmpty()) {
			object.remove(u"goto"_q);
		} else {
			object.insert(u"goto"_q, target);
		}
		SetTriggerObject(flow, ref.key, object);
		return;
	}
	auto block = FlowBlock(flow, ref.key);
	if (block.isEmpty()) {
		return;
	}
	const auto setKey = [&](QJsonObject &object, const QString &name) {
		if (target.isEmpty()) {
			object.remove(name);
		} else {
			object.insert(name, target);
		}
	};
	const auto inArray = [&](const QString &name) {
		auto list = block.value(name).toArray();
		if (ref.index >= 0 && ref.index < list.size()) {
			auto object = list[ref.index].toObject();
			setKey(object, u"goto"_q);
			if (name == u"buttons"_q) {
				object.remove(u"url"_q);
			}
			list[ref.index] = object;
			block.insert(name, list);
		}
	};
	switch (ref.field) {
	case LinkField::Main: SetMainTarget(block, target); break;
	case LinkField::Button: inArray(u"buttons"_q); break;
	case LinkField::Condition: inArray(u"conditions"_q); break;
	case LinkField::Else: setKey(block, u"else"_q); break;
	case LinkField::Branch: inArray(u"branches"_q); break;
	case LinkField::Cart: setKey(block, u"cart"_q); break;
	case LinkField::Checkout: setKey(block, u"checkout"_q); break;
	case LinkField::Catalog: setKey(block, u"catalog"_q); break;
	case LinkField::Remind: inArray(u"actions"_q); break;
	}
	SetFlowBlock(flow, block);
}

// Normalizes the scheme and gives trigger nodes a place left of the
// blocks they lead to.
[[nodiscard]] QJsonObject PrepareFlow(QJsonObject flow) {
	flow = NormalizeFlow(std::move(flow));
	// Triggers without a place go to a column left of all blocks, each
	// near the block it leads to, without covering each other.
	auto left = 0;
	auto first = true;
	for (const auto &value : FlowBlocks(flow)) {
		const auto x = value.toObject().value(u"x"_q).toInt();
		left = first ? x : std::min(left, x);
		first = false;
	}
	auto taken = std::vector<int>();
	for (const auto &list : { u"commands"_q, u"keywords"_q }) {
		for (const auto &value : flow.value(list).toArray()) {
			const auto object = value.toObject();
			if (object.contains(u"y"_q)) {
				taken.push_back(object.value(u"y"_q).toInt());
			}
		}
	}
	const auto place = [&](QJsonObject &object, const QString &target) {
		if (object.contains(u"x"_q) && object.contains(u"y"_q)) {
			return;
		}
		const auto id = target.isEmpty()
			? flow.value(u"start"_q).toString()
			: target;
		auto y = FlowBlock(flow, id).value(u"y"_q).toInt();
		const auto busy = [&] {
			for (const auto other : taken) {
				if (std::abs(other - y) < 90) {
					return true;
				}
			}
			return false;
		};
		while (busy()) {
			y += 30;
		}
		taken.push_back(y);
		object.insert(u"x"_q, left - 290);
		object.insert(u"y"_q, y);
	};
	for (const auto &list : { u"commands"_q, u"keywords"_q }) {
		auto array = flow.value(list).toArray();
		for (auto i = 0; i != array.size(); ++i) {
			auto object = array[i].toObject();
			place(object, object.value(u"goto"_q).toString());
			array[i] = object;
		}
		flow.insert(list, array);
	}
	auto fallback = flow.value(u"fallback"_q).toObject();
	place(fallback, fallback.value(u"mode"_q).toString() == u"block"_q
		? fallback.value(u"goto"_q).toString()
		: QString());
	flow.insert(u"fallback"_q, fallback);
	return flow;
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

namespace {

constexpr auto kHeaderHeight = 46;
constexpr auto kTriggerWidth = 210;
constexpr auto kPortRadius = 5.;
constexpr auto kPlusOffset = 30.;
constexpr auto kPlusRadius = 9.;
constexpr auto kMinZoom = 0.3;
constexpr auto kMaxZoom = 2.;
const auto kTriggerColor = QColor(0xff, 0x8a, 0x3d);

struct Connection {
	LinkRef ref;
	QString target;
	QPainterPath path;
	QPointF middle;
	bool back = false;
};

[[nodiscard]] int Snap(double value) {
	return qRound(value / kSnap) * kSnap;
}

[[nodiscard]] QString NodeEmoji(const QJsonObject &flow, const QString &key) {
	if (key.startsWith(u"#cmd:"_q)) {
		return u"⚡"_q;
	} else if (key.startsWith(u"#kw:"_q)) {
		return u"🔑"_q;
	} else if (IsTrigger(key)) {
		return u"❓"_q;
	}
	return KindOf(BlockType(FlowBlock(flow, key))).title.section(' ', 0, 0);
}

[[nodiscard]] QString NodeSubtitle(const QJsonObject &flow, const QString &key) {
	if (key.startsWith(u"#cmd:"_q)) {
		return u"Запуск · команда"_q;
	} else if (key.startsWith(u"#kw:"_q)) {
		return u"Запуск · ключевые слова"_q;
	} else if (IsTrigger(key)) {
		return u"Запуск · всё остальное"_q;
	}
	const auto block = FlowBlock(flow, key);
	auto parts = QStringList{
		KindOf(BlockType(block)).title.section(' ', 1),
	};
	const auto delay = block.value(u"delay"_q).toDouble();
	if (delay > 0) {
		parts.push_back(u"⏱ %1 с"_q.arg(delay));
	}
	if (block.value(u"notifyOwner"_q).toBool()) {
		parts.push_back(u"📩 заявка"_q);
	}
	return parts.join(u" · "_q);
}

[[nodiscard]] QColor NodeColor(const QJsonObject &flow, const QString &key) {
	if (IsTrigger(key)) {
		return kTriggerColor;
	}
	return BlockColor(
		FlowBlock(flow, key),
		key == flow.value(u"start"_q).toString());
}

[[nodiscard]] QPainterPath NodeShape(QRectF r, bool trigger) {
	const auto small = double(st::botStudioNodeRadius);
	const auto big = trigger ? std::min(26., r.height() / 2.) : small;
	auto path = QPainterPath();
	path.moveTo(r.left() + big, r.top());
	path.lineTo(r.right() - small, r.top());
	path.arcTo(QRectF(r.right() - 2 * small, r.top(), 2 * small, 2 * small), 90, -90);
	path.lineTo(r.right(), r.bottom() - small);
	path.arcTo(QRectF(r.right() - 2 * small, r.bottom() - 2 * small, 2 * small, 2 * small), 0, -90);
	path.lineTo(r.left() + big, r.bottom());
	path.arcTo(QRectF(r.left(), r.bottom() - 2 * big, 2 * big, 2 * big), 270, -90);
	path.lineTo(r.left(), r.top() + big);
	path.arcTo(QRectF(r.left(), r.top(), 2 * big, 2 * big), 180, -90);
	path.closeSubpath();
	return path;
}

[[nodiscard]] QPainterPath LinkPath(QPointF from, QPointF to) {
	auto path = QPainterPath(from);
	if (to.x() >= from.x() + 40) {
		const auto dx = std::max(50., (to.x() - from.x()) / 2.);
		path.cubicTo(
			QPointF(from.x() + dx, from.y()),
			QPointF(to.x() - dx, to.y()),
			to);
		return path;
	}
	// Back links go around: out to the right, back below, in from the left.
	const auto loop = 70.;
	const auto down = (std::abs(to.y() - from.y()) < 120.)
		? (std::max(from.y(), to.y()) + 110.)
		: (from.y() + to.y()) / 2.;
	const auto middle = QPointF((from.x() + to.x()) / 2., down);
	path.cubicTo(
		QPointF(from.x() + loop * 1.6, from.y()),
		QPointF(from.x() + loop * 1.6, down),
		QPointF(from.x(), down));
	path.lineTo(QPointF(to.x(), down));
	path.cubicTo(
		QPointF(to.x() - loop * 1.6, down),
		QPointF(to.x() - loop * 1.6, to.y()),
		to);
	(void)middle;
	return path;
}

} // namespace

class FlowCanvas final : public Ui::RpWidget {
public:
	FlowCanvas(QWidget *parent, std::shared_ptr<FlowState> state);

	void refresh() {
		update();
	}
	void fitView();
	void zoomTo(double zoom, QPointF anchor);
	[[nodiscard]] double zoom() const {
		return _zoom;
	}
	[[nodiscard]] rpl::producer<double> zoomValue() const {
		return _zoomValue.value();
	}
	[[nodiscard]] QRect nodeRect(const QString &key) const {
		return layout(key).rect;
	}
	[[nodiscard]] QPointF toLogical(QPointF point) const {
		return (point - _offset) / _zoom;
	}
	[[nodiscard]] QPointF toScreen(QPointF point) const {
		return point * _zoom + _offset;
	}
	void ensureVisible(const QString &key);
	void arrange();
	void setRightInset(int inset) {
		_rightInset = inset;
	}
	void resetLinkSelection() {
		_selectedLink = std::nullopt;
		_hoverLink = std::nullopt;
	}

	Fn<void(QPoint screen, QPointF logical, std::optional<LinkRef> from)> pickRequest;
	Fn<void(QString, QPoint)> contextRequest;
	Fn<void()> pressed;
	Fn<void()> deleteRequest;
	Fn<void()> duplicateRequest;
	Fn<void()> undoRequest;
	Fn<void()> redoRequest;

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void wheelEvent(QWheelEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	enum class Drag {
		None,
		Node,
		Pan,
		Link,
	};

	[[nodiscard]] NodeLayout layout(const QString &key) const;
	[[nodiscard]] std::vector<std::pair<QString, NodeLayout>> layouts() const;
	[[nodiscard]] std::vector<Connection> connections(
		const std::vector<std::pair<QString, NodeLayout>> &all) const;
	[[nodiscard]] QString nodeAt(QPointF point) const;
	[[nodiscard]] std::optional<PortHit> portAt(QPointF point, bool plus) const;
	[[nodiscard]] std::optional<Connection> connectionAt(QPointF point) const;
	[[nodiscard]] std::optional<LinkRef> removeButtonAt(QPointF point) const;
	void paintNode(QPainter &p, const QString &key, const NodeLayout &layout);
	void paintPorts(QPainter &p, const QString &key, const NodeLayout &layout);
	void select(const QString &key);
	void updateHover(QPoint position);

	const std::shared_ptr<FlowState> _state;
	double _zoom = 1.;
	QPointF _offset = QPointF(60., 60.);
	rpl::variable<double> _zoomValue = 1.;
	bool _fitted = false;
	int _rightInset = 0;

	Drag _drag = Drag::None;
	QString _dragNode;
	QPoint _pressPosition;
	QPointF _pressOffset;
	QPoint _dragOrigin;
	bool _moved = false;
	PortHit _linkFrom;
	QPointF _linkEnd;
	QString _linkOver;

	std::optional<LinkRef> _hoverLink;
	std::optional<LinkRef> _selectedLink;
	std::optional<LinkRef> _hoverPlus;
	QString _hoverNode;

};

FlowCanvas::FlowCanvas(QWidget *parent, std::shared_ptr<FlowState> state)
: RpWidget(parent)
, _state(std::move(state)) {
	setMouseTracking(true);
	setFocusPolicy(Qt::ClickFocus);
}

NodeLayout FlowCanvas::layout(const QString &key) const {
	auto result = NodeLayout();
	const auto &flow = _state->flow;
	const auto trigger = IsTrigger(key);
	const auto object = NodeObject(flow, key);
	const auto position = NodePosition(flow, key);
	const auto width = trigger ? kTriggerWidth : st::botStudioNodeWidth;
	const auto &padding = st::botStudioNodePadding;
	const auto inner = width - padding.left() - padding.right();
	auto top = kHeaderHeight;

	const auto font = style::font(st::botStudioNodeTextFont);
	const auto preview = trigger
		? TriggerPreview(flow, key)
		: BlockPreview(object);
	if (!preview.isEmpty()
		|| (!trigger && BlockType(object) == u"message"_q)) {
		const auto text = preview.isEmpty() ? u"Пустое сообщение"_q : preview;
		const auto bounding = QFontMetrics(font->f).boundingRect(
			QRect(0, 0, inner, font->height * kMaxTextLines),
			Qt::TextWordWrap,
			text);
		const auto textHeight = std::min(
			bounding.height(),
			font->height * kMaxTextLines);
		result.textRect = QRect(
			position.x() + padding.left(),
			position.y() + top,
			inner,
			textHeight);
		top += textHeight + 4;
	}
	const auto rows = trigger
		? std::vector<NodeRow>()
		: BlockRows(flow, object);
	if (!rows.empty()) {
		top += st::botStudioNodeButtonSkip;
	}
	for (auto i = 0; i != int(rows.size()); ++i) {
		result.rows.push_back(QRect(
			position.x() + padding.left(),
			position.y() + top,
			inner,
			st::botStudioNodeButtonHeight));
		top += st::botStudioNodeButtonHeight + st::botStudioNodeButtonSkip;
	}
	if (!trigger
		&& BlockType(object) == u"message"_q
		&& HasInput(object)) {
		top += 2;
		result.footer = QRect(
			position.x() + padding.left(),
			position.y() + top,
			inner,
			style::font(st::botStudioNodeBadgeFont)->height + 4);
		top += result.footer.height();
	}
	top += (top == kHeaderHeight) ? 2 : padding.bottom();
	result.rect = QRect(position, QSize(width, top));

	const auto headerY = position.y() + kHeaderHeight / 2.;
	const auto color = NodeColor(flow, key);
	if (!trigger) {
		result.hasInput = true;
		result.input = QPointF(position.x(), headerY);
	}
	if (trigger ? TriggerHasOutput(flow, key) : HasMainOutput(object)) {
		result.ports.push_back({
			.ref = LinkRef{ key },
			.pos = QPointF(position.x() + width, headerY),
			.target = trigger ? TriggerTarget(flow, key) : MainTarget(object),
			.color = color,
		});
	}
	for (auto i = 0; i != int(rows.size()); ++i) {
		if (rows[i].port) {
			result.ports.push_back({
				.ref = LinkRef{ key, rows[i].field, rows[i].index },
				.pos = QPointF(
					position.x() + width,
					result.rows[i].y() + result.rows[i].height() / 2.),
				.target = rows[i].target,
				.color = color,
			});
		}
	}
	return result;
}

std::vector<std::pair<QString, NodeLayout>> FlowCanvas::layouts() const {
	auto result = std::vector<std::pair<QString, NodeLayout>>();
	for (const auto &key : NodeKeys(_state->flow)) {
		result.emplace_back(key, layout(key));
	}
	return result;
}

std::vector<Connection> FlowCanvas::connections(
		const std::vector<std::pair<QString, NodeLayout>> &all) const {
	auto result = std::vector<Connection>();
	const auto find = [&](const QString &key) -> const NodeLayout* {
		for (const auto &[k, l] : all) {
			if (k == key) {
				return &l;
			}
		}
		return nullptr;
	};
	for (const auto &[key, l] : all) {
		for (const auto &port : l.ports) {
			if (port.target.isEmpty() || IsTrigger(port.target)) {
				continue;
			}
			const auto target = find(port.target);
			if (!target) {
				continue;
			}
			auto path = LinkPath(port.pos, target->input);
			const auto middle = path.pointAtPercent(0.5);
			result.push_back({
				.ref = port.ref,
				.target = port.target,
				.path = std::move(path),
				.middle = middle,
				.back = (target->input.x() < port.pos.x() + 40),
			});
		}
	}
	return result;
}

QString FlowCanvas::nodeAt(QPointF point) const {
	const auto all = layouts();
	for (auto i = all.size(); i != 0;) {
		--i;
		if (QRectF(all[i].second.rect).contains(point)) {
			return all[i].first;
		}
	}
	return QString();
}

std::optional<PortHit> FlowCanvas::portAt(QPointF point, bool plus) const {
	const auto radius = std::max(9., 9. / _zoom);
	for (const auto &[key, l] : layouts()) {
		for (const auto &port : l.ports) {
			const auto connected = !port.target.isEmpty()
				&& FlowBlockIndex(_state->flow, port.target) >= 0;
			if (plus) {
				if (connected) {
					continue;
				}
				const auto center = port.pos + QPointF(kPlusOffset, 0);
				if (QLineF(center, point).length() <= kPlusRadius + 3) {
					return port;
				}
			} else if (QLineF(port.pos, point).length() <= radius) {
				return port;
			}
		}
	}
	return std::nullopt;
}

std::optional<Connection> FlowCanvas::connectionAt(QPointF point) const {
	auto stroker = QPainterPathStroker();
	stroker.setWidth(std::max(10., 10. / _zoom));
	for (auto &connection : connections(layouts())) {
		if (stroker.createStroke(connection.path).contains(point)) {
			return connection;
		}
	}
	return std::nullopt;
}

std::optional<LinkRef> FlowCanvas::removeButtonAt(QPointF point) const {
	for (const auto &connection : connections(layouts())) {
		const auto shown = (_hoverLink && *_hoverLink == connection.ref)
			|| (_selectedLink && *_selectedLink == connection.ref);
		if (shown && QLineF(connection.middle, point).length() <= 13.) {
			return connection.ref;
		}
	}
	return std::nullopt;
}

void FlowCanvas::paintNode(
		QPainter &p,
		const QString &key,
		const NodeLayout &layout) {
	const auto &flow = _state->flow;
	const auto trigger = IsTrigger(key);
	const auto selected = (key == _state->selected);
	const auto isStart = !trigger && (key == flow.value(u"start"_q).toString());
	const auto rect = QRectF(layout.rect);
	const auto color = NodeColor(flow, key);
	const auto shape = NodeShape(rect, trigger);

	// Soft shadow.
	p.setPen(Qt::NoPen);
	p.setBrush(WithAlpha(QColor(0, 0, 0), selected ? 60 : 32));
	p.drawPath(NodeShape(rect.translated(0, 3), trigger));

	p.setBrush(st::botStudioCardBg);
	p.setPen(selected
		? QPen(st::windowBgActive->c, 2)
		: (key == _hoverNode || key == _linkOver)
		? QPen(WithAlpha(st::windowBgActive->c, 160), 1.5)
		: QPen(WithAlpha(st::botStudioCardBorder->c, 120), 1));
	p.drawPath(shape);

	const auto &padding = st::botStudioNodePadding;
	const auto x = layout.rect.x();
	const auto y = layout.rect.y();
	const auto width = layout.rect.width();

	// Icon tile.
	const auto tile = QRectF(x + 12, y + 8, 30, 30);
	p.setPen(Qt::NoPen);
	p.setBrush(WithAlpha(color, trigger ? 70 : 55));
	p.drawRoundedRect(tile, 9, 9);
	auto emojiFont = style::font(st::botStudioNodeTitleFont)->f;
	emojiFont.setPixelSize(16);
	p.setFont(emojiFont);
	p.setPen(st::windowFg);
	p.drawText(tile, Qt::AlignCenter, NodeEmoji(flow, key));

	// Title, a start badge and the subtitle.
	const auto titleFont = style::font(st::botStudioNodeTitleFont);
	const auto badgeFont = style::font(st::botStudioNodeBadgeFont);
	auto titleRight = x + width - padding.right();
	if (isStart) {
		const auto badge = u"▶ старт"_q;
		const auto w = badgeFont->width(badge) + 12;
		const auto badgeRect = QRect(titleRight - w, y + 9, w, badgeFont->height + 2);
		p.setPen(Qt::NoPen);
		p.setBrush(WithAlpha(kStartColor, 45));
		p.drawRoundedRect(badgeRect, badgeRect.height() / 2., badgeRect.height() / 2.);
		p.setFont(badgeFont);
		p.setPen(kStartColor);
		p.drawText(badgeRect, Qt::AlignCenter, badge);
		titleRight -= w + 4;
	}
	const auto textLeft = x + 50;
	p.setFont(titleFont);
	p.setPen(st::windowBoldFg);
	p.drawText(
		textLeft,
		y + 9 + titleFont->ascent,
		titleFont->elided(NodeTitle(flow, key), titleRight - textLeft));
	p.setFont(badgeFont);
	p.setPen(trigger ? kTriggerColor : st::windowSubTextFg->c);
	p.drawText(
		textLeft,
		y + 26 + badgeFont->ascent,
		badgeFont->elided(
			NodeSubtitle(flow, key),
			x + width - padding.right() - textLeft));

	if (!layout.textRect.isEmpty()) {
		auto text = trigger
			? TriggerPreview(flow, key)
			: BlockPreview(FlowBlock(flow, key));
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

	const auto rows = trigger
		? std::vector<NodeRow>()
		: BlockRows(flow, FlowBlock(flow, key));
	const auto buttonFont = style::font(st::botStudioNodeButtonFont);
	const auto inner = width - padding.left() - padding.right();
	p.setFont(buttonFont);
	for (auto i = 0; i != int(layout.rows.size()); ++i) {
		const auto &row = rows[i];
		const auto r = layout.rows[i];
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
			: (row.button || row.port)
			? st::windowActiveTextFg->c
			: st::windowFg->c);
		p.drawText(
			r.marginsRemoved({ 10, 0, 14, 0 }),
			Qt::AlignVCenter | (row.button ? Qt::AlignHCenter : Qt::AlignLeft),
			buttonFont->elided(row.text + suffix, inner - 24));
	}

	if (!layout.footer.isEmpty()) {
		const auto input = Input(FlowBlock(flow, key));
		p.setFont(badgeFont);
		p.setPen(kInputColor);
		p.drawText(
			layout.footer,
			Qt::AlignVCenter | Qt::AlignLeft,
			badgeFont->elided(
				u"✍ ждёт ответ: "_q
					+ KindName(input.value(u"kind"_q).toString())
					+ u" → {"_q
					+ input.value(u"variable"_q).toString()
					+ u"}"_q,
				inner));
	}
}

void FlowCanvas::paintPorts(
		QPainter &p,
		const QString &key,
		const NodeLayout &layout) {
	const auto sub = st::windowSubTextFg->c;
	if (layout.hasInput) {
		p.setPen(QPen(WithAlpha(sub, (_drag == Drag::Link && key == _linkOver) ? 255 : 190), 1.5));
		p.setBrush((_drag == Drag::Link && key == _linkOver)
			? st::windowBgActive->c
			: st::botStudioCardBg->c);
		p.drawEllipse(layout.input, kPortRadius, kPortRadius);
	}
	for (const auto &port : layout.ports) {
		const auto connected = !port.target.isEmpty()
			&& FlowBlockIndex(_state->flow, port.target) >= 0;
		if (connected) {
			p.setPen(QPen(st::botStudioCardBg->c, 1.5));
			p.setBrush(port.color);
			p.drawEllipse(port.pos, kPortRadius + 0.5, kPortRadius + 0.5);
			continue;
		}
		const auto plus = port.pos + QPointF(kPlusOffset, 0);
		const auto hover = _hoverPlus && (*_hoverPlus == port.ref);
		p.setPen(QPen(WithAlpha(sub, 140), 1.5));
		p.drawLine(port.pos + QPointF(kPortRadius, 0), plus - QPointF(kPlusRadius, 0));
		p.setBrush(st::botStudioCardBg);
		p.setPen(QPen(port.color, 1.8));
		p.drawEllipse(port.pos, kPortRadius, kPortRadius);
		const auto box = QRectF(
			plus - QPointF(kPlusRadius, kPlusRadius),
			QSizeF(kPlusRadius * 2, kPlusRadius * 2));
		p.setPen(QPen(hover ? st::windowBgActive->c : WithAlpha(sub, 170), 1.4));
		p.setBrush(hover ? st::windowBgActive->c : st::botStudioCardBg->c);
		p.drawRoundedRect(box, 4, 4);
		p.setPen(QPen(hover ? st::windowFgActive->c : sub, 1.6));
		p.drawLine(plus - QPointF(4, 0), plus + QPointF(4, 0));
		p.drawLine(plus - QPointF(0, 4), plus + QPointF(0, 4));
	}
}

void FlowCanvas::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	p.fillRect(e->rect(), st::botStudioCanvasBg);

	// Dotted grid that moves with the view.
	const auto step = 22. * _zoom;
	if (step >= 7.) {
		p.setPen(Qt::NoPen);
		p.setBrush(WithAlpha(st::botStudioCanvasDot->c, 50));
		const auto radius = std::clamp(1.1 * _zoom, 0.7, 1.4);
		const auto startX = std::fmod(_offset.x(), step) - step;
		const auto startY = std::fmod(_offset.y(), step) - step;
		for (auto x = startX; x < width(); x += step) {
			for (auto y = startY; y < height(); y += step) {
				p.drawEllipse(QPointF(x, y), radius, radius);
			}
		}
	}

	p.translate(_offset);
	p.scale(_zoom, _zoom);
	const auto all = layouts();
	const auto links = connections(all);
	const auto accent = st::windowBgActive->c;
	for (const auto &link : links) {
		const auto active = (_hoverLink && *_hoverLink == link.ref)
			|| (_selectedLink && *_selectedLink == link.ref);
		const auto selected = _selectedLink && (*_selectedLink == link.ref);
		// Links back (to a menu) are secondary: thin and dashed.
		const auto faded = link.back && !active;
		const auto color = active
			? accent
			: WithAlpha(st::botStudioArrowFg->c, faded ? 85 : 150);
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(
			color,
			selected ? 3. : faded ? 1.4 : 2.,
			faded ? Qt::DashLine : Qt::SolidLine,
			Qt::RoundCap));
		p.drawPath(link.path);
		const auto end = link.path.currentPosition();
		auto head = QPainterPath(end);
		head.lineTo(end.x() - 9, end.y() - 5);
		head.lineTo(end.x() - 9, end.y() + 5);
		head.closeSubpath();
		p.setPen(Qt::NoPen);
		p.setBrush(color);
		p.drawPath(head.translated(-kPortRadius, 0));
	}
	if (_drag == Drag::Link) {
		auto to = _linkEnd;
		for (const auto &[key, l] : all) {
			if (key == _linkOver && l.hasInput) {
				to = l.input;
			}
		}
		auto pen = QPen(accent, 2., Qt::DashLine, Qt::RoundCap);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		p.drawPath(LinkPath(_linkFrom.pos, to));
	}
	for (const auto &[key, l] : all) {
		paintNode(p, key, l);
	}
	for (const auto &[key, l] : all) {
		paintPorts(p, key, l);
	}
	// A remove button on the hovered or selected connection.
	for (const auto &link : links) {
		const auto shown = (_hoverLink && *_hoverLink == link.ref)
			|| (_selectedLink && *_selectedLink == link.ref);
		if (!shown) {
			continue;
		}
		p.setPen(QPen(accent, 1.5));
		p.setBrush(st::botStudioCardBg);
		p.drawEllipse(link.middle, 11., 11.);
		p.setPen(QPen(accent, 1.8, Qt::SolidLine, Qt::RoundCap));
		p.drawLine(link.middle + QPointF(-4, -4), link.middle + QPointF(4, 4));
		p.drawLine(link.middle + QPointF(-4, 4), link.middle + QPointF(4, -4));
	}
	p.resetTransform();

	if (FlowBlocks(_state->flow).size() < 3) {
		p.setFont(st::botStudioNodeTextFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			QRect(0, height() - 70, width(), 24),
			Qt::AlignCenter,
			u"Тяните от точки справа у блока, чтобы соединить. «+» или двойной "
			"щелчок — новый узел. Ctrl + колесо — масштаб."_q);
	}
}

void FlowCanvas::resizeEvent(QResizeEvent *e) {
	if (!_fitted && width() > 100 && height() > 100) {
		_fitted = true;
		fitView();
	}
}

void FlowCanvas::fitView() {
	auto bounds = QRect();
	for (const auto &[key, l] : layouts()) {
		bounds = bounds.isNull() ? l.rect : bounds.united(l.rect);
	}
	if (bounds.isNull() || width() <= 0 || height() <= 0) {
		return;
	}
	// Room for the zoom bar below and the settings panel on the right.
	bounds = bounds.marginsAdded({ 40, 30, 80, 30 });
	const auto available = QSize(
		std::max(width() - _rightInset, 200),
		std::max(height() - 70, 150));
	_zoom = std::clamp(
		std::min(
			std::min(
				available.width() / double(bounds.width()),
				available.height() / double(bounds.height())),
			1.),
		kMinZoom,
		kMaxZoom);
	_offset = QPointF(
		(available.width() - bounds.width() * _zoom) / 2. - bounds.x() * _zoom,
		std::max((available.height() - bounds.height() * _zoom) / 2., 0.) - bounds.y() * _zoom);
	_zoomValue = _zoom;
	update();
}

void FlowCanvas::zoomTo(double zoom, QPointF anchor) {
	zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
	const auto logical = toLogical(anchor);
	_zoom = zoom;
	_offset = anchor - logical * zoom;
	_zoomValue = zoom;
	update();
}

void FlowCanvas::ensureVisible(const QString &key) {
	const auto rect = layout(key).rect;
	if (rect.isNull()) {
		return;
	}
	const auto screen = QRectF(toScreen(rect.topLeft()), toScreen(rect.bottomRight()));
	const auto view = QRectF(this->rect()).marginsRemoved(
		{ 30., 30., 30. + _rightInset, 70. });
	auto shift = QPointF();
	if (screen.right() > view.right()) {
		shift.setX(view.right() - screen.right());
	}
	if (screen.left() + shift.x() < view.left()) {
		shift.setX(view.left() - screen.left());
	}
	if (screen.bottom() > view.bottom()) {
		shift.setY(view.bottom() - screen.bottom());
	}
	if (screen.top() + shift.y() < view.top()) {
		shift.setY(view.top() - screen.top());
	}
	_offset += shift;
	update();
}

void FlowCanvas::arrange() {
	auto &flow = _state->flow;
	const auto keys = NodeKeys(flow);
	auto column = base::flat_map<QString, int>();
	auto order = std::vector<QString>();
	auto queue = std::deque<QString>();
	const auto visit = [&](const QString &key, int col) {
		if (key.isEmpty() || column.contains(key)) {
			return;
		} else if (!IsTrigger(key) && FlowBlockIndex(flow, key) < 0) {
			return;
		}
		column[key] = col;
		order.push_back(key);
		queue.push_back(key);
	};
	const auto run = [&] {
		while (!queue.empty()) {
			const auto key = queue.front();
			queue.pop_front();
			for (const auto &port : layout(key).ports) {
				visit(port.target, column[key] + 1);
			}
		}
	};
	for (const auto &key : keys) {
		if (IsTrigger(key)) {
			visit(key, 0);
		}
	}
	run();
	visit(flow.value(u"start"_q).toString(), 1);
	run();
	for (const auto &key : keys) {
		visit(key, 1);
		run();
	}
	auto columnHeight = base::flat_map<int, int>();
	auto y = base::flat_map<QString, int>();
	for (const auto &key : order) {
		const auto col = column[key];
		y[key] = columnHeight[col];
		columnHeight[col] += layout(key).rect.height() + 36;
	}
	auto tallest = 0;
	for (const auto &[col, height] : columnHeight) {
		tallest = std::max(tallest, height);
	}
	for (const auto &key : order) {
		const auto col = column[key];
		const auto x = (col == 0)
			? 0
			: (kTriggerWidth + 90 + (col - 1) * (st::botStudioNodeWidth + 90));
		const auto shift = (tallest - columnHeight[col]) / 2;
		SetNodePosition(flow, key, QPoint(Snap(x), Snap(y[key] + shift)));
	}
	fitView();
}

void FlowCanvas::select(const QString &key) {
	if (_state->selected != key) {
		_state->selected = key;
		_state->selected_changed();
	}
}

void FlowCanvas::updateHover(QPoint position) {
	const auto point = toLogical(position);
	auto hoverPlus = std::optional<LinkRef>();
	auto hoverLink = std::optional<LinkRef>();
	auto hoverNode = QString();
	auto cursor = style::cur_default;
	if (const auto plus = portAt(point, true)) {
		hoverPlus = plus->ref;
		cursor = style::cur_pointer;
	} else if (portAt(point, false)) {
		cursor = style::cur_cross;
	} else if (const auto remove = removeButtonAt(point)) {
		hoverLink = *remove;
		cursor = style::cur_pointer;
	} else if (const auto node = nodeAt(point); !node.isEmpty()) {
		hoverNode = node;
		cursor = style::cur_pointer;
	} else if (const auto link = connectionAt(point)) {
		hoverLink = link->ref;
		cursor = style::cur_pointer;
	}
	setCursor(cursor);
	if (hoverPlus != _hoverPlus
		|| hoverLink != _hoverLink
		|| hoverNode != _hoverNode) {
		_hoverPlus = hoverPlus;
		_hoverLink = hoverLink;
		_hoverNode = hoverNode;
		update();
	}
}

void FlowCanvas::mousePressEvent(QMouseEvent *e) {
	if (pressed) {
		pressed();
	}
	setFocus();
	const auto point = toLogical(e->pos());
	if (e->button() == Qt::MiddleButton) {
		_drag = Drag::Pan;
		_pressPosition = e->pos();
		_pressOffset = _offset;
		_moved = true;
		return;
	} else if (e->button() != Qt::LeftButton) {
		return;
	}
	if (const auto plus = portAt(point, true)) {
		_selectedLink = std::nullopt;
		if (pickRequest) {
			const auto at = plus->pos + QPointF(kPlusOffset, 0);
			pickRequest(
				toScreen(at).toPoint(),
				QPointF(at.x() + 60, at.y() - kHeaderHeight / 2.),
				plus->ref);
		}
		return;
	} else if (const auto port = portAt(point, false)) {
		_drag = Drag::Link;
		_linkFrom = *port;
		_linkEnd = point;
		_linkOver = QString();
		_moved = false;
		_pressPosition = e->pos();
		update();
		return;
	} else if (const auto remove = removeButtonAt(point)) {
		SetLink(_state->flow, *remove, QString());
		_selectedLink = _hoverLink = std::nullopt;
		_state->changed(true);
		return;
	}
	const auto node = nodeAt(point);
	if (!node.isEmpty()) {
		_selectedLink = std::nullopt;
		select(node);
		_drag = Drag::Node;
		_dragNode = node;
		_pressPosition = e->pos();
		_dragOrigin = NodePosition(_state->flow, node);
		_moved = false;
		update();
		return;
	} else if (const auto link = connectionAt(point)) {
		_selectedLink = link->ref;
		update();
		return;
	}
	_selectedLink = std::nullopt;
	_drag = Drag::Pan;
	_pressPosition = e->pos();
	_pressOffset = _offset;
	_moved = false;
	setCursor(style::cur_sizeall);
	update();
}

void FlowCanvas::mouseMoveEvent(QMouseEvent *e) {
	const auto point = toLogical(e->pos());
	switch (_drag) {
	case Drag::None:
		updateHover(e->pos());
		return;
	case Drag::Pan:
		if ((e->pos() - _pressPosition).manhattanLength() > 3) {
			_moved = true;
		}
		_offset = _pressOffset + QPointF(e->pos() - _pressPosition);
		update();
		return;
	case Drag::Node: {
		const auto delta = QPointF(e->pos() - _pressPosition) / _zoom;
		if (!_moved && (e->pos() - _pressPosition).manhattanLength() < 4) {
			return;
		}
		_moved = true;
		setCursor(style::cur_sizeall);
		SetNodePosition(
			_state->flow,
			_dragNode,
			QPoint(
				Snap(_dragOrigin.x() + delta.x()),
				Snap(_dragOrigin.y() + delta.y())));
		update();
	} return;
	case Drag::Link: {
		_linkEnd = point;
		if ((e->pos() - _pressPosition).manhattanLength() > 4) {
			_moved = true;
		}
		const auto over = nodeAt(point);
		_linkOver = (!over.isEmpty() && !IsTrigger(over)) ? over : QString();
		update();
	} return;
	}
}

void FlowCanvas::mouseReleaseEvent(QMouseEvent *e) {
	const auto drag = std::exchange(_drag, Drag::None);
	switch (drag) {
	case Drag::None:
		break;
	case Drag::Pan:
		if (!_moved) {
			select(QString());
		}
		break;
	case Drag::Node:
		if (_moved) {
			_state->changed(false);
		}
		break;
	case Drag::Link:
		if (!_linkOver.isEmpty()) {
			SetLink(_state->flow, _linkFrom.ref, _linkOver);
			_linkOver = QString();
			_state->changed(true);
		} else if (_moved && pickRequest) {
			pickRequest(
				e->pos(),
				QPointF(_linkEnd.x() + 20, _linkEnd.y() - kHeaderHeight / 2.),
				_linkFrom.ref);
		}
		break;
	}
	_moved = false;
	updateHover(e->pos());
	update();
}

void FlowCanvas::mouseDoubleClickEvent(QMouseEvent *e) {
	const auto point = toLogical(e->pos());
	if (nodeAt(point).isEmpty() && !connectionAt(point) && pickRequest) {
		pickRequest(e->pos(), point, std::nullopt);
	}
}

void FlowCanvas::wheelEvent(QWheelEvent *e) {
	const auto position = e->position();
	if (e->modifiers() & Qt::ControlModifier) {
		const auto delta = e->angleDelta().y();
		zoomTo(_zoom * std::pow(1.0015, delta), position);
	} else {
		auto delta = !e->pixelDelta().isNull()
			? QPointF(e->pixelDelta())
			: (QPointF(e->angleDelta()) / 2.);
		if (e->modifiers() & Qt::ShiftModifier) {
			delta = QPointF(delta.y(), delta.x());
		}
		_offset += delta;
		update();
	}
	updateHover(position.toPoint());
	e->accept();
}

void FlowCanvas::keyPressEvent(QKeyEvent *e) {
	const auto ctrl = (e->modifiers() & Qt::ControlModifier);
	const auto shift = (e->modifiers() & Qt::ShiftModifier);
	if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) {
		if (_selectedLink) {
			SetLink(_state->flow, *_selectedLink, QString());
			_selectedLink = std::nullopt;
			_state->changed(true);
		} else if (!_state->selected.isEmpty() && deleteRequest) {
			deleteRequest();
		}
	} else if (ctrl && e->key() == Qt::Key_Z) {
		if (const auto callback = shift ? redoRequest : undoRequest) {
			callback();
		}
	} else if (ctrl && e->key() == Qt::Key_Y) {
		if (redoRequest) {
			redoRequest();
		}
	} else if (ctrl && e->key() == Qt::Key_D) {
		if (duplicateRequest) {
			duplicateRequest();
		}
	} else if (ctrl && (e->key() == Qt::Key_Plus || e->key() == Qt::Key_Equal)) {
		zoomTo(_zoom * 1.2, QPointF(width() / 2., height() / 2.));
	} else if (ctrl && e->key() == Qt::Key_Minus) {
		zoomTo(_zoom / 1.2, QPointF(width() / 2., height() / 2.));
	} else if (ctrl && e->key() == Qt::Key_0) {
		zoomTo(1., QPointF(width() / 2., height() / 2.));
	} else if (e->key() == Qt::Key_Escape) {
		if (_selectedLink) {
			_selectedLink = std::nullopt;
			update();
		} else {
			select(QString());
		}
	} else {
		RpWidget::keyPressEvent(e);
	}
}

void FlowCanvas::contextMenuEvent(QContextMenuEvent *e) {
	const auto point = toLogical(e->pos());
	const auto node = nodeAt(point);
	if (!node.isEmpty()) {
		select(node);
		if (contextRequest) {
			contextRequest(node, e->globalPos());
		}
	} else if (pickRequest) {
		pickRequest(e->pos(), point, std::nullopt);
	}
}

void FlowCanvas::leaveEventHook(QEvent *e) {
	if (_hoverPlus || _hoverLink || !_hoverNode.isEmpty()) {
		_hoverPlus = _hoverLink = std::nullopt;
		_hoverNode = QString();
		update();
	}
}

namespace {

struct PickerItem {
	QString type;
	QString emoji;
	QString title;
	QString about;
	QString category;
	QColor color;
};

[[nodiscard]] std::vector<PickerItem> PickerItems(bool triggers) {
	auto result = std::vector<PickerItem>();
	if (triggers) {
		result.push_back({
			u"#command"_q,
			u"⚡"_q,
			u"Команда"_q,
			u"Бот отвечает на /команду из меню"_q,
			u"Запуск"_q,
			kTriggerColor,
		});
		result.push_back({
			u"#keyword"_q,
			u"🔑"_q,
			u"Ключевые слова"_q,
			u"Слово в сообщении открывает блок"_q,
			u"Запуск"_q,
			kTriggerColor,
		});
	}
	for (const auto &[type, category] : std::vector<std::pair<QString, QString>>{
			{ u"message"_q, u"Сообщения"_q },
			{ u"ai"_q, u"Сообщения"_q },
			{ u"operator"_q, u"Сообщения"_q },
			{ u"condition"_q, u"Логика"_q },
			{ u"random"_q, u"Логика"_q },
			{ u"action"_q, u"Логика"_q },
			{ u"catalog"_q, u"Магазин"_q },
			{ u"cart"_q, u"Магазин"_q },
			{ u"order"_q, u"Магазин"_q } }) {
		const auto &kind = KindOf(type);
		auto dummy = QJsonObject();
		if (type != u"message"_q) {
			dummy.insert(u"type"_q, type);
		}
		result.push_back({
			type,
			kind.title.section(' ', 0, 0),
			kind.title.section(' ', 1),
			kind.about,
			category,
			BlockColor(dummy, false),
		});
	}
	return result;
}

} // namespace

// The "add node" panel: search and node kinds by category.
class NodePicker final : public Ui::RpWidget {
public:
	NodePicker(QWidget *parent, bool triggers);

	void popup(QPoint point);
	Fn<void(QString)> chosen;
	Fn<void()> closed;

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void wheelEvent(QWheelEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	struct Line {
		int item = -1;
		QString header;
	};
	static constexpr auto kWidth = 330;
	static constexpr auto kHeaderLine = 26;
	static constexpr auto kItemLine = 48;
	static constexpr auto kListTop = 66;

	void applyFilter();
	void choose(int item);
	void moveSelection(int delta);
	[[nodiscard]] int lineTop(int index) const;
	[[nodiscard]] int lineHeight(int index) const;
	[[nodiscard]] int listHeight() const;
	[[nodiscard]] int lineAt(QPoint point) const;
	void ensureVisible(int line);

	const std::vector<PickerItem> _all;
	std::vector<Line> _lines;
	const not_null<Ui::InputField*> _search;
	int _selected = -1;
	int _over = -1;
	int _scroll = 0;

};

NodePicker::NodePicker(QWidget *parent, bool triggers)
: RpWidget(parent)
, _all(PickerItems(triggers))
, _search(Ui::CreateChild<Ui::InputField>(
	this,
	st::botStudioField,
	rpl::single(u"Поиск узла…"_q))) {
	setMouseTracking(true);
	_search->changes() | rpl::on_next([=] {
		applyFilter();
	}, _search->lifetime());
	_search->setSubmitSettings(Ui::InputField::SubmitSettings::Enter);
	_search->submits() | rpl::on_next([=] {
		if (_selected >= 0) {
			choose(_selected);
		}
	}, _search->lifetime());
	_search->cancelled() | rpl::on_next([=] {
		if (closed) {
			closed();
		}
	}, _search->lifetime());
	const auto edit = static_cast<QObject*>(_search->rawTextEdit().get());
	base::install_event_filter(edit, [=](not_null<QEvent*> e) {
		if (e->type() == QEvent::KeyPress) {
			const auto key = static_cast<QKeyEvent*>(e.get())->key();
			if (key == Qt::Key_Down || key == Qt::Key_Up) {
				moveSelection(key == Qt::Key_Down ? 1 : -1);
				return base::EventFilterResult::Cancel;
			}
		}
		return base::EventFilterResult::Continue;
	});
	applyFilter();
}

void NodePicker::applyFilter() {
	const auto query = _search->getLastText().trimmed().toLower();
	_lines.clear();
	auto category = QString();
	auto first = -1;
	for (auto i = 0; i != int(_all.size()); ++i) {
		const auto &item = _all[i];
		if (!query.isEmpty()
			&& !item.title.toLower().contains(query)
			&& !item.about.toLower().contains(query)
			&& !item.category.toLower().contains(query)) {
			continue;
		}
		if (item.category != category) {
			category = item.category;
			_lines.push_back({ .header = category });
		}
		_lines.push_back({ .item = i });
		if (first < 0) {
			first = i;
		}
	}
	_selected = first;
	_scroll = 0;
	update();
}

int NodePicker::lineHeight(int index) const {
	return (_lines[index].item >= 0) ? kItemLine : kHeaderLine;
}

int NodePicker::lineTop(int index) const {
	auto top = kListTop;
	for (auto i = 0; i != index; ++i) {
		top += lineHeight(i);
	}
	return top - _scroll;
}

int NodePicker::listHeight() const {
	auto result = 0;
	for (auto i = 0; i != int(_lines.size()); ++i) {
		result += lineHeight(i);
	}
	return result;
}

int NodePicker::lineAt(QPoint point) const {
	if (point.y() < kListTop || point.y() > height() - 8) {
		return -1;
	}
	for (auto i = 0; i != int(_lines.size()); ++i) {
		const auto top = lineTop(i);
		if (point.y() >= top && point.y() < top + lineHeight(i)) {
			return (_lines[i].item >= 0) ? i : -1;
		}
	}
	return -1;
}

void NodePicker::ensureVisible(int line) {
	const auto top = lineTop(line) + _scroll;
	const auto bottom = top + lineHeight(line);
	const auto visible = height() - 8 - kListTop;
	if (top - kListTop < _scroll) {
		_scroll = std::max(top - kListTop - kHeaderLine, 0);
	} else if (bottom - kListTop > _scroll + visible) {
		_scroll = bottom - kListTop - visible;
	}
}

void NodePicker::moveSelection(int delta) {
	auto items = std::vector<int>();
	for (const auto &line : _lines) {
		if (line.item >= 0) {
			items.push_back(line.item);
		}
	}
	if (items.empty()) {
		return;
	}
	const auto current = ranges::find(items, _selected);
	auto index = (current == end(items)) ? 0 : int(current - begin(items)) + delta;
	index = std::clamp(index, 0, int(items.size()) - 1);
	_selected = items[index];
	for (auto i = 0; i != int(_lines.size()); ++i) {
		if (_lines[i].item == _selected) {
			ensureVisible(i);
		}
	}
	update();
}

void NodePicker::choose(int item) {
	if (item >= 0 && item < int(_all.size()) && chosen) {
		chosen(_all[item].type);
	}
}

void NodePicker::popup(QPoint point) {
	const auto parent = parentWidget();
	const auto height = std::min(
		kListTop + listHeight() + 10,
		std::max(parent->height() - 20, 200));
	resize(kWidth, height);
	_search->resize(kWidth - 32, _search->height());
	_search->moveToLeft(16, 8);
	auto x = std::clamp(point.x() + 12, 10, parent->width() - kWidth - 10);
	auto y = std::clamp(point.y() - 30, 10, parent->height() - height - 10);
	move(x, y);
	show();
	raise();
	_search->setFocusFast();
}

void NodePicker::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto r = QRectF(rect()).marginsRemoved({ 1., 1., 1., 1. });
	p.setPen(QPen(WithAlpha(st::botStudioCardBorder->c, 160), 1));
	p.setBrush(st::botStudioCardBg);
	p.drawRoundedRect(r, 14, 14);

	p.setClipRect(QRect(0, kListTop, width(), height() - kListTop - 6));
	const auto titleFont = style::font(st::botStudioNodeTitleFont);
	const auto textFont = style::font(st::botStudioNodeTextFont);
	const auto badgeFont = style::font(st::botStudioNodeBadgeFont);
	auto emojiFont = titleFont->f;
	emojiFont.setPixelSize(17);
	for (auto i = 0; i != int(_lines.size()); ++i) {
		const auto top = lineTop(i);
		const auto &line = _lines[i];
		if (top + lineHeight(i) < kListTop || top > height()) {
			continue;
		}
		if (line.item < 0) {
			p.setFont(badgeFont);
			p.setPen(st::windowSubTextFg);
			p.drawText(
				QRect(18, top, width() - 36, kHeaderLine),
				Qt::AlignLeft | Qt::AlignBottom,
				line.header.toUpper());
			continue;
		}
		const auto &item = _all[line.item];
		const auto row = QRect(8, top + 2, width() - 16, kItemLine - 4);
		if (line.item == _selected || i == _over) {
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowBgOver);
			p.drawRoundedRect(row, 10, 10);
		}
		const auto tile = QRectF(row.x() + 8, row.y() + 7, 30, 30);
		p.setPen(Qt::NoPen);
		p.setBrush(WithAlpha(item.color, 60));
		p.drawRoundedRect(tile, 9, 9);
		p.setFont(emojiFont);
		p.setPen(st::windowFg);
		p.drawText(tile, Qt::AlignCenter, item.emoji);
		const auto left = row.x() + 48;
		p.setFont(titleFont);
		p.setPen(st::windowBoldFg);
		p.drawText(left, row.y() + 6 + titleFont->ascent, item.title);
		p.setFont(textFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			left,
			row.y() + 24 + textFont->ascent,
			textFont->elided(item.about, row.right() - left - 8));
	}
	if (_lines.empty()) {
		p.setFont(textFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			QRect(0, kListTop, width(), 40),
			Qt::AlignCenter,
			u"Ничего не найдено"_q);
	}
}

void NodePicker::mouseMoveEvent(QMouseEvent *e) {
	const auto over = lineAt(e->pos());
	if (_over != over) {
		_over = over;
		setCursor(over >= 0 ? style::cur_pointer : style::cur_default);
		update();
	}
}

void NodePicker::mousePressEvent(QMouseEvent *e) {
	const auto line = lineAt(e->pos());
	if (line >= 0) {
		choose(_lines[line].item);
	}
}

void NodePicker::wheelEvent(QWheelEvent *e) {
	const auto visible = height() - 8 - kListTop;
	const auto max = std::max(listHeight() - visible, 0);
	_scroll = std::clamp(_scroll - e->angleDelta().y() / 3, 0, max);
	update();
	e->accept();
}

void NodePicker::leaveEventHook(QEvent *e) {
	if (_over >= 0) {
		_over = -1;
		update();
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
, _add(MakeButton(this, u"＋ Добавить узел"_q, st::botStudioSmallButton))
, _undo(MakeButton(this, u"↶"_q, st::botStudioSmallButton))
, _redo(MakeButton(this, u"↷"_q, st::botStudioSmallButton))
, _settings(MakeButton(this, u"⚙ Настройки бота"_q, st::botStudioSmallButton))
, _products(MakeButton(this, u"🛍 Товары"_q, st::botStudioSmallButton))
, _save(MakeButton(this, u"Сохранить и запустить"_q, st::botStudioActionButton))
, _status(Ui::CreateChild<Ui::FlatLabel>(this, QString(), st::botStudioHint))
, _canvas(Ui::CreateChild<FlowCanvas>(this, _state))
, _zoomBar(Ui::CreateChild<Ui::RpWidget>(this))
, _inspectorScroll(Ui::CreateChild<Ui::ScrollArea>(
	this,
	st::defaultSolidScroll))
, _panelClose(Ui::CreateChild<Ui::IconButton>(
	this,
	st::botStudioRemoveButton))
, _panelBorder(Ui::CreateChild<Ui::RpWidget>(this)) {
	_panelBorder->paintRequest(
	) | rpl::on_next([=](QRect clip) {
		auto p = QPainter(_panelBorder);
		p.fillRect(clip, st::shadowFg);
	}, _panelBorder->lifetime());
	_state->flow = PrepareFlow(_studio->flow(_botId));
	_committed = _state->flow;
	_state->changed = [=](bool rebuild) {
		markChanged(rebuild);
	};
	_state->selected_changed = [=] {
		_canvas->update();
		rebuildInspector();
		updateGeometry();
	};
	_canvas->pickRequest = [=](
			QPoint screen,
			QPointF logical,
			std::optional<LinkRef> from) {
		showPicker(_canvas->pos() + screen, logical, from);
	};
	_canvas->pressed = [=] {
		hidePicker();
	};
	_canvas->deleteRequest = [=] { deleteSelected(); };
	_canvas->duplicateRequest = [=] { duplicateSelected(); };
	_canvas->undoRequest = [=] { undo(); };
	_canvas->redoRequest = [=] { redo(); };
	_canvas->contextRequest = [=](QString key, QPoint global) {
		const auto menu = Ui::CreateChild<Ui::PopupMenu>(
			this,
			st::defaultPopupMenu);
		if (!IsTrigger(key)) {
			menu->addAction(u"★ Сделать стартовым"_q, [=] {
				_state->flow.insert(u"start"_q, key);
				markChanged(true);
			});
			menu->addAction(u"⧉ Дублировать (Ctrl+D)"_q, [=] {
				duplicateSelected();
			});
		}
		if (key != u"#fallback"_q) {
			menu->addAction(u"🗑 Удалить (Delete)"_q, [=] {
				deleteSelected();
			});
		}
		menu->popup(global);
	};
	setupToolbar();
	setupZoomBar();
	_inspectorScroll->paintRequest(
	) | rpl::on_next([=](QRect clip) {
		auto p = QPainter(_inspectorScroll);
		p.fillRect(clip, st::botStudioInspectorBg);
	}, _inspectorScroll->lifetime());
	_panelClose->setClickedCallback([=] {
		_state->selected = QString();
		_state->selected_changed();
	});
	rebuildInspector();
	updateStatus();
}

FlowEditor::~FlowEditor() = default;

bool FlowEditor::dirty() const {
	return _state->dirty;
}

void FlowEditor::setupToolbar() {
	_add->setClickedCallback([=] {
		// The picker opens a bit above the point it is given.
		const auto at = _add->geometry().bottomLeft() + QPoint(-12, 36);
		showPicker(at, freeSpot(), std::nullopt);
	});
	_undo->setClickedCallback([=] { undo(); });
	_redo->setClickedCallback([=] { redo(); });
	_settings->setClickedCallback([=] { showSettings(); });
	_products->setClickedCallback([=] { showProducts(); });
	_save->setClickedCallback([=] { save(); });
	updateUndoButtons();
}

void FlowEditor::setupZoomBar() {
	const auto out = MakeButton(_zoomBar, u"−"_q, st::botStudioSmallButton);
	_zoomLabel = MakeButton(_zoomBar, u"100%"_q, st::botStudioSmallButton);
	const auto in = MakeButton(_zoomBar, u"+"_q, st::botStudioSmallButton);
	const auto fit = MakeButton(_zoomBar, u"⤢ Вместить"_q, st::botStudioSmallButton);
	const auto arrange = MakeButton(_zoomBar, u"✨ Упорядочить"_q, st::botStudioSmallButton);
	const auto center = [=] {
		return QPointF(_canvas->width() / 2., _canvas->height() / 2.);
	};
	out->setClickedCallback([=] { _canvas->zoomTo(_canvas->zoom() / 1.2, center()); });
	in->setClickedCallback([=] { _canvas->zoomTo(_canvas->zoom() * 1.2, center()); });
	_zoomLabel->setClickedCallback([=] { _canvas->zoomTo(1., center()); });
	fit->setClickedCallback([=] { _canvas->fitView(); });
	arrange->setClickedCallback([=] {
		_canvas->arrange();
		markChanged(false);
	});
	_canvas->zoomValue(
	) | rpl::on_next([=](double zoom) {
		_zoomLabel->setText(rpl::single(u"%1%"_q.arg(qRound(zoom * 100))));
		layoutZoomBar();
	}, _zoomBar->lifetime());
	_zoomBar->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(_zoomBar);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(QPen(WithAlpha(st::botStudioCardBorder->c, 120), 1));
		p.setBrush(st::botStudioCardBg);
		const auto r = QRectF(_zoomBar->rect()).marginsRemoved({ .5, .5, .5, .5 });
		p.drawRoundedRect(r, r.height() / 2., r.height() / 2.);
	}, _zoomBar->lifetime());
	_zoomButtons = { out, _zoomLabel, in, fit, arrange };
	layoutZoomBar();
}

void FlowEditor::layoutZoomBar() {
	auto left = 5;
	auto height = 0;
	for (const auto button : _zoomButtons) {
		button->moveToLeft(left, 5);
		left += button->width() + 2;
		height = std::max(height, button->height());
	}
	_zoomBar->resize(left + 3, height + 10);
}

QPointF FlowEditor::freeSpot() const {
	const auto selected = _state->selected;
	if (!selected.isEmpty()) {
		const auto rect = _canvas->nodeRect(selected);
		return QPointF(rect.x() + rect.width() + 90, rect.y());
	}
	return _canvas->toLogical(QPointF(
		_canvas->width() / 2. - 120,
		_canvas->height() / 2. - 60));
}

void FlowEditor::showPicker(
		QPoint at,
		QPointF logical,
		std::optional<LinkRef> from) {
	hidePicker();
	_picker = base::make_unique_q<NodePicker>(this, !from.has_value());
	_picker->chosen = [=](QString type) {
		hidePicker();
		createNode(type, logical, from);
	};
	_picker->closed = [=] {
		hidePicker();
	};
	_picker->popup(at);
}

void FlowEditor::hidePicker() {
	if (const auto picker = _picker.release()) {
		picker->hide();
		picker->deleteLater();
	}
}

void FlowEditor::createNode(
		const QString &type,
		QPointF logical,
		std::optional<LinkRef> from) {
	auto &flow = _state->flow;
	auto position = QPoint(Snap(logical.x()), Snap(logical.y()));
	const auto trigger = type.startsWith('#');
	// Move down until the new card doesn't cover another one.
	for (auto attempt = 0; attempt != 60; ++attempt) {
		const auto wanted = QRect(
			position,
			QSize(trigger ? kTriggerWidth : st::botStudioNodeWidth, trigger ? 80 : 120));
		auto busy = false;
		for (const auto &other : NodeKeys(flow)) {
			const auto rect = _canvas->nodeRect(other);
			if (rect.marginsAdded({ 16, 16, 16, 16 }).intersects(wanted)) {
				busy = true;
				position.setY(Snap(rect.y() + rect.height() + 30));
				break;
			}
		}
		if (!busy) {
			break;
		}
	}
	auto key = QString();
	if (type == u"#command"_q || type == u"#keyword"_q) {
		const auto list = (type == u"#command"_q) ? u"commands"_q : u"keywords"_q;
		auto array = flow.value(list).toArray();
		auto object = QJsonObject{
			{ u"x"_q, position.x() },
			{ u"y"_q, position.y() },
		};
		if (type == u"#command"_q) {
			auto n = array.size() + 1;
			auto names = QStringList();
			for (const auto &value : array) {
				names.push_back(value.toObject().value(u"command"_q).toString());
			}
			while (names.contains(u"command%1"_q.arg(n))) {
				++n;
			}
			object.insert(u"command"_q, u"command%1"_q.arg(n));
			object.insert(u"description"_q, u"Новая команда"_q);
		} else {
			object.insert(u"words"_q, QString());
		}
		array.push_back(object);
		flow.insert(list, array);
		key = ((type == u"#command"_q) ? u"#cmd:%1"_q : u"#kw:%1"_q)
			.arg(array.size() - 1);
	} else {
		key = NewFlowBlockId(flow);
		auto block = QJsonObject{
			{ u"id"_q, key },
			{ u"title"_q, DefaultTitle(type, FlowBlocks(flow).size() + 1) },
		};
		if (type == u"message"_q) {
			block.insert(u"text"_q, QString());
		}
		ApplyTypeDefaults(block, type);
		block.insert(u"x"_q, position.x());
		block.insert(u"y"_q, position.y());
		SetFlowBlock(flow, block);
		if (from) {
			SetLink(flow, *from, key);
		}
	}
	_state->selected = key;
	markChanged(true);
	updateGeometry();
	_canvas->ensureVisible(key);
}

void FlowEditor::deleteSelected() {
	const auto key = _state->selected;
	if (key.isEmpty() || key == u"#fallback"_q) {
		return;
	} else if (IsTrigger(key)) {
		RemoveTrigger(_state->flow, key);
	} else if (FlowBlocks(_state->flow).size() < 2) {
		return;
	} else {
		RemoveFlowBlock(_state->flow, key);
	}
	_canvas->resetLinkSelection();
	_state->selected = QString();
	markChanged(true);
	updateGeometry();
}

void FlowEditor::duplicateSelected() {
	const auto key = _state->selected;
	if (key.isEmpty() || IsTrigger(key)) {
		return;
	}
	auto copy = FlowBlock(_state->flow, key);
	copy.insert(u"id"_q, NewFlowBlockId(_state->flow));
	copy.insert(u"title"_q, copy.value(u"title"_q).toString() + u" (копия)"_q);
	copy.insert(u"x"_q, copy.value(u"x"_q).toInt() + 40);
	copy.insert(u"y"_q, copy.value(u"y"_q).toInt() + 40);
	SetFlowBlock(_state->flow, copy);
	_state->selected = copy.value(u"id"_q).toString();
	markChanged(true);
}

void FlowEditor::commit(bool force) {
	if (_state->flow == _committed) {
		return;
	}
	const auto now = crl::now();
	if (force || _undoStack.empty() || now - _lastCommit > 1000) {
		_undoStack.push_back(_committed);
		if (_undoStack.size() > 100) {
			_undoStack.erase(_undoStack.begin());
		}
	}
	_redoStack.clear();
	_committed = _state->flow;
	_lastCommit = now;
	updateUndoButtons();
}

void FlowEditor::restore(QJsonObject flow) {
	_state->flow = std::move(flow);
	_committed = _state->flow;
	_lastCommit = 0;
	_state->dirty = true;
	if (!_state->selected.isEmpty()
		&& NodeObject(_state->flow, _state->selected).isEmpty()) {
		_state->selected = QString();
	}
	_canvas->resetLinkSelection();
	_canvas->update();
	rebuildInspector();
	updateStatus();
	updateUndoButtons();
}

void FlowEditor::undo() {
	commit(true);
	if (_undoStack.empty()) {
		return;
	}
	_redoStack.push_back(_state->flow);
	auto flow = std::move(_undoStack.back());
	_undoStack.pop_back();
	restore(std::move(flow));
}

void FlowEditor::redo() {
	if (_redoStack.empty()) {
		return;
	}
	_undoStack.push_back(_state->flow);
	auto flow = std::move(_redoStack.back());
	_redoStack.pop_back();
	restore(std::move(flow));
}

void FlowEditor::updateUndoButtons() {
	_undo->setTextFgOverride(_undoStack.empty()
		? std::make_optional(st::windowSubTextFg->c)
		: std::nullopt);
	_redo->setTextFgOverride(_redoStack.empty()
		? std::make_optional(st::windowSubTextFg->c)
		: std::nullopt);
}

void FlowEditor::markChanged(bool rebuild) {
	_state->dirty = true;
	commit(rebuild);
	_canvas->update();
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
	const auto flow = PrepareFlow(_studio->flow(_botId));
	if (flow == _state->flow) {
		return;
	}
	_state->flow = flow;
	_committed = flow;
	_undoStack.clear();
	_redoStack.clear();
	updateUndoButtons();
	if (!_state->selected.isEmpty()
		&& NodeObject(flow, _state->selected).isEmpty()) {
		_state->selected = QString();
	}
	_canvas->update();
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
	if (IsTrigger(id)) {
		const auto object = TriggerObject(_state->flow, id);
		const auto updateTrigger = [=](Fn<void(QJsonObject&)> change, bool rebuild) {
			auto o = TriggerObject(_state->flow, id);
			change(o);
			SetTriggerObject(_state->flow, id, o);
			if (rebuild) {
				crl::on_main(this, [=] { markChanged(true); });
			} else {
				markChanged(false);
			}
		};
		const auto setGoto = [=](QString chosen) {
			SetLink(_state->flow, LinkRef{ id }, chosen);
			crl::on_main(this, [=] { markChanged(true); });
		};
		const auto stringField = [&](
				const QString &key,
				const QString &placeholder,
				bool multiline,
				Fn<QString(QString)> clean) {
			field(placeholder, object.value(key).toString(), multiline, [=](QString text) {
				updateTrigger([&](QJsonObject &o) {
					o.insert(key, clean ? clean(text) : text);
				}, false);
			});
		};
		if (id.startsWith(u"#cmd:"_q)) {
			section(u"⚡ Команда"_q, 16);
			hint(u"Пользователь отправляет команду или выбирает её в меню "
				"бота — бот показывает блок, к которому подключён выход."_q);
			stringField(u"command"_q, u"Команда без «/», например: start"_q, false, [](QString text) {
				return text.trimmed().remove('/').toLower();
			});
			stringField(u"description"_q, u"Описание в меню бота"_q, false, nullptr);
			labeledTarget(u"Показать:"_q, object.value(u"goto"_q).toString(), setGoto);
		} else if (id.startsWith(u"#kw:"_q)) {
			section(u"🔑 Ключевые слова"_q, 16);
			hint(u"Если сообщение содержит одно из слов (через запятую), бот "
				"покажет подключённый блок."_q);
			stringField(u"words"_q, u"Слова через запятую: цена, стоимость"_q, true, nullptr);
			labeledTarget(u"Показать:"_q, object.value(u"goto"_q).toString(), setGoto);
		} else {
			section(u"❓ Если бот не понял"_q, 16);
			hint(u"Что делать с сообщением, которое не подошло ни под кнопку, "
				"ни под команду, ни под ключевые слова."_q);
			const auto mode = object.value(u"mode"_q).toString();
			chips({
				{ u"start"_q, u"Показать старт"_q },
				{ u"block"_q, u"Показать блок"_q },
				{ u"text"_q, u"Ответить текстом"_q },
				{ u"ai"_q, u"✦ Ответить через ИИ"_q },
			}, mode.isEmpty() ? u"start"_q : mode, [=](QString value) {
				updateTrigger([&](QJsonObject &o) {
					o.insert(u"mode"_q, value);
				}, true);
			});
			if (mode == u"block"_q) {
				labeledTarget(u"Показать:"_q, object.value(u"goto"_q).toString(), setGoto);
			} else if (mode == u"text"_q) {
				stringField(u"text"_q, u"Текст ответа"_q, true, nullptr);
			} else if (mode == u"ai"_q) {
				stringField(u"prompt"_q, u"Кто такой бот и как отвечать (роль для ИИ)"_q, true, nullptr);
				hint(u"Используется ИИ, настроенный в Agentgram. ИИ помнит "
					"последние сообщения собеседника."_q);
			}
		}
		if (id != u"#fallback"_q) {
			const auto remove = row(object_ptr<Ui::RoundButton>(
				layout,
				rpl::single(u"Удалить"_q),
				st::botStudioDangerButton), 18);
			const auto button = static_cast<Ui::RoundButton*>(remove);
			button->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
			button->setClickedCallback([=] {
				crl::on_main(this, [=] { deleteSelected(); });
			});
		}
	} else if (block.isEmpty()) {
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
		box->setTitle(rpl::single(u"Настройки бота"_q));
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
			state->flow = PrepareFlow(*draft);
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
	for (const auto button : { _add.get(), _undo.get(), _redo.get(), _settings.get(), _products.get() }) {
		button->moveToLeft(left, 0);
		left += button->width() + ((button == _redo.get()) ? st::botStudioButtonSkip * 2 : 4);
	}
	left += st::botStudioButtonSkip;
	_save->moveToLeft(width - _save->width(), 0);
	const auto statusWidth = std::max(
		width - left - _save->width() - st::botStudioButtonSkip * 2,
		60);
	_status->resizeToWidth(statusWidth);
	_status->moveToLeft(left, (_save->height() - _status->height()) / 2);

	const auto top = _save->height() + st::botStudioToolbarSkip;
	const auto height = std::max(this->height() - top, 100);
	_canvas->setGeometry(0, top, width, height);
	_zoomBar->moveToLeft(12, top + height - _zoomBar->height() - 12);

	const auto panel = !_state->selected.isEmpty();
	const auto panelWidth = std::min(st::botStudioInspectorWidth, width / 2);
	_inspectorScroll->setVisible(panel);
	_panelClose->setVisible(panel);
	_panelBorder->setVisible(panel);
	_canvas->setRightInset(panel ? panelWidth : 0);
	if (panel) {
		_inspectorScroll->setGeometry(
			width - panelWidth,
			top,
			panelWidth,
			height);
		if (_inspector) {
			_inspector->resizeToWidth(panelWidth);
		}
		_inspectorScroll->raise();
		_panelBorder->setGeometry(width - panelWidth - 1, top, 1, height);
		_panelBorder->raise();
		_panelClose->moveToLeft(width - _panelClose->width() - 6, top + 6);
		_panelClose->raise();
	}
	_zoomBar->raise();
	if (_picker) {
		_picker->raise();
	}
}

} // namespace BotStudio
