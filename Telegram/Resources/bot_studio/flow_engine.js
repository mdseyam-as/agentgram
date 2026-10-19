// ---- Agentgram visual bot engine -------------------------------------
// Runs the FLOW scheme built in the Bot Studio visual constructor.

const BLOCKS = new Map((FLOW.blocks || []).map((b) => [b.id, b]));
const INPUT_ERRORS = {
  phone: 'Пожалуйста, отправьте номер телефона кнопкой ниже или цифрами.',
  number: 'Пожалуйста, введите число.',
  email: 'Пожалуйста, введите корректный e-mail.',
  location: 'Пожалуйста, отправьте геопозицию кнопкой ниже или напишите адрес.',
  photo: 'Пожалуйста, пришлите фото.',
  text: 'Пожалуйста, ответьте текстом.',
};
// Answers that hold a photo file_id: shown as "📷" and sent to the owner.
const PHOTO_VARS = new Set([...BLOCKS.values()]
  .filter((b) => b.input && b.input.enabled && b.input.kind === 'photo')
  .map((b) => b.input.variable || 'answer'));

// ---- shop ------------------------------------------------------------
const SHOP = Object.assign({ currency: 'RUB' }, FLOW.shop || {});
const PRODUCTS = (FLOW.products || []).filter((p) => p && p.title);
const SIGNS = { RUB: '₽', USD: '$', EUR: '€', UAH: '₴', KZT: '₸', BYN: 'Br', UZS: 'сум', GBP: '£', XTR: '⭐' };
const STATUSES = {
  awaiting_payment: { title: '⏳ Ждёт оплаты' },
  new: { title: '🆕 Новый' },
  paid: { title: '💳 Оплачен' },
  accepted: { title: '✅ Принят', user: '✅ Ваш заказ №{id} принят.' },
  delivering: { title: '🚚 В пути', user: '🚚 Ваш заказ №{id} передан в доставку.' },
  done: { title: '✔️ Выполнен', user: '✔️ Заказ №{id} выполнен. Спасибо, что выбрали нас!' },
  canceled: { title: '✖️ Отменён', user: '✖️ Заказ №{id} отменён. Если это ошибка — напишите нам.' },
};

function money(value) {
  const number = Math.round((Number(value) || 0) * 100) / 100;
  const parts = Math.abs(number).toFixed(2).split('.');
  const grouped = parts[0].replace(/\B(?=(\d{3})+(?!\d))/g, ' ');
  const text = (number < 0 ? '−' : '') + grouped + (parts[1] !== '00' ? ',' + parts[1] : '');
  const sign = SIGNS[SHOP.currency] || SHOP.currency || '';
  return (SHOP.currency === 'USD' || SHOP.currency === 'GBP') ? sign + text : (text + ' ' + sign).trim();
}

function productKey(product) {
  return String(product.id !== undefined && product.id !== '' ? product.id : product.title);
}

function cart(ctx) {
  const session = ctx.session;
  if (!session.cart || typeof session.cart !== 'object') session.cart = {};
  return session.cart;
}

function cartSummary(ctx) {
  const items = [];
  const saved = ctx.session.cart;
  const contents = (saved && typeof saved === 'object') ? saved : {};
  for (const product of PRODUCTS) {
    const qty = Number(contents[productKey(product)]) || 0;
    if (qty > 0) items.push({ product, qty, sum: (Number(product.price) || 0) * qty });
  }
  const subtotal = items.reduce((s, i) => s + i.sum, 0);
  const fee = Number(SHOP.delivery) || 0;
  const freeFrom = Number(SHOP.freeFrom) || 0;
  const delivery = (items.length && fee > 0 && !(freeFrom > 0 && subtotal >= freeFrom)) ? fee : 0;
  return {
    items,
    subtotal,
    delivery,
    total: subtotal + delivery,
    count: items.reduce((s, i) => s + i.qty, 0),
  };
}

function cartLines(summary) {
  const lines = summary.items.map((i) => '• ' + i.product.title + ' × ' + i.qty + ' — ' + money(i.sum));
  if (summary.delivery) lines.push('• Доставка — ' + money(summary.delivery));
  return lines.join('\n');
}

function orders() {
  return db.get('orders', []);
}

function saveOrders(list) {
  db.set('orders', list.slice(-5000));
}

function myOrders(ctx) {
  const id = (ctx.from || {}).id;
  const list = orders().filter((o) => o.user_id === id).slice(-5).reverse();
  if (!list.length) return 'У вас пока нет заказов.';
  return list.map((o) => '№' + o.id + ' от ' + formatDate(new Date(o.date))
    + ' — ' + money(o.total) + ' — ' + (STATUSES[o.status] || { title: o.status }).title).join('\n');
}

// ---- variables ---------------------------------------------------------
const pad = (n) => String(n).padStart(2, '0');
function formatDate(date) {
  return pad(date.getDate()) + '.' + pad(date.getMonth() + 1) + '.' + date.getFullYear();
}
function formatTime(date) {
  return pad(date.getHours()) + ':' + pad(date.getMinutes());
}

function vars(ctx) {
  const session = ctx.session;
  session.vars = session.vars || {};
  return session.vars;
}

function values(ctx, text) {
  const from = ctx.from || {};
  const summary = cartSummary(ctx);
  const result = Object.assign({
    first_name: from.first_name || '',
    last_name: from.last_name || '',
    name: [from.first_name, from.last_name].filter(Boolean).join(' '),
    username: from.username ? '@' + from.username : '',
    user_id: from.id,
    bot: '@' + (bot.username || ''),
    last_message: typeof ctx.text === 'string' ? ctx.text : '',
    date: formatDate(new Date()),
    time: formatTime(new Date()),
    cart: summary.items.length ? cartLines(summary) : 'Корзина пуста',
    cart_total: money(summary.total),
    cart_count: summary.count,
  }, vars(ctx));
  if (text && text.includes('{my_orders}')) result.my_orders = myOrders(ctx);
  return result;
}

function fill(text, ctx, encode) {
  text = String(text === undefined || text === null ? '' : text);
  if (!text.includes('{')) return text;
  const map = values(ctx, text);
  return text.replace(/\{([^{}\s]+)\}/g, (match, key) => {
    if (map[key] === undefined || map[key] === null) return match;
    const value = PHOTO_VARS.has(key) && !encode ? '📷 фото' : String(map[key]);
    return encode ? encodeURIComponent(value) : value;
  });
}

