/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/rp_widget.h"
#include "base/object_ptr.h"

namespace Ui {
class ScrollArea;
class VerticalLayout;
class InputField;
class IconButton;
class FlatLabel;
class AbstractButton;
class PlainShadow;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace AiAgent {

class Manager;
class EntryWidget;

class Panel final : public Ui::RpWidget {
public:
	Panel(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		bool separateWindow);
	~Panel();

	void setCloseCallback(Fn<void()> callback);
	void setPopoutCallback(Fn<void()> callback);
	void focusInput();
	void sendPrompt(const QString &text);

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void focusInEvent(QFocusEvent *e) override;

private:
	void setupHeader();
	void setupComposer();
	void setupList();
	void rebuildEntries();
	void addEntryWidget(uint64 id);
	void updateEntryWidget(uint64 id);
	void refreshEmptyState();
	void submit();
	void updateControlsGeometry();
	void updateSendButton();
	void showSettings();
	void scrollToBottom();

	const not_null<Window::SessionController*> _controller;
	const not_null<Manager*> _manager;
	const bool _separateWindow = false;

	object_ptr<Ui::RpWidget> _header;
	Ui::FlatLabel *_title = nullptr;
	Ui::FlatLabel *_subtitle = nullptr;
	Ui::IconButton *_close = nullptr;
	Ui::IconButton *_popout = nullptr;
	Ui::IconButton *_theme = nullptr;
	Ui::IconButton *_settings = nullptr;
	Ui::IconButton *_newChat = nullptr;
	object_ptr<Ui::PlainShadow> _headerShadow;
	object_ptr<Ui::PlainShadow> _leftShadow;

	object_ptr<Ui::ScrollArea> _scroll;
	QPointer<Ui::VerticalLayout> _list;
	base::flat_map<uint64, EntryWidget*> _entryWidgets;
	Ui::RpWidget *_empty = nullptr;

	object_ptr<Ui::RpWidget> _composer;
	Ui::InputField *_field = nullptr;
	Ui::AbstractButton *_send = nullptr;
	Ui::FlatLabel *_hint = nullptr;
	object_ptr<Ui::PlainShadow> _composerShadow;

	Fn<void()> _closeCallback;
	Fn<void()> _popoutCallback;
	bool _stickToBottom = true;

};

void TogglePanel(
	not_null<Window::SessionController*> controller,
	const QString &prompt = QString());
void ShowPanel(
	not_null<Window::SessionController*> controller,
	const QString &prompt = QString());
[[nodiscard]] bool PanelShown(
	not_null<Window::SessionController*> controller);
[[nodiscard]] rpl::producer<bool> PanelShownValue(
	not_null<Window::SessionController*> controller);

} // namespace AiAgent
