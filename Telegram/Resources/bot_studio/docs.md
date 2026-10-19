# Agentgram Bot Studio — reference

A bot is either a **visual scheme** (flow, edited in the visual constructor —
best for menus, FAQ, forms, bookings, AI chats) or a **JavaScript file** (any
logic). Both run locally inside Agentgram.

## Visual scheme (flow)

Set it with `bot_studio_set_flow` / `bot_studio_create {flow}`. JSON:

```json
{
  "start": "welcome",
  "buttonsStyle": "inline",
  "blocks": [
    { "id": "welcome", "title": "Приветствие", "x": 40, "y": 40,
      "text": "Здравствуйте, {first_name}! Выберите:",
      "photo": "",
      "buttons": [ { "text": "💳 Цены", "goto": "prices" },
                   { "text": "🌐 Сайт", "url": "https://example.com" } ] },
    { "id": "prices", "title": "Цены", "x": 360, "y": 40,
      "text": "Стрижка — 1500 ₽", "next": "ask_phone" },
    { "id": "ask_phone", "title": "Телефон", "x": 680, "y": 40,
      "text": "Оставьте телефон:",
      "input": { "enabled": true, "variable": "телефон", "kind": "phone", "next": "done" } },
    { "id": "done", "title": "Готово", "x": 680, "y": 220, "notifyOwner": true,
      "text": "Спасибо! Перезвоним на {телефон}.",
      "buttons": [ { "text": "⬅️ В меню", "goto": "welcome" } ] }
  ],
  "commands": [ { "command": "start", "description": "Меню", "goto": "welcome" } ],
  "keywords": [ { "words": "цена, стоимость", "goto": "prices" } ],
  "fallback": { "mode": "ai", "prompt": "Ты вежливый администратор салона." }
}
```

- Block = one bot message: `text` (with `{variables}`), optional `photo`
  (https URL or file_id), `buttons` (each goes to a block `goto` or opens `url`;
  2 short buttons per row), `next` (show another block right after, when the
  block has no buttons/input), `input` (wait for the user's answer, save it to
  `variable`; `kind`: text, phone (shows a "send phone" button), number, email;
  then go to `next`), `notifyOwner` (send the owner all collected answers when
  the block is shown — use it on the final "thanks" block of a form).
- Variables: `{first_name}`, `{last_name}`, `{name}`, `{username}`,
  `{user_id}`, `{bot}` and every saved input variable (any language).
- `x`, `y`: card position on the canvas; lay blocks out left-to-right by
  step (≈320 px apart horizontally, ≈190 px vertically).
- `commands`: slash commands → block (`start` defaults to `start` block).
- `keywords`: comma-separated words; a message containing one opens the block.
- `fallback` for any other message: `start` (show the start block), `block`
  (`goto`), `text` (`text`), `ai` (answer with AI, `prompt` = its role).
- `buttonsStyle`: `inline` (buttons under the message) or `keyboard`
  (buttons replace the keyboard).
