/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "bot_studio/bot_studio_manager.h"

#include "ai_agent/ai_agent_manager.h"
#include "base/unixtime.h"
#include "bot_studio/bot_studio_bot_api.h"
#include "bot_studio/bot_studio_flow.h"
#include "main/main_session.h"
#include "storage/storage_account.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace BotStudio {
namespace {

constexpr auto kBotsPref = "bot_studio_bots";
constexpr auto kStoragePrefPrefix = "bot_studio_storage_";
constexpr auto kMaxLogs = 500;
constexpr auto kStorageSaveDelay = crl::time(2000);
constexpr auto kHttpTimeout = 30 * 1000;
constexpr auto kMaxHttpResponse = 5 * 1024 * 1024;
constexpr auto kMaxLogText = 4000;

[[nodiscard]] std::string StorageKey(const QString &id) {
	return kStoragePrefPrefix + id.toStdString();
}

[[nodiscard]] QString Short(QString text, int limit = 80) {
	text = text.simplified();
	return (text.size() > limit) ? (text.left(limit - 1) + QChar(0x2026)) : text;
}

[[nodiscard]] QString UserName(const QJsonObject &user) {
	const auto username = user.value(u"username"_q).toString();
	if (!username.isEmpty()) {
		return '@' + username;
	}
	return (user.value(u"first_name"_q).toString()
		+ ' '
		+ user.value(u"last_name"_q).toString()).trimmed();
}

[[nodiscard]] QString DescribeUpdate(const QByteArray &json) {
	const auto update = QJsonDocument::fromJson(json).object();
	for (const auto key : {
			"message",
			"edited_message",
			"channel_post",
			"business_message" }) {
		const auto message = update.value(QString::fromLatin1(key)).toObject();
		if (message.isEmpty()) {
			continue;
		}
		const auto from = message.value(u"from"_q).toObject();
		const auto chat = message.value(u"chat"_q).toObject();
		auto who = UserName(from);
		if (who.isEmpty()) {
			who = chat.value(u"title"_q).toString();
		}
		const auto chatTitle = chat.value(u"title"_q).toString();
		if (!chatTitle.isEmpty() && who != chatTitle) {
			who += u" в «"_q + chatTitle + u"»"_q;
		}
		auto content = message.value(u"text"_q).toString();
		if (content.isEmpty()) {
			for (const auto kind : {
					"photo", "video", "document", "voice", "audio",
					"sticker", "animation", "video_note", "location",
					"contact", "poll" }) {
				if (message.contains(QString::fromLatin1(kind))) {
					content = '[' + QString::fromLatin1(kind) + ']';
					break;
				}
			}
			const auto caption = message.value(u"caption"_q).toString();
			if (!caption.isEmpty()) {
				content += ' ' + caption;
			}
		}
		if (content.isEmpty()) {
			content = u"[служебное сообщение]"_q;
		}
		return who + u": "_q + Short(content);
	}
	const auto callback = update.value(u"callback_query"_q).toObject();
	if (!callback.isEmpty()) {
		// Show the button text, not its internal data.
		const auto data = callback.value(u"data"_q).toString();
		auto label = data;
		const auto keyboard = callback.value(u"message"_q).toObject().value(
			u"reply_markup"_q).toObject().value(u"inline_keyboard"_q).toArray();
		for (const auto &row : keyboard) {
			for (const auto &button : row.toArray()) {
				const auto object = button.toObject();
				if (object.value(u"callback_data"_q).toString() == data) {
					label = object.value(u"text"_q).toString();
				}
			}
		}
		return UserName(callback.value(u"from"_q).toObject())
			+ u" нажал кнопку «"_q
			+ Short(label, 40)
			+ u"»"_q;
	}
	const auto keys = update.keys();
	for (const auto &key : keys) {
		if (key != u"update_id"_q) {
			return u"событие "_q + key;
		}
	}
	return u"событие"_q;
}

[[nodiscard]] QString FriendlyError(const ApiError &error) {
	if (error.code == 401 || error.code == 404) {
		return u"Telegram не принял токен бота. Проверьте токен из "
			"@BotFather."_q;
	} else if (error.code == 409) {
		return u"Этот бот уже получает обновления в другом месте "
			"(другая программа или вебхук). Остановите её."_q;
	}
	return error.text;
}

[[nodiscard]] QString NewBotCode() {
	return Templates().front().code;
}

} // namespace

