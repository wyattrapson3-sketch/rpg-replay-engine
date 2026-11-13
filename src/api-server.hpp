/*
RPG Replay Engine
Copyright (C) 2026 Rapson Production Group

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

/* Local HTTP control API on 127.0.0.1 (default port 8765).
 * Bitfocus Companion's Generic HTTP module and the control page use it. */
void rpg_api_start(int port);
void rpg_api_stop();
