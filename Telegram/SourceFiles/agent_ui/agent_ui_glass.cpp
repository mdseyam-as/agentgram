/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "agent_ui/agent_ui_glass.h"

#include "agent_ui/agent_ui.h"
#include "ui/agent_glass.h"
#include "ui/image/image_prepare.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/style/style_core_palette.h"
#include "ui/window_palette.h"
#include "base/flat_map.h"
#include "window/themes/window_theme.h"
#include "styles/palette.h"
#include "styles/style_basic.h"

#include <QtCore/QPointer>
#include <QtGui/QPainterPath>
#include <QtWidgets/QApplication>

#include <random>

namespace AgentUi {
namespace {

// The backdrop is rendered this many times smaller than the window and
// blurred there, then drawn scaled back up: cheap and very soft.
constexpr auto kBackdropDownscale = 10;
constexpr auto kBackdropBlur = 3;
constexpr auto kBackdropCachesLimit = 4;
constexpr auto kFrostDownscale = 6;
constexpr auto kFrostBlur = 5;
constexpr auto kGrainSize = 128;

struct BackdropCache {
	QSize size;
	uint64 key = 0;
	QImage image;
};

struct MenuFrost {
	QPointer<QWidget> menu;
	QRect global;
	QImage image;
	bool registered = false;
};

struct LayerFrost {
	QPointer<QWidget> widget;
	qint64 key = 0;
	QImage image;
};

uint64 Generation = 0;

[[nodiscard]] std::vector<BackdropCache> &BackdropCaches() {
	static auto result = std::vector<BackdropCache>();
	return result;
}

[[nodiscard]] std::vector<MenuFrost> &MenuFrosts() {
	static auto result = std::vector<MenuFrost>();
	return result;
}

[[nodiscard]] std::vector<LayerFrost> &LayerFrosts() {
	static auto result = std::vector<LayerFrost>();
	return result;
}

[[nodiscard]] uint64 BackgroundKey() {
	const auto background = Window::Theme::Background();
	auto result = Generation;
	const auto mix = [&](uint64 value) {
		result ^= value + 0x9e3779b97f4a7c15ULL + (result << 6) + (result >> 2);
	};
	mix(uint64(background->prepared().cacheKey()));
	mix(uint64(background->gradientForFill().cacheKey()));
	if (const auto color = background->colorForFill()) {
		mix(uint64(color->rgba()) + 1);
	}
	return result;
}

[[nodiscard]] QImage RenderBackdrop(QSize size) {
	const auto width = std::max(size.width() / kBackdropDownscale, 8);
	const auto height = std::max(size.height() / kBackdropDownscale, 8);
	auto result = QImage(width, height, QImage::Format_ARGB32_Premultiplied);
	result.fill(st::windowBg->c);

	const auto background = Window::Theme::Background();
	if (const auto color = background->colorForFill()) {
		result.fill(*color);
	} else {
		const auto source = background->createCurrentImage();
		if (!source.isNull()) {
			// Cover: keep the aspect ratio, crop what doesn't fit.
			const auto from = QSizeF(source.size());
			const auto scale = std::max(
				width / from.width(),
				height / from.height());
			const auto scaled = from * scale;
			const auto target = QRectF(
				(width - scaled.width()) / 2.,
				(height - scaled.height()) / 2.,
				scaled.width(),
				scaled.height());
			auto p = QPainter(&result);
			p.setRenderHint(QPainter::SmoothPixmapTransform);
			p.drawImage(target, source);
		}
	}
	return Images::BlurLargeImage(std::move(result), kBackdropBlur);
}

[[nodiscard]] const QImage &BackdropFor(QSize size) {
	const auto key = BackgroundKey();
	auto &caches = BackdropCaches();
	for (auto &cache : caches) {
		if (cache.size == size) {
			if (cache.key != key) {
				cache.key = key;
				cache.image = RenderBackdrop(size);
			}
			return cache.image;
		}
	}
	if (caches.size() >= kBackdropCachesLimit) {
		caches.erase(begin(caches));
	}
	caches.push_back({ size, key, RenderBackdrop(size) });
	return caches.back().image;
}

[[nodiscard]] const QImage &GrainTile() {
	static const auto result = [] {
		const auto ratio = style::DevicePixelRatio();
		const auto side = kGrainSize * ratio;
		auto image = QImage(side, side, QImage::Format_ARGB32_Premultiplied);
		auto engine = std::mt19937(0x5eed);
		auto distribution = std::uniform_int_distribution<int>(0, 255);
		const auto bytes = image.bits();
		const auto stride = image.bytesPerLine();
		for (auto y = 0; y != side; ++y) {
			const auto line = reinterpret_cast<uint32*>(bytes + y * stride);
			for (auto x = 0; x != side; ++x) {
				const auto value = uint32(distribution(engine));
				line[x] = 0xFF000000U | (value << 16) | (value << 8) | value;
			}
		}
		image.setDevicePixelRatio(ratio);
		return image;
	}();
	return result;
}

[[nodiscard]] QColor WithAlpha(QColor color, float64 alpha) {
	color.setAlphaF(alpha);
	return color;
}

[[nodiscard]] QColor MenuTint() {
	return Dark()
		? QColor(24, 28, 36, 178)
		: QColor(251, 252, 253, 172);
}

[[nodiscard]] float64 GrainOpacity() {
	return Dark() ? 0.045 : 0.035;
}

struct GlassRoot {
	GlassLevel level = GlassLevel::Surfaces;
	Surface surface = Surface::ChatList;
};

base::flat_map<const QWidget*, GlassRoot> &GlassRoots() {
	static auto result = base::flat_map<const QWidget*, GlassRoot>();
	return result;
}

[[nodiscard]] QWidget *WindowBehind(not_null<QWidget*> menu) {
	const auto own = menu->window();
	if (const auto parent = menu->parentWidget()) {
		if (const auto window = parent->window(); window != own) {
			return window;
		}
	}
	const auto active = QApplication::activeWindow();
	return (active && active != own) ? active : nullptr;
}

[[nodiscard]] QImage FrostBehind(not_null<QWidget*> menu, QRect global) {
	const auto ratio = style::DevicePixelRatio();
	auto canvas = QImage(
		global.size() * ratio,
		QImage::Format_ARGB32_Premultiplied);
	canvas.setDevicePixelRatio(ratio);
	canvas.fill(st::windowBg->c);
	if (const auto behind = WindowBehind(menu)) {
		const auto local = QRect(
			behind->mapFromGlobal(global.topLeft()),
			global.size());
		const auto visible = local.intersected(behind->rect());
		if (!visible.isEmpty()) {
			const auto grab = behind->grab(visible);
			auto p = QPainter(&canvas);
			p.drawPixmap(visible.topLeft() - local.topLeft(), grab);
		}
	}
	return Frost(std::move(canvas), kFrostDownscale, kFrostBlur);
}

bool PaintMenuBackdrop(QPainter &p, QWidget *menu, const QRect &inner) {
	if (!Active() || !menu || inner.isEmpty()) {
		return false;
	}
	auto &frosts = MenuFrosts();
	frosts.erase(ranges::remove_if(frosts, [](const MenuFrost &frost) {
		return !frost.menu;
	}), end(frosts));

	const auto global = QRect(menu->mapToGlobal(inner.topLeft()), inner.size());
	auto i = ranges::find(frosts, menu, [](const MenuFrost &frost) {
		return frost.menu.data();
	});
	if (i == end(frosts)) {
		frosts.push_back({ .menu = menu });
		i = end(frosts) - 1;
	}
	if (!i->registered) {
		if (const auto rp = dynamic_cast<Ui::RpWidget*>(menu)) {
			Ui::SetWindowPalette(rp, [] {
				return GlassPalette(GlassLevel::Menu);
			});
			i->registered = true;
		}
	}
	if (i->global != global || i->image.isNull()) {
		i->global = global;
		i->image = FrostBehind(menu, global);
	}
	{
		auto hq = PainterHighQualityEnabler(p);
		p.drawImage(QRectF(inner), i->image);
	}
	p.fillRect(inner, MenuTint());
	PaintGrain(p, menu, inner, GrainOpacity());
	return true;
}

void PaintLayerBackdrop(
		QPainter &p,
		QWidget *widget,
		const QPixmap &bodyCache,
		float64 opacity) {
	auto &frosts = LayerFrosts();
	frosts.erase(ranges::remove_if(frosts, [](const LayerFrost &frost) {
		return !frost.widget;
	}), end(frosts));
	if (!Active() || !widget) {
		return;
	}
	auto i = ranges::find(frosts, widget, [](const LayerFrost &frost) {
		return frost.widget.data();
	});
	if (i == end(frosts)) {
		frosts.push_back({ .widget = widget });
		i = end(frosts) - 1;
	}
	if (!bodyCache.isNull() && bodyCache.cacheKey() != i->key) {
		i->key = bodyCache.cacheKey();
		i->image = Frost(bodyCache.toImage(), kFrostDownscale, kFrostBlur);
	}
	if (i->image.isNull() || opacity <= 0.) {
		return;
	}
	auto hq = PainterHighQualityEnabler(p);
	p.setOpacity(opacity);
	p.drawImage(QRectF(widget->rect()), i->image);
	p.setOpacity(1.);
}

} // namespace

QColor GlassTint(Surface surface) {
	if (Dark()) {
		const auto base = QColor(12, 15, 20);
		switch (surface) {
		case Surface::ChatList: return WithAlpha(QColor(18, 22, 29), 0.46);
		case Surface::Folders: return FrameTint();
		case Surface::Header: return WithAlpha(base, 0.50);
		case Surface::Composer: return WithAlpha(base, 0.54);
		case Surface::Bar: return WithAlpha(base, 0.48);
		case Surface::Panel: return WithAlpha(base, 0.60);
		}
	} else {
		const auto base = QColor(248, 250, 252);
		switch (surface) {
		case Surface::ChatList: return WithAlpha(base, 0.66);
		case Surface::Folders: return FrameTint();
		case Surface::Header: return WithAlpha(base, 0.54);
		case Surface::Composer: return WithAlpha(base, 0.60);
		case Surface::Bar: return WithAlpha(base, 0.50);
		case Surface::Panel: return WithAlpha(base, 0.64);
		}
	}
	Unexpected("Surface in AgentUi::GlassTint.");
}

QColor EdgeColor() {
	return Dark() ? QColor(255, 255, 255, 20) : QColor(15, 23, 42, 20);
}

QColor FieldTint() {
	return Dark() ? QColor(255, 255, 255, 15) : QColor(255, 255, 255, 178);
}

QColor FieldEdge() {
	return Dark() ? QColor(255, 255, 255, 22) : QColor(15, 23, 42, 18);
}

void PaintBackdrop(
		QPainter &p,
		not_null<const QWidget*> widget,
		QRect rect) {
	const auto window = widget->window();
	const auto size = window->size();
	if (rect.isEmpty() || size.isEmpty()) {
		return;
	}
	const auto &image = BackdropFor(size);
	const auto origin = widget->mapTo(window, QPoint());
	const auto kx = image.width() / float64(size.width());
	const auto ky = image.height() / float64(size.height());
	const auto source = QRectF(
		(rect.x() + origin.x()) * kx,
		(rect.y() + origin.y()) * ky,
		rect.width() * kx,
		rect.height() * ky);
	auto hq = PainterHighQualityEnabler(p);
	p.drawImage(QRectF(rect), image, source);
}

void PaintGrain(
		QPainter &p,
		not_null<const QWidget*> widget,
		QRect rect,
		float64 opacity) {
	if (rect.isEmpty() || opacity <= 0.) {
		return;
	}
	const auto &tile = GrainTile();
	const auto ratio = tile.devicePixelRatio();
	const auto origin = widget->mapTo(widget->window(), QPoint());
	auto brush = QBrush(tile);
	brush.setTransform(QTransform()
		.translate(-origin.x(), -origin.y())
		.scale(1. / ratio, 1. / ratio));
	const auto was = p.opacity();
	p.setOpacity(was * opacity);
	p.fillRect(rect, brush);
	p.setOpacity(was);
}

void PaintGlass(
		QPainter &p,
		not_null<const QWidget*> widget,
		QRect rect,
		Surface surface,
		RectParts edges) {
	if (rect.isEmpty()) {
		return;
	}
	if (surface == Surface::ChatList) {
		// The chat list floats as a rounded glass card inside a darker
		// frame of the same backdrop.
		const auto root = GlassFor(widget).root;
		const auto card = root
			? QRect(
				widget->mapFrom(root, QPoint()),
				root->size()).marginsRemoved(CardMargins())
			: widget->rect().marginsRemoved(CardMargins());
		PaintBackdrop(p, widget, rect);
		auto hq = PainterHighQualityEnabler(p);
		const auto radius = float64(CardRadius());
		auto cardPath = QPainterPath();
		cardPath.addRoundedRect(QRectF(card), radius, radius);
		if (!card.contains(rect)) {
			auto frame = QPainterPath();
			frame.addRect(QRectF(rect));
			p.fillPath(frame.subtracted(cardPath), FrameTint());
		}
		p.save();
		p.setClipRect(rect);
		p.fillPath(cardPath, GlassTint(surface));
		PaintGrain(p, widget, rect, GrainOpacity());
		p.setPen(QPen(EdgeColor(), 1.));
		p.setBrush(Qt::NoBrush);
		p.drawPath(cardPath.translated(0.5, 0.5));
		p.restore();
		return;
	}
	PaintBackdrop(p, widget, rect);
	p.fillRect(rect, GlassTint(surface));
	PaintGrain(p, widget, rect, GrainOpacity());
	if (edges) {
		const auto line = std::max(style::ConvertScale(1), 1);
		const auto color = EdgeColor();
		const auto full = widget->rect();
		if (edges & RectPart::Top) {
			p.fillRect(
				QRect(full.x(), full.y(), full.width(), line).intersected(rect),
				color);
		}
		if (edges & RectPart::Bottom) {
			p.fillRect(
				QRect(
					full.x(),
					full.y() + full.height() - line,
					full.width(),
					line).intersected(rect),
				color);
		}
		if (edges & RectPart::Left) {
			p.fillRect(
				QRect(full.x(), full.y(), line, full.height()).intersected(rect),
				color);
		}
		if (edges & RectPart::Right) {
			p.fillRect(
				QRect(
					full.x() + full.width() - line,
					full.y(),
					line,
					full.height()).intersected(rect),
				color);
		}
	}
}

QMargins CardMargins() {
	const auto skip = style::ConvertScale(6);
	return { skip, skip, skip, skip };
}

int CardRadius() {
	return style::ConvertScale(16);
}

QColor FrameTint() {
	return Dark()
		? QColor(7, 9, 12, 205)
		: QColor(222, 229, 236, 190);
}

void PaintCapsule(
		QPainter &p,
		not_null<const QWidget*> widget,
		QRect capsule) {
	PaintFloating(
		p,
		widget,
		capsule,
		Surface::Composer,
		style::ConvertScale(22));
}

void PaintFloating(
		QPainter &p,
		not_null<const QWidget*> widget,
		QRect capsule,
		Surface surface,
		int maxRadius) {
	if (capsule.isEmpty()) {
		return;
	}
	auto hq = PainterHighQualityEnabler(p);
	const auto radius = std::min(
		float64(maxRadius),
		capsule.height() / 2.);
	auto path = QPainterPath();
	path.addRoundedRect(QRectF(capsule), radius, radius);

	// Soft shadow under the floating capsule.
	const auto shadow = Dark() ? 70 : 26;
	for (auto i = 1; i <= 3; ++i) {
		auto ring = QPainterPath();
		const auto grow = style::ConvertScale(2.) * i;
		ring.addRoundedRect(
			QRectF(capsule).marginsAdded({ grow, grow * 0.5, grow, grow * 1.5 }),
			radius + grow,
			radius + grow);
		p.fillPath(ring.subtracted(path), QColor(0, 0, 0, shadow / (i * 2)));
	}

	p.save();
	p.setClipPath(path);
	PaintBackdrop(p, widget, capsule);
	p.fillRect(capsule, GlassTint(surface));
	PaintGrain(p, widget, capsule, GrainOpacity());
	auto gloss = QLinearGradient(capsule.topLeft(), capsule.bottomLeft());
	gloss.setColorAt(0., QColor(255, 255, 255, Dark() ? 18 : 90));
	gloss.setColorAt(0.45, QColor(255, 255, 255, 0));
	p.fillRect(capsule, gloss);
	p.restore();
	p.setPen(QPen(EdgeColor(), 1.));
	p.setBrush(Qt::NoBrush);
	p.drawPath(path);
}

void FillSurface(
		QPainter &p,
		not_null<const QWidget*> widget,
		QRect rect,
		const style::color &fallback,
		Surface surface,
		RectParts edges) {
	if (Active()) {
		PaintGlass(p, widget, rect, surface, edges);
	} else {
		p.fillRect(rect, fallback);
	}
}

QImage Frost(QImage image, int downscale, int radius) {
	if (image.isNull()) {
		return image;
	}
	const auto width = std::max(image.width() / downscale, 1);
	const auto height = std::max(image.height() / downscale, 1);
	auto small = image.scaled(
		width,
		height,
		Qt::IgnoreAspectRatio,
		Qt::SmoothTransformation
	).convertToFormat(QImage::Format_ARGB32_Premultiplied);
	small.setDevicePixelRatio(1.);
	return Images::BlurLargeImage(std::move(small), radius);
}

void InvalidateBackdrop() {
	++Generation;
}

const style::palette *GlassPalette(GlassLevel level) {
	if (!Active()) {
		return nullptr;
	}
	struct Cached {
		std::unique_ptr<style::palette> palette;
		uint64 version = uint64(-1);
	};
	static auto cached = std::array<Cached, 3>();
	auto &entry = cached[int(level)];
	const auto key = BackgroundKey();
	if (!entry.palette || entry.version != key) {
		entry.version = key;
		entry.palette = std::make_unique<style::palette>();
		const auto palette = entry.palette.get();
		palette->load(style::main_palette::save());
		const auto clear = [&](const char *name, const style::color &color) {
			const auto c = color->c;
			palette->setColor(
				QLatin1String(name),
				uchar(c.red()),
				uchar(c.green()),
				uchar(c.blue()),
				uchar(0));
		};
		if (level == GlassLevel::Menu) {
			clear("menuBg", st::menuBg);
		} else {
			clear("dialogsBg", st::dialogsBg);
			clear("topBarBg", st::topBarBg);
			clear("historyComposeAreaBg", st::historyComposeAreaBg);
			clear("historyReplyBg", st::historyReplyBg);
			clear("historyComposeButtonBg", st::historyComposeButtonBg);
			clear("historyPinnedBg", st::historyPinnedBg);
			clear("sideBarBg", st::sideBarBg);
			if (level == GlassLevel::Panel) {
				clear("windowBg", st::windowBg);
			}
		}
		palette->finalize();
	}
	return entry.palette.get();
}

void RegisterGlassRoot(
		not_null<Ui::RpWidget*> root,
		Surface surface,
		GlassLevel level) {
	const auto key = static_cast<const QWidget*>(root.get());
	GlassRoots()[key] = GlassRoot{ .level = level, .surface = surface };
	root->lifetime().add([=] {
		GlassRoots().remove(key);
	});
}

bool HasGlassRoots() {
	return !GlassRoots().empty();
}

GlassTarget GlassFor(not_null<const QWidget*> widget) {
	const auto &roots = GlassRoots();
	if (roots.empty() || !Active()) {
		return {};
	}
	for (auto w = widget.get(); w; w = w->isWindow() ? nullptr : w->parentWidget()) {
		if (const auto i = roots.find(w); i != end(roots)) {
			return {
				.palette = GlassPalette(i->second.level),
				.root = w,
				.surface = i->second.surface,
			};
		}
	}
	return {};
}

void PaintGlassUnderOpaque(
		not_null<QWidget*> widget,
		const GlassTarget &target,
		QRect clip) {
	if (!target.palette
		|| widget == target.root
		|| !widget->testAttribute(Qt::WA_OpaquePaintEvent)
		|| clip.isEmpty()) {
		return;
	}
	auto p = QPainter(widget);
	PaintGlass(p, widget, clip, target.surface);
}

// Clears the opaque flag of `widget` and its wrappers up to the widget
// that paints the chat wallpaper. Returns true if anything changed, the
// widget is repainted then.
bool MakeSeeThrough(not_null<QWidget*> widget) {
	auto changed = false;
	auto w = widget.get();
	for (auto level = 0; w && !w->isWindow() && level != 4; ++level) {
		const auto i = GlassRoots().find(w);
		if (i != end(GlassRoots())
			&& w != widget.get()
			&& i->second.surface == Surface::Bar) {
			break;
		}
		if (w->testAttribute(Qt::WA_OpaquePaintEvent)) {
			w->setAttribute(Qt::WA_OpaquePaintEvent, false);
			changed = true;
		}
		w = w->parentWidget();
	}
	if (changed) {
		widget->update();
	}
	return changed;
}

void InstallGlassHooks() {
	Ui::AgentGlass::SetHooks({
		.menuBackdrop = PaintMenuBackdrop,
		.layerBackdrop = PaintLayerBackdrop,
	});
	Ui::AgentSurface::SetPainter([](
			QPainter &p,
			not_null<const QWidget*> widget,
			QRect rect,
			Surface surface,
			RectParts edges) {
		if (!Active()) {
			return false;
		}
		if (surface == Surface::Header || surface == Surface::Bar) {
			// Header and bars float as cards over the chat wallpaper,
			// so the widget below has to be painted around them.
			if (MakeSeeThrough(const_cast<QWidget*>(widget.get()))) {
				PaintGlass(p, widget, rect, surface, edges);
				return true;
			}
			const auto skip = style::ConvertScale(8);
			const auto card = widget->rect().marginsRemoved(
				(surface == Surface::Header)
					? QMargins(skip, skip * 3 / 4, skip, skip / 4)
					: QMargins(skip, skip / 2, skip, skip / 4));
			PaintFloating(
				p,
				widget,
				card,
				surface,
				style::ConvertScale(14));
			return true;
		}
		PaintGlass(p, widget, rect, surface, edges);
		return true;
	});
}

} // namespace AgentUi