struct Studio::Instance {
	std::unique_ptr<BotApi> api;
	std::unique_ptr<Runtime> runtime;
	BotStatus status = BotStatus::Stopped;
	QString error;
	QString lastPollError;
};

Studio::Studio(not_null<Main::Session*> session)
: _session(session)
, _network(std::make_unique<QNetworkAccessManager>())
, _storageTimer([=] { saveStorage(); }) {
	load();
	crl::on_main(this, [=] {
		for (const auto &bot : _bots) {
			if (bot.enabled) {
				launch(bot.id);
			}
		}
	});
}

Studio::~Studio() {
	saveStorage();
	for (auto &[id, instance] : base::take(_instances)) {
		instance->runtime = nullptr;
		instance->api = nullptr;
	}
}

not_null<Studio*> Studio::Get(not_null<Main::Session*> session) {
	static auto studios = base::flat_map<
		not_null<Main::Session*>,
		std::unique_ptr<Studio>>();
	const auto i = studios.find(session);
	if (i != end(studios)) {
		return i->second.get();
	}
	const auto result = studios.emplace(
		session,
		std::make_unique<Studio>(session)).first->second.get();
	session->lifetime().add([=] {
		studios.remove(session);
	});
	return result;
}

void Studio::load() {
	const auto raw = _session->local().readPref<QString>(kBotsPref);
	const auto list = QJsonDocument::fromJson(raw.toUtf8()).array();
	for (const auto &value : list) {
		const auto object = value.toObject();
		auto bot = BotInfo{
			.id = object.value(u"id"_q).toString(),
			.name = object.value(u"name"_q).toString(),
			.username = object.value(u"username"_q).toString(),
			.botId = int64(object.value(u"bot_id"_q).toDouble()),
			.token = object.value(u"token"_q).toString(),
			.code = object.value(u"code"_q).toString(),
			.flow = object.value(u"flow"_q).toString(),
			.enabled = object.value(u"enabled"_q).toBool(),
			.created = int64(object.value(u"created"_q).toDouble()),
			.updated = int64(object.value(u"updated"_q).toDouble()),
		};
		if (!bot.id.isEmpty() && !bot.token.isEmpty()) {
			_bots.push_back(std::move(bot));
		}
	}
}

void Studio::save() {
	auto list = QJsonArray();
	for (const auto &bot : _bots) {
		auto object = QJsonObject();
		object.insert(u"id"_q, bot.id);
		object.insert(u"name"_q, bot.name);
		object.insert(u"username"_q, bot.username);
		object.insert(u"bot_id"_q, double(bot.botId));
		object.insert(u"token"_q, bot.token);
		object.insert(u"code"_q, bot.code);
		if (bot.visual()) {
			object.insert(u"flow"_q, bot.flow);
		}
		object.insert(u"enabled"_q, bot.enabled);
		object.insert(u"created"_q, double(bot.created));
		object.insert(u"updated"_q, double(bot.updated));
		list.push_back(object);
	}
	_session->local().writePref<QString>(
		kBotsPref,
		QString::fromUtf8(QJsonDocument(list).toJson(
			QJsonDocument::Compact)));
}

const BotInfo *Studio::bot(const QString &id) const {
	const auto i = ranges::find(_bots, id, &BotInfo::id);
	return (i != end(_bots)) ? &*i : nullptr;
}

BotInfo *Studio::botMutable(const QString &id) {
	const auto i = ranges::find(_bots, id, &BotInfo::id);
	return (i != end(_bots)) ? &*i : nullptr;
}

const BotInfo *Studio::find(const QString &query) const {
	auto text = query.trimmed();
	if (const auto result = bot(text)) {
		return result;
	}
	if (text.startsWith('@')) {
		text = text.mid(1);
	}
	for (const auto &bot : _bots) {
		if (!bot.username.compare(text, Qt::CaseInsensitive)
			|| !bot.name.compare(text, Qt::CaseInsensitive)
			|| (bot.botId && QString::number(bot.botId) == text)) {
			return &bot;
		}
	}
	return (_bots.size() == 1 && text.isEmpty()) ? &_bots.front() : nullptr;
}

