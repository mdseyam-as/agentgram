# Agentgram Bot Studio — bot script reference

A bot is one JavaScript file (modern JS: async/await, classes, regex, top-level
await). Agentgram runs it locally inside the app: it long-polls the Telegram
Bot API with the bot token and calls the handlers. No server, no deploy, no
npm/require/import. The bot works while Agentgram is running.

## Handlers (first matching handler wins, call `next()` to continue)

```js
bot.start(async (ctx) => {            // /start, ctx.payload = deep-link payload
  await ctx.reply('Hello, ' + ctx.from.first_name);
});
bot.command(['help', 'h'], (ctx) => ctx.reply('Commands: ...'));  // ctx.args, ctx.payload
bot.hears('Price', (ctx) => ctx.reply('100$'));        // exact text, case-insensitive
bot.hears(/^order (\d+)$/i, (ctx) => ctx.reply('Order ' + ctx.match[1]));
bot.action('like', (ctx) => ctx.answer('Thanks!'));    // inline button callback_data
bot.action(/^item:(\d+)$/, (ctx) => ctx.edit('Item ' + ctx.match[1]));
bot.on('photo', (ctx) => ctx.reply('Nice photo'));     // see types below
bot.on('message', (ctx) => ctx.reply('You said: ' + ctx.text));  // fallback, put last
bot.use(async (ctx, next) => { console.log(ctx.updateType); await next(); }); // middleware
bot.catch((error, where) => console.error(where, error));
```

`bot.on` types: `message`, `text`, `photo`, `video`, `document`, `audio`,
`voice`, `video_note`, `sticker`, `animation`, `media`, `location`, `contact`,
`poll`, `dice`, `new_chat_members`, `left_chat_member`, `web_app_data`,
`edited_message`, `channel_post`, `callback_query`, `inline_query`,
`my_chat_member`, `chat_member`, `chat_join_request`, `pre_checkout_query`,
`*` (any update). Handlers run in registration order.

## ctx

Fields: `ctx.update`, `ctx.updateType`, `ctx.message`, `ctx.chat`, `ctx.chatId`,
`ctx.from` (user), `ctx.text` (text or caption), `ctx.data` (callback data),
`ctx.match`, `ctx.command`, `ctx.args`, `ctx.payload`, `ctx.messageId`,
`ctx.callbackQuery`, `ctx.inlineQuery`, `ctx.isOwner` (sender is the Agentgram
user), `ctx.session` (per-chat object, saved automatically after the handler;
assign `ctx.session = null` to clear).

Methods (all return Promises of the Bot API result):
`reply(text, options)`, `replyHTML(text, options)`, `replyPhoto(photo, options)`,
`replyDocument`, `replyVideo`, `replyAudio`, `replySticker`,
`replyLocation(lat, lon)`, `replyPoll(question, ['a','b'], options)`,
`replyDice(emoji)`, `typing()`, `react('👍')`, `answer(text, {show_alert})`
(callback query; called automatically if you don't), `edit(text, options)`
(edits the message with the pressed button), `editButtons(buttons)`,
`deleteMessage(id?)`, `forwardTo(chatId)`, `copyTo(chatId)`.
Media accepts a file_id or an https URL.

`options` = any Bot API fields plus shortcuts:
- `buttons: [[{text:'Yes', data:'yes'}, {text:'Site', url:'https://...'}], ['Plain']]`
  → inline keyboard (a string button uses its text as callback data);
- `keyboard: [['A', 'B'], [{text:'Send phone', contact:true}]]` → reply keyboard;
- `removeKeyboard: true`, `html: true` (parse_mode HTML), `markdown: true`
  (MarkdownV2), `replyTo: messageId`, `silent: true`, `noPreview: true`, `caption`.

`Keyboard.inline(rows)`, `Keyboard.reply(rows, {once, persistent, placeholder})`,
`Keyboard.remove()`, `Keyboard.button(text, data)`, `Keyboard.url(text, url)`
build `reply_markup` objects manually.

## bot

`bot.me` (getMe result), `bot.username`, `bot.id`, `bot.ownerId` (Telegram id of
the Agentgram user — the bot owner; the owner must /start the bot first to
receive messages from it), `bot.send(chatId, text, options)`,
`bot.sendPhoto(chatId, photo, options)`, `bot.sendToOwner(text, options)`,
`bot.setCommands({start: 'Start', help: 'Help'})`,
`bot.api.anyMethod(params)` / `bot.call('method', params)` — any Bot API method.

## Storage — `db` (persistent, per bot, up to 8 MB, JSON values)

`db.get(key, fallback)`, `db.set(key, value)`, `db.delete(key)`, `db.has(key)`,
`db.keys(prefix)`, `db.all(prefix)`, `db.clear(prefix)`, `db.increment(key, by)`.
Example: `db.set('user:' + ctx.from.id, {name, phone})`, `db.keys('user:')`.

## AI — `ai` (uses the AI provider configured in Agentgram)

`await ai.ask('prompt')`, `await ai.ask({system, prompt, history: [{role:'user'|'assistant', text}]})`
→ string. `await ai.chat(ctx, {system, memory: 20})` — answers `ctx.text` with
per-chat memory of the last N exchanges; `ai.reset(ctx)` clears it.
Call `ctx.typing()` before slow AI answers.

## HTTP, timers, console

`await http.get(url, {headers})`, `await http.post(url, body, {headers})`,
`await http.request(url, {method, headers, body})` → `{status, ok, text, headers, json()}`.
`fetch(url, {method, headers, body})` also works (`res.json()`, `res.text()`).
`setTimeout`, `setInterval`, `clearTimeout`, `clearInterval` (timers are lost
when the bot restarts — store schedules in `db` and restore them at start).
`console.log/warn/error` write to the bot log in Bot Studio.

## Rules

- Keep handlers fast; await Promises; never block with long loops (a sync slice
  over 5 s is interrupted).
- Telegram limits: message text ≤ 4096 chars, callback_data ≤ 64 bytes.
- Code at the top level runs once at bot start (good place for
  `bot.setCommands(...)` and restoring timers).
- Register `bot.on('message')` fallbacks after specific handlers.
