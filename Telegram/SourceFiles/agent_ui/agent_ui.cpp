/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "agent_ui/agent_ui.h"

#include "agent_ui/agent_ui_glass.h"
#include "agent_ui/agent_ui_reveal.h"
#include "data/data_wall_paper.h"
#include "mainwidget.h"
#include "settings.h"
#include "storage/localstorage.h"
#include "window/themes/window_theme.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace AgentUi {
namespace {

constexpr auto kDarkTheme = ":/gui/agentui-dark.tdesktop-theme"_cs;
constexpr auto kLightTheme = ":/gui/agentui-light.tdesktop-theme"_cs;

// Freeform gradients: Telegram animates them on every sent message.
constexpr auto kDarkPaper = "0c141b~123039~1c1b33~0d2522"_cs;
constexpr auto kLightPaper = "d4e6ea~eef1f6~d9e1f1~e1ede6"_cs;

// What the user had before AgentUI, to come back to on Vanilla.
struct Previous {
	QString dayPath;
	QString nightPath;
	bool night = false;
	QByteArray paper;
};

bool RowPills = false;

[[nodiscard]] rpl::variable<Design> &DesignVariable() {
	static auto result = rpl::variable<Design>(Design::Vanilla);
	return result;
}

[[nodiscard]] QString StatePath() {
	return cWorkingDir() + u"tdata/agentgram_design.json"_q;
}

[[nodiscard]] Previous ReadPrevious() {
	auto file = QFile(StatePath());
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	const auto object = QJsonDocument::fromJson(file.readAll()).object();
	return {
		.dayPath = object.value(u"dayPath"_q).toString(),
		.nightPath = object.value(u"nightPath"_q).toString(),
		.night = object.value(u"night"_q).toBool(),
		.paper = QByteArray::fromBase64(
			object.value(u"paper"_q).toString().toLatin1()),
	};
}

void WritePrevious(const Previous &previous) {
	auto object = QJsonObject();
	object.insert(u"dayPath"_q, previous.dayPath);
	object.insert(u"nightPath"_q, previous.nightPath);
	object.insert(u"night"_q, previous.night);
	object.insert(
		u"paper"_q,
		QString::fromLatin1(previous.paper.toBase64()));
	auto file = QFile(StatePath());
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
	}
}

[[nodiscard]] std::optional<Data::WallPaper> AgentPaper(bool dark) {
	return Data::WallPaper::FromColorsSlug(
		(dark ? kDarkPaper : kLightPaper).utf16());
}

[[nodiscard]] bool IsAgentPaper(const Data::WallPaper &paper) {
	if (paper.document()) {
		return false;
	}
	const auto &colors = paper.backgroundColors();
	for (const auto dark : { true, false }) {
		if (const auto agent = AgentPaper(dark)) {
			if (agent->backgroundColors() == colors) {
				return true;
			}
		}
	}
	return false;
}

// The palettes carry only a one pixel placeholder background, AgentUI
// puts its own gradient instead. A wallpaper the user chose while in
// AgentUI is kept as it is.
void EnsureAgentPaper() {
	using namespace Window::Theme;
	if (!Active()) {
		return;
	}
	const auto wanted = AgentPaper(Dark());
	if (!wanted) {
		return;
	}
	const auto &paper = Background()->paper();
	const auto replace = Data::IsThemeWallPaper(paper)
		|| Data::IsDefaultWallPaper(paper)
		|| (IsAgentPaper(paper)
			&& (paper.backgroundColors() != wanted->backgroundColors()));
	if (replace) {
		Background()->set(*wanted);
	}
}

void RefreshDesign() {
	DesignVariable() = Active() ? Design::AgentUI : Design::Vanilla;
}

[[nodiscard]] QString VanillaPathFor(const Previous &previous, bool night) {
	using namespace Window::Theme;
	auto path = night ? previous.nightPath : previous.dayPath;
	if (IsAgentThemePath(path)) {
		path = QString();
	}
	if (!path.isEmpty() && !IsEmbeddedTheme(path) && !QFile::exists(path)) {
		path = QString();
	}
	if (path.isEmpty() && night) {
		path = NightThemePath();
	}
	return path;
}

// Applies the theme in the current night mode, or toggles the mode when
// the theme belongs to the other one, then calls `done`.
void ApplyTheme(
		not_null<Window::Controller*> window,
		const QString &path,
		bool night,
		Fn<void()> done) {
	using namespace Window::Theme;
	if (IsNightMode() == night) {
		ApplyDefaultWithPath(path);
		KeepApplied();
		done();
	} else {
		ToggleNightModeWithConfirmation(window, [=] {
			ToggleNightMode(path);
			KeepApplied();
			done();
		});
	}
}