BotStatus Studio::status(const QString &id) const {
	const auto i = _instances.find(id);
	return (i != end(_instances)) ? i->second->status : BotStatus::Stopped;
}

QString Studio::statusError(const QString &id) const {
	const auto i = _instances.find(id);
	return (i != end(_instances)) ? i->second->error : QString();
}

int Studio::errorCount(const QString &id) const {
	const auto i = _errors.find(id);
	return (i != end(_errors)) ? i->second : 0;
}

const std::deque<LogEntry> &Studio::logs(const QString &id) const {
	static const auto empty = std::deque<LogEntry>();
	const auto i = _logs.find(id);
	return (i != end(_logs)) ? i->second : empty;
}

void Studio::clearLogs(const QString &id) {
	_logs.remove(id);
	_errors.remove(id);
	_botUpdated.fire_copy(id);
}

QByteArray Studio::storage(const QString &id) const {
	return loadStorage(id);
}

AudienceStats Studio::audience(const QString &id) const {
	auto result = AudienceStats();
	const auto root = QJsonDocument::fromJson(loadStorage(id)).object();
	const auto users = root.value(u"__users"_q).toObject();
	const auto weekAgo = base::unixtime::now() - 7 * 24 * 3600;
	for (auto i = users.begin(); i != users.end(); ++i) {
		const auto user = i.value().toObject();
		++result.users;
		if (user.value(u"blocked"_q).toBool()) {
			++result.blocked;
		} else if (user.value(u"last_seen"_q).toDouble() >= weekAgo) {
			++result.active;
		}
	}
	result.leads = root.value(u"leads"_q).toArray().size();
	return result;
}

QJsonArray Studio::leads(const QString &id) const {
	return QJsonDocument::fromJson(
		loadStorage(id)).object().value(u"leads"_q).toArray();
}

QString Studio::broadcast(
		const QString &id,
		const QString &text,
		const QString &photo) {
	const auto i = _instances.find(id);
	if (i == end(_instances)
		|| !i->second->runtime
		|| i->second->status != BotStatus::Running) {
		return u"Бот не запущен. Запустите его и повторите рассылку."_q;
	}
	auto request = QJsonObject{ { u"text"_q, text } };
	if (!photo.trimmed().isEmpty()) {
		request.insert(u"photo"_q, photo.trimmed());
	}
	i->second->runtime->broadcast(
		QJsonDocument(request).toJson(QJsonDocument::Compact));
	return QString();
}

void Studio::clearStorage(const QString &id) {
	_storage[id] = QByteArray("{}");
	_storageDirty.emplace(id);
	saveStorage();
	if (status(id) == BotStatus::Running) {
		restart(id);
	}
}

rpl::producer<> Studio::botsChanged() const {
	return _botsChanged.events();
}

rpl::producer<QString> Studio::botUpdated() const {
	return _botUpdated.events();
}

rpl::producer<QString> Studio::logAdded() const {
	return _logAdded.events();
}

