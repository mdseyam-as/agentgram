/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/unique_qptr.h"
#include "ui/rp_widget.h"

#include <QtCore/QJsonObject>

namespace Ui {
class LayerManager;
class RoundButton;
class FlatLabel;
class ScrollArea;
class VerticalLayout;
class IconButton;
} // namespace Ui

namespace BotStudio {

class Studio;
class FlowCanvas;
class NodePicker;
struct FlowState;
struct LinkRef;

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
	void setupZoomBar();
	void layoutZoomBar();
	void rebuildInspector();
	void updateStatus();
	void updateGeometry();
	void markChanged(bool rebuild);
	void showSettings();
	void showProducts();
	void showPicker(QPoint at, QPointF logical, std::optional<LinkRef> from);
	void hidePicker();
	void createNode(
		const QString &type,
		QPointF logical,
		std::optional<LinkRef> from);
	[[nodiscard]] QPointF freeSpot() const;
	void deleteSelected();
	void duplicateSelected();
	void commit(bool force);
	void restore(QJsonObject flow);
	void undo();
	void redo();
	void updateUndoButtons();

	const not_null<Studio*> _studio;
	const not_null<Ui::LayerManager*> _layers;
	const QString _botId;
	const std::shared_ptr<FlowState> _state;

	not_null<Ui::RoundButton*> _add;
	not_null<Ui::RoundButton*> _undo;
	not_null<Ui::RoundButton*> _redo;
	not_null<Ui::RoundButton*> _settings;
	not_null<Ui::RoundButton*> _products;
	not_null<Ui::RoundButton*> _save;
	not_null<Ui::FlatLabel*> _status;
	not_null<FlowCanvas*> _canvas;
	not_null<Ui::RpWidget*> _zoomBar;
	Ui::RoundButton *_zoomLabel = nullptr;
	std::vector<Ui::RoundButton*> _zoomButtons;
	not_null<Ui::ScrollArea*> _inspectorScroll;
	not_null<Ui::IconButton*> _panelClose;
	not_null<Ui::RpWidget*> _panelBorder;
	Ui::VerticalLayout *_inspector = nullptr;
	QString _inspected;
	base::unique_qptr<NodePicker> _picker;

	QJsonObject _committed;
	std::vector<QJsonObject> _undoStack;
	std::vector<QJsonObject> _redoStack;
	crl::time _lastCommit = 0;

};

} // namespace BotStudio