[[nodiscard]] QString SavedPathForOtherMode() {
	using namespace Window::Theme;
	const auto night = IsNightMode();
	SetNightModeValue(!night);
	auto result = Local::readThemeAfterSwitch().object.pathAbsolute;
	SetNightModeValue(night);
	return result;
}

void SwitchToAgent(not_null<Window::Controller*> window) {
	using namespace Window::Theme;
	const auto night = IsNightMode();
	const auto current = Background()->themeObject().pathAbsolute;
	const auto other = SavedPathForOtherMode();
	auto previous = Previous{
		.dayPath = night ? other : current,
		.nightPath = night ? current : other,
		.night = night,
		.paper = Background()->paper().serialize(),
	};
	if (!IsAgentThemePath(current)) {
		WritePrevious(previous);
	}
	ApplyTheme(window, ThemePath(night), night, [] {
		EnsureAgentPaper();
		RefreshDesign();
	});
}

void SwitchToVanilla(not_null<Window::Controller*> window) {
	using namespace Window::Theme;
	const auto previous = ReadPrevious();
	const auto night = IsNightMode();
	const auto path = VanillaPathFor(previous, night);
	const auto weak = base::make_weak(window);
	ApplyTheme(window, path, night, [=] {
		RefreshDesign();
		if (previous.night != night || previous.paper.isEmpty()) {
			return;
		}
		const auto paper = Data::WallPaper::FromSerialized(previous.paper);
		if (!paper
			|| IsAgentPaper(*paper)
			|| Data::IsThemeWallPaper(*paper)
			|| Data::IsDefaultWallPaper(*paper)) {
			return;
		}
		const auto strong = weak.get();
		const auto session = strong ? strong->sessionController() : nullptr;
		if (session) {
			session->content()->setChatBackground(*paper);
		} else if (!paper->document()) {
			Background()->set(*paper);
		}
	});
}

} // namespace

void Start() {
	static auto started = false;
	if (started) {
		return;
	}
	started = true;

	InstallGlassHooks();
	RefreshDesign();

	using Type = Window::Theme::BackgroundUpdate::Type;
	static auto lifetime = rpl::lifetime();
	Window::Theme::Background()->updates(
	) | rpl::on_next([](const Window::Theme::BackgroundUpdate &update) {
		InvalidateBackdrop();
		if (update.type == Type::ApplyingTheme
			|| update.type == Type::RevertingTheme) {
			RefreshDesign();
			if (update.type == Type::ApplyingTheme) {
				crl::on_main([] { EnsureAgentPaper(); });
			}
		}
	}, lifetime);
}

bool Active() {
	return IsAgentThemePath(
		Window::Theme::Background()->themeObject().pathAbsolute);
}

bool Dark() {
	return (Window::Theme::Background()->themeObject().pathAbsolute
		== kDarkTheme.utf16());
}

Design CurrentDesign() {
	return DesignVariable().current();
}

rpl::producer<Design> DesignValue() {
	return DesignVariable().value();
}

bool IsAgentThemePath(const QString &path) {
	return (path == kDarkTheme.utf16()) || (path == kLightTheme.utf16());
}

QString ThemePath(bool dark) {
	return (dark ? kDarkTheme : kLightTheme).utf16();
}

void SwitchDesign(
		not_null<Window::Controller*> window,
		Design design,
		QPoint origin) {
	if (design == CurrentDesign()) {
		return;
	}
	const auto weak = base::make_weak(window);
	RunReveal(window->widget(), origin, [=] {
		if (const auto strong = weak.get()) {
			if (design == Design::AgentUI) {
				SwitchToAgent(strong);
			} else {
				SwitchToVanilla(strong);
			}
		}
	});
}

std::optional<QString> NightModeThemeOverride(
		bool newNightMode,
		const QString &savedPath) {
	if (Active()) {
		return ThemePath(newNightMode);
	} else if (IsAgentThemePath(savedPath)) {
		return VanillaPathFor(ReadPrevious(), newNightMode);
	}
	return std::nullopt;
}

RowPillsScope::RowPillsScope(bool enabled)
: _was(RowPills) {
	RowPills = enabled;
}

RowPillsScope::~RowPillsScope() {
	RowPills = _was;
}

bool RowsUsePills() {
	return RowPills;
}

} // namespace AgentUi
