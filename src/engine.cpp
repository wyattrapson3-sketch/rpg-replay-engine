/*
RPG Replay Engine
Copyright (C) 2026 Rapson Production Group

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "engine.hpp"
#include "recorder.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>
#include <util/bmem.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>

namespace rpg {

const char *audio_mode_name(AudioMode mode)
{
	switch (mode) {
	case AudioMode::PreservePitch:
		return "preserve";
	case AudioMode::Mute:
		return "mute";
	case AudioMode::FollowSpeed:
	default:
		return "follow";
	}
}

bool audio_mode_from_name(const std::string &name, AudioMode &out)
{
	if (name == "follow") {
		out = AudioMode::FollowSpeed;
	} else if (name == "preserve") {
		out = AudioMode::PreservePitch;
	} else if (name == "mute") {
		out = AudioMode::Mute;
	} else {
		return false;
	}
	return true;
}

namespace {

std::string default_record_dir()
{
#ifdef _WIN32
	const char *home = getenv("USERPROFILE");
#else
	const char *home = getenv("HOME");
#endif
	std::string base = home ? home : ".";
	return base + "/Videos/RPG Replay";
}

std::string config_file_path()
{
	char *p = obs_module_config_path("config.json");
	std::string path = p ? p : "";
	bfree(p);
	return path;
}

std::string timestamp_folder_name()
{
	std::time_t t = std::time(nullptr);
	std::tm tmv{};
#ifdef _WIN32
	localtime_s(&tmv, &t);
#else
	localtime_r(&t, &tmv);
#endif
	char buf[64];
	std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", &tmv);
	return buf;
}

} // namespace

Engine &Engine::get()
{
	static Engine instance;
	return instance;
}

Engine::Engine()
{
	cfg_.record_dir = default_record_dir();
}

Engine::~Engine() = default;

void Engine::load_config()
{
	std::lock_guard<std::mutex> lock(mtx_);
	std::string path = config_file_path();
	obs_data_t *d = obs_data_create_from_json_file_safe(path.c_str(), "bak");
	if (!d)
		return;
	obs_data_array_t *cams = obs_data_get_array(d, "cameras");
	for (size_t i = 0; cams && i < obs_data_array_count(cams) && i < kMaxCameras; i++) {
		obs_data_t *c = obs_data_array_item(cams, i);
		cfg_.camera_sources[i] = obs_data_get_string(c, "source");
		obs_data_release(c);
	}
	obs_data_array_release(cams);
	const char *dir = obs_data_get_string(d, "record_dir");
	if (dir && *dir)
		cfg_.record_dir = dir;
	if (obs_data_has_user_value(d, "video_bitrate_kbps"))
		cfg_.video_bitrate_kbps = (int)obs_data_get_int(d, "video_bitrate_kbps");
	if (obs_data_has_user_value(d, "audio_bitrate_kbps"))
		cfg_.audio_bitrate_kbps = (int)obs_data_get_int(d, "audio_bitrate_kbps");
	if (obs_data_has_user_value(d, "api_port"))
		cfg_.api_port = (int)obs_data_get_int(d, "api_port");
	AudioMode m;
	if (audio_mode_from_name(obs_data_get_string(d, "audio_mode"), m))
		pb_.audio_mode = m;
	obs_data_release(d);
}

void Engine::save_config()
{
	EngineConfig cfg;
	AudioMode mode;
	{
		std::lock_guard<std::mutex> lock(mtx_);
		cfg = cfg_;
		mode = pb_.audio_mode;
	}
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
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
	obs_data_set_int(d, "api_port", cfg.api_port);
	obs_data_set_string(d, "audio_mode", audio_mode_name(mode));
	obs_data_save_json_safe(d, config_file_path().c_str(), "tmp", "bak");
	obs_data_release(d);
}

EngineConfig Engine::config()
{
	std::lock_guard<std::mutex> lock(mtx_);
	return cfg_;
}

void Engine::set_config(const EngineConfig &cfg)
{
	{
		std::lock_guard<std::mutex> lock(mtx_);
		cfg_ = cfg;
		cfg_.video_bitrate_kbps = std::clamp(cfg_.video_bitrate_kbps, 5000, 150000);
		cfg_.audio_bitrate_kbps = std::clamp(cfg_.audio_bitrate_kbps, 96, 320);
		if (cfg_.record_dir.empty())
			cfg_.record_dir = default_record_dir();
	}
	save_config();
}

/* ------------------------------------------------------------------ */
/* Recording                                                            */

