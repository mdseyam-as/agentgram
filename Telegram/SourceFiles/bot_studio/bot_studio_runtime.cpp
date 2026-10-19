/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "bot_studio/bot_studio_runtime.h"

#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTimer>

extern "C" {
#include <quickjs.h>
} // extern "C"

namespace BotStudio {
namespace {

constexpr auto kMemoryLimit = size_t(128) * 1024 * 1024;
constexpr auto kStackLimit = size_t(1024) * 1024;
constexpr auto kSliceTimeout = crl::time(5000);
constexpr auto kMaxStorage = 8 * 1024 * 1024;
constexpr auto kMaxTimers = 1000;

[[nodiscard]] QByteArray Prelude() {
	auto file = QFile(u":/bot_studio/prelude.js"_q);
	return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

[[nodiscard]] QString ToString(JSContext *ctx, JSValueConst value) {
	auto length = size_t();
	const auto data = JS_ToCStringLen(ctx, &length, value);
	if (!data) {
		return QString();
	}
	auto result = QString::fromUtf8(data, int(length));
	JS_FreeCString(ctx, data);
	return result;
}

[[nodiscard]] QByteArray ToUtf8(JSContext *ctx, JSValueConst value) {
	auto length = size_t();
	const auto data = JS_ToCStringLen(ctx, &length, value);
	if (!data) {
		return QByteArray();
	}
	auto result = QByteArray(data, int(length));
	JS_FreeCString(ctx, data);
	return result;
}

[[nodiscard]] JSValue NewString(JSContext *ctx, const QString &text) {
	const auto utf8 = text.toUtf8();
	return JS_NewStringLen(ctx, utf8.constData(), utf8.size());
}

[[nodiscard]] QString ExceptionText(JSContext *ctx, JSValueConst error) {
	auto result = QString();
	if (JS_IsError(error)) {
		const auto name = JS_GetPropertyStr(ctx, error, "name");
		const auto message = JS_GetPropertyStr(ctx, error, "message");
		const auto stack = JS_GetPropertyStr(ctx, error, "stack");
		result = ToString(ctx, name) + u": "_q + ToString(ctx, message);
		if (JS_IsString(stack)) {
			const auto trace = ToString(ctx, stack).trimmed();
			if (!trace.isEmpty()) {
				result += '\n' + trace;
			}
		}
		JS_FreeValue(ctx, name);
		JS_FreeValue(ctx, message);
		JS_FreeValue(ctx, stack);
	} else {
		result = ToString(ctx, error);
	}
	return result;
}

int Interrupt(JSRuntime *rt, void *opaque) {
	const auto deadline = *static_cast<crl::time*>(opaque);
	return (deadline > 0 && crl::now() > deadline) ? 1 : 0;
}

} // namespace

struct Runtime::Rejected {
	void *key = nullptr;
	QString text;
};

struct Runtime::Pending {
	JSValue resolve = JS_UNDEFINED;
	JSValue reject = JS_UNDEFINED;
	bool text = false;
};

struct Runtime::Native {
	[[nodiscard]] static Runtime *Get(JSContext *ctx) {
		return static_cast<Runtime*>(JS_GetContextOpaque(ctx));
	}

	static JSValue Me(JSContext *ctx, JSValueConst, int, JSValueConst*) {
		const auto &me = Get(ctx)->_me;
		return JS_NewStringLen(ctx, me.constData(), me.size());
	}

	static JSValue OwnerId(JSContext *ctx, JSValueConst, int, JSValueConst*) {
		return JS_NewFloat64(ctx, double(Get(ctx)->_ownerId));
	}

	static JSValue Log(
			JSContext *ctx,
			JSValueConst,
			int argc,
			JSValueConst *argv) {
		auto level = int32_t();
		if (argc < 2 || JS_ToInt32(ctx, &level, argv[0]) < 0) {
			return JS_UNDEFINED;
		}
		const auto runtime = Get(ctx);
		if (runtime->_host.log) {
			runtime->_host.log(
				(level == 2)
					? LogLevel::Error
					: (level == 1)
					? LogLevel::Warning
					: LogLevel::Info,
				ToString(ctx, argv[1]));
		}
		return JS_UNDEFINED;
	}

