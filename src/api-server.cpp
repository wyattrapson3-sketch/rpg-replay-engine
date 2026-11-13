/*
RPG Replay Engine
Copyright (C) 2026 Rapson Production Group

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "api-server.hpp"
#include "engine.hpp"

#include <httplib.h>

#include <obs-module.h>
#include <plugin-support.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <memory>
#include <cstring>
#include <string>
#include <thread>

namespace {

using rpg::Engine;

std::unique_ptr<httplib::Server> g_server;
std::thread g_thread;
int g_port = 8765;

std::string json_result(bool ok, const std::string &error = std::string())
{
	obs_data_t *d = obs_data_create();
	obs_data_set_bool(d, "ok", ok);
	if (!error.empty())
		obs_data_set_string(d, "error", error.c_str());
	std::string s = obs_data_get_json(d);
	obs_data_release(d);
	return s;
}

void reply(httplib::Response &res, bool ok, const std::string &error = std::string())
{
	res.status = ok ? 200 : 400;
	res.set_content(json_result(ok, error), "application/json");
}

/* Only this computer may control replay, and web pages from other sites
 * must not be able to send commands through the browser. */
bool request_allowed(const httplib::Request &req)
{
	const std::string host = req.get_header_value("Host");
	const std::string port = std::to_string(g_port);
	if (!host.empty() && host != "127.0.0.1:" + port && host != "localhost:" + port && host != "127.0.0.1" &&
	    host != "localhost")
		return false;
	const std::string origin = req.get_header_value("Origin");
	if (!origin.empty() && origin != "http://127.0.0.1:" + port && origin != "http://localhost:" + port)
		return false;
	return true;
}

/* Lists sources that can be recorded as cameras (anything with video). */
std::string sources_json()
{
	obs_data_t *d = obs_data_create();
	obs_data_array_t *arr = obs_data_array_create();
	obs_enum_sources(
		[](void *param, obs_source_t *src) {
			auto *list = static_cast<obs_data_array_t *>(param);
			const uint32_t flags = obs_source_get_output_flags(src);
			const char *id = obs_source_get_unversioned_id(src);
			if ((flags & OBS_SOURCE_VIDEO) == 0 || (id && strcmp(id, "rpg_replay_player") == 0))
				return true;
			obs_data_t *item = obs_data_create();
			obs_data_set_string(item, "name", obs_source_get_name(src));
			obs_data_set_string(item, "kind", id ? id : "");
			obs_data_set_bool(item, "audio", (flags & OBS_SOURCE_AUDIO) != 0);
			obs_data_array_push_back(list, item);
			obs_data_release(item);
			return true;
		},
		arr);
	obs_data_set_array(d, "sources", arr);
	obs_data_array_release(arr);
	std::string s = obs_data_get_json(d);
	obs_data_release(d);
	return s;
}

std::string config_json()
{
	rpg::EngineConfig cfg = Engine::get().config();
	obs_data_t *d = obs_data_create();
	obs_data_array_t *cams = obs_data_array_create();
	for (const auto &name : cfg.camera_sources) {
		obs_data_t *c = obs_data_create();
		obs_data_set_string(c, "source", name.c_str());
		obs_data_array_push_back(cams, c);
		obs_data_release(c);
	}
	obs_data_set_array(d, "cameras", cams);
	obs_data_array_release(cams);
	obs_data_set_string(d, "record_dir", cfg.record_dir.c_str());
	obs_data_set_int(d, "video_bitrate_kbps", cfg.video_bitrate_kbps);
	obs_data_set_int(d, "audio_bitrate_kbps", cfg.audio_bitrate_kbps);
	std::string s = obs_data_get_json(d);
	obs_data_release(d);
	return s;
}

bool apply_config(const std::string &body, std::string &error)
{
	obs_data_t *d = obs_data_create_from_json(body.c_str());
	if (!d) {
		error = "body is not valid JSON";
		return false;
	}
	if (Engine::get().recording()) {
		obs_data_release(d);
		error = "stop recording before changing setup";
		return false;
	}
	rpg::EngineConfig cfg = Engine::get().config();
	obs_data_array_t *cams = obs_data_get_array(d, "cameras");
	for (size_t i = 0; cams && i < obs_data_array_count(cams) && i < rpg::kMaxCameras; i++) {
		obs_data_t *c = obs_data_array_item(cams, i);
		cfg.camera_sources[i] = obs_data_get_string(c, "source");
		obs_data_release(c);
	}
	obs_data_array_release(cams);
	if (obs_data_has_user_value(d, "record_dir"))
		cfg.record_dir = obs_data_get_string(d, "record_dir");
	if (obs_data_has_user_value(d, "video_bitrate_kbps"))
		cfg.video_bitrate_kbps = (int)obs_data_get_int(d, "video_bitrate_kbps");
	if (obs_data_has_user_value(d, "audio_bitrate_kbps"))
		cfg.audio_bitrate_kbps = (int)obs_data_get_int(d, "audio_bitrate_kbps");
	obs_data_release(d);
	Engine::get().set_config(cfg);
	return true;
}

std::string control_page()
{
	char *path = obs_module_file("control.html");
	std::string html;
	if (path) {
		char *text = os_quick_read_utf8_file(path);
		if (text) {
			html = text;
			bfree(text);
		}
		bfree(path);
	}
	if (html.empty())
		html = "<!doctype html><title>RPG Replay</title><p>control.html is missing from the plugin's data folder.";
	return html;
}

