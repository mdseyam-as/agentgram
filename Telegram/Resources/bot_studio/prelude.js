// Agentgram Bot Studio runtime prelude: the bot framework scripts use.
// Host functions come from C++ through globalThis.__host.
(function (global) {
'use strict';

const host = global.__host;
delete global.__host;

const handlers = [];
let errorHandler = null;
let me = null;
try { me = JSON.parse(host.me()); } catch (e) { me = null; }
const ownerId = host.ownerId();

// ---- console ----------------------------------------------------------
function show(value) {
	if (typeof value === 'string') return value;
	if (value instanceof Error) {
		return value.stack ? value.message + '\n' + value.stack : String(value);
	}
	try {
		const json = JSON.stringify(value);
		return json === undefined ? String(value) : json;
	} catch (e) {
		return String(value);
	}
}
function log(level, args) {
	host.log(level, Array.prototype.map.call(args, show).join(' '));
}
global.console = {
	log() { log(0, arguments); },
	info() { log(0, arguments); },
	debug() { log(0, arguments); },
	warn() { log(1, arguments); },
	error() { log(2, arguments); },
};

function reportError(error, where) {
	if (errorHandler) {
		try {
			const result = errorHandler(error, where);
			if (result && typeof result.then === 'function') {
				result.then(undefined, (e) => log(2, ['Error in bot.catch:', e]));
			}
			return;
		} catch (e) {
			log(2, ['Error in bot.catch:', e]);
		}
	}
	log(2, [where ? where + ':' : 'Error:', error]);
}

// ---- timers -----------------------------------------------------------
const timers = new Map();
let timerSeq = 0;
function addTimer(fn, ms, args, repeat) {
	if (typeof fn !== 'function') throw new TypeError('Timer callback must be a function');
	const id = ++timerSeq;
	timers.set(id, { fn, args, repeat });
	host.setTimer(id, Math.max(repeat ? 10 : 0, Number(ms) || 0), repeat);
	return id;
}
global.setTimeout = (fn, ms, ...args) => addTimer(fn, ms, args, false);
global.setInterval = (fn, ms, ...args) => addTimer(fn, ms, args, true);
global.clearTimeout = global.clearInterval = (id) => {
	if (timers.delete(id)) host.clearTimer(id);
};
global.__agentgram_timer = (id) => {
	const timer = timers.get(id);
	if (!timer) return;
	if (!timer.repeat) timers.delete(id);
	run(() => timer.fn(...timer.args), 'timer');
};

function run(fn, where) {
	try {
		const result = fn();
		if (result && typeof result.then === 'function') {
			return result.then(undefined, (e) => reportError(e, where));
		}
		return Promise.resolve(result);
	} catch (e) {
		reportError(e, where);
		return Promise.resolve();
	}
}

// ---- storage ----------------------------------------------------------
let store = {};
try { store = JSON.parse(host.storageLoad() || '{}') || {}; } catch (e) { store = {}; }
let saveScheduled = false;
function scheduleSave() {
	if (saveScheduled) return;
	saveScheduled = true;
	Promise.resolve().then(() => {
		saveScheduled = false;
		host.storageSave(JSON.stringify(store));
	});
}
function clone(value) {
	return value === undefined ? undefined : JSON.parse(JSON.stringify(value));
}
const has = (key) => Object.prototype.hasOwnProperty.call(store, key);
const db = {
	get(key, fallback) { key = String(key); return has(key) ? clone(store[key]) : fallback; },
	set(key, value) {
		key = String(key);
		if (value === undefined) delete store[key]; else store[key] = clone(value);
		scheduleSave();
		return value;
	},
	delete(key) {
		key = String(key);
		const had = has(key);
		delete store[key];
		if (had) scheduleSave();
		return had;
	},
	has(key) { return has(String(key)); },
	keys(prefix) {
		prefix = prefix === undefined ? '' : String(prefix);
		return Object.keys(store).filter((k) => k.startsWith(prefix));
	},
	all(prefix) {
		const result = {};
		for (const key of db.keys(prefix)) result[key] = clone(store[key]);
		return result;
	},
	clear(prefix) {
		for (const key of db.keys(prefix)) delete store[key];
		scheduleSave();
	},
	increment(key, by) {
		const value = (Number(db.get(key, 0)) || 0) + (by === undefined ? 1 : Number(by));
		db.set(key, value);
		return value;
	},
};

// ---- Bot API ----------------------------------------------------------
function call(method, params) {
	return host.call(String(method), JSON.stringify(params || {}));
}
const api = new Proxy({}, {
	get(target, method) {
		if (typeof method !== 'string' || method === 'then') return undefined;
		return (params) => call(method, params);
	},
});

function rows(value) {
	if (!Array.isArray(value)) return [[value]];
	return value.map((row) => (Array.isArray(row) ? row : [row]));
}
function inlineButton(button) {
	if (typeof button === 'string') return { text: button, callback_data: button };
	const result = { text: String(button.text) };
	if (button.url) result.url = String(button.url);
	else if (button.web_app) result.web_app = button.web_app;
	else if (button.switch_inline_query !== undefined) result.switch_inline_query = button.switch_inline_query;
	else if (button.copy_text) result.copy_text = button.copy_text;
	else result.callback_data = String(button.data !== undefined ? button.data
		: button.callback_data !== undefined ? button.callback_data
		: button.text);
	return result;
}
function replyButton(button) {
	if (typeof button === 'string') return { text: button };
	const result = { text: String(button.text) };
	if (button.contact || button.request_contact) result.request_contact = true;
	if (button.location || button.request_location) result.request_location = true;
	if (button.web_app) result.web_app = button.web_app;
	return result;
}
const Keyboard = {
	inline(buttons) {
		return { inline_keyboard: rows(buttons).map((row) => row.map(inlineButton)) };
	},
	reply(buttons, options) {
		options = options || {};
		const result = {
			keyboard: rows(buttons).map((row) => row.map(replyButton)),
			resize_keyboard: options.resize !== false,
		};
		if (options.once) result.one_time_keyboard = true;
		if (options.persistent) result.is_persistent = true;
		if (options.placeholder) result.input_field_placeholder = String(options.placeholder);
		return result;
	},
	remove() { return { remove_keyboard: true }; },
	button(text, data) { return { text: String(text), data: data === undefined ? String(text) : String(data) }; },
	url(text, url) { return { text: String(text), url: String(url) }; },
};
function extra(options) {
	if (!options) return {};
	const result = Object.assign({}, options);
	if (result.buttons !== undefined) {
		result.reply_markup = Keyboard.inline(result.buttons);
		delete result.buttons;
	}
	if (result.keyboard !== undefined) {
		result.reply_markup = Array.isArray(result.keyboard)
			? Keyboard.reply(result.keyboard)
			: result.keyboard;
		delete result.keyboard;
	}
	if (result.removeKeyboard) {
		result.reply_markup = Keyboard.remove();
		delete result.removeKeyboard;
	}
	if (result.html) { result.parse_mode = 'HTML'; delete result.html; }
	if (result.markdown) { result.parse_mode = 'MarkdownV2'; delete result.markdown; }
	if (result.replyTo !== undefined) {
		result.reply_parameters = { message_id: result.replyTo };
		delete result.replyTo;
	}
	if (result.silent) { result.disable_notification = true; delete result.silent; }
	if (result.noPreview) { result.link_preview_options = { is_disabled: true }; delete result.noPreview; }
	return result;
}
function send(method, chatId, fields, options) {
	if (chatId === undefined || chatId === null) {
		return Promise.reject(new Error('No chat to send ' + method + ' to'));
	}
	return call(method, Object.assign({ chat_id: chatId }, fields, extra(options)));
}

// ---- AI and HTTP ------------------------------------------------------
const ai = {
	ask(prompt, options) {
		const o = (prompt && typeof prompt === 'object')
			? prompt
			: Object.assign({}, options || {}, { prompt });
		const history = Array.isArray(o.history) ? o.history : [];
		return host.ai(JSON.stringify({
			system: o.system === undefined ? '' : String(o.system),
			prompt: o.prompt === undefined ? '' : String(o.prompt),
			history: history.map((m) => ({
				role: (m.role === 'assistant' || m.role === 'bot') ? 'assistant' : 'user',
				text: String(m.text !== undefined ? m.text : (m.content !== undefined ? m.content : '')),
			})),
		}));
	},
	async chat(ctx, options) {
		options = options || {};
		const key = 'ai_history:' + ctx.chatId;
		const history = db.get(key, []);
		const text = options.prompt !== undefined ? String(options.prompt) : (ctx.text || '');
		const answer = await ai.ask({ system: options.system, prompt: text, history });
		const memory = options.memory === undefined ? 20 : Number(options.memory);
		history.push({ role: 'user', text }, { role: 'assistant', text: answer });
		db.set(key, history.slice(-memory * 2));
		return answer;
	},
	reset(ctx) {
		db.delete('ai_history:' + (ctx && typeof ctx === 'object' ? ctx.chatId : ctx));
	},
};

const http = {
	request(url, options) {
		options = options || {};
		let body = options.body;
		const headers = Object.assign({}, options.headers || {});
		if (body !== undefined && typeof body !== 'string') {
			body = JSON.stringify(body);
			if (!Object.keys(headers).some((h) => h.toLowerCase() === 'content-type')) {
				headers['Content-Type'] = 'application/json';
			}
		}
		return host.http(JSON.stringify({
			url: String(url),
			method: String(options.method || 'GET').toUpperCase(),
			headers,
			body,
		})).then((r) => ({
			status: r.status,
			ok: r.status >= 200 && r.status < 300,
			headers: r.headers,
			text: r.body,
			json() { return JSON.parse(r.body); },
		}));
	},
	get(url, options) { return http.request(url, Object.assign({}, options, { method: 'GET' })); },
	post(url, body, options) { return http.request(url, Object.assign({}, options, { method: 'POST', body })); },
};
global.fetch = (url, options) => {
	options = options || {};
	return http.request(url, options).then((r) => ({
		ok: r.ok,
		status: r.status,
		headers: r.headers,
		text: async () => r.text,
		json: async () => r.json(),
	}));
};

// ---- context ----------------------------------------------------------
const MESSAGE_KINDS = [
	'text', 'photo', 'video', 'document', 'audio', 'voice', 'video_note',
	'sticker', 'animation', 'location', 'venue', 'contact', 'poll', 'dice',
	'new_chat_members', 'left_chat_member', 'pinned_message', 'web_app_data',
	'successful_payment', 'forward_origin', 'reply_to_message',
];

class Context {
	constructor(update) {
		const u = update;
		this.update = u;
		this.updateType = Object.keys(u).find((k) => k !== 'update_id');
		this.message = u.message || u.edited_message || u.channel_post
			|| u.edited_channel_post || u.business_message
			|| (u.callback_query && u.callback_query.message) || undefined;
		this.callbackQuery = u.callback_query;
		this.inlineQuery = u.inline_query;
		const source = u.callback_query || u.inline_query || u.message
			|| u.edited_message || u.my_chat_member || u.chat_member
			|| u.chat_join_request || u.pre_checkout_query
			|| u.shipping_query || u.business_message || {};
		this.from = source.from;
		this.chat = this.message
			? this.message.chat
			: (u.my_chat_member || u.chat_member || u.chat_join_request || {}).chat;
		const own = u.message || u.edited_message || u.channel_post
			|| u.edited_channel_post || u.business_message;
		this.text = own ? (own.text !== undefined ? own.text : own.caption) : undefined;
		this.data = u.callback_query ? u.callback_query.data : undefined;
		this.match = null;
		this.command = undefined;
		this.args = [];
		this.payload = '';
		this.bot = bot;
		this.api = api;
		this.db = db;
		this._answered = false;
		this._session = undefined;
	}
	get chatId() { return this.chat ? this.chat.id : undefined; }
	get messageId() { return this.message ? this.message.message_id : undefined; }
	get isOwner() { return !!this.from && this.from.id === ownerId; }
	get session() {
		if (this._session === undefined) this._session = db.get('session:' + this.chatId, {});
		return this._session;
	}
	set session(value) { this._session = value === undefined ? null : value; }
	reply(text, options) { return send('sendMessage', this.chatId, { text: String(text) }, options); }
	replyHTML(text, options) { return this.reply(text, Object.assign({}, options, { parse_mode: 'HTML' })); }
	replyPhoto(photo, options) { return send('sendPhoto', this.chatId, { photo }, options); }
	replyDocument(document, options) { return send('sendDocument', this.chatId, { document }, options); }
	replyVideo(video, options) { return send('sendVideo', this.chatId, { video }, options); }
	replyAudio(audio, options) { return send('sendAudio', this.chatId, { audio }, options); }
	replySticker(sticker, options) { return send('sendSticker', this.chatId, { sticker }, options); }
	replyLocation(latitude, longitude, options) { return send('sendLocation', this.chatId, { latitude, longitude }, options); }
	replyPoll(question, answers, options) {
		return send('sendPoll', this.chatId, {
			question: String(question),
			options: answers.map((a) => (typeof a === 'string' ? { text: a } : a)),
		}, options);
	}
	replyDice(emoji, options) { return send('sendDice', this.chatId, { emoji: emoji || '🎲' }, options); }
	typing(action) { return call('sendChatAction', { chat_id: this.chatId, action: action || 'typing' }); }
	react(emoji) {
		return call('setMessageReaction', {
			chat_id: this.chatId,
			message_id: this.messageId,
			reaction: emoji ? [{ type: 'emoji', emoji }] : [],
		});
	}
	answer(text, options) {
		if (this.callbackQuery) {
			this._answered = true;
			const fields = { callback_query_id: this.callbackQuery.id };
			if (text) fields.text = String(text);
			return call('answerCallbackQuery', Object.assign(fields, options || {}));
		}
		if (this.inlineQuery) {
			return call('answerInlineQuery', Object.assign({
				inline_query_id: this.inlineQuery.id,
				results: Array.isArray(text) ? text : [],
			}, options || {}));
		}
		return Promise.resolve(false);
	}
	edit(text, options) {
		if (!this.message) return Promise.reject(new Error('Nothing to edit'));
		return call('editMessageText', Object.assign({
			chat_id: this.chatId,
			message_id: this.messageId,
			text: String(text),
		}, extra(options)));
	}
	editButtons(buttons) {
		if (!this.message) return Promise.reject(new Error('Nothing to edit'));
		return call('editMessageReplyMarkup', {
			chat_id: this.chatId,
			message_id: this.messageId,
			reply_markup: buttons ? Keyboard.inline(buttons) : { inline_keyboard: [] },
		});
	}
	deleteMessage(messageId) {
		return call('deleteMessage', {
			chat_id: this.chatId,
			message_id: messageId === undefined ? this.messageId : messageId,
		});
	}
	forwardTo(chatId) {
		return call('forwardMessage', {
			chat_id: chatId,
			from_chat_id: this.chatId,
			message_id: this.messageId,
		});
	}
	copyTo(chatId, options) {
		return call('copyMessage', Object.assign({
			chat_id: chatId,
			from_chat_id: this.chatId,
			message_id: this.messageId,
		}, extra(options)));
	}
}

// ---- routing ----------------------------------------------------------
function matchTrigger(trigger, value, ctx) {
	if (value === undefined || value === null) return false;
	if (trigger instanceof RegExp) {
		trigger.lastIndex = 0;
		const match = trigger.exec(value);
		if (match) ctx.match = match;
		return !!match;
	}
	if (typeof trigger === 'function') return !!trigger(value, ctx);
	return String(trigger).trim().toLowerCase() === String(value).trim().toLowerCase();
}

function kinds(ctx) {
	const result = new Set([ctx.updateType, '*', 'update']);
	const u = ctx.update;
	const own = u.message || u.edited_message || u.channel_post
		|| u.edited_channel_post || u.business_message;
	if (own) {
		result.add('message');
		for (const kind of MESSAGE_KINDS) if (own[kind] !== undefined) result.add(kind);
		if (own.photo || own.video || own.document || own.audio || own.voice
			|| own.video_note || own.animation || own.sticker) result.add('media');
	}
	if (u.callback_query) result.add('action');
	return result;
}

function prepare(handler, ctx) {
	switch (handler.type) {
	case 'use':
		return true;
	case 'command': {
		const text = ctx.text;
		if (!ctx.message || typeof text !== 'string' || !text.startsWith('/')) return false;
		if (ctx.updateType !== 'message' && ctx.updateType !== 'channel_post'
			&& ctx.updateType !== 'business_message') return false;
		const space = text.search(/\s/);
		const head = space < 0 ? text.slice(1) : text.slice(1, space);
		const at = head.indexOf('@');
		const name = (at < 0 ? head : head.slice(0, at)).toLowerCase();
		const target = at < 0 ? '' : head.slice(at + 1).toLowerCase();
		if (target && me && me.username && target !== me.username.toLowerCase()) return false;
		if (!handler.names.includes(name)) return false;
		ctx.command = name;
		ctx.payload = space < 0 ? '' : text.slice(space + 1).trim();
		ctx.args = ctx.payload ? ctx.payload.split(/\s+/) : [];
		return true;
	}
	case 'hears':
		if (typeof ctx.text !== 'string' || ctx.updateType === 'edited_message') return false;
		return handler.triggers.some((t) => matchTrigger(t, ctx.text, ctx));
	case 'action':
		if (!ctx.callbackQuery) return false;
		return handler.triggers.some((t) => matchTrigger(t, ctx.data, ctx));
	case 'on': {
		const set = kinds(ctx);
		return handler.types.some((t) => set.has(t));
	}
	}
	return false;
}

function finalize(ctx) {
	if (ctx._session !== undefined && ctx.chatId !== undefined) {
		if (ctx._session === null) db.delete('session:' + ctx.chatId);
		else db.set('session:' + ctx.chatId, ctx._session);
	}
	if (ctx.callbackQuery && !ctx._answered) {
		ctx._answered = true;
		call('answerCallbackQuery', { callback_query_id: ctx.callbackQuery.id }).then(undefined, () => {});
	}
}

// ---- audience and broadcasts -----------------------------------------
const USERS_KEY = '__users';
function trackUser(ctx) {
	const chat = ctx.chat;
	const from = ctx.from;
	if (!chat || chat.type !== 'private' || !from || from.is_bot) return;
	const users = store[USERS_KEY] || (store[USERS_KEY] = {});
	const id = String(chat.id);
	const now = Math.floor(Date.now() / 1000);
	const user = users[id] || { first_seen: now };
	user.name = [from.first_name, from.last_name].filter(Boolean).join(' ');
	user.username = from.username || '';
	user.last_seen = now;
	user.messages = (user.messages || 0) + 1;
	delete user.blocked;
	users[id] = user;
	scheduleSave();
}

global.__agentgram_broadcast = (json) => {
	let request;
	try {
		request = JSON.parse(json);
	} catch (e) {
		return;
	}
	const users = store[USERS_KEY] || {};
	const ids = Object.keys(users).filter((id) => !users[id].blocked);
	const options = request.buttons ? { buttons: request.buttons } : {};
	if (request.html) options.html = true;
	log(0, ['📣 Рассылка начата: получателей ' + ids.length]);
	(async () => {
		let sent = 0;
		let failed = 0;
		for (const id of ids) {
			try {
				if (request.photo) {
					await send('sendPhoto', id, { photo: request.photo, caption: request.text }, options);
				} else {
					await send('sendMessage', id, { text: String(request.text) }, options);
				}
				++sent;
			} catch (e) {
				++failed;
				if (/blocked|deactivated|not found|chat not found/i.test(e.message)) {
					users[id].blocked = true;
					scheduleSave();
				}
			}
			await new Promise((resolve) => setTimeout(resolve, 45));
		}
		log(0, ['📣 Рассылка завершена: доставлено ' + sent + ', не доставлено ' + failed]);
	})();
};

global.__agentgram_dispatch = (json) => {
	let update;
	try {
		update = JSON.parse(json);
	} catch (e) {
		reportError(e, 'update');
		return;
	}
	const ctx = new Context(update);
	trackUser(ctx);
	let index = 0;
	const next = () => {
		while (index < handlers.length) {
			const handler = handlers[index++];
			ctx.match = null;
			let matched = false;
			try {
				matched = prepare(handler, ctx);
			} catch (e) {
				return Promise.reject(e);
			}
			if (matched) {
				try {
					return Promise.resolve(handler.fn(ctx, next));
				} catch (e) {
					return Promise.reject(e);
				}
			}
		}
		return Promise.resolve();
	};
	Promise.resolve().then(next).then(
		() => finalize(ctx),
		(e) => {
			reportError(e, ctx.command ? 'handler /' + ctx.command
				: ctx.callbackQuery ? 'action "' + ctx.data + '"'
				: 'handler for ' + ctx.updateType);
			finalize(ctx);
		});
};

function list(value) {
	return Array.isArray(value) ? value : [value];
}
function checkHandler(fn) {
	if (typeof fn !== 'function') throw new TypeError('Handler must be a function');
	return fn;
}

const bot = {
	get me() { return me; },
	get username() { return me ? me.username : undefined; },
	get id() { return me ? me.id : undefined; },
	ownerId,
	api,
	call,
	command(names, fn) {
		handlers.push({
			type: 'command',
			names: list(names).map((n) => String(n).replace(/^\//, '').toLowerCase()),
			fn: checkHandler(fn),
		});
		return bot;
	},
	start(fn) { return bot.command('start', fn); },
	help(fn) { return bot.command('help', fn); },
	hears(triggers, fn) {
		handlers.push({ type: 'hears', triggers: list(triggers), fn: checkHandler(fn) });
		return bot;
	},
	action(triggers, fn) {
		handlers.push({ type: 'action', triggers: list(triggers), fn: checkHandler(fn) });
		return bot;
	},
	on(types, fn) {
		handlers.push({ type: 'on', types: list(types).map(String), fn: checkHandler(fn) });
		return bot;
	},
	use(fn) {
		handlers.push({ type: 'use', fn: checkHandler(fn) });
		return bot;
	},
	catch(fn) {
		errorHandler = checkHandler(fn);
		return bot;
	},
	send(chatId, text, options) { return send('sendMessage', chatId, { text: String(text) }, options); },
	sendPhoto(chatId, photo, options) { return send('sendPhoto', chatId, { photo }, options); },
	sendToOwner(text, options) { return send('sendMessage', ownerId, { text: String(text) }, options); },
	setCommands(commands) {
		const entries = Array.isArray(commands)
			? commands
			: Object.keys(commands).map((command) => ({ command, description: commands[command] }));
		return call('setMyCommands', {
			commands: entries.map((c) => ({
				command: String(c.command).replace(/^\//, ''),
				description: String(c.description || c.command),
			})),
		});
	},
};

global.bot = bot;
global.Keyboard = Keyboard;
global.db = db;
global.ai = ai;
global.http = http;
})(globalThis);