	static JSValue SetTimer(
			JSContext *ctx,
			JSValueConst,
			int argc,
			JSValueConst *argv) {
		auto id = int32_t();
		auto ms = double();
		if (argc < 3
			|| JS_ToInt32(ctx, &id, argv[0]) < 0
			|| JS_ToFloat64(ctx, &ms, argv[1]) < 0) {
			return JS_EXCEPTION;
		}
		const auto runtime = Get(ctx);
		if (runtime->_timers.size() >= kMaxTimers) {
			return JS_ThrowRangeError(ctx, "Too many active timers");
		}
		const auto repeat = JS_ToBool(ctx, argv[2]) > 0;
		auto timer = std::make_unique<QTimer>();
		timer->setSingleShot(!repeat);
		timer->setInterval(int(std::clamp(ms, 0., 24. * 3600. * 1000.)));
		const auto weak = base::make_weak(runtime);
		QObject::connect(timer.get(), &QTimer::timeout, [=] {
			if (const auto strong = weak.get()) {
				strong->callTimer(id);
			}
		});
		timer->start();
		runtime->_timers[id] = std::move(timer);
		return JS_UNDEFINED;
	}

	static JSValue ClearTimer(
			JSContext *ctx,
			JSValueConst,
			int argc,
			JSValueConst *argv) {
		auto id = int32_t();
		if (argc > 0 && JS_ToInt32(ctx, &id, argv[0]) >= 0) {
			auto &timers = Get(ctx)->_timers;
			const auto i = timers.find(id);
			if (i != end(timers)) {
				// clearInterval() may run inside this timer's own timeout.
				i->second->stop();
				i->second.release()->deleteLater();
				timers.erase(i);
			}
		}
		return JS_UNDEFINED;
	}

	static JSValue StorageLoad(
			JSContext *ctx,
			JSValueConst,
			int,
			JSValueConst*) {
		const auto &storage = Get(ctx)->_storage;
		return JS_NewStringLen(ctx, storage.constData(), storage.size());
	}

	static JSValue StorageSave(
			JSContext *ctx,
			JSValueConst,
			int argc,
			JSValueConst *argv) {
		if (argc < 1) {
			return JS_UNDEFINED;
		}
		auto data = ToUtf8(ctx, argv[0]);
		if (data.size() > kMaxStorage) {
			return JS_ThrowRangeError(
				ctx,
				"Bot storage is limited to %d MB",
				kMaxStorage / (1024 * 1024));
		}
		const auto runtime = Get(ctx);
		runtime->_storage = data;
		if (runtime->_host.storageSave) {
			runtime->_host.storageSave(std::move(data));
		}
		return JS_UNDEFINED;
	}

	static JSValue Call(
			JSContext *ctx,
			JSValueConst,
			int argc,
			JSValueConst *argv) {
		if (argc < 2) {
			return JS_ThrowTypeError(ctx, "call(method, params) expected");
		}
		const auto runtime = Get(ctx);
		const auto method = ToString(ctx, argv[0]);
		const auto params = ToUtf8(ctx, argv[1]);
		auto promise = JS_UNDEFINED;
		const auto id = runtime->createPromise(&promise);
		if (!id) {
			return promise;
		}
		const auto weak = base::make_weak(runtime);
		runtime->_host.call(method, params, [=](
				QByteArray result,
				QString error) {
			if (const auto strong = weak.get()) {
				strong->settle(
					id,
					error.isEmpty(),
					result,
					error.isEmpty() ? QString() : (method + u": "_q + error));
			}
		});
		return promise;
	}

