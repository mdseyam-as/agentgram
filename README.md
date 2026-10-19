# Agentgram — Telegram Desktop with an AI agent and Bot Studio

**English** · [Русский](README.ru.md)

Agentgram is an **unofficial** fork of [Telegram Desktop](https://github.com/telegramdesktop/tdesktop)
(native C++/Qt client, GPLv3 with OpenSSL exception). You sign in with your
real Telegram account and keep every feature of Telegram Desktop, plus:

* an **AI agent** that reads, searches, summarizes and acts in your chats;
* **Bot Studio** — build Telegram bots visually or with JavaScript and run them
  right inside the app, with no servers and no deployment.

Agentgram is not affiliated with Telegram FZ-LLC.

## AI agent

* **Side panel** — the ✦ button in any chat header or *Menu → AI agent*. It docks
  to the right of the main window or pops out into its own window.
* **Tools**: list and find chats, read history, global and per-chat search,
  unread digest, group members, folders, send / forward / edit / delete / pin
  messages, mark as read, create groups, add members, put drafts into the input
  field, open chats — and build bots in Bot Studio.
* **Safety**: every action that changes something is shown as a card with
  *Allow / Decline* until you choose "allow everything".
* **Providers**
  * ChatGPT sign-in (OAuth, uses your ChatGPT Plus/Pro subscription)
  * OpenRouter (OAuth PKCE or API key)
  * OpenAI, Anthropic Claude, Google Gemini, DeepSeek, xAI, Mistral, Groq (API keys)
  * Ollama / LM Studio / any OpenAI-compatible server
* Streaming answers with Markdown; model lists are loaded from the provider
  automatically, quick model switch by clicking the panel title.
* Dark / light theme switch right in the panel.

## Bot Studio *(Linux build for now)*

*Menu → Bot Studio* opens a separate window where you create and run your own
Telegram bots. A bot runs **inside Agentgram** while the app is open: it
receives messages through the Telegram Bot API, so there is nothing to host or
deploy. Enabled bots start together with the app.

* **Node editor in the style of n8n / Make**: an endless canvas with zoom
  (Ctrl + wheel) and panning, drag a connection from the dot on the right of a
  block to another block, "+" on a free output or a drop on empty space opens
  the node picker with search, click a connection to delete it, the settings
  panel slides in from the right, triggers (commands, keywords, "anything
  else") are nodes too, *Arrange* lays the scheme out, Ctrl+Z / Ctrl+Y,
  Delete, Ctrl+D.
* **Visual constructor** for people who don't code. A bot is a scheme of message
  cards on a canvas:
  * a message with text (`{first_name}` and other variables), an optional
    picture and buttons;
  * each button leads to another message or opens a link — arrows on the
    scheme show where;
  * a message can wait for the user's answer (text, phone with a
    "send phone" button, number, e-mail) and save it;
  * "send me the answers" turns a branch into a lead form — collected answers
    arrive to you in Telegram;
  * commands (`/start`, `/help`…), keywords and what to do with any other
    message: show a block, reply with text or **answer with AI**.
* **Block types**: *message* (text, photo/video/file/audio, buttons, a question
  with answer check), *condition* (branch by answers: equals, contains, greater /
  less, filled), *AI answer* (the text is written by AI from your prompt with
  variables), *actions* (set a value, add points, notify you, post to a chat or
  channel, send data to a webhook — Make, Zapier, Google Sheets — save a lead).
  Any block can wait N seconds with "typing…" before it.
* **More blocks**: *random choice* (prize wheels, A/B tests), *operator chat*
  (the customer writes to the bot, you answer from Telegram with "reply"),
  conditions on a **channel subscription**, user **tags** and **working
  hours**; actions to calculate a formula, tag users, **remind later** (drip
  lessons, abandoned carts), call any API and save a field of its answer.
  Inputs also take a location or a photo; a pressed button can be saved as an
  answer.
* **Shop**: a product list with categories, prices, photos and delivery
  settings (*🛍 Products*), blocks *catalog* (cards with paging and
  "add to cart"), *cart* (± quantity, delivery, minimum order) and *order &
  payment* — pay on delivery, with **Telegram Stars** or by card through any
  @BotFather payment provider. Orders come to you in Telegram with status
  buttons (accepted, on the way, done, canceled), the buyer gets a message on
  every change; `/admin` in the bot shows stats.
* **Audience**: users, active in 7 days, blocked; tables of leads and
  **orders** with revenue, status changes and CSV export (opens in Excel);
  **broadcasts** to every user or only to a tag.
* **Templates**: online shop, food delivery, paid access for Stars, menu &
  FAQ, lead form, booking, support with an operator, bonus for a channel
  subscription, quiz with points, prize wheel, drip course, AI assistant,
  blank.
* **Build with AI**: describe the bot in plain words — the agent registers it in
  @BotFather (with your confirmation), builds the scheme, starts it and checks
  the log. Then describe any change in the "✦ Make" field.
* **Code mode** for developers: one JavaScript file on an embedded QuickJS
  engine with a small framework — `bot.command / hears / action / on`, inline
  and reply keyboards, per-chat `ctx.session`, persistent `db`, `ai.ask` /
  `ai.chat` through the provider configured in Agentgram, `http` / `fetch`,
  timers. Syntax highlighting, syntax check on save, an infinite loop is
  interrupted after 5 seconds.
* **Logs** in real time (incoming messages, `console.log`, errors with line
  numbers) and a **Data** tab with everything the bot stored.

## Other

* Your own `api_id` can be set at runtime (start screen or agent settings,
  or `AGENTGRAM_API_ID` / `AGENTGRAM_API_HASH`).
* Separate data directory; auto-update is disabled (it would install the
  official Telegram).
* AI keys, tokens and bots are kept in the account's encrypted local storage.

## Builds

GitHub Actions in this repository build:

* **Linux x64** (portable, glibc 2.28+) — `.github/workflows/agentgram-linux.yml`,
  on every push.
* **Windows x64** — `.github/workflows/agentgram.yml`, started manually. Bot
  Studio is not included in the Windows build yet.

There is one branch, `agentgram`, for both platforms: platform differences are
handled by the build (`if (LINUX)` in CMake).

Set repository secrets `TDESKTOP_API_ID` / `TDESKTOP_API_HASH` to bake your own
credentials into the binaries; otherwise the limited test credentials are used.

Local builds follow the upstream instructions in [docs](docs/); for a quick
local Linux build pass `-D AGENTGRAM_FAST_BUILD=ON` (lighter optimization of
the app code, much less memory). A one-click Windows script is in
`Telegram/build/agentgram/build_windows.bat`.

Code: AI agent — `Telegram/SourceFiles/ai_agent/`, Bot Studio —
`Telegram/SourceFiles/bot_studio/` and `Telegram/Resources/bot_studio/`.

## License

GPLv3 with OpenSSL exception, see [LICENSE](LICENSE) and [LEGAL](LEGAL).
QuickJS (`Telegram/ThirdParty/quickjs`) is MIT licensed.
