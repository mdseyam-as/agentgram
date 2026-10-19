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

async function show(ctx, id, depth) {
  const block = BLOCKS.get(id);
  if (!block) {
    console.warn('Блок не найден:', id);
    return;
  }
  depth = depth || 0;
  const text = fill(block.text, ctx).trim();
  const extra = markup(block, ctx);
  if (block.photo) {
    await ctx.replyPhoto(block.photo, Object.assign({ caption: text || undefined }, extra));
  } else {
    await ctx.reply(text || '…', extra);
  }
  if (block.notifyOwner) {
    await bot.sendToOwner(summary(ctx, block)).catch((e) =>
      console.warn('Не удалось отправить заявку владельцу (откройте бота и нажмите /start):', e.message));
  }
  const input = block.input && block.input.enabled ? block.input : null;
  if (input) {
    ctx.session.wait = id;
  } else {
    ctx.session.wait = null;
    if (block.next && depth < 10) {
      await new Promise((resolve) => setTimeout(resolve, 600));
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
