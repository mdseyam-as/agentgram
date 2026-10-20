/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/agent_surface.h"

namespace style {
class palette;
} // namespace style

namespace Ui {
class RpWidget;
} // namespace Ui

namespace AgentUi {

// Glass surfaces of AgentUI. Each one shows the blurred chat wallpaper
// of the window under its own tint, so panels read as matte glass laid
// over a single backdrop.
using Surface = Ui::AgentSurface::Kind;

// Paints the glass of `surface` over `rect` of `widget`. `edges` get a
// thin highlight line, like the lit edge of a glass sheet.
void PaintGlass(
	QPainter &p,
	not_null<const QWidget*> widget,
	QRect rect,
	Surface surface,
	RectParts edges = RectPart::None);

// Floating card layout of AgentUI: the chat list is a rounded card in
// a darker frame, the message field a floating capsule.
[[nodiscard]] QMargins CardMargins();
[[nodiscard]] int CardRadius();
[[nodiscard]] QColor FrameTint();
void PaintCapsule(
	QPainter &p,
	not_null<const QWidget*> widget,
	QRect capsule);
void PaintFloating(
	QPainter &p,
	not_null<const QWidget*> widget,
	QRect card,
	Surface surface,
	int maxRadius);

// Glass when AgentUI is active, a plain `fallback` fill otherwise.
void FillSurface(
	QPainter &p,
	not_null<const QWidget*> widget,
	QRect rect,
	const style::color &fallback,
	Surface surface,
	RectParts edges = RectPart::None);

// Only the blurred backdrop of the window, without the tint.
void PaintBackdrop(
	QPainter &p,
	not_null<const QWidget*> widget,
	QRect rect);

// Film grain that keeps glass from looking like flat plastic.
void PaintGrain(
	QPainter &p,
	not_null<const QWidget*> widget,
	QRect rect,
	float64 opacity);

[[nodiscard]] QColor GlassTint(Surface surface);
[[nodiscard]] QColor EdgeColor();

// Inner glass elements: input fields, cards and bubbles on a glass panel.
[[nodiscard]] QColor FieldTint();
[[nodiscard]] QColor FieldEdge();

// Downscaled and blurred copy, meant to be drawn scaled back up.
[[nodiscard]] QImage Frost(QImage image, int downscale, int radius);

void InvalidateBackdrop();
void InstallGlassHooks();

// The main palette with the glass surface colors made transparent, so
// nested fills inside a glass surface keep the glass visible.
enum class GlassLevel {
	Surfaces, // chat list, header, composer, folders
	Panel, // the above plus the plain window background
	Menu, // popup menu background
};
[[nodiscard]] const style::palette *GlassPalette(GlassLevel level);

// Widgets of `root` paint with the glass palette while AgentUI is on.
// Opaque widgets inside get the glass of `surface` painted under them,
// the root paints its own glass.
void RegisterGlassRoot(
	not_null<Ui::RpWidget*> root,
	Surface surface,
	GlassLevel level = GlassLevel::Surfaces);
[[nodiscard]] bool HasGlassRoots();

struct GlassTarget {
	const style::palette *palette = nullptr;
	const QWidget *root = nullptr;
	Surface surface = Surface::ChatList;
};
[[nodiscard]] GlassTarget GlassFor(not_null<const QWidget*> widget);

// Called for every paint event inside a glass root, before the widget
// paints itself.
void PaintGlassUnderOpaque(
	not_null<QWidget*> widget,
	const GlassTarget &target,
	QRect clip);

} // namespace AgentUi
