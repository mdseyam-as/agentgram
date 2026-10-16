/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ai_agent/ai_agent_settings_box.h"

#include "ai_agent/ai_agent_manager.h"
#include "base/unique_qptr.h"
#include "core/api_credentials.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/fields/password_input.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/ui_utility.h"
#include "styles/style_ai_agent.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

namespace AiAgent {
namespace {

class FlowLayout final : public Ui::RpWidget {
public:
	using RpWidget::RpWidget;

	void addWidget(not_null<Ui::RpWidget*> widget) {
		_widgets.push_back(widget);
		widget->widthValue() | rpl::on_next([=] {
			if (width() > 0) {
				resizeToWidth(width());
			}
		}, widget->lifetime());
	}

	void clearWidgets() {
		for (const auto &widget : base::take(_widgets)) {
			delete widget.get();
		}
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto skip = st::aiAgentProviderSkip;
		auto left = 0;
		auto top = 0;
		auto rowHeight = 0;
		for (const auto &widget : _widgets) {
			if (widget->isHidden()) {
				continue;
			}
			if (left > 0 && left + widget->width() > newWidth) {
				left = 0;
				top += rowHeight + skip;
				rowHeight = 0;
			}
			widget->moveToLeft(left, top, newWidth);
			left += widget->width() + skip;
			rowHeight = std::max(rowHeight, widget->height());
		}
		return top + rowHeight;
	}

private:
	std::vector<not_null<Ui::RpWidget*>> _widgets;

};

[[nodiscard]] not_null<Ui::FlatLabel*> AddLabel(
		not_null<Ui::VerticalLayout*> container,
		const QString &text,
		const style::FlatLabel &st,
		style::margins margins = st::aiAgentSettingsPadding) {
	return container->add(
		object_ptr<Ui::FlatLabel>(container, text, st),
		margins);
}

void AddSection(
		not_null<Ui::VerticalLayout*> container,
		const QString &text) {
	AddLabel(
		container,
		text,
		st::aiAgentSettingsSection,
		QMargins(
			st::aiAgentSettingsPadding.left(),
			st::aiAgentSettingsPadding.top() * 2,
			st::aiAgentSettingsPadding.right(),
			st::aiAgentSettingsPadding.bottom()));
}

[[nodiscard]] not_null<Ui::RoundButton*> MakeChip(
		not_null<QWidget*> parent,
		const QString &text,
		const style::RoundButton &st) {
	const auto result = Ui::CreateChild<Ui::RoundButton>(
		parent.get(),
		rpl::single(text),
		st);
	result->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
	return result;
}

[[nodiscard]] not_null<Ui::PasswordInput*> AddPasswordField(
		not_null<Ui::VerticalLayout*> container,
		const QString &placeholder,
		const QString &value) {
	const auto wrap = container->add(
		object_ptr<Ui::RpWidget>(container),
		st::aiAgentSettingsFieldPadding);
	const auto field = Ui::CreateChild<Ui::PasswordInput>(
		wrap,
		st::defaultInputField,
		rpl::single(placeholder),
		value);
	wrap->resize(wrap->width(), field->height());
	wrap->widthValue() | rpl::on_next([=](int width) {
		field->resize(width, field->height());
		field->moveToLeft(0, 0);
	}, wrap->lifetime());
	field->show();
	return field;
}

struct ProviderFields {
	QPointer<Ui::PasswordInput> apiKey;
	QPointer<Ui::InputField> baseUrl;
	QPointer<Ui::InputField> model;
};

void FillProvider(
		not_null<Ui::GenericBox*> box,
		not_null<Ui::VerticalLayout*> container,
		not_null<Manager*> manager,
		const QString &id,
		std::shared_ptr<ProviderFields> fields,
		Fn<void()> rebuild) {
	const auto &info = FindProvider(id);
	const auto &config = manager->config();
	const auto settings = config.find(id);
	const auto oauthLoggedIn = settings
		&& ((info.oauth == OAuthKind::ChatGPT && settings->loggedIn())
			|| (info.oauth == OAuthKind::OpenRouter
				&& !settings->apiKey.isEmpty()));

	AddLabel(container, info.description, st::aiAgentSettingsLabel);

	if (info.oauth != OAuthKind::None) {
		if (info.oauth == OAuthKind::ChatGPT && oauthLoggedIn) {
			const auto who = settings->accountEmail.isEmpty()
				? u"аккаунт ChatGPT"_q
				: settings->accountEmail;
			AddLabel(
				container,
				u"Вы вошли: "_q + who,
				st::boxLabel);
			const auto logout = container->add(
				object_ptr<Ui::RoundButton>(
					container,
					rpl::single(u"Выйти"_q),
					st::aiAgentRejectButton),
				st::aiAgentSettingsPadding,
				style::al_left);
			logout->setTextTransform(
				Ui::RoundButtonTextTransform::NoTransform);
			logout->setClickedCallback([=] {
				manager->logout(id);
				rebuild();
			});
		} else {
			const auto text = (info.oauth == OAuthKind::ChatGPT)
				? u"Войти через ChatGPT"_q
				: (oauthLoggedIn
					? u"Войти через OpenRouter заново"_q
					: u"Войти через OpenRouter"_q);
			const auto login = container->add(
				object_ptr<Ui::RoundButton>(
					container,
					rpl::single(text),
					st::aiAgentLoginButton),
				st::aiAgentSettingsPadding,
				style::al_left);
			login->setTextTransform(
				Ui::RoundButtonTextTransform::NoTransform);
			const auto status = AddLabel(
				container,
				QString(),
				st::aiAgentSettingsLabel);
			status->hide();
			const auto copyLink = container->add(
				object_ptr<Ui::LinkButton>(
					container,
					u"Скопировать ссылку для входа"_q),
				st::aiAgentSettingsPadding,
				style::al_left);
			copyLink->hide();
			const auto url = std::make_shared<QString>();
			copyLink->setClickedCallback([=] {
				QGuiApplication::clipboard()->setText(*url);
			});
			login->setClickedCallback([=] {
				manager->startOAuth(id, [=](QString link) {
					*url = link;
					status->setText(u"Откройте браузер и подтвердите вход. "
						"Окно настроек можно не закрывать."_q);
					status->show();
					copyLink->show();
					File::OpenUrl(link);
				}, crl::guard(box, [=](QString error) {
					if (error.isEmpty()) {
						rebuild();
					} else {
						status->setText(u"Ошибка входа: "_q + error);
						status->show();
					}
				}));
			});
		}
		if (info.oauth == OAuthKind::ChatGPT) {
			AddLabel(
				container,
				u"Нужна подписка ChatGPT Plus, Pro или Business. "
					"Запросы идут через ваш аккаунт, как в Codex CLI."_q,
				st::aiAgentSettingsLabel);
		}
	}

	if (info.apiKey) {
		AddLabel(
			container,
			info.keyOptional
				? u"Ключ API (если нужен)"_q
				: u"Ключ API"_q,
			st::aiAgentSettingsSection);
		fields->apiKey = AddPasswordField(
			container,
			u"sk-..."_q,
			settings ? settings->apiKey : QString());
		if (!info.keysUrl.isEmpty()) {
			const auto link = container->add(
				object_ptr<Ui::LinkButton>(
					container,
					u"Где взять ключ?"_q),
				st::aiAgentSettingsPadding,
				style::al_left);
			const auto keysUrl = info.keysUrl;
			link->setClickedCallback([=] { File::OpenUrl(keysUrl); });
		}
	}

	if (info.editableBaseUrl) {
		AddLabel(container, u"Адрес API"_q, st::aiAgentSettingsSection);
		fields->baseUrl = container->add(
			object_ptr<Ui::InputField>(
				container,
				st::defaultInputField,
				rpl::single(info.defaultBaseUrl),
				config.baseUrl(id)),
			st::aiAgentSettingsFieldPadding);
	}

	AddLabel(container, u"Модель"_q, st::aiAgentSettingsSection);
	fields->model = container->add(
		object_ptr<Ui::InputField>(
			container,
			st::defaultInputField,
			rpl::single(u"Название модели"_q),
			config.model(id)),
		st::aiAgentSettingsFieldPadding);
	const auto flow = container->add(
		object_ptr<FlowLayout>(container),
		st::aiAgentSettingsPadding);
	const auto setModel = [=](const QString &model) {
		if (const auto field = fields->model.data()) {
			field->setText(model);
			field->setCursorPosition(model.size());
		}
	};
	for (const auto &model : info.models) {
		const auto chip = MakeChip(flow, model, st::aiAgentModelChip);
		chip->setClickedCallback([=] { setModel(model); });
		flow->addWidget(chip);
	}
	if (info.format != ApiFormat::OpenAIResponses) {
		const auto more = MakeChip(
			flow,
			u"Все модели…"_q,
			st::aiAgentModelChip);
		flow->addWidget(more);
		const auto menu = std::make_shared<base::unique_qptr<Ui::PopupMenu>>();
		more->setClickedCallback([=] {
			if (fields->apiKey) {
				manager->updateConfig([&](Config &config) {
					config.settings(id).apiKey
						= fields->apiKey->getLastText().trimmed();
					if (fields->baseUrl) {
						config.settings(id).baseUrl
							= fields->baseUrl->getLastText().trimmed();
					}
				});
			}
			manager->fetchModels(id, crl::guard(more, [=](
					QStringList models,
					QString error) {
				*menu = base::make_unique_q<Ui::PopupMenu>(
					more,
					st::defaultPopupMenu);
				if (!error.isEmpty()) {
					(*menu)->addAction(error.left(120), [] {});
				} else {
					const auto filter = fields->model
						? fields->model->getLastText().trimmed()
						: QString();
					auto shown = 0;
					for (const auto &model : models) {
						if (!filter.isEmpty()
							&& !model.contains(filter, Qt::CaseInsensitive)
							&& shown + 1 < models.size()) {
							continue;
						}
						(*menu)->addAction(model, [=] { setModel(model); });
						if (++shown >= 40) {
							break;
						}
					}
					if (!shown) {
						for (const auto &model : models.mid(0, 40)) {
							(*menu)->addAction(model, [=] {
								setModel(model);
							});
						}
					}
				}
				(*menu)->popup(QCursor::pos());
			}));
		});
	}
}

} // namespace

void SettingsBox(not_null<Ui::GenericBox*> box, not_null<Manager*> manager) {
	box->setTitle(rpl::single(u"Настройки ИИ-агента"_q));
	box->setWidth(st::aiAgentSettingsWidth);

	const auto fields = std::make_shared<ProviderFields>();
	const auto selected = std::make_shared<QString>(
		manager->config().provider);

	const auto saveFields = [=] {
		const auto id = *selected;
		manager->updateConfig([&](Config &config) {
			auto &s = config.settings(id);
			if (fields->apiKey) {
				s.apiKey = fields->apiKey->getLastText().trimmed();
			}
			if (fields->baseUrl) {
				s.baseUrl = fields->baseUrl->getLastText().trimmed();
			}
			if (fields->model) {
				s.model = fields->model->getLastText().trimmed();
			}
		});
	};

	const auto content = box->verticalLayout();
	AddSection(content, u"Провайдер"_q);
	const auto providers = content->add(
		object_ptr<FlowLayout>(content),
		st::aiAgentSettingsPadding);
	const auto details = content->add(
		object_ptr<Ui::VerticalLayout>(content));

	const auto rebuild = std::make_shared<Fn<void()>>();
	const auto refreshChips = [=] {
		providers->clearWidgets();
		for (const auto &info : Providers()) {
			const auto active = (info.id == *selected);
			const auto chip = MakeChip(
				providers,
				info.name,
				active
					? st::aiAgentProviderButtonActive
					: st::aiAgentProviderButton);
			const auto id = info.id;
			chip->setClickedCallback([=] {
				if (*selected == id) {
					return;
				}
				saveFields();
				*selected = id;
				manager->updateConfig([&](Config &config) {
					config.provider = id;
				});
				(*rebuild)();
			});
			chip->show();
			providers->addWidget(chip);
		}
		providers->resizeToWidth(providers->width());
	};
	*rebuild = [=] {
		*fields = ProviderFields();
		details->clear();
		refreshChips();
		FillProvider(
			box,
			details,
			manager,
			*selected,
			fields,
			crl::guard(box, [=] {
				crl::on_main(box, [=] { (*rebuild)(); });
			}));
		details->resizeToWidth(content->width());
	};
	(*rebuild)();

	AddSection(content, u"Поведение агента"_q);
	const auto confirm = content->add(
		object_ptr<Ui::Checkbox>(
			content,
			u"Спрашивать подтверждение перед отправкой, удалением "
				"и другими изменениями"_q,
			manager->config().confirmActions,
			st::defaultBoxCheckbox),
		st::aiAgentSettingsPadding);
	AddLabel(
		content,
		u"Дополнительные инструкции для агента"_q,
		st::aiAgentSettingsLabel);
	const auto instructions = content->add(
		object_ptr<Ui::InputField>(
			content,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			rpl::single(u"Например: отвечай кратко, пиши на «ты»"_q),
			manager->config().customInstructions),
		st::aiAgentSettingsFieldPadding);

	AddSection(content, u"Telegram API"_q);
	const auto credentials = Core::CurrentApiCredentials();
	AddLabel(
		content,
		Core::UsingTestApiCredentials()
			? u"Сейчас используется тестовый api_id. Для постоянной "
				"работы получите свой на my.telegram.org."_q
			: (u"Используется api_id "_q + QString::number(credentials.id)),
		st::aiAgentSettingsLabel);
	const auto apiLink = content->add(
		object_ptr<Ui::LinkButton>(content, u"Указать свой api_id…"_q),
		st::aiAgentSettingsPadding,
		style::al_left);
	apiLink->setClickedCallback([=] {
		box->uiShow()->showBox(Box(ApiCredentialsBox));
	});

	box->addButton(rpl::single(u"Сохранить"_q), [=] {
		saveFields();
		manager->updateConfig([&](Config &config) {
			config.confirmActions = confirm->checked();
			config.customInstructions = instructions->getLastText().trimmed();
		});
		box->closeBox();
	});
	box->addButton(rpl::single(u"Отмена"_q), [=] {
		manager->cancelOAuth();
		box->closeBox();
	});
}

void ApiCredentialsBox(not_null<Ui::GenericBox*> box) {
	box->setTitle(rpl::single(u"Свой api_id Telegram"_q));
	const auto content = box->verticalLayout();
	AddLabel(
		content,
		u"Откройте my.telegram.org → API development tools, создайте "
			"приложение и вставьте api_id и api_hash. После сохранения "
			"Agentgram перезапустится."_q,
		st::aiAgentSettingsLabel);
	const auto saved = Core::SavedApiCredentials();
	const auto id = content->add(
		object_ptr<Ui::InputField>(
			content,
			st::defaultInputField,
			rpl::single(u"api_id"_q),
			saved ? QString::number(saved->id) : QString()),
		st::aiAgentSettingsFieldPadding);
	const auto hash = content->add(
		object_ptr<Ui::InputField>(
			content,
			st::defaultInputField,
			rpl::single(u"api_hash"_q),
			saved ? saved->hash : QString()),
		st::aiAgentSettingsFieldPadding);
	const auto link = content->add(
		object_ptr<Ui::LinkButton>(content, u"Открыть my.telegram.org"_q),
		st::aiAgentSettingsPadding,
		style::al_left);
	link->setClickedCallback([] {
		File::OpenUrl(u"https://my.telegram.org/apps"_q);
	});
	box->addButton(rpl::single(u"Сохранить и перезапустить"_q), [=] {
		const auto value = id->getLastText().trimmed().toInt();
		const auto hashValue = hash->getLastText().trimmed();
		if (value <= 0) {
			id->showError();
			return;
		} else if (hashValue.size() < 16) {
			hash->showError();
			return;
		}
		if (Core::SaveApiCredentials({ value, hashValue })) {
			Core::Restart();
		}
	});
	if (saved) {
		box->addLeftButton(rpl::single(u"Сбросить"_q), [=] {
			Core::ClearSavedApiCredentials();
			Core::Restart();
		});
	}
	box->addButton(rpl::single(u"Отмена"_q), [=] { box->closeBox(); });
}

} // namespace AiAgent
