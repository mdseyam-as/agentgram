/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "bot_studio/bot_studio_manager.h"

namespace BotStudio {

const std::vector<Template> &Templates() {
	static const auto result = std::vector<Template>{
		{
			.id = u"starter"_q,
			.title = u"Простой бот"_q,
			.description = u"Приветствие, меню с кнопками и ответ на "
				"любое сообщение."_q,
			.code = uR"JS(// Бот работает прямо в Agentgram — ничего деплоить не нужно.
// Справка по API: кнопка «Справка» или спросите ИИ-агента.

await bot.setCommands({ start: 'Начать', help: 'Помощь' });

bot.start(async (ctx) => {
  await ctx.reply(`Привет, ${ctx.from.first_name}! 👋\nЯ бот @${bot.username}.`, {
    buttons: [
      [{ text: 'ℹ️ О боте', data: 'about' }, { text: '🎲 Кубик', data: 'dice' }],
    ],
  });
});

bot.help((ctx) => ctx.reply('Напишите мне что-нибудь — я повторю. /start — меню.'));

bot.action('about', (ctx) => ctx.edit('Этот бот собран в Agentgram Bot Studio.'));
bot.action('dice', (ctx) => ctx.replyDice());

bot.on('message', (ctx) => ctx.reply('Вы написали: ' + (ctx.text ?? 'не текст')));
)JS"_q,
		},
		{
			.id = u"ai"_q,
			.title = u"ИИ-ассистент"_q,
			.description = u"Отвечает на вопросы через ИИ, подключённый "
				"в Agentgram, и помнит контекст беседы."_q,
			.code = uR"JS(// ИИ-ассистент: использует провайдера ИИ, настроенного в Agentgram.
const SYSTEM = `Ты дружелюбный помощник в Telegram. Отвечай кратко и по делу,
на языке пользователя. Не используй Markdown-разметку.`;

await bot.setCommands({ start: 'Начать', reset: 'Забыть беседу' });

bot.start((ctx) => {
  ai.reset(ctx);
  return ctx.reply('Привет! Задайте любой вопрос — я отвечу. /reset — начать заново.');
});

bot.command('reset', (ctx) => {
  ai.reset(ctx);
  return ctx.reply('Начинаем с чистого листа.');
});

bot.on('text', async (ctx) => {
  await ctx.typing();
  const answer = await ai.chat(ctx, { system: SYSTEM, memory: 15 });
  await ctx.reply(answer.slice(0, 4000));
});

bot.on('message', (ctx) => ctx.reply('Пока я понимаю только текст.'));
)JS"_q,
		},
		{
			.id = u"faq"_q,
			.title = u"Меню и FAQ"_q,
			.description = u"Каталог разделов на inline-кнопках с "
				"переходами назад."_q,
			.code = uR"JS(// Меню на кнопках: разделы и ответы на частые вопросы.
const SECTIONS = {
  hours: { title: '🕒 Часы работы', text: 'Пн–Пт 9:00–19:00, Сб 10:00–16:00.' },
  price: { title: '💳 Цены', text: 'Консультация — 1000 ₽, пакет из 5 — 4000 ₽.' },
  address: { title: '📍 Адрес', text: 'Москва, ул. Примерная, 1.' },
  contact: { title: '📞 Связаться', text: 'Напишите сюда свой вопрос — мы ответим.' },
};

const menu = () => Object.entries(SECTIONS).map(([id, s]) => [{ text: s.title, data: 'faq:' + id }]);

bot.start((ctx) => ctx.reply('Выберите раздел:', { buttons: menu() }));

bot.action(/^faq:(\w+)$/, (ctx) => {
  const section = SECTIONS[ctx.match[1]];
  return ctx.edit(`${section.title}\n\n${section.text}`, {
    buttons: [[{ text: '⬅️ Назад', data: 'menu' }]],
  });
});

bot.action('menu', (ctx) => ctx.edit('Выберите раздел:', { buttons: menu() }));

// Вопросы пользователей пересылаются владельцу бота.
bot.on('message', async (ctx) => {
  if (ctx.isOwner) return ctx.reply('Это вы, владелец бота 🙂');
  await ctx.forwardTo(bot.ownerId).catch(() => {});
  await ctx.reply('Спасибо! Мы получили сообщение и скоро ответим.');
});
)JS"_q,
		},
		{
			.id = u"form"_q,
			.title = u"Сбор заявок"_q,
			.description = u"Пошаговая анкета: имя, телефон, комментарий. "
				"Заявки приходят вам в Telegram."_q,
			.code = uR"JS(// Пошаговая анкета. Готовые заявки приходят владельцу бота
// (сначала откройте бота и нажмите /start, чтобы он мог вам писать).

await bot.setCommands({ start: 'Оставить заявку', cancel: 'Отменить' });

bot.start((ctx) => {
  ctx.session = { step: 'name' };
  return ctx.reply('Оставьте заявку. Как вас зовут?', { removeKeyboard: true });
});

bot.command('cancel', (ctx) => {
  ctx.session = null;
  return ctx.reply('Заявка отменена. /start — начать заново.', { removeKeyboard: true });
});

bot.on('contact', (ctx) => handle(ctx, ctx.message.contact.phone_number));
bot.on('text', (ctx) => handle(ctx, ctx.text));

async function handle(ctx, value) {
  const s = ctx.session;
  if (!s.step) return ctx.reply('Нажмите /start, чтобы оставить заявку.');
  if (s.step === 'name') {
    s.name = value;
    s.step = 'phone';
    return ctx.reply('Ваш телефон?', {
      keyboard: [[{ text: '📱 Отправить номер', contact: true }]],
    });
  }
  if (s.step === 'phone') {
    s.phone = value;
    s.step = 'comment';
    return ctx.reply('Коротко опишите задачу:', { removeKeyboard: true });
  }
  if (s.step === 'comment') {
    const id = db.increment('orders:count');
    const order = { id, name: s.name, phone: s.phone, comment: value,
      user: ctx.from.id, date: new Date().toISOString() };
    db.set('order:' + id, order);
    ctx.session = null;
    await ctx.reply(`Заявка №${id} принята! Мы свяжемся с вами.`);
    await bot.sendToOwner(
      `🆕 Заявка №${id}\nИмя: ${order.name}\nТелефон: ${order.phone}\n` +
      `Задача: ${order.comment}\nОт: ${ctx.from.username ? '@' + ctx.from.username : ctx.from.id}`
    ).catch((e) => console.warn('Владелец ещё не нажал /start у бота:', e.message));
  }
}
)JS"_q,
		},
		{
			.id = u"empty"_q,
			.title = u"Пустой"_q,
			.description = u"Минимальный каркас, чтобы начать с нуля."_q,
			.code = uR"JS(bot.start((ctx) => ctx.reply('Привет!'));

bot.on('message', (ctx) => {
  console.log('Новое сообщение', ctx.text);
});
)JS"_q,
		},
	};
	return result;
}

} // namespace BotStudio