function who(ctx) {
  const from = ctx.from || {};
  const name = [from.first_name, from.last_name].filter(Boolean).join(' ');
  return (name || 'Пользователь') + (from.username ? ' (@' + from.username + ')' : '') + ' · id ' + from.id;
}

// ---- buttons -----------------------------------------------------------
function buttonRows(buttons, make) {
  const rows = [];
  let row = [];
  for (const button of buttons) {
    const wide = String(button.text).length > 18 || button.newRow;
    if (row.length && (wide || row.length >= 2)) {
      rows.push(row);
      row = [];
    }
    row.push(make(button));
    if (wide) {
      rows.push(row);
      row = [];
    }
  }
  if (row.length) rows.push(row);
  return rows;
}

// Inline rows for the block's own buttons.
function blockButtonRows(block, ctx) {
  const buttons = (block.buttons || [])
    .map((b, index) => Object.assign({ index }, b))
    .filter((b) => b && b.text);
  return buttonRows(buttons, (b) => (b.url
    ? { text: fill(b.text, ctx), url: fill(b.url, ctx, true) }
    : { text: fill(b.text, ctx), data: 'fb:' + block.id + ':' + b.index }));
}

function markup(block, ctx) {
  const buttons = (block.buttons || []).filter((b) => b && b.text);
  const input = block.input && block.input.enabled ? block.input : null;
  if (input && input.kind === 'phone') {
    return { keyboard: [[{ text: input.buttonText || '📱 Отправить номер', contact: true }]] };
  }
  if (input && input.kind === 'location') {
    return { keyboard: [[{ text: input.buttonText || '📍 Отправить геопозицию', location: true }]] };
  }
  if (!buttons.length) {
    return (FLOW.buttonsStyle === 'keyboard' || ctx._removeKeyboard) ? { removeKeyboard: true } : {};
  }
  if (FLOW.buttonsStyle === 'keyboard') {
    return { keyboard: buttonRows(buttons.filter((b) => !b.url), (b) => fill(b.text, ctx)) };
  }
  return { buttons: blockButtonRows(block, ctx) };
}

// ---- owner notifications and leads --------------------------------------
function summary(ctx, block) {
  const lines = ['📩 ' + (block.title || 'Новая заявка') + ' — ' + who(ctx)];
  const answers = vars(ctx);
  for (const key of Object.keys(answers)) {
    lines.push(key + ': ' + (PHOTO_VARS.has(key) ? '📷 фото ниже' : answers[key]));
  }
  return lines.join('\n');
}

async function sendPhotos(ctx, chat) {
  const answers = vars(ctx);
  for (const key of PHOTO_VARS) {
    if (answers[key]) {
      await bot.sendPhoto(chat, answers[key], { caption: key }).catch((e) =>
        console.warn('Не удалось переслать фото «' + key + '»:', e.message));
    }
  }
}

