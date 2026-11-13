/*
RPG Replay Engine
Copyright (C) 2026 Rapson Production Group

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

class CameraRecorder;

namespace rpg {

constexpr int kMaxCameras = 4;
constexpr double kReplayFps = 60.0;     // replay frame step size (1080p60 shows)
constexpr double kLiveDelaySec = 0.35;  // how far behind the record head "Live" plays

enum class AudioMode { FollowSpeed = 0, PreservePitch = 1, Mute = 2 };

const char *audio_mode_name(AudioMode mode);
bool audio_mode_from_name(const std::string &name, AudioMode &out);

/* What the operator asked the replay output to do. The player thread reads
 * this; every change bumps `seq` so the player knows to react. */
struct PlaybackState {
	uint64_t seq = 0;
	int camera = 1;           // 1..kMaxCameras
	bool playing = false;
	bool live = false;
	double speed = 1.0;       // 0.25, 0.5, 1.0
	AudioMode audio_mode = AudioMode::FollowSpeed;
	bool seek_pending = false;
	double seek_to = 0.0;     // session seconds
};

struct CameraFile {
	std::string source_name;  // OBS source that was recorded
	std::string path;         // .ts file on disk ("" if not recorded)
};

/* One recording session: every camera recorded on a shared clock. */
struct SessionInfo {
	bool valid = false;
	std::string dir;
	uint64_t start_ns = 0;    // os_gettime_ns() when recording started
	uint64_t stop_ns = 0;     // 0 while still recording
	std::array<CameraFile, kMaxCameras> cameras;
};

struct EngineConfig {
	std::array<std::string, kMaxCameras> camera_sources;
	std::string record_dir;
	int video_bitrate_kbps = 50000;
	int audio_bitrate_kbps = 192;
	int api_port = 8765;
};

class Engine {
public:
	static Engine &get();

	void load_config();
	void save_config();
	EngineConfig config();
	void set_config(const EngineConfig &cfg);

	/* Recording */
	bool start_recording(std::string &error);
	void stop_recording();
	bool recording();
	SessionInfo session();
	double head_seconds();   // newest recorded moment, in session seconds

	/* Playback commands (from the API / hotkeys) */
	void cmd_live();
	void cmd_play();
	void cmd_pause();
	bool cmd_camera(int camera);
	bool cmd_speed(double speed);
	void cmd_audio_mode(AudioMode mode);
	void cmd_audio_mode_next();
	void cmd_step_frames(int frames);
	void cmd_step_seconds(double seconds);
	void cmd_seek(double seconds);

	/* Player side */
	PlaybackState playback();
	bool wait_playback_change(uint64_t seen_seq, int timeout_ms);
	void consume_seek(uint64_t seq);
	void report_position(double seconds);
	double position();
	void set_player_status(const std::string &status);

	/* Status as JSON for the control page / Companion */
	std::string status_json();

	void shutdown();

private:
	Engine();
	~Engine();
	void bump_locked();
	void request_seek_locked(double seconds);

	std::mutex mtx_;
	std::condition_variable cv_;
	EngineConfig cfg_;
	PlaybackState pb_;
	SessionInfo session_;
	std::array<std::unique_ptr<CameraRecorder>, kMaxCameras> recorders_;
	bool recording_ = false;
	std::string last_error_;
	std::string player_status_ = "No replay player source in OBS yet";
	std::atomic<double> position_{0.0};
};

} // namespace rpg