	static JSValue Ai(
			JSContext *ctx,
			JSValueConst,
			int argc,
			JSValueConst *argv) {
		if (argc < 1) {
			return JS_ThrowTypeError(ctx, "ai(request) expected");
		}
		const auto runtime = Get(ctx);
		const auto request = QJsonDocument::fromJson(
			ToUtf8(ctx, argv[0])).object();
		auto promise = JS_UNDEFINED;
		const auto id = runtime->createPromise(&promise);
		if (!id) {
			return promise;
		}
		const auto weak = base::make_weak(runtime);
		runtime->_host.ai(request, [=](QString text, QString error) {
			if (const auto strong = weak.get()) {
				if (error.isEmpty()) {
					strong->settleString(id, text);
				} else {
					strong->settle(id, false, {}, u"ai.ask: "_q + error);
				}
			}
		});
		return promise;
	}

	static JSValue Http(
			JSContext *ctx,
			JSValueConst,
			int argc,
			JSValueConst *argv) {
		if (argc < 1) {
			return JS_ThrowTypeError(ctx, "http(request) expected");
		}
		const auto runtime = Get(ctx);
		const auto request = QJsonDocument::fromJson(
			ToUtf8(ctx, argv[0])).object();
		auto promise = JS_UNDEFINED;
		const auto id = runtime->createPromise(&promise);
		if (!id) {
			return promise;
		}
		const auto weak = base::make_weak(runtime);
		runtime->_host.http(request, [=](HttpResponse response) {
			const auto strong = weak.get();
			if (!strong) {
				return;
			} else if (!response.error.isEmpty() && !response.status) {
				strong->settle(id, false, {}, u"http: "_q + response.error);
				return;
			}
			auto object = QJsonObject();
			object.insert(u"status"_q, response.status);
			object.insert(u"body"_q, QString::fromUtf8(response.body));
			object.insert(u"headers"_q, response.headers);
			strong->settle(
				id,
				true,
				QJsonDocument(object).toJson(QJsonDocument::Compact),
				QString());
		});
		return promise;
	}