/* Commands are POST only: a GET could be triggered by any web page (e.g. an
 * <img> tag), while a cross-site POST always carries an Origin header that
 * request_allowed() rejects. */
template<typename Fn> void command(httplib::Server &srv, const std::string &pattern, Fn fn)
{
	srv.Post(pattern, [fn](const httplib::Request &req, httplib::Response &res) { fn(req, res); });
}

void build_routes(httplib::Server &srv)
{
	srv.set_pre_routing_handler([](const httplib::Request &req, httplib::Response &res) {
		if (request_allowed(req))
			return httplib::Server::HandlerResponse::Unhandled;
		res.status = 403;
		res.set_content(json_result(false, "forbidden"), "application/json");
		return httplib::Server::HandlerResponse::Handled;
	});

	srv.Get("/", [](const httplib::Request &, httplib::Response &res) {
		res.set_content(control_page(), "text/html; charset=utf-8");
	});
	srv.Get("/api/status", [](const httplib::Request &, httplib::Response &res) {
		res.set_content(Engine::get().status_json(), "application/json");
	});
	srv.Get("/api/sources", [](const httplib::Request &, httplib::Response &res) {
		res.set_content(sources_json(), "application/json");
	});
	srv.Get("/api/config", [](const httplib::Request &, httplib::Response &res) {
		res.set_content(config_json(), "application/json");
	});
	srv.Post("/api/config", [](const httplib::Request &req, httplib::Response &res) {
		std::string err;
		bool ok = apply_config(req.body, err);
		reply(res, ok, err);
	});

	command(srv, "/api/record/start", [](const httplib::Request &, httplib::Response &res) {
		std::string err;
		bool ok = Engine::get().start_recording(err);
		reply(res, ok, err);
	});
	command(srv, "/api/record/stop", [](const httplib::Request &, httplib::Response &res) {
		Engine::get().stop_recording();
		reply(res, true);
	});
	command(srv, "/api/record/toggle", [](const httplib::Request &, httplib::Response &res) {
		if (Engine::get().recording()) {
			Engine::get().stop_recording();
			reply(res, true);
		} else {
			std::string err;
			bool ok = Engine::get().start_recording(err);
			reply(res, ok, err);
		}
	});
	command(srv, "/api/live", [](const httplib::Request &, httplib::Response &res) {
		Engine::get().cmd_live();
		reply(res, true);
	});
	command(srv, "/api/play", [](const httplib::Request &, httplib::Response &res) {
		Engine::get().cmd_play();
		reply(res, true);
	});
	command(srv, "/api/pause", [](const httplib::Request &, httplib::Response &res) {
		Engine::get().cmd_pause();
		reply(res, true);
	});
	command(srv, R"(/api/camera/(\d+))", [](const httplib::Request &req, httplib::Response &res) {
		bool ok = Engine::get().cmd_camera(std::stoi(req.matches[1].str()));
		reply(res, ok, ok ? "" : "camera must be 1-4");
	});
	command(srv, R"(/api/speed/(\d+))", [](const httplib::Request &req, httplib::Response &res) {
		const int pct = std::stoi(req.matches[1].str());
		bool ok = Engine::get().cmd_speed(pct / 100.0);
		reply(res, ok, ok ? "" : "speed must be 1-100");
	});
	command(srv, R"(/api/audio-mode/(\w+))", [](const httplib::Request &req, httplib::Response &res) {
		const std::string name = req.matches[1].str();
		if (name == "next") {
			Engine::get().cmd_audio_mode_next();
			reply(res, true);
			return;
		}
		rpg::AudioMode mode;
		if (!rpg::audio_mode_from_name(name, mode)) {
			reply(res, false, "audio mode must be follow, preserve, mute or next");
			return;
		}
		Engine::get().cmd_audio_mode(mode);
		reply(res, true);
	});
	command(srv, R"(/api/step/frames/(-?\d+))", [](const httplib::Request &req, httplib::Response &res) {
		Engine::get().cmd_step_frames(std::stoi(req.matches[1].str()));
		reply(res, true);
	});
	command(srv, R"(/api/step/seconds/(-?\d+(?:\.\d+)?))", [](const httplib::Request &req, httplib::Response &res) {
		Engine::get().cmd_step_seconds(std::stod(req.matches[1].str()));
		reply(res, true);
	});
	command(srv, R"(/api/seek/(\d+(?:\.\d+)?))", [](const httplib::Request &req, httplib::Response &res) {
		Engine::get().cmd_seek(std::stod(req.matches[1].str()));
		reply(res, true);
	});
}

} // namespace

void rpg_api_start(int port)
{
	rpg_api_stop();
	g_port = port;
	g_server = std::make_unique<httplib::Server>();
	build_routes(*g_server);
	httplib::Server *srv = g_server.get();
	g_thread = std::thread([srv, port] {
		obs_log(LOG_INFO, "Control API listening on http://127.0.0.1:%d/", port);
		if (!srv->listen("127.0.0.1", port))
			obs_log(LOG_ERROR, "Control API could not listen on 127.0.0.1:%d (port in use?)", port);
	});
}

void rpg_api_stop()
{
	if (g_server)
		g_server->stop();
	if (g_thread.joinable())
		g_thread.join();
	g_server.reset();
}
