/*
RPG Replay Engine
Copyright (C) 2026 Rapson Production Group

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <plugin-support.h>

#include "api-server.hpp"
#include "engine.hpp"
#include "player.hpp"

#ifdef RPG_HAVE_FRONTEND
#include <obs-frontend-api.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include <string>
#include <cstdlib>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "RPG Replay Engine: multi-camera instant replay with slow motion";
}

namespace {

std::string control_url()
{
	return "http://127.0.0.1:" + std::to_string(rpg::Engine::get().config().api_port) + "/";
}

#ifdef RPG_HAVE_FRONTEND
void open_control_page(void *)
{
	const std::string url = control_url();
#ifdef _WIN32
	ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
	std::string cmd = "open '" + url + "'";
	(void)system(cmd.c_str());
#else
	std::string cmd = "xdg-open '" + url + "' >/dev/null 2>&1 &";
	(void)system(cmd.c_str());
#endif
}

void frontend_event(enum obs_frontend_event event, void *)
{
	/* Stop and finalize recordings before OBS tears down outputs. */
	if (event == OBS_FRONTEND_EVENT_EXIT)
		rpg::Engine::get().shutdown();
}
#endif

} // namespace

bool obs_module_load(void)
{
	rpg::Engine::get().load_config();
	rpg_register_player_source();
	rpg_api_start(rpg::Engine::get().config().api_port);

#ifdef RPG_HAVE_FRONTEND
	obs_frontend_add_event_callback(frontend_event, nullptr);
	obs_frontend_add_tools_menu_item(obs_module_text("RPGReplay.OpenControl"), open_control_page, nullptr);
#endif

	obs_log(LOG_INFO, "RPG Replay Engine loaded (version %s). Control page: %s", PLUGIN_VERSION,
		control_url().c_str());
	return true;
}

void obs_module_unload(void)
{
#ifdef RPG_HAVE_FRONTEND
	obs_frontend_remove_event_callback(frontend_event, nullptr);
#endif
	rpg_api_stop();
	rpg::Engine::get().shutdown();
	obs_log(LOG_INFO, "RPG Replay Engine unloaded");
}
