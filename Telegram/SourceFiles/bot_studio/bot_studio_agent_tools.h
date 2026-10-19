/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ai_agent/ai_agent_tools.h"

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

namespace BotStudio {

// Tools that let the AI agent build, run and debug Bot Studio bots.
[[nodiscard]] bool IsAgentTool(const QString &name);
[[nodiscard]] std::vector<AiAgent::ToolDefinition> AgentToolDefinitions();
[[nodiscard]] AiAgent::ToolPresentation PresentAgentTool(
	not_null<Main::Session*> session,
	const AiAgent::ToolCall &call);
void ExecuteAgentTool(
	not_null<Main::Session*> session,
	const AiAgent::ToolCall &call,
	Window::SessionController *window,
	Fn<void(AiAgent::ToolResult)> done);
[[nodiscard]] QString AgentPromptSection();

} // namespace BotStudio