void Studio::create(
		const QString &token,
		const QString &code,
		const QJsonObject &flow,
		Fn<void(QString id, QString error)> done) {
	const auto trimmed = token.trimmed();
	static const auto format = QRegularExpression(
		u"^\\d{5,}:[A-Za-z0-9_-]{30,}$"_q);
	if (!format.match(trimmed).hasMatch()) {
		done(QString(), u"Это не похоже на токен бота. Токен выглядит "
			"так: 123456789:AAH... — его выдаёт @BotFather."_q);
		return;
	}
	for (const auto &bot : _bots) {
		if (bot.token == trimmed) {
			done(bot.id, u"Этот бот уже добавлен в Bot Studio."_q);
			return;
		}
	}
	const auto api = std::make_shared<std::unique_ptr<BotApi>>(
		std::make_unique<BotApi>(_network.get(), trimmed));
	(*api)->call(u"getMe"_q, QJsonObject(), crl::guard(this, [=](
			QByteArray result,
			std::optional<ApiError> error) {
		// Destroy the client after its own callback returns.
		crl::on_main([=] { *api = nullptr; });
		if (error) {
			done(QString(), FriendlyError(*error));
			return;
		}
		const auto me = QJsonDocument::fromJson(result).object();
		const auto botId = int64(me.value(u"id"_q).toDouble());
		for (const auto &bot : _bots) {
			if (bot.botId == botId) {
				done(bot.id, u"Этот бот уже добавлен в Bot Studio."_q);
				return;
			}
		}
		const auto now = int64(base::unixtime::now());
		auto bot = BotInfo{
			.id = u"b"_q + QString::number(botId),
			.name = me.value(u"first_name"_q).toString(),
			.username = me.value(u"username"_q).toString(),
			.botId = botId,
			.token = trimmed,
			.code = !flow.isEmpty()
				? GenerateFlowCode(flow)
				: code.trimmed().isEmpty()
				? NewBotCode()
				: code,
			.flow = flow.isEmpty()
				? QString()
				: QString::fromUtf8(QJsonDocument(
					NormalizeFlow(flow)).toJson(QJsonDocument::Compact)),
			.created = now,
			.updated = now,
		};
		const auto id = bot.id;
		_bots.push_back(std::move(bot));
		save();
		_botsChanged.fire({});
		addLog(id, LogLevel::System, u"Бот добавлен в Bot Studio."_q);
		done(id, QString());
	}));
}

void Studio::setToken(
		const QString &id,
		const QString &token,
		Fn<void(QString error)> done) {
	const auto trimmed = token.trimmed();
	const auto current = bot(id);
	if (!current) {
		done(u"Бот не найден."_q);
		return;
	}
	const auto api = std::make_shared<std::unique_ptr<BotApi>>(
		std::make_unique<BotApi>(_network.get(), trimmed));
	(*api)->call(u"getMe"_q, QJsonObject(), crl::guard(this, [=](
			QByteArray result,
			std::optional<ApiError> error) {
		crl::on_main([=] { *api = nullptr; });
		const auto bot = botMutable(id);
		if (!bot) {
			done(u"Бот не найден."_q);
			return;
		} else if (error) {
			done(FriendlyError(*error));
			return;
		}
		const auto me = QJsonDocument::fromJson(result).object();
		const auto botId = int64(me.value(u"id"_q).toDouble());
		if (bot->botId && botId != bot->botId) {
			done(u"Это токен другого бота (@%1)."_q.arg(
				me.value(u"username"_q).toString()));
			return;
		}
		bot->token = trimmed;
		bot->username = me.value(u"username"_q).toString();
		bot->updated = int64(base::unixtime::now());
		save();
		_botUpdated.fire_copy(id);
		addLog(id, LogLevel::System, u"Токен обновлён."_q);
		restart(id);
		done(QString());
	}));
}

QString Studio::setCode(const QString &id, const QString &code) {
	if (const auto bot = botMutable(id)) {
		bot->flow = QString();
	}
	return applyCode(id, code);
}

QString Studio::setFlow(const QString &id, const QJsonObject &flow) {
	const auto bot = botMutable(id);
	if (!bot) {
		return u"Бот не найден."_q;
	}
	const auto normalized = NormalizeFlow(flow);
	bot->flow = QString::fromUtf8(
		QJsonDocument(normalized).toJson(QJsonDocument::Compact));
	return applyCode(id, GenerateFlowCode(normalized));
}

QJsonObject Studio::flow(const QString &id) const {
	const auto bot = this->bot(id);
	return (bot && bot->visual())
		? QJsonDocument::fromJson(bot->flow.toUtf8()).object()
		: QJsonObject();
}

void Studio::convertToCode(const QString &id) {
	if (const auto bot = botMutable(id); bot && bot->visual()) {
		bot->flow = QString();
		save();
		_botUpdated.fire_copy(id);
		addLog(id, LogLevel::System, u"Бот переведён в режим кода."_q);
	}
}