	static void Rejection(
			JSContext *ctx,
			JSValueConst promise,
			JSValueConst reason,
			bool handled,
			void *opaque) {
		// A promise is often rejected first and gets its handler a
		// microtask later, so report only what is still unhandled when
		// the job queue is empty.
		const auto runtime = static_cast<Runtime*>(opaque);
		if (runtime->_destroying) {
			return;
		}
		const auto key = JS_VALUE_GET_PTR(promise);
		auto &list = runtime->_rejections;
		const auto i = ranges::find(list, key, &Rejected::key);
		if (handled) {
			if (i != end(list)) {
				list.erase(i);
			}
		} else if (i == end(list)) {
			list.push_back({ key, ExceptionText(ctx, reason) });
		}
	}
};

Runtime::Runtime(RuntimeDescriptor descriptor)
: _host(std::move(descriptor.host))
, _storage(std::move(descriptor.storage))
, _me(std::move(descriptor.me))
, _ownerId(descriptor.ownerId) {
	_rt = JS_NewRuntime();
	JS_SetMemoryLimit(_rt, kMemoryLimit);
	JS_SetMaxStackSize(_rt, kStackLimit);
	JS_SetInterruptHandler(_rt, Interrupt, &_deadline);
	JS_SetHostPromiseRejectionTracker(_rt, Native::Rejection, this);
	_ctx = JS_NewContext(_rt);
	JS_SetContextOpaque(_ctx, this);

	const auto global = JS_GetGlobalObject(_ctx);
	const auto object = JS_NewObject(_ctx);
	const auto add = [&](const char *name, JSCFunction *function, int args) {
		JS_SetPropertyStr(
			_ctx,
			object,
			name,
			JS_NewCFunction(_ctx, function, name, args));
	};
	add("me", Native::Me, 0);
	add("ownerId", Native::OwnerId, 0);
	add("log", Native::Log, 2);
	add("setTimer", Native::SetTimer, 3);
	add("clearTimer", Native::ClearTimer, 1);
	add("storageLoad", Native::StorageLoad, 0);
	add("storageSave", Native::StorageSave, 1);
	add("call", Native::Call, 2);
	add("ai", Native::Ai, 1);
	add("http", Native::Http, 1);
	JS_SetPropertyStr(_ctx, global, "__host", object);
	JS_FreeValue(_ctx, global);

	const auto prelude = Prelude();
	enter();
	const auto result = JS_Eval(
		_ctx,
		prelude.constData(),
		prelude.size(),
		"agentgram-prelude.js",
		JS_EVAL_TYPE_GLOBAL);
	if (JS_IsException(result)) {
		reportException(_ctx, u"prelude"_q);
	}
	JS_FreeValue(_ctx, result);
	drain();
}

Runtime::~Runtime() {
	_destroying = true;
	_timers.clear();
	for (const auto &[id, pending] : base::take(_pending)) {
		JS_FreeValue(_ctx, pending->resolve);
		JS_FreeValue(_ctx, pending->reject);
	}
	JS_FreeContext(_ctx);
	JS_FreeRuntime(_rt);
}

QString Runtime::load(const QString &code) {
	const auto utf8 = code.toUtf8();
	enter();
	const auto result = JS_Eval(
		_ctx,
		utf8.constData(),
		utf8.size(),
		"bot.js",
		JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_ASYNC);
	auto error = QString();
	if (JS_IsException(result)) {
		const auto exception = JS_GetException(_ctx);
		error = ExceptionText(_ctx, exception);
		JS_FreeValue(_ctx, exception);
	}
	JS_FreeValue(_ctx, result);
	drain();
	return error;
}

QString Runtime::Check(const QString &code) {
	const auto rt = JS_NewRuntime();
	JS_SetMemoryLimit(rt, kMemoryLimit);
	const auto ctx = JS_NewContext(rt);
	const auto utf8 = code.toUtf8();
	const auto result = JS_Eval(
		ctx,
		utf8.constData(),
		utf8.size(),
		"bot.js",
		JS_EVAL_TYPE_GLOBAL
			| JS_EVAL_FLAG_ASYNC
			| JS_EVAL_FLAG_COMPILE_ONLY);
	auto error = QString();
	if (JS_IsException(result)) {
		const auto exception = JS_GetException(ctx);
		error = ExceptionText(ctx, exception);
		JS_FreeValue(ctx, exception);
	}
	JS_FreeValue(ctx, result);
	JS_FreeContext(ctx);
	JS_FreeRuntime(rt);
	return error;
}

QString Runtime::Documentation() {
	auto file = QFile(u":/bot_studio/docs.md"_q);
	return file.open(QIODevice::ReadOnly)
		? QString::fromUtf8(file.readAll())
		: QString();
}

void Runtime::dispatch(const QByteArray &update) {
	callGlobal("__agentgram_dispatch", update);
}

void Runtime::broadcast(const QByteArray &request) {
	callGlobal("__agentgram_broadcast", request);
}

void Runtime::hook(const QByteArray &request) {
	callGlobal("__agentgram_hook", request);
}

void Runtime::callTimer(int id) {
	const auto i = _timers.find(id);
	if (i == end(_timers)) {
		return;
	} else if (i->second->isSingleShot()) {
		// The QTimer emits from inside itself, destroy it later.
		i->second->stop();
		i->second.release()->deleteLater();
		_timers.erase(i);
	}
	enter();
	const auto global = JS_GetGlobalObject(_ctx);
	const auto function = JS_GetPropertyStr(_ctx, global, "__agentgram_timer");
	auto argument = JS_NewInt32(_ctx, id);
	const auto result = JS_Call(_ctx, function, global, 1, &argument);
	if (JS_IsException(result)) {
		reportException(_ctx, u"timer"_q);
	}
	JS_FreeValue(_ctx, result);
	JS_FreeValue(_ctx, function);
	JS_FreeValue(_ctx, global);
	drain();
}

void Runtime::callGlobal(const char *name, const QByteArray &argument) {
	enter();
	const auto global = JS_GetGlobalObject(_ctx);
	const auto function = JS_GetPropertyStr(_ctx, global, name);
	auto value = JS_NewStringLen(_ctx, argument.constData(), argument.size());
	const auto result = JS_Call(_ctx, function, global, 1, &value);
	if (JS_IsException(result)) {
		reportException(_ctx, QString::fromLatin1(name));
	}
	JS_FreeValue(_ctx, value);
	JS_FreeValue(_ctx, result);
	JS_FreeValue(_ctx, function);
	JS_FreeValue(_ctx, global);
	drain();
}

void Runtime::enter() {
	_deadline = crl::now() + kSliceTimeout;
}

void Runtime::drain() {
	while (true) {
		auto ctx = (JSContext*)nullptr;
		const auto result = JS_ExecutePendingJob(_rt, &ctx);
		if (result == 0) {
			break;
		} else if (result < 0) {
			reportException(ctx ? ctx : _ctx, u"async"_q);
		}
		// Each job gets its own time slice.
		enter();
	}
	_deadline = 0;
	for (auto &rejected : base::take(_rejections)) {
		if (_host.log) {
			_host.log(
				LogLevel::Error,
				u"Unhandled promise rejection: "_q + rejected.text);
		}
	}
}

void Runtime::reportException(JSContext *ctx, const QString &where) {
	const auto exception = JS_GetException(ctx);
	auto text = ExceptionText(ctx, exception);
	JS_FreeValue(ctx, exception);
	if (text.contains(u"interrupted"_q)) {
		text = u"Код выполнялся дольше %1 секунд без пауз и был "
			"остановлен. Проверьте бесконечные циклы."_q.arg(
				kSliceTimeout / 1000);
	}
	if (_host.log) {
		_host.log(LogLevel::Error, where + u": "_q + text);
	}
}

uint64 Runtime::createPromise(void *result) {
	JSValue functions[2];
	const auto promise = JS_NewPromiseCapability(_ctx, functions);
	*static_cast<JSValue*>(result) = promise;
	if (JS_IsException(promise)) {
		return 0;
	}
	const auto id = ++_promiseId;
	auto pending = std::make_unique<Pending>();
	pending->resolve = functions[0];
	pending->reject = functions[1];
	_pending.emplace(id, std::move(pending));
	return id;
}

void Runtime::settleString(uint64 id, QString text) {
	const auto i = _pending.find(id);
	if (i == end(_pending)) {
		return;
	}
	auto pending = std::move(i->second);
	_pending.erase(i);
	enter();
	auto value = NewString(_ctx, text);
	const auto result = JS_Call(_ctx, pending->resolve, JS_UNDEFINED, 1, &value);
	if (JS_IsException(result)) {
		reportException(_ctx, u"promise"_q);
	}
	JS_FreeValue(_ctx, result);
	JS_FreeValue(_ctx, value);
	JS_FreeValue(_ctx, pending->resolve);
	JS_FreeValue(_ctx, pending->reject);
	drain();
}

void Runtime::settle(
		uint64 id,
		bool success,
		QByteArray json,
		QString error) {
	const auto i = _pending.find(id);
	if (i == end(_pending)) {
		return;
	}
	auto pending = std::move(i->second);
	_pending.erase(i);
	enter();
	auto value = JS_UNDEFINED;
	if (success) {
		if (json.isEmpty()) {
			json = "null";
		}
		value = JS_ParseJSON(_ctx, json.constData(), json.size(), "result");
		if (JS_IsException(value)) {
			JS_FreeValue(_ctx, JS_GetException(_ctx));
			success = false;
			error = u"Invalid JSON in the response."_q;
			value = JS_UNDEFINED;
		}
	}
	if (!success) {
		value = JS_NewError(_ctx);
		JS_SetPropertyStr(_ctx, value, "message", NewString(_ctx, error));
	}
	const auto function = success ? pending->resolve : pending->reject;
	const auto result = JS_Call(_ctx, function, JS_UNDEFINED, 1, &value);
	if (JS_IsException(result)) {
		reportException(_ctx, u"promise"_q);
	}
	JS_FreeValue(_ctx, result);
	JS_FreeValue(_ctx, value);
	JS_FreeValue(_ctx, pending->resolve);
	JS_FreeValue(_ctx, pending->reject);
	drain();
}

} // namespace BotStudio