bool Engine::start_recording(std::string &error)
{
	EngineConfig cfg = config();
	{
		std::lock_guard<std::mutex> lock(mtx_);
		if (recording_)
			return true;
	}

	bool any = false;
	for (const auto &name : cfg.camera_sources)
		any = any || !name.empty();
	if (!any) {
		error = "no cameras assigned; pick sources for Camera 1-4 in Setup";
		return false;
	}

	SessionInfo s;
	s.dir = cfg.record_dir + "/" + timestamp_folder_name();
	if (os_mkdirs(s.dir.c_str()) == MKDIR_ERROR) {
		error = "cannot create recording folder " + s.dir;
		return false;
	}

	std::array<std::unique_ptr<CameraRecorder>, kMaxCameras> recs;
	std::string first_error;
	int started = 0;
	s.start_ns = os_gettime_ns();
	for (int i = 0; i < kMaxCameras; i++) {
		const std::string &name = cfg.camera_sources[i];
		if (name.empty())
			continue;
		obs_source_t *src = obs_get_source_by_name(name.c_str());
		std::string label = "RPG Replay Cam " + std::to_string(i + 1);
		std::string path = s.dir + "/cam" + std::to_string(i + 1) + ".ts";
		auto rec = std::make_unique<CameraRecorder>();
		std::string err;
		if (rec->start(src, path, cfg.video_bitrate_kbps, cfg.audio_bitrate_kbps, label, err)) {
			s.cameras[i].source_name = name;
			s.cameras[i].path = path;
			recs[i] = std::move(rec);
			started++;
		} else {
			obs_log(LOG_WARNING, "Camera %d (%s): %s", i + 1, name.c_str(), err.c_str());
			if (first_error.empty())
				first_error = "Camera " + std::to_string(i + 1) + ": " + err;
		}
		obs_source_release(src);
	}

	if (started == 0) {
		error = first_error.empty() ? "no camera could start recording" : first_error;
		return false;
	}

	s.valid = true;
	{
		std::lock_guard<std::mutex> lock(mtx_);
		recorders_ = std::move(recs);
		session_ = s;
		recording_ = true;
		last_error_ = first_error; // partial start is reported but not fatal
		/* jump the replay output to live on the new session */
		pb_.live = true;
		pb_.playing = true;
		pb_.speed = 1.0;
		request_seek_locked(0.0);
		bump_locked();
	}
	obs_log(LOG_INFO, "Recording session started: %s (%d camera%s)", s.dir.c_str(), started,
		started == 1 ? "" : "s");
	return true;
}

void Engine::stop_recording()
{
	std::array<std::unique_ptr<CameraRecorder>, kMaxCameras> recs;
	{
		std::lock_guard<std::mutex> lock(mtx_);
		if (!recording_)
			return;
		recs = std::move(recorders_);
		recording_ = false;
		session_.stop_ns = os_gettime_ns();
		if (pb_.live) {
			pb_.live = false;
			pb_.playing = false;
			bump_locked();
		}
	}
	for (auto &r : recs) {
		if (r)
			r->stop();
	}
	obs_log(LOG_INFO, "Recording session stopped");
}

bool Engine::recording()
{
	std::lock_guard<std::mutex> lock(mtx_);
	return recording_;
}

SessionInfo Engine::session()
{
	std::lock_guard<std::mutex> lock(mtx_);
	return session_;
}

double Engine::head_seconds()
{
	std::lock_guard<std::mutex> lock(mtx_);
	if (!session_.valid)
		return 0.0;
	uint64_t end = session_.stop_ns ? session_.stop_ns : os_gettime_ns();
	return end > session_.start_ns ? (double)(end - session_.start_ns) / 1e9 : 0.0;
}

/* ------------------------------------------------------------------ */
/* Playback commands                                                    */

void Engine::bump_locked()
{
	pb_.seq++;
	cv_.notify_all();
}

void Engine::request_seek_locked(double seconds)
{
	pb_.seek_pending = true;
	pb_.seek_to = std::max(0.0, seconds);
}

void Engine::cmd_live()
{
	double head = head_seconds();
	std::lock_guard<std::mutex> lock(mtx_);
	pb_.live = true;
	pb_.playing = true;
	pb_.speed = 1.0;
	request_seek_locked(head - kLiveDelaySec);
	bump_locked();
}

void Engine::cmd_play()
{
	std::lock_guard<std::mutex> lock(mtx_);
	if (pb_.playing)
		return;
	pb_.playing = true;
	bump_locked();
}

void Engine::cmd_pause()
{
	std::lock_guard<std::mutex> lock(mtx_);
	pb_.playing = false;
	pb_.live = false;
	bump_locked();
}

bool Engine::cmd_camera(int camera)
{
	if (camera < 1 || camera > kMaxCameras)
		return false;
	std::lock_guard<std::mutex> lock(mtx_);
	pb_.camera = camera;
	/* same moment, different angle */
	request_seek_locked(position_.load());
	bump_locked();
	return true;
}