QString Studio::applyCode(const QString &id, const QString &code) {
	const auto bot = botMutable(id);
	if (!bot) {
		return u"Бот не найден."_q;
	}
	const auto error = Runtime::Check(code);
	bot->code = code;
	bot->updated = int64(base::unixtime::now());
	save();
	_botUpdated.fire_copy(id);
	if (!error.isEmpty()) {
		addLog(id, LogLevel::Error, u"Ошибка в коде, бот не "
			"перезапущен:\n"_q + error);
		return error;
	}
	if (bot->enabled) {
		addLog(id, LogLevel::System, u"Код сохранён, перезапускаю бота."_q);
		restart(id);
	} else {
		addLog(id, LogLevel::System, u"Код сохранён."_q);
	}
	return QString();
}

void Studio::start(const QString &id) {
	const auto bot = botMutable(id);
	if (!bot) {
		return;
	}
	if (!bot->enabled) {
		bot->enabled = true;
		save();
	}
	if (status(id) != BotStatus::Running
		&& status(id) != BotStatus::Starting) {
		launch(id);
	}
}

void Studio::stop(const QString &id) {
	if (const auto bot = botMutable(id); bot && bot->enabled) {
		bot->enabled = false;
		save();
	}
	if (_instances.contains(id)) {
		shutdown(id);
		addLog(id, LogLevel::System, u"Бот остановлен."_q);
	}
	setStatus(id, BotStatus::Stopped);
}

void Studio::restart(const QString &id) {
	const auto bot = this->bot(id);
	if (!bot || !bot->enabled) {
		return;
	}
	shutdown(id);
	launch(id);
}

void Studio::remove(const QString &id) {
	shutdown(id);
	_bots.erase(
		ranges::remove(_bots, id, &BotInfo::id),
		end(_bots));
	_logs.remove(id);
	_errors.remove(id);
	_storage.remove(id);
	_storageDirty.remove(id);
	_session->local().clearPref(StorageKey(id));
	save();
	_botsChanged.fire({});
}

void Studio::whenStarted(const QString &id, Fn<void()> callback) {
	if (status(id) != BotStatus::Starting) {
		callback();
	} else {
		_startWaiters[id].push_back(std::move(callback));
	}
}

void Studio::launch(const QString &id) {
	const auto bot = this->bot(id);
	if (!bot) {
		return;
	}
	shutdown(id);
	auto instance = std::make_unique<Instance>();
	instance->api = std::make_unique<BotApi>(_network.get(), bot->token);
	const auto api = instance->api.get();
	_instances.emplace(id, std::move(instance));
	setStatus(id, BotStatus::Starting);
	addLog(id, LogLevel::System, u"Запускаю бота…"_q);

	api->call(u"getMe"_q, QJsonObject(), crl::guard(api, [=](
			QByteArray me,
			std::optional<ApiError> error) {
		if (error) {
			const auto text = FriendlyError(*error);
			addLog(id, LogLevel::Error, u"Не удалось запустить: "_q + text);
			crl::on_main(this, [=] {
				shutdown(id);
				setStatus(id, BotStatus::Error, text);
			});
			return;
		}
		const auto object = QJsonDocument::fromJson(me).object();
		if (const auto bot = botMutable(id)) {
			const auto username = object.value(u"username"_q).toString();
			const auto name = object.value(u"first_name"_q).toString();
			if (bot->username != username || bot->name != name) {
				bot->username = username;
				bot->name = name;
				save();
				_botUpdated.fire_copy(id);
			}
		}
		const auto i = _instances.find(id);
		const auto bot = this->bot(id);
		if (i == end(_instances) || !bot) {
			return;
		}
		auto runtime = std::make_unique<Runtime>(RuntimeDescriptor{
			.host = makeHost(id),
			.storage = loadStorage(id),
			.me = me,
			.ownerId = int64(_session->userId().bare),
		});
		const auto raw = runtime.get();
		i->second->runtime = std::move(runtime);
		const auto loadError = raw->load(bot->code);
		if (!loadError.isEmpty()) {
			addLog(id, LogLevel::Error, u"Ошибка в коде бота:\n"_q + loadError);
			crl::on_main(this, [=] {
				shutdown(id);
				setStatus(
					id,
					BotStatus::Error,
					u"Ошибка в коде: "_q + loadError.section('\n', 0, 0));
			});
			return;
		}
		api->call(
			u"deleteWebhook"_q,
			QJsonObject(),
			[](QByteArray, std::optional<ApiError>) {});
		api->startPolling([=](QByteArray update) {
			addLog(id, LogLevel::Incoming, DescribeUpdate(update));
			const auto i = _instances.find(id);
			if (i != end(_instances) && i->second->runtime) {
				if (!i->second->lastPollError.isEmpty()) {
					i->second->lastPollError = QString();
				}
				i->second->runtime->dispatch(update);
			}
		}, [=](ApiError error) {
			const auto i = _instances.find(id);
			if (i == end(_instances)) {
				return;
			}
			const auto text = FriendlyError(error);
			if (i->second->lastPollError != text) {
				i->second->lastPollError = text;
				addLog(id, LogLevel::Warning, u"Ошибка получения "
					"обновлений: "_q + text);
			}
			if (error.code == 401 || error.code == 404) {
				crl::on_main(this, [=] {
					shutdown(id);
					setStatus(id, BotStatus::Error, text);
				});
			}
		});
		setStatus(id, BotStatus::Running);
		addLog(
			id,
			LogLevel::System,
			u"Бот @%1 работает. Напишите ему в Telegram."_q.arg(
				object.value(u"username"_q).toString()));
	}));
}

