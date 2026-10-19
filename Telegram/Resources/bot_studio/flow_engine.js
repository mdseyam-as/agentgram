// ---- Agentgram visual bot engine -------------------------------------
// Runs the FLOW scheme built in the Bot Studio visual constructor.

const BLOCKS = new Map((FLOW.blocks || []).map((b) => [b.id, b]));
const INPUT_ERRORS = {
  phone: 'Пожалуйста, отправьте номер телефона кнопкой ниже или цифрами.',
  number: 'Пожалуйста, введите число.',
  email: 'Пожалуйста, введите корректный e-mail.',
  text: 'Пожалуйста, ответьте текстом.',
};

function vars(ctx) {
  const session = ctx.session;
  session.vars = session.vars || {};
  return session.vars;
}

function fill(text, ctx) {
  const from = ctx.from || {};
  const values = Object.assign({
    first_name: from.first_name || '',
    last_name: from.last_name || '',
    name: [from.first_name, from.last_name].filter(Boolean).join(' '),
    username: from.username ? '@' + from.username : '',
    user_id: from.id,
    bot: '@' + (bot.username || ''),
    last_message: typeof ctx.text === 'string' ? ctx.text : '',
    date: new Date().toLocaleDateString('ru-RU'),
    time: new Date().toLocaleTimeString('ru-RU', { hour: '2-digit', minute: '2-digit' }),
  }, vars(ctx));
  return String(text || '').replace(/\{([^{}\s]+)\}/g, (match, key) =>
    (values[key] !== undefined && values[key] !== null) ? String(values[key]) : match);
}

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

function markup(block, ctx) {
  const buttons = (block.buttons || []).filter((b) => b && b.text);
  const input = block.input && block.input.enabled ? block.input : null;
  if (input && input.kind === 'phone') {
    return { keyboard: [[{ text: input.buttonText || '📱 Отправить номер', contact: true }]] };
  }
  if (!buttons.length) {
    return (FLOW.buttonsStyle === 'keyboard' || ctx._removeKeyboard) ? { removeKeyboard: true } : {};
  }
  if (FLOW.buttonsStyle === 'keyboard') {
    return { keyboard: buttonRows(buttons.filter((b) => !b.url), (b) => String(b.text)) };
  }
  return {
    buttons: buttonRows(buttons, (b) => (b.url
      ? { text: String(b.text), url: String(b.url) }
      : { text: String(b.text), data: 'flow:' + (b.goto || '') })),
  };
}

function summary(ctx, block) {
  const from = ctx.from || {};
  const who = from.username ? '@' + from.username : [from.first_name, from.last_name].filter(Boolean).join(' ');
  const lines = ['📩 ' + (block.title || 'Новая заявка') + ' — ' + (who || from.id)];
  const values = vars(ctx);
  for (const key of Object.keys(values)) lines.push(key + ': ' + values[key]);
  return lines.join('\n');
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

function compare(condition, ctx) {
  const raw = vars(ctx)[condition.variable];
  const value = raw === undefined || raw === null ? '' : String(raw);
  const expected = fill(condition.value, ctx);
  const number = (v) => Number(String(v).replace(',', '.').replace(/\s/g, ''));
  switch (condition.op || 'eq') {
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

async function runAction(ctx, action, block) {
  const values = vars(ctx);
  switch (action.kind) {
  case 'set':
    if (action.variable) values[action.variable] = fill(action.value, ctx);
    return;
  case 'increment': {
    if (!action.variable) return;
    const by = Number(fill(action.value === undefined ? '1' : action.value, ctx)) || 0;
    values[action.variable] = (Number(values[action.variable]) || 0) + by;
    return;
  }
  case 'clear':
    ctx.session.vars = {};
    return;
  case 'notify':
    await bot.sendToOwner(action.text ? fill(action.text, ctx) : summary(ctx, block)).catch((e) =>
      console.warn('Не удалось написать владельцу (откройте бота и нажмите /start):', e.message));
    return;
  case 'send':
    if (!action.chat) return;
    await bot.send(String(action.chat).trim(), fill(action.text, ctx) || summary(ctx, block)).catch((e) =>
      console.warn('Не удалось отправить в ' + action.chat + ' (бот должен быть участником/админом):', e.message));
    return;
  case 'webhook': {
    if (!action.url) return;
    const from = ctx.from || {};
    const payload = {
      bot: bot.username,
      step: block.title || block.id,
      user_id: from.id,
      username: from.username || '',
      name: [from.first_name, from.last_name].filter(Boolean).join(' '),
      date: new Date().toISOString(),
      answers: Object.assign({}, values),
    };
    const result = await http.post(fill(action.url, ctx), payload).catch((e) => ({ ok: false, status: 0, text: e.message }));
    if (!result.ok) console.warn('Вебхук ответил ' + result.status + ': ' + String(result.text).slice(0, 200));
    return;
  }
  case 'save_lead':
    saveLead(ctx, block);
    return;
  }
}

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
      if (condition.goto && compare(condition, ctx)) {
        return show(ctx, condition.goto, depth + 1);
      }
    }
    if (block.else) return show(ctx, block.else, depth + 1);
    return;
  }
  if (type === 'action') {
    for (const action of block.actions || []) {
      await runAction(ctx, action, block);
    }
    if (block.next) return show(ctx, block.next, depth + 1);
    return;
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
    await bot.sendToOwner(summary(ctx, block)).catch((e) =>
      console.warn('Не удалось отправить заявку владельцу (откройте бота и нажмите /start):', e.message));
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
      return show(ctx, c.goto);
    });
  }
}
if (!commands.some((c) => c.command === 'start')) {
  bot.start((ctx) => {
    ctx.session.wait = null;
    return show(ctx, FLOW.start);
  });
}

bot.action(/^flow:(.*)$/, async (ctx) => {
  await ctx.answer();
  if (ctx.match[1]) await show(ctx, ctx.match[1]);
});

bot.on('message', async (ctx) => {
  const waiting = BLOCKS.get(ctx.session.wait);
  if (waiting && waiting.input && waiting.input.enabled) {
    const input = waiting.input;
    const value = readInput(ctx, input);
    if (value === undefined) {
      return ctx.reply(input.error || INPUT_ERRORS[input.kind || 'text'] || INPUT_ERRORS.text);
    }
    vars(ctx)[input.variable || 'answer'] = value;
    ctx.session.wait = null;
    if (input.kind === 'phone' && FLOW.buttonsStyle !== 'keyboard') {
      // Hide the "send phone" keyboard: with buttons the next message
      // can't do it itself.
      const next = BLOCKS.get(input.next);
      if (next && (next.buttons || []).some((b) => b && b.text)) {
        await ctx.reply('✅ Принято', { removeKeyboard: true });
      } else {
        ctx._removeKeyboard = true;
      }
    }
    if (input.next) {
      return show(ctx, input.next);
    }
    return ctx.reply('Спасибо!', FLOW.buttonsStyle === 'keyboard' || input.kind === 'phone'
      ? { removeKeyboard: true }
      : {});
  }
  const text = typeof ctx.text === 'string' ? ctx.text.trim().toLowerCase() : '';
  if (text) {
    for (const block of BLOCKS.values()) {
      for (const button of block.buttons || []) {
        if (button.goto && !button.url && String(button.text).trim().toLowerCase() === text) {
          return show(ctx, button.goto);
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
