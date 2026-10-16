/*
This file is part of Agentgram, a fork of Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/layers/generic_box.h"

namespace AiAgent {

class Manager;

void SettingsBox(not_null<Ui::GenericBox*> box, not_null<Manager*> manager);

void ApiCredentialsBox(not_null<Ui::GenericBox*> box);

} // namespace AiAgent