void Studio::shutdown(const QString &id) {
	const auto i = _instances.find(id);
	if (i == end(_instances)) {
		return;
	}
	auto instance = std::move(i->second);
	_instances.erase(i);
	if (instance->api) {
		instance->api->stopPolling();
	}
	// The runtime may be the caller, destroy everything later.
	crl::on_main([raw = instance.release()] {
		raw->runtime = nullptr;
		raw->api = nullptr;
		delete raw;
	});
	if (_storageDirty.contains(id)) {
		saveStorage();
	}
}

void Studio::setStatus(
		const QString &id,
		BotStatus status,
		const QString &error) {
	if (const auto i = _instances.find(id); i != end(_instances)) {
		i->second->status = status;
		i->second->error = error;
	} else if (status == BotStatus::Error) {
		auto instance = std::make_unique<Instance>();
		instance->status = status;
		instance->error = error;
		_instances.emplace(id, std::move(instance));
	}
	_botUpdated.fire_copy(id);
	if (status != BotStatus::Starting) {
		if (const auto i = _startWaiters.find(id); i != end(_startWaiters)) {
			const auto waiters = std::move(i->second);
			_startWaiters.erase(i);
			for (const auto &callback : waiters) {
				callback();
			}
		}
	}
}

void Studio::addLog(const QString &id, LogLevel level, const QString &text) {
	auto &list = _logs[id];
	list.push_back({
		.when = QDateTime::currentDateTime(),
		.level = level,
		.text = (text.size() > kMaxLogText)
			? (text.left(kMaxLogText) + QChar(0x2026))
			: text,
	});
	while (list.size() > kMaxLogs) {
		list.pop_front();
	}
	if (level == LogLevel::Error) {
		++_errors[id];
	}
	_logAdded.fire_copy(id);
}

