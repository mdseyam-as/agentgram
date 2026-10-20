/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Window {
class Controller;
} // namespace Window

namespace AgentUi {

// Vanilla is the classic Telegram look, AgentUI is the frosted glass
// design of Agentgram. AgentUI is active exactly while one of its
// palettes is the applied theme, so choosing any other theme in the
// settings returns to Vanilla on its own.
enum class Design {
	Vanilla,
	AgentUI,
};

// Installs the glass hooks and follows theme changes.
void Start();

[[nodiscard]] bool Active();
[[nodiscard]] bool Dark();
[[nodiscard]] Design CurrentDesign();
[[nodiscard]] rpl::producer<Design> DesignValue();

[[nodiscard]] bool IsAgentThemePath(const QString &path);
[[nodiscard]] QString ThemePath(bool dark);

// Switches the design behind a circular reveal that grows from the
// global point `origin`.
void SwitchDesign(
	not_null<Window::Controller*> window,
	Design design,
	QPoint origin);

// Night mode toggles keep the user inside the chosen design: AgentUI
// switches between its dark and light palettes, Vanilla returns to the
// theme the user had before AgentUI. `savedPath` is the theme saved for
// the new mode. Used by Window::Theme::ChatBackground.
[[nodiscard]] std::optional<QString> NightModeThemeOverride(
	bool newNightMode,
	const QString &savedPath);

// The chat list paints the active and hovered rows as animated pills,
// the row painter skips those backgrounds while such a scope is alive.
class RowPillsScope final {
public:
	explicit RowPillsScope(bool enabled);
	~RowPillsScope();

private:
	bool _was = false;

};
[[nodiscard]] bool RowsUsePills();

} // namespace AgentUi