- Block `type` (default `message`):
  - `message` — as above; instead of `photo` you may use
    `media: {kind: photo|video|document|audio, url}`; `delay` (seconds, ≤30)
    waits with "typing…" before the block (works for every type).
  - `condition` — invisible branching: `conditions: [{variable, op, value,
    goto}]` checked in order, `else: blockId`. `op`: eq, neq, contains, gt, lt
    (numbers), filled, empty. Example: quiz score `{баллы} gt 3`.
  - `ai` — the bot answers with text written by AI: `prompt` (with
    {variables}, `{last_message}` = the user's last message), optional `system`
    (role), `variable` (save the answer), `send` (default true), `buttons`,
    `next`.
  - `action` — `actions: [...]`, then `next`. Kinds: `{kind:"set", variable,
    value}`, `{kind:"increment", variable, value}` (score/counter),
    `{kind:"clear"}` (forget answers), `{kind:"notify", text}` (message to the
    owner; empty text = all answers), `{kind:"send", chat:"@channel", text}`
    (bot must be a member/admin), `{kind:"webhook", url}` (POST JSON with the
    user and answers — Make/Zapier/Google Apps Script), `{kind:"save_lead"}`
    (adds the answers to the Leads table in the Audience tab).
  - more action kinds: `{kind:"calc", variable, value:"{кол} * 250 + 300"}`
    (arithmetic), `{kind:"tag"|"untag", value:"vip"}` (user tags: used by
    conditions and broadcasts), `{kind:"remind", value:"1", unit:"min"|"hour"|
    "day", goto}` (show a block to the user later — drip lessons, abandoned
    cart), `{kind:"cancel_reminders"}`, `{kind:"request", url, method:"GET"|
    "POST", variable, path:"data.price"}` (call an API, save a JSON field;
    `{var}` in the URL is URL-encoded), `{kind:"add_to_cart", product, value}`,
    `{kind:"clear_cart"}`.
  - condition `op` also: `member` (value `@channel`: the user is subscribed;
    the bot must be a channel admin), `tag` (value: tag name), `hours` (value
    like `пн-пт 9:00-18:00`, `10-22`, `сб,вс` — local time). These ignore
    `variable`.
  - `random` — `branches: [{goto, weight}]`: picks a branch at random by
    weight (prize wheels, A/B tests).
  - `operator` — live chat with the owner: `text`, `chat` (empty = owner, or
    a support group @name/id), `endText`, `next` (after the chat ends). User
    messages are relayed to the owner, the owner answers with Telegram
    "reply" and the bot copies the answer to the user. /start ends the chat.
  - Shop: flow-level `products: [{id, title, price, category, description,
    photo, url}]` and `shop: {currency: "RUB"|"USD"|"EUR"|"KZT"|"UAH"|"BYN"|
    "UZS"|"XTR" (Telegram Stars), delivery, freeFrom, minOrder, ordersChat,
    starsRate (price of 1 ⭐ in the shop currency), providerToken}`.
    - `catalog` — product cards with photo, price, ◀️ ▶️ paging and
      🛒 add/➖/➕; with several categories and no `category` filter it first
      shows category buttons. Fields: `text` (header), `category`, `cart`
      (cart block id), `buttons` (extra, e.g. back to menu).
    - `cart` — items with ➖/➕, total with delivery; `text`, `emptyText`,
      `checkout` (block id for "Оформить заказ" — usually name/phone/address
      inputs, then the order block), `catalog`, `buttons`.
    - `order` — creates an order from the cart (`{order_id}`, `{order_total}`,
      `{order_items}`), sends `text`, notifies the owner (or `shop.ordersChat`)
      with status buttons (accepted/delivering/done/canceled — the buyer gets
      a message on each change). `payment`: `none` (pay on delivery, then
      `next`), `stars` (Telegram Stars invoice, then `paid` block after
      payment), `card` (needs `shop.providerToken` from @BotFather Payments;
      then `paid`). `empty` = block for an empty cart.
    - Variables `{cart}`, `{cart_total}`, `{cart_count}`, `{my_orders}` (the
      user's last orders with statuses). `/admin` in the bot shows the owner
      stats and recent orders.
- Input `kind` also: `location` (geo button or a typed address), `photo`
  (saves file_id; sent to the owner with the lead).
- Block `saveChoice: "variable"` saves the text (or button `value`) of the
  pressed button — use it for choices like size, service, time.
- `notifyOwner` on a message also saves the answers as a lead.
- Variables also: `{last_message}`, `{date}`, `{time}`.
- Ids: short latin words. Keep texts friendly; emoji are welcome.

## Audience

Every private chat that writes to a bot is counted (Audience tab:
users, active in 7 days, blocked, tags, leads and orders with CSV export,
revenue, broadcast — to everyone or to a tag). `bot_studio_audience` reads it,
`bot_studio_broadcast` sends a message to all users or a tag (only on the
user's request), `bot_studio_order_status` changes an order status.

Templates in the window (good starting points to copy structure from): shop,
food delivery, paid access for Stars, menu, leads, booking, support with an
operator, bonus for a channel subscription, quiz, prize wheel, drip course,
AI assistant.

# JavaScript bots

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
