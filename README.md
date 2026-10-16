# Agentgram — Telegram Desktop with a built-in AI agent

Agentgram is an **unofficial** fork of [Telegram Desktop](https://github.com/telegramdesktop/tdesktop)
(native C++/Qt client, GPLv3 with OpenSSL exception). You sign in with your
real Telegram account and keep every feature of Telegram Desktop, plus an AI
agent panel that can read, search, summarize and act in your chats and groups.

Agentgram is not affiliated with Telegram FZ-LLC.

## Features

* **AI agent side panel** — open it from the ✦ button in any chat header or
  *Menu → ИИ-агент*. It docks to the right of the main window or pops out into a
  separate window.
* **Agent tools**: list and find chats, read history, global and per-chat
  search, unread digest, group members, folders, send / forward / edit / delete /
  pin messages, mark as read, create groups, add members, put drafts into the
  input field, open chats.
* **Safety**: every action that changes something is shown as a card with
  *Allow / Decline* until you choose "allow everything".
* **Providers**
  * ChatGPT sign-in (OAuth, uses your ChatGPT Plus/Pro subscription, Responses API)
  * OpenRouter (OAuth PKCE or API key)
  * OpenAI, Anthropic Claude, Google Gemini, DeepSeek, xAI, Mistral, Groq (API keys)
  * Ollama / LM Studio / any OpenAI-compatible server
* Streaming answers with Markdown, model picker with live model lists.
* Dark / light theme switch right in the panel (follows Telegram themes).
* Your own `api_id` can be set at runtime (start screen or agent settings,
  or `AGENTGRAM_API_ID` / `AGENTGRAM_API_HASH`).
* Separate data directory, auto-update disabled (it would install official Telegram).

AI keys and tokens are stored in the account's encrypted local storage.

## Builds

GitHub Actions in this repository build:

* **Windows x64** — `.github/workflows/agentgram.yml`
* **Linux x64** (portable, glibc 2.28+) — `.github/workflows/agentgram-linux.yml`

Set repository secrets `TDESKTOP_API_ID` / `TDESKTOP_API_HASH` to bake your own
credentials into the binaries; otherwise the limited test credentials are used.

Local builds follow the upstream instructions in [docs](docs/). The agent code
lives in `Telegram/SourceFiles/ai_agent/`.

## License

GPLv3 with OpenSSL exception, see [LICENSE](LICENSE) and [LEGAL](LEGAL).