bool Engine::cmd_speed(double speed)
{
	if (!(speed > 0.0 && speed <= 1.0))
		return false;
	std::lock_guard<std::mutex> lock(mtx_);
	pb_.speed = speed;
	if (speed < 1.0)
		pb_.live = false; // slow motion always falls behind live
	bump_locked();
	return true;
}

void Engine::cmd_audio_mode(AudioMode mode)
{
	{
		std::lock_guard<std::mutex> lock(mtx_);
		pb_.audio_mode = mode;
		bump_locked();
	}
	save_config();
}

void Engine::cmd_audio_mode_next()
{
	AudioMode next;
	{
		std::lock_guard<std::mutex> lock(mtx_);
		next = static_cast<AudioMode>((static_cast<int>(pb_.audio_mode) + 1) % 3);
	}
	cmd_audio_mode(next);
}

void Engine::cmd_step_frames(int frames)
{
	std::lock_guard<std::mutex> lock(mtx_);
	pb_.playing = false;
	pb_.live = false;
	double base = pb_.seek_pending ? pb_.seek_to : position_.load();
	request_seek_locked(base + frames / kReplayFps);
	bump_locked();
}

void Engine::cmd_step_seconds(double seconds)
{
	std::lock_guard<std::mutex> lock(mtx_);
	pb_.live = false;
	double base = pb_.seek_pending ? pb_.seek_to : position_.load();
	request_seek_locked(base + seconds);
	bump_locked();
}

void Engine::cmd_seek(double seconds)
{
	std::lock_guard<std::mutex> lock(mtx_);
	pb_.live = false;
	request_seek_locked(seconds);
	bump_locked();
}

/* ------------------------------------------------------------------ */
/* Player side                                                          */

PlaybackState Engine::playback()
{
	std::lock_guard<std::mutex> lock(mtx_);
	return pb_;
}

bool Engine::wait_playback_change(uint64_t seen_seq, int timeout_ms)
{
	std::unique_lock<std::mutex> lock(mtx_);
	return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return pb_.seq != seen_seq; });
}

void Engine::consume_seek(uint64_t seq)
{
	std::lock_guard<std::mutex> lock(mtx_);
	/* only clear if no newer seek arrived meanwhile */
	if (pb_.seq == seq)
		pb_.seek_pending = false;
}

void Engine::report_position(double seconds)
{
	position_.store(seconds);
}

double Engine::position()
{
	return position_.load();
}

void Engine::set_player_status(const std::string &status)
{
	std::lock_guard<std::mutex> lock(mtx_);
	player_status_ = status;
}

/* ------------------------------------------------------------------ */

std::string Engine::status_json()
{
	EngineConfig cfg = config();
	double head = head_seconds();
	obs_data_t *d = obs_data_create();
	{
		std::lock_guard<std::mutex> lock(mtx_);
		obs_data_set_bool(d, "recording", recording_);
		obs_data_set_string(d, "session_dir", session_.dir.c_str());
		obs_data_set_double(d, "head", head);
		obs_data_set_double(d, "position", position_.load());
		obs_data_set_int(d, "camera", pb_.camera);
		obs_data_set_bool(d, "playing", pb_.playing);
		obs_data_set_bool(d, "live", pb_.live);
		obs_data_set_int(d, "speed", (int)std::lround(pb_.speed * 100.0));
		obs_data_set_string(d, "audio_mode", audio_mode_name(pb_.audio_mode));
		obs_data_set_string(d, "last_error", last_error_.c_str());
		obs_data_set_string(d, "player_status", player_status_.c_str());

		obs_data_array_t *cams = obs_data_array_create();
		for (int i = 0; i < kMaxCameras; i++) {
			obs_data_t *c = obs_data_create();
			obs_data_set_int(c, "camera", i + 1);
			obs_data_set_string(c, "source", cfg.camera_sources[i].c_str());
			obs_data_set_bool(c, "recorded", !session_.cameras[i].path.empty());
			obs_data_set_bool(c, "recording", recorders_[i] && recorders_[i]->active());
			obs_data_set_string(c, "encoder", recorders_[i] ? recorders_[i]->encoder_id().c_str() : "");
			obs_data_array_push_back(cams, c);
			obs_data_release(c);
		}
		obs_data_set_array(d, "cameras", cams);
		obs_data_array_release(cams);
	}
	std::string json = obs_data_get_json(d);
	obs_data_release(d);
	return json;
}

void Engine::shutdown()
{
	stop_recording();
	save_config();
}

} // namespace rpg
