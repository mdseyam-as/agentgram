/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "intro/intro_start.h"

#include "ai_agent/ai_agent_settings_box.h"
#include "core/api_credentials.h"
#include "boxes/abstract_box.h"
#include "ui/layers/generic_box.h"

#include "lang/lang_keys.h"
#include "intro/intro_qr.h"
#include "intro/intro_phone.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "main/main_account.h"
#include "main/main_app_config.h"

namespace Intro {
namespace details {

StartWidget::StartWidget(
	QWidget *parent,
	not_null<Main::Account*> account,
	not_null<Data*> data)
: Step(parent, account, data, true) {
	setMouseTracking(true);
	setTitleText(rpl::single(u"Agentgram"_q));
	setDescriptionText(rpl::single(
		u"Telegram с встроенным ИИ-агентом.\n"
		"Неофициальный клиент на основе Telegram Desktop."_q));
	const auto api = Ui::CreateChild<Ui::LinkButton>(
		this,
		Core::UsingCustomApiCredentials()
			? u"Свой api_id: %1"_q.arg(Core::CurrentApiCredentials().id)
			: u"Указать свой api_id Telegram"_q);
	api->setClickedCallback([] {
		Ui::show(Box(AiAgent::ApiCredentialsBox));
	});
	sizeValue() | rpl::on_next([=](QSize size) {
		api->moveToLeft(
			(size.width() - api->width()) / 2,
			size.height() - api->height() * 3);
	}, api->lifetime());
	show();
}

void StartWidget::submit() {
	account().destroyStaleAuthorizationKeys();
	goNext<QrWidget>();
}

rpl::producer<QString> StartWidget::nextButtonText() const {
	return tr::lng_start_msgs();
}

rpl::producer<> StartWidget::nextButtonFocusRequests() const {
	return _nextButtonFocusRequests.events();
}

void StartWidget::activate() {
	Step::activate();
	setInnerFocus();
}

void StartWidget::setInnerFocus() {
	_nextButtonFocusRequests.fire({});
}

} // namespace details
} // namespace Intro
