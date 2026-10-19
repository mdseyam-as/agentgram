/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/rp_widget.h"

#include <QtCore/QJsonObject>

namespace Ui {
class LayerManager;
class RoundButton;
class FlatLabel;
class ScrollArea;
class VerticalLayout;
} // namespace Ui

namespace BotStudio {

class Studio;
class FlowCanvas;
struct FlowState;

// The visual constructor: a canvas with the scheme of message blocks and
// an inspector to edit the selected block.
class FlowEditor final : public Ui::RpWidget {
public:
	FlowEditor(
		QWidget *parent,
		not_null<Studio*> studio,
		not_null<Ui::LayerManager*> layers,
		QString botId);
	~FlowEditor();

	[[nodiscard]] bool dirty() const;
	void save();
	// Picks up a scheme changed elsewhere (by the AI agent), unless the
	// user has unsaved edits.
	void reload();

protected:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;

private:
	void setupToolbar();
	void rebuildInspector();
	void updateStatus();
	void updateGeometry();
	void addBlock(QPoint position, bool select = true);
	void markChanged(bool rebuild);
	void showSettings();

	const not_null<Studio*> _studio;
	const not_null<Ui::LayerManager*> _layers;
	const QString _botId;
	const std::shared_ptr<FlowState> _state;

	not_null<Ui::RoundButton*> _add;
	not_null<Ui::RoundButton*> _settings;
	not_null<Ui::RoundButton*> _save;
	not_null<Ui::FlatLabel*> _status;
	not_null<Ui::ScrollArea*> _canvasScroll;
	not_null<FlowCanvas*> _canvas;
	not_null<Ui::ScrollArea*> _inspectorScroll;
	Ui::VerticalLayout *_inspector = nullptr;
	QString _inspected;

};

} // namespace BotStudio