async function notifyOwner(ctx, block, text) {
  try {
    await bot.sendToOwner(text || summary(ctx, block));
    if (!text) await sendPhotos(ctx, bot.ownerId);
  } catch (e) {
    console.warn('Не удалось написать владельцу (откройте бота и нажмите /start):', e.message);
  }
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function saveLead(ctx, block) {
  const from = ctx.from || {};
  const leads = db.get('leads', []);
  leads.push({
    date: new Date().toISOString(),
    step: block.title || block.id,
    user_id: from.id,
    username: from.username ? '@' + from.username : '',
    name: [from.first_name, from.last_name].filter(Boolean).join(' '),
    answers: Object.assign({}, vars(ctx)),
  });
  db.set('leads', leads.slice(-5000));
}

// ---- conditions ----------------------------------------------------------
const DAYS = ['пн', 'вт', 'ср', 'чт', 'пт', 'сб', 'вс'];

// "пн-пт 9:00-18:00", "10-22", "сб,вс" — local time of this computer.
function inHours(spec) {
  const text = String(spec || '').toLowerCase();
  const now = new Date();
  const today = (now.getDay() + 6) % 7;
  const range = text.match(/(пн|вт|ср|чт|пт|сб|вс)\s*-\s*(пн|вт|ср|чт|пт|сб|вс)/);
  if (range) {
    const from = DAYS.indexOf(range[1]);
    const to = DAYS.indexOf(range[2]);
    const inside = from <= to ? (today >= from && today <= to) : (today >= from || today <= to);
    if (!inside) return false;
  } else {
    const listed = DAYS.filter((d) => text.includes(d));
    if (listed.length && !listed.includes(DAYS[today])) return false;
  }
  const time = text.match(/(\d{1,2})(?:[:.](\d{2}))?\s*-\s*(\d{1,2})(?:[:.](\d{2}))?/);
  if (time) {
    const start = Number(time[1]) * 60 + Number(time[2] || 0);
    const end = Number(time[3]) * 60 + Number(time[4] || 0);
    const current = now.getHours() * 60 + now.getMinutes();
    return start <= end
      ? (current >= start && current < end)
      : (current >= start || current < end);
  }
  return true;
}

async function isMember(ctx, chat) {
  chat = String(chat || '').trim();
  if (!chat || !ctx.from) return false;
  try {
    const member = await bot.api.getChatMember({ chat_id: chat, user_id: ctx.from.id });
    return ['creator', 'administrator', 'member'].includes(member.status)
      || (member.status === 'restricted' && !!member.is_member);
  } catch (e) {
    console.warn('Не удалось проверить подписку на ' + chat + ' (бот должен быть администратором канала):', e.message);
    return false;
  }
}

async function compare(condition, ctx) {
  const op = condition.op || 'eq';
  if (op === 'member') return isMember(ctx, fill(condition.value, ctx));
  if (op === 'tag') return bot.tags.has(ctx.chatId, fill(condition.value, ctx).trim());
  if (op === 'hours') return inHours(condition.value);
  const raw = values(ctx)[condition.variable];
  const value = raw === undefined || raw === null ? '' : String(raw);
  const expected = fill(condition.value, ctx);
  const number = (v) => Number(String(v).replace(',', '.').replace(/[^\d.\-]/g, ''));
  switch (op) {
  case 'eq': return value.trim().toLowerCase() === expected.trim().toLowerCase();
  case 'neq': return value.trim().toLowerCase() !== expected.trim().toLowerCase();
  case 'contains': return value.toLowerCase().includes(expected.toLowerCase());
  case 'gt': return number(value) > number(expected);
  case 'lt': return number(value) < number(expected);
  case 'filled': return value.trim() !== '';
  case 'empty': return value.trim() === '';
  }
  return false;
}

// ---- reminders -------------------------------------------------------------
const JOBS = '__jobs';
const UNITS = { min: 60, hour: 3600, day: 86400 };

function schedule(ctx, action) {
  if (!action.goto || ctx.chatId === undefined) return;
  const amount = Number(fill(action.value === undefined ? '1' : action.value, ctx).replace(',', '.')) || 0;
  const seconds = Math.max(10, amount * (UNITS[action.unit] || 60));
  const jobs = db.get(JOBS, []);
  jobs.push({ at: Date.now() + seconds * 1000, chat: ctx.chatId, goto: action.goto, from: ctx.from });
  db.set(JOBS, jobs.slice(-20000));
}

async function runJobs() {
  const now = Date.now();
  const jobs = db.get(JOBS, []);
  const due = jobs.filter((j) => j.at <= now);
  if (!due.length) return;
  db.set(JOBS, jobs.filter((j) => j.at > now));
  for (const job of due) {
    await bot.withChat(job.chat, (ctx) => show(ctx, job.goto), job.from);
  }
}
setInterval(runJobs, 15000);
setTimeout(runJobs, 3000);

// ---- actions ---------------------------------------------------------------
function pick(object, path) {
  for (const key of String(path).split('.').map((s) => s.trim()).filter(Boolean)) {
    if (object === undefined || object === null) return '';
    object = object[key];
  }
  return object;
}

function payload(ctx, block) {
  const from = ctx.from || {};
  return {
    bot: bot.username,
    step: block.title || block.id,
    user_id: from.id,
    username: from.username || '',
    name: [from.first_name, from.last_name].filter(Boolean).join(' '),
    date: new Date().toISOString(),
    answers: Object.assign({}, vars(ctx)),
  };
}

async function runAction(ctx, action, block) {
  const answers = vars(ctx);
  switch (action.kind) {
  case 'set':
    if (action.variable) answers[action.variable] = fill(action.value, ctx);
    return;
  case 'increment': {
    if (!action.variable) return;
    const by = Number(fill(action.value === undefined ? '1' : action.value, ctx)) || 0;
    answers[action.variable] = (Number(answers[action.variable]) || 0) + by;
    return;
  }
  case 'calc': {
    if (!action.variable) return;
    const expression = fill(action.value, ctx).replace(/,/g, '.').replace(/\s/g, '');
    if (!/^[\d+\-*/().%]+$/.test(expression)) {
      console.warn('Формула «' + expression + '» содержит не только числа и + - * / ( )');
      return;
    }
    try {
      const result = Function('"use strict"; return (' + expression + ');')();
      answers[action.variable] = Number.isFinite(result) ? Math.round(result * 100) / 100 : 0;
    } catch (e) {
      console.warn('Ошибка в формуле «' + expression + '»:', e.message);
    }
    return;
  }
  case 'clear':
    ctx.session.vars = {};
    return;
  case 'clear_cart':
    ctx.session.cart = {};
    return;
  case 'add_to_cart': {
    const product = PRODUCTS.find((p) => productKey(p) === String(action.product));
    if (!product) {
      console.warn('Товар «' + action.product + '» не найден в каталоге');
      return;
    }
    const contents = cart(ctx);
    const key = productKey(product);
    contents[key] = (Number(contents[key]) || 0) + (Number(fill(action.value || '1', ctx)) || 1);
    return;
  }
  case 'tag':
    bot.tags.add(ctx.chatId, fill(action.value, ctx));
    return;
  case 'untag':
    bot.tags.remove(ctx.chatId, fill(action.value, ctx));
    return;
  case 'remind':
    schedule(ctx, action);
    return;
  case 'cancel_reminders': {
    const jobs = db.get(JOBS, []);
    db.set(JOBS, jobs.filter((j) => j.chat !== ctx.chatId));
    return;
  }
  case 'notify':
    await notifyOwner(ctx, block, action.text ? fill(action.text, ctx) : '');
    return;
  case 'send':
    if (!action.chat) return;
    await bot.send(String(action.chat).trim(), fill(action.text, ctx) || summary(ctx, block)).catch((e) =>
      console.warn('Не удалось отправить в ' + action.chat + ' (бот должен быть участником/админом):', e.message));
    return;
  case 'webhook': {
    if (!action.url) return;
    const result = await http.post(fill(action.url, ctx, true), payload(ctx, block))
      .catch((e) => ({ ok: false, status: 0, text: e.message }));
    if (!result.ok) console.warn('Вебхук ответил ' + result.status + ': ' + String(result.text).slice(0, 200));
    return;
  }
  case 'request': {
    const url = fill(action.url, ctx, true).trim();
    if (!/^https?:\/\//i.test(url)) return;
    try {
      const result = String(action.method || 'GET').toUpperCase() === 'POST'
        ? await http.post(url, payload(ctx, block))
        : await http.get(url);
      if (!result.ok) console.warn('Запрос ' + url + ' ответил ' + result.status);
      let value = result.text;
      if (action.path) {
        try {
          value = pick(JSON.parse(result.text), action.path);
        } catch (e) {
          value = '';
        }
      }
      if (action.variable) {
        answers[action.variable] = (value !== null && typeof value === 'object')
          ? JSON.stringify(value)
          : (value === undefined || value === null ? '' : String(value).slice(0, 3000));
      }
    } catch (e) {
      console.warn('Запрос ' + url + ' не удался:', e.message);
      if (action.variable) answers[action.variable] = '';
    }
    return;
  }
  case 'save_lead':
    saveLead(ctx, block);
    return;
  }
}

// ---- messages --------------------------------------------------------------
async function sendMessage(ctx, block, extra) {
  const text = fill(block.text, ctx).trim();
  const media = block.media && block.media.url
    ? block.media
    : (block.photo ? { kind: 'photo', url: block.photo } : null);
  if (media) {
    const url = fill(media.url, ctx).trim();
    const options = Object.assign({ caption: text || undefined }, extra);
    if (media.kind === 'video') return ctx.replyVideo(url, options);
    if (media.kind === 'document') return ctx.replyDocument(url, options);
    if (media.kind === 'audio') return ctx.replyAudio(url, options);
    return ctx.replyPhoto(url, options);
  }
  if (block.location && block.location.latitude !== undefined) {
    if (text) await ctx.reply(text, extra);
    return ctx.replyLocation(Number(block.location.latitude), Number(block.location.longitude));
  }
  return ctx.reply(text || '…', extra);
}

// Shows a view {text, photo, rows}. With edit it replaces the message with
// the pressed button when possible, otherwise sends a new one.
async function render(ctx, view, edit, sameMedia) {
  const message = edit && ctx.callbackQuery && ctx.callbackQuery.message;
  const reply_markup = Keyboard.inline(view.rows.length ? view.rows : []);
  if (message && message.message_id) {
    const hadPhoto = !!(message.photo && message.photo.length);
    const base = { chat_id: ctx.chatId, message_id: message.message_id, reply_markup };
    try {
      if (hadPhoto && view.photo) {
        if (sameMedia) {
          await bot.api.editMessageCaption(Object.assign({ caption: view.text }, base));
        } else {
          await bot.api.editMessageMedia(Object.assign({
            media: { type: 'photo', media: view.photo, caption: view.text },
          }, base));
        }
        return;
      }
      if (!hadPhoto && !view.photo) {
        await bot.api.editMessageText(Object.assign({ text: view.text }, base));
        return;
      }
    } catch (e) {
      if (/not modified/i.test(e.message)) return;
    }
    await ctx.deleteMessage(message.message_id).catch(() => {});
  }
  if (view.photo) {
    return ctx.replyPhoto(view.photo, { caption: view.text, reply_markup });
  }
  return ctx.reply(view.text, { reply_markup });
}

// ---- catalog and cart --------------------------------------------------------
function categories() {
  const result = [];
  for (const product of PRODUCTS) {
    const category = String(product.category || '').trim();
    if (category && !result.includes(category)) result.push(category);
  }
  return result;
}

function firstBlockOfType(type) {
  for (const block of BLOCKS.values()) {
    if (block.type === type) return block.id;
  }
  return '';
}

function catalogList(block, catIndex) {
  const filter = catIndex === '-'
    ? String(block.category || '').trim()
    : categories()[Number(catIndex)];
  return PRODUCTS.filter((p) => !filter || String(p.category || '').trim() === filter);
}

function useCategories(block) {
  return !String(block.category || '').trim() && categories().length > 1;
}

function cartButton(ctx, block) {
  const target = block.cart || firstBlockOfType('cart');
  const totals = cartSummary(ctx);
  return (target && totals.count)
    ? [{ text: '🧺 Корзина (' + totals.count + ') · ' + money(totals.total), data: 'flow:' + target }]
    : null;
}

function categoryView(ctx, block) {
  const rows = buttonRows(
    categories().map((title, i) => ({ text: title, data: 'ct:' + block.id + ':' + i + ':0:v' })),
    (b) => b);
  const toCart = cartButton(ctx, block);
  if (toCart) rows.push(toCart);
  return {
    text: fill(block.text, ctx).trim() || '🛍 Выберите категорию:',
    rows: rows.concat(blockButtonRows(block, ctx)),
  };
}

function productView(ctx, block, catIndex, index) {
  const list = catalogList(block, catIndex);
  if (!list.length) {
    return { text: 'В этом разделе пока нет товаров.', rows: blockButtonRows(block, ctx) };
  }
  index = ((Number(index) % list.length) + list.length) % list.length;
  const product = list[index];
  const qty = Number(cart(ctx)[productKey(product)]) || 0;
  const data = (op, i) => 'ct:' + block.id + ':' + catIndex + ':' + (i === undefined ? index : i) + ':' + op;
  const lines = [product.title];
  if (product.description) lines.push('', product.description);
  lines.push('', '💰 ' + money(product.price));
  const rows = [];
  if (list.length > 1) {
    rows.push([
      { text: '◀️', data: data('v', index - 1) },
      { text: (index + 1) + ' / ' + list.length, data: data('n') },
      { text: '▶️', data: data('v', index + 1) },
    ]);
  }
  if (product.url) {
    rows.push([{ text: '🔗 Подробнее', url: String(product.url) }]);
  }
  rows.push(qty > 0
    ? [
      { text: '➖', data: data('m') },
      { text: '🛒 ' + qty + ' шт.', data: data('n') },
      { text: '➕', data: data('a') },
    ]
    : [{ text: '🛒 В корзину · ' + money(product.price), data: data('a') }]);
  const toCart = cartButton(ctx, block);
  if (toCart) rows.push(toCart);
  if (catIndex !== '-') rows.push([{ text: '⬅️ Категории', data: 'ct:' + block.id + ':-:0:c' }]);
  return {
    text: lines.join('\n'),
    photo: product.photo ? String(product.photo).trim() : '',
    rows: rows.concat(blockButtonRows(block, ctx)),
  };
}

async function showCatalog(ctx, block) {
  if (!PRODUCTS.length) {
    console.warn('Каталог пуст: добавьте товары в Bot Studio (кнопка «🛍 Товары»).');
    return ctx.reply(fill(block.text, ctx).trim() || 'Каталог скоро появится.');
  }
  if (useCategories(block)) return render(ctx, categoryView(ctx, block));
  const header = fill(block.text, ctx).trim();
  if (header) await ctx.reply(header);
  return render(ctx, productView(ctx, block, '-', 0));
}

bot.action(/^ct:([^:]+):([^:]+):(-?\d+):(\w)$/, async (ctx) => {
  const block = BLOCKS.get(ctx.match[1]);
  if (!block) return ctx.answer();
  const catIndex = ctx.match[2];
  const index = Number(ctx.match[3]);
  const op = ctx.match[4];
  if (op === 'c') {
    await ctx.answer();
    return render(ctx, useCategories(block) ? categoryView(ctx, block) : productView(ctx, block, '-', 0), true);
  }
  const list = catalogList(block, catIndex);
  const product = list.length ? list[((index % list.length) + list.length) % list.length] : null;
  if ((op === 'a' || op === 'm') && product) {
    const contents = cart(ctx);
    const key = productKey(product);
    const qty = Math.max(0, (Number(contents[key]) || 0) + (op === 'a' ? 1 : -1));
    if (qty) contents[key] = qty; else delete contents[key];
    await ctx.answer(op === 'a' ? '🛒 ' + product.title + ': ' + qty + ' шт.' : (qty ? qty + ' шт.' : 'Убрано из корзины'));
    return render(ctx, productView(ctx, block, catIndex, index), true, true);
  }
  await ctx.answer();
  if (op === 'v') return render(ctx, productView(ctx, block, catIndex, index), true);
});

function cartView(ctx, block) {
  const totals = cartSummary(ctx);
  const rows = [];
  if (!totals.items.length) {
    const catalog = block.catalog || firstBlockOfType('catalog');
    if (catalog) rows.push([{ text: '🛍 Перейти в каталог', data: 'flow:' + catalog }]);
    return {
      text: fill(block.emptyText, ctx).trim() || '🧺 Корзина пуста.',
      rows: rows.concat(blockButtonRows(block, ctx)),
    };
  }
  const lines = [fill(block.text, ctx).trim() || '🧺 Ваша корзина:', '', cartLines(totals), '', '💰 Итого: ' + money(totals.total)];
  const freeFrom = Number(SHOP.freeFrom) || 0;
  if (totals.delivery && freeFrom > totals.subtotal) {
    lines.push('До бесплатной доставки: ' + money(freeFrom - totals.subtotal));
  }
  for (const item of totals.items.slice(0, 12)) {
    const i = PRODUCTS.indexOf(item.product);
    rows.push([
      { text: '➖', data: 'kt:' + block.id + ':' + i + ':m' },
      { text: item.product.title.slice(0, 24) + ' × ' + item.qty, data: 'kt:' + block.id + ':' + i + ':n' },
      { text: '➕', data: 'kt:' + block.id + ':' + i + ':p' },
    ]);
  }
  rows.push([{ text: '🗑 Очистить корзину', data: 'kt:' + block.id + ':0:x' }]);
  const minOrder = Number(SHOP.minOrder) || 0;
  if (minOrder > totals.subtotal) {
    rows.push([{ text: '🔒 Минимальный заказ — ' + money(minOrder), data: 'kt:' + block.id + ':0:l' }]);
  } else if (block.checkout) {
    rows.push([{ text: '✅ Оформить заказ · ' + money(totals.total), data: 'flow:' + block.checkout }]);
  }
  return { text: lines.join('\n'), rows: rows.concat(blockButtonRows(block, ctx)) };
}

bot.action(/^kt:([^:]+):(\d+):(\w)$/, async (ctx) => {
  const block = BLOCKS.get(ctx.match[1]);
  if (!block) return ctx.answer();
  const product = PRODUCTS[Number(ctx.match[2])];
  const op = ctx.match[3];
  const contents = cart(ctx);
  if (op === 'x') {
    ctx.session.cart = {};
  } else if (op === 'l') {
    return ctx.answer('Добавьте товаров на ' + money((Number(SHOP.minOrder) || 0) - cartSummary(ctx).subtotal), { show_alert: true });
  } else if (product && (op === 'p' || op === 'm')) {
    const key = productKey(product);
    const qty = Math.max(0, (Number(contents[key]) || 0) + (op === 'p' ? 1 : -1));
    if (qty) contents[key] = qty; else delete contents[key];
  }
  await ctx.answer();
  if (op !== 'n') return render(ctx, cartView(ctx, block), true);
});

// ---- orders and payments -------------------------------------------------------
function orderText(order, forStaff) {
  const status = (STATUSES[order.status] || { title: order.status }).title;
  const lines = [(forStaff ? '🛒 Заказ №' : 'Заказ №') + order.id + ' · ' + status];
  if (forStaff) {
    lines.push('👤 ' + (order.name || 'Покупатель') + (order.username ? ' (' + order.username + ')' : '') + ' · id ' + order.user_id);
  }
  lines.push('');
  for (const item of order.items) lines.push('• ' + item.title + ' × ' + item.qty + ' — ' + money(item.sum));
  if (order.delivery) lines.push('• Доставка — ' + money(order.delivery));
  lines.push('💰 Итого: ' + money(order.total) + (order.payment === 'none'
    ? ' (оплата при получении)'
    : order.status === 'awaiting_payment' ? ' (ждёт оплаты)' : ' (оплачено онлайн)'));
  const answers = order.answers || {};
  const keys = Object.keys(answers);
  if (forStaff && keys.length) {
    lines.push('');
    for (const key of keys) lines.push(key + ': ' + (PHOTO_VARS.has(key) ? '📷 фото' : answers[key]));
  }
  return lines.join('\n');
}

function statusRows(order) {
  const button = (status) => ({
    text: (order.status === status ? '● ' : '') + STATUSES[status].title,
    data: 'os:' + order.id + ':' + status,
  });
  return [[button('accepted'), button('delivering')], [button('done'), button('canceled')]];
}

async function noticeOrder(order) {
  const chat = SHOP.ordersChat ? String(SHOP.ordersChat).trim() : bot.ownerId;
  try {
    const sent = await bot.send(chat, orderText(order, true), { buttons: statusRows(order) });
    const list = orders();
    const saved = list.find((o) => o.id === order.id);
    if (saved && sent && sent.chat) {
      saved.notice = { chat: sent.chat.id, message_id: sent.message_id };
      saveOrders(list);
    }
  } catch (e) {
    console.warn('Не удалось отправить заказ №' + order.id + (SHOP.ordersChat
      ? ' в ' + SHOP.ordersChat + ' (добавьте туда бота)'
      : ' владельцу (откройте бота и нажмите /start)') + ':', e.message);
  }
}

async function setOrderStatus(id, status) {
  const list = orders();
  const order = list.find((o) => o.id === Number(id));
  if (!order || !STATUSES[status]) return null;
  if (order.status === status) return order;
  order.status = status;
  order.updated = new Date().toISOString();
  saveOrders(list);
  console.log('🛒 Заказ №' + order.id + ': ' + STATUSES[status].title);
  const text = STATUSES[status].user;
  if (text && order.chat_id) {
    await bot.send(order.chat_id, text.replace('{id}', order.id)).catch((e) =>
      console.warn('Покупатель не получил статус заказа:', e.message));
  }
  if (order.notice) {
    await bot.api.editMessageText({
      chat_id: order.notice.chat,
      message_id: order.notice.message_id,
      text: orderText(order, true),
      reply_markup: Keyboard.inline(statusRows(order)),
    }).catch(() => {});
  }
  return order;
}

bot.onHook('order_status', (request) => setOrderStatus(request.order, request.status));

bot.action(/^os:(\d+):(\w+)$/, async (ctx) => {
  const order = orders().find((o) => o.id === Number(ctx.match[1]));
  const staff = ctx.isOwner || (order && order.notice && order.notice.chat === ctx.chatId
    && ctx.chat && ctx.chat.type !== 'private');
  if (!staff) return ctx.answer('Менять статус может только владелец');
  const result = await setOrderStatus(ctx.match[1], ctx.match[2]);
  return ctx.answer(result ? STATUSES[result.status].title : 'Заказ не найден');
});

function starsAmount(total) {
  if (SHOP.currency === 'XTR') return Math.max(1, Math.round(total));
  const rate = Number(SHOP.starsRate) || 0;
  return rate > 0 ? Math.max(1, Math.ceil(total / rate)) : 0;
}

async function requestPayment(ctx, block, order) {
  const stars = block.payment === 'stars';
  const amount = stars ? starsAmount(order.total) : Math.round(order.total * 100);
  if (!amount) {
    console.error('Оплата звёздами: укажите в «🛍 Товары» сколько стоит 1 ⭐ в валюте магазина.');
    return false;
  }
  if (!stars && !SHOP.providerToken) {
    console.error('Оплата картой: вставьте токен платёжной системы из @BotFather в «🛍 Товары».');
    return false;
  }
  const description = order.items.map((i) => i.title + ' × ' + i.qty).join(', ');
  await bot.api.sendInvoice({
    chat_id: ctx.chatId,
    title: ('Заказ №' + order.id).slice(0, 32),
    description: (description || 'Заказ').slice(0, 250),
    payload: 'order:' + order.id,
    currency: stars ? 'XTR' : SHOP.currency,
    provider_token: stars ? '' : String(SHOP.providerToken),
    prices: [{ label: 'Заказ №' + order.id, amount }],
  });
  return true;
}

async function placeOrder(ctx, block, depth) {
  const totals = cartSummary(ctx);
  if (!totals.items.length) {
    if (block.empty) return show(ctx, block.empty, depth + 1);
    return ctx.reply('🧺 Корзина пуста — добавьте товары из каталога.');
  }
  const minOrder = Number(SHOP.minOrder) || 0;
  if (minOrder > totals.subtotal) {
    return ctx.reply('Минимальная сумма заказа — ' + money(minOrder) + '.');
  }
  const from = ctx.from || {};
  const payment = block.payment === 'stars' || block.payment === 'card' ? block.payment : 'none';
  const order = {
    id: db.increment('order_seq'),
    date: new Date().toISOString(),
    chat_id: ctx.chatId,
    user_id: from.id,
    name: [from.first_name, from.last_name].filter(Boolean).join(' '),
    username: from.username ? '@' + from.username : '',
    items: totals.items.map((i) => ({
      id: productKey(i.product),
      title: i.product.title,
      price: Number(i.product.price) || 0,
      qty: i.qty,
      sum: i.sum,
    })),
    subtotal: totals.subtotal,
    delivery: totals.delivery,
    total: totals.total,
    currency: SHOP.currency,
    payment,
    status: payment === 'none' ? 'new' : 'awaiting_payment',
    answers: Object.assign({}, vars(ctx)),
    block: block.id,
  };
  const list = orders();
  list.push(order);
  saveOrders(list);
  Object.assign(vars(ctx), {
    order_id: order.id,
    order_total: money(order.total),
    order_items: cartLines(totals),
  });
  ctx.session.cart = {};
  console.log('🛒 Новый заказ №' + order.id + ' на ' + money(order.total) + ' от ' + who(ctx));
  if (block.text) await sendMessage(ctx, block, markup(block, ctx));
  if (payment !== 'none') {
    if (!await requestPayment(ctx, block, order)) {
      await ctx.reply('Онлайн-оплата временно недоступна. Мы свяжемся с вами для оплаты.');
      order.payment = 'none';
      order.status = 'new';
      const saved = orders();
      const index = saved.findIndex((o) => o.id === order.id);
      if (index >= 0) saved[index] = order;
      saveOrders(saved);
      await noticeOrder(order);
    }
    return;
  }
  await noticeOrder(order);
  if (block.next) {
    await sleep(500);
    return show(ctx, block.next, depth + 1);
  }
}

bot.on('pre_checkout_query', async (ctx) => {
  const query = ctx.update.pre_checkout_query;
  const match = /^order:(\d+)$/.exec(query.invoice_payload || '');
  const order = match && orders().find((o) => o.id === Number(match[1]));
  const ok = !!order && order.status === 'awaiting_payment';
  await bot.api.answerPreCheckoutQuery(ok
    ? { pre_checkout_query_id: query.id, ok: true }
    : { pre_checkout_query_id: query.id, ok: false, error_message: 'Заказ не найден или уже оплачен.' });
});

bot.on('successful_payment', async (ctx) => {
  const payment = ctx.message.successful_payment;
  const match = /^order:(\d+)$/.exec(payment.invoice_payload || '');
  if (!match) return;
  const list = orders();
  const order = list.find((o) => o.id === Number(match[1]));
  if (!order) return;
  order.status = 'paid';
  order.paid = {
    amount: payment.total_amount,
    currency: payment.currency,
    charge: payment.telegram_payment_charge_id,
  };
  saveOrders(list);
  console.log('💳 Заказ №' + order.id + ' оплачен: ' + payment.total_amount + ' ' + payment.currency);
  Object.assign(vars(ctx), { order_id: order.id, order_total: money(order.total) });
  await noticeOrder(order);
  const block = BLOCKS.get(order.block);
  if (block && block.paid) return show(ctx, block.paid);
  return ctx.reply('✅ Оплата получена! Заказ №' + order.id + ' принят.');
});

// ---- operator chat -------------------------------------------------------------
const RELAY = '__relay';

function remember(sent, chatId) {
  if (!sent || !sent.message_id) return;
  const map = db.get(RELAY, {});
  map[String(sent.message_id)] = { user: chatId, to: sent.chat ? sent.chat.id : undefined };
  const keys = Object.keys(map);
  if (keys.length > 1500) {
    keys.sort((a, b) => Number(a) - Number(b)).slice(0, keys.length - 1500).forEach((k) => delete map[k]);
  }
  db.set(RELAY, map);
}

function operatorChat(ctx) {
  const live = ctx.session.live || {};
  return live.chat ? String(live.chat).trim() : bot.ownerId;
}

async function startOperator(ctx, block) {
  ctx.session.live = { block: block.id, chat: block.chat || '' };
  ctx.session.wait = null;
  await sendMessage(ctx, Object.assign({}, block, {
    text: block.text || '💬 Напишите ваш вопрос — оператор ответит прямо здесь.',
  }), { buttons: [[{ text: block.endText || '✖️ Завершить диалог', data: 'op:end' }]] });
  const sent = await bot.send(operatorChat(ctx), '🙋 ' + who(ctx) + ' просит оператора.\n'
    + 'Отвечайте ответом (reply) на его сообщения — бот перешлёт ваш ответ.').catch((e) => {
    console.warn('Оператор не получил запрос (откройте бота и нажмите /start или добавьте бота в чат поддержки):', e.message);
  });
  remember(sent, ctx.chatId);
}

async function relayToOperator(ctx) {
  const chat = operatorChat(ctx);
  try {
    const own = ctx.message;
    if (typeof own.text === 'string') {
      remember(await bot.send(chat, '💬 ' + who(ctx) + ':\n' + own.text), ctx.chatId);
    } else {
      const header = await bot.send(chat, '💬 ' + who(ctx) + ':');
      remember(header, ctx.chatId);
      const copy = await ctx.copyTo(chat);
      if (copy && header) remember({ message_id: copy.message_id, chat: header.chat }, ctx.chatId);
    }
  } catch (e) {
    console.warn('Сообщение не передано оператору:', e.message);
    await ctx.reply('Не удалось передать сообщение. Попробуйте позже.');
  }
}

async function relayFromOperator(ctx) {
  const reply = ctx.message && ctx.message.reply_to_message;
  if (!reply) return false;
  const entry = db.get(RELAY, {})[String(reply.message_id)];
  if (!entry || (entry.to !== undefined && entry.to !== ctx.chatId)) return false;
  if (!ctx.isOwner && !(ctx.chat && ctx.chat.type !== 'private')) return false;
  try {
    await ctx.copyTo(entry.user);
    await ctx.react('👍').catch(() => {});
  } catch (e) {
    await ctx.reply('Не доставлено: ' + e.message);
  }
  return true;
}

bot.action('op:end', async (ctx) => {
  await ctx.answer();
  const live = ctx.session.live;
  if (!live) return;
  ctx.session.live = null;
  await ctx.reply('Диалог с оператором завершён. Спасибо!');
  await bot.send(live.chat ? String(live.chat).trim() : bot.ownerId, '✅ ' + who(ctx) + ' завершил диалог.').catch(() => {});
  const block = BLOCKS.get(live.block);
  if (block && block.next) return show(ctx, block.next);
});

// ---- the scheme -----------------------------------------------------------------
async function show(ctx, id, depth) {
  const block = BLOCKS.get(id);
  if (!block) {
    console.warn('Блок не найден:', id);
    return;
  }
  depth = depth || 0;
  if (depth > 25) {
    console.warn('Слишком длинная цепочка блоков без ответа пользователя, остановлено на', block.title || id);
    return;
  }
  const type = block.type || 'message';
  if (block.delay > 0) {
    await ctx.typing().catch(() => {});
    await sleep(Math.min(Number(block.delay) || 0, 30) * 1000);
  }
  if (type === 'condition') {
    for (const condition of block.conditions || []) {
      if (condition.goto && await compare(condition, ctx)) {
        return show(ctx, condition.goto, depth + 1);
      }
    }
    if (block.else) return show(ctx, block.else, depth + 1);
    return;
  }
  if (type === 'random') {
    const branches = (block.branches || []).filter((b) => b && b.goto);
    const total = branches.reduce((s, b) => s + Math.max(0, Number(b.weight === undefined ? 1 : b.weight) || 0), 0);
    let roll = Math.random() * total;
    for (const branch of branches) {
      roll -= Math.max(0, Number(branch.weight === undefined ? 1 : branch.weight) || 0);
      if (roll < 0) return show(ctx, branch.goto, depth + 1);
    }
    return;
  }
  if (type === 'action') {
    for (const action of block.actions || []) {
      await runAction(ctx, action, block);
    }
    if (block.next) return show(ctx, block.next, depth + 1);
    return;
  }
  if (type === 'catalog') {
    ctx.session.wait = null;
    return showCatalog(ctx, block);
  }
  if (type === 'cart') {
    ctx.session.wait = null;
    return render(ctx, cartView(ctx, block));
  }
  if (type === 'order') {
    ctx.session.wait = null;
    return placeOrder(ctx, block, depth);
  }
  if (type === 'operator') {
    return startOperator(ctx, block);
  }
  if (type === 'ai') {
    await ctx.typing().catch(() => {});
    let answer = '';
    try {
      answer = await ai.ask({
        system: fill(block.system, ctx) || 'Ты помощник в Telegram-боте. Отвечай кратко, по делу, без Markdown.',
        prompt: fill(block.prompt || '{last_message}', ctx),
      });
    } catch (e) {
      console.error('Ответ ИИ не получен:', e.message);
      answer = block.fallbackText ? fill(block.fallbackText, ctx) : '';
    }
    if (block.variable) vars(ctx)[block.variable] = answer;
    if (answer && block.send !== false) {
      await ctx.reply(answer.slice(0, 4000), markup(block, ctx));
    }
    const buttons = (block.buttons || []).filter((b) => b && b.text);
    if (!buttons.length && block.next) {
      await sleep(400);
      return show(ctx, block.next, depth + 1);
    }
    ctx.session.wait = null;
    return;
  }

  await sendMessage(ctx, block, markup(block, ctx));
  if (block.notifyOwner) {
    saveLead(ctx, block);
    await notifyOwner(ctx, block);
  }
  const input = block.input && block.input.enabled ? block.input : null;
  if (input) {
    ctx.session.wait = id;
  } else {
    ctx.session.wait = null;
    if (block.next) {
      await sleep(600);
      await show(ctx, block.next, depth + 1);
    }
  }
}

function readInput(ctx, input) {
  const kind = input.kind || 'text';
  const message = ctx.message || {};
  if (kind === 'phone') {
    if (message.contact) return message.contact.phone_number;
    const digits = String(ctx.text || '').replace(/[^\d+]/g, '');
    return digits.replace(/\D/g, '').length >= 6 ? digits : undefined;
  }
  if (kind === 'location') {
    const point = message.location || (message.venue && message.venue.location);
    if (point) {
      return (message.venue ? message.venue.address + ' — ' : '')
        + 'https://maps.google.com/?q=' + point.latitude + ',' + point.longitude;
    }
  }
  if (kind === 'photo') {
    if (message.photo && message.photo.length) return message.photo[message.photo.length - 1].file_id;
    if (message.document && /^image\//.test(message.document.mime_type || '')) return message.document.file_id;
    return undefined;
  }
  if (typeof ctx.text !== 'string' || !ctx.text.trim()) return undefined;
  const text = ctx.text.trim();
  if (kind === 'number') {
    const value = Number(text.replace(',', '.').replace(/\s/g, ''));
    return Number.isFinite(value) ? value : undefined;
  }
  if (kind === 'email') {
    return /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(text) ? text : undefined;
  }
  return text;
}

function choose(ctx, block, button) {
  ctx.session.live = null;
  if (block.saveChoice) {
    vars(ctx)[block.saveChoice] = button.value !== undefined && button.value !== ''
      ? fill(button.value, ctx)
      : fill(button.text, ctx);
  }
  if (ctx.session.wait === block.id) ctx.session.wait = null;
  if (button.goto) return show(ctx, button.goto);
}

// ---- handlers ---------------------------------------------------------------------
const commands = FLOW.commands || [];
if (commands.length) {
  bot.setCommands(commands.map((c) => ({
    command: c.command,
    description: c.description || c.command,
  }))).catch((e) => console.warn('setMyCommands:', e.message));
}
for (const c of commands) {
  if (c.command && c.goto) {
    bot.command(c.command, (ctx) => {
      ctx.session.wait = null;
      ctx.session.live = null;
      return show(ctx, c.goto);
    });
  }
}
if (!commands.some((c) => c.command === 'start')) {
  bot.start((ctx) => {
    ctx.session.wait = null;
    ctx.session.live = null;
    return show(ctx, FLOW.start);
  });
}

// Owner's dashboard right in Telegram.
if (!commands.some((c) => c.command === 'admin')) {
  bot.command('admin', async (ctx, next) => {
    if (!ctx.isOwner) return next();
    const audience = bot.audience();
    const list = orders();
    const today = new Date().toDateString();
    const counted = list.filter((o) => o.status !== 'canceled' && o.status !== 'awaiting_payment');
    const lines = [
      '📊 ' + (bot.me ? bot.me.first_name : 'Бот'),
      '',
      '👥 Пользователей: ' + audience.users + ' (активны за 7 дней: ' + audience.active + ')',
      '🗂 Заявок: ' + db.get('leads', []).length,
    ];
    if (list.length) {
      lines.push('🛒 Заказов: ' + list.length + ', сегодня: '
        + list.filter((o) => new Date(o.date).toDateString() === today).length);
      lines.push('💰 Выручка: ' + money(counted.reduce((s, o) => s + (Number(o.total) || 0), 0)));
      lines.push('', 'Последние заказы:');
      for (const o of list.slice(-5).reverse()) {
        lines.push('№' + o.id + ' · ' + money(o.total) + ' · ' + (STATUSES[o.status] || { title: o.status }).title + ' · ' + (o.name || o.username || o.user_id));
      }
    }
    return ctx.reply(lines.join('\n'));
  });
}

bot.action(/^fb:([^:]+):(\d+)$/, async (ctx) => {
  await ctx.answer();
  const block = BLOCKS.get(ctx.match[1]);
  const button = block && (block.buttons || [])[Number(ctx.match[2])];
  if (button) await choose(ctx, block, button);
});

bot.action(/^flow:(.*)$/, async (ctx) => {
  await ctx.answer();
  ctx.session.live = null;
  if (ctx.match[1]) await show(ctx, ctx.match[1]);
});

bot.on('message', async (ctx) => {
  if (await relayFromOperator(ctx)) return;
  if (ctx.session.live) return relayToOperator(ctx);
  const waiting = BLOCKS.get(ctx.session.wait);
  if (waiting && waiting.input && waiting.input.enabled) {
    const input = waiting.input;
    const value = readInput(ctx, input);
    if (value === undefined) {
      const text = typeof ctx.text === 'string' ? ctx.text.trim().toLowerCase() : '';
      const button = text && (waiting.buttons || []).find((b) => b && !b.url && String(b.text).trim().toLowerCase() === text);
      if (button) return choose(ctx, waiting, button);
      return ctx.reply(input.error || INPUT_ERRORS[input.kind || 'text'] || INPUT_ERRORS.text);
    }
    vars(ctx)[input.variable || 'answer'] = value;
    ctx.session.wait = null;
    if ((input.kind === 'phone' || input.kind === 'location') && FLOW.buttonsStyle !== 'keyboard') {
      // Hide the "send phone" keyboard: with buttons the next message
      // can't do it itself.
      const next = BLOCKS.get(input.next);
      if (next && ((next.buttons || []).some((b) => b && b.text) || (next.type && next.type !== 'message'))) {
        await ctx.reply('✅ Принято', { removeKeyboard: true });
      } else {
        ctx._removeKeyboard = true;
      }
    }
    if (input.next) {
      return show(ctx, input.next);
    }
    return ctx.reply('Спасибо!', FLOW.buttonsStyle === 'keyboard' || input.kind === 'phone' || input.kind === 'location'
      ? { removeKeyboard: true }
      : {});
  }
  const text = typeof ctx.text === 'string' ? ctx.text.trim().toLowerCase() : '';
  if (text) {
    for (const block of BLOCKS.values()) {
      for (const button of block.buttons || []) {
        if (!button.url && (button.goto || block.saveChoice)
          && fill(button.text, ctx).trim().toLowerCase() === text) {
          return choose(ctx, block, button);
        }
      }
    }
    for (const keyword of FLOW.keywords || []) {
      const words = String(keyword.words || '').split(',').map((w) => w.trim().toLowerCase()).filter(Boolean);
      if (keyword.goto && words.some((w) => text.includes(w))) {
        return show(ctx, keyword.goto);
      }
    }
  }
  const fallback = FLOW.fallback || { mode: 'start' };
  if (fallback.mode === 'ai' && text) {
    await ctx.typing();
    const answer = await ai.chat(ctx, {
      system: fallback.prompt || 'Ты вежливый помощник в Telegram-боте. Отвечай кратко.',
      memory: 10,
    });
    return ctx.reply(answer.slice(0, 4000));
  }
  if (fallback.mode === 'text' && fallback.text) {
    return ctx.reply(fill(fallback.text, ctx));
  }
  if (fallback.mode === 'block' && fallback.goto) {
    return show(ctx, fallback.goto);
  }
  return show(ctx, FLOW.start);
});