RuntimeHost Studio::makeHost(const QString &id) {
	const auto weak = base::make_weak(this);
	return {
		.call = [=](
				QString method,
				QByteArray params,
				Fn<void(QByteArray, QString)> done) {
			const auto strong = weak.get();
			const auto api = strong ? strong->instanceApi(id) : nullptr;
			if (!api) {
				done({}, u"bot is stopped"_q);
				return;
			}
			api->call(method, params, [=](
					QByteArray result,
					std::optional<ApiError> error) {
				done(result, error ? error->text : QString());
			});
		},
		.ai = [=](QJsonObject request, Fn<void(QString, QString)> done) {
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			auto history = std::vector<AiAgent::ChatMessage>();
			for (const auto &value : request.value(u"history"_q).toArray()) {
				const auto object = value.toObject();
				const auto text = object.value(u"text"_q).toString();
				if (text.isEmpty()) {
					continue;
				}
				history.push_back({
					.role = (object.value(u"role"_q).toString()
						== u"assistant"_q)
						? AiAgent::ChatMessage::Role::Assistant
						: AiAgent::ChatMessage::Role::User,
					.text = text,
				});
			}
			const auto prompt = request.value(u"prompt"_q).toString();
			if (!prompt.isEmpty()) {
				history.push_back({
					.role = AiAgent::ChatMessage::Role::User,
					.text = prompt,
				});
			}
			if (history.empty()
				|| history.back().role != AiAgent::ChatMessage::Role::User) {
				done(QString(), u"empty prompt"_q);
				return;
			}
			auto system = request.value(u"system"_q).toString();
			if (system.isEmpty()) {
				system = u"You are a helpful Telegram bot assistant. "
					"Answer in the user's language, concisely."_q;
			}
			AiAgent::Manager::Get(strong->_session)->complete(
				system,
				std::move(history),
				std::move(done));
		},
		.http = [=](QJsonObject request, Fn<void(HttpResponse)> done) {
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			const auto url = QUrl(request.value(u"url"_q).toString());
			if (!url.isValid()
				|| (url.scheme() != u"https"_q && url.scheme() != u"http"_q)) {
				done({ .error = u"only http(s) URLs are allowed"_q });
				return;
			}
			auto http = QNetworkRequest(url);
			http.setTransferTimeout(kHttpTimeout);
			const auto headers = request.value(u"headers"_q).toObject();
			for (auto i = headers.begin(); i != headers.end(); ++i) {
				http.setRawHeader(
					i.key().toUtf8(),
					i.value().toVariant().toString().toUtf8());
			}
			const auto method = request.value(u"method"_q).toString();
			const auto body = request.value(u"body"_q).toString().toUtf8();
			const auto reply = strong->_network->sendCustomRequest(
				http,
				method.isEmpty() ? QByteArray("GET") : method.toUtf8(),
				body);
			QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
				reply->deleteLater();
				auto response = HttpResponse{
					.status = reply->attribute(
						QNetworkRequest::HttpStatusCodeAttribute).toInt(),
					.body = reply->read(kMaxHttpResponse),
				};
				for (const auto &[key, value] : reply->rawHeaderPairs()) {
					response.headers.insert(
						QString::fromUtf8(key).toLower(),
						QString::fromUtf8(value));
				}
				if (reply->error() != QNetworkReply::NoError) {
					response.error = reply->errorString();
				}
				done(std::move(response));
			});
		},
		.log = [=](LogLevel level, QString text) {
			if (const auto strong = weak.get()) {
				strong->addLog(id, level, text);
			}
		},
		.storageSave = [=](QByteArray data) {
			if (const auto strong = weak.get()) {
				strong->storageChanged(id, std::move(data));
			}
		},
	};
}

BotApi *Studio::instanceApi(const QString &id) const {
	const auto i = _instances.find(id);
	return (i != end(_instances)) ? i->second->api.get() : nullptr;
}

QByteArray Studio::loadStorage(const QString &id) const {
	if (const auto i = _storage.find(id); i != end(_storage)) {
		return i->second;
	}
	return _session->local().readPref<QString>(
		StorageKey(id),
		u"{}"_q).toUtf8();
}

void Studio::storageChanged(const QString &id, QByteArray data) {
	_storage[id] = std::move(data);
	_storageDirty.emplace(id);
	if (!_storageTimer.isActive()) {
		_storageTimer.callOnce(kStorageSaveDelay);
	}
}

void Studio::saveStorage() {
	_storageTimer.cancel();
	for (const auto &id : base::take(_storageDirty)) {
		const auto i = _storage.find(id);
		if (i == end(_storage) || !bot(id)) {
			continue;
		}
		_session->local().writePref<QString>(
			StorageKey(id),
			QString::fromUtf8(i->second));
	}
}

QString StatusText(BotStatus status) {
	switch (status) {
	case BotStatus::Stopped: return u"остановлен"_q;
	case BotStatus::Starting: return u"запускается…"_q;
	case BotStatus::Running: return u"работает"_q;
	case BotStatus::Error: return u"ошибка"_q;
	}
	return QString();
}

} // namespace BotStudio
