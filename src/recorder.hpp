/*
RPG Replay Engine
Copyright (C) 2026 Rapson Production Group

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <obs.h>
#include <media-io/audio-io.h>

#include <deque>
#include <mutex>
#include <string>

/* Records one OBS source (video + that source's own audio) to an MPEG-TS
 * file. MPEG-TS can be read while it is still being written, which is what
 * lets the replay player scrub the live recording.
 *
 *   source -> private obs_view -> video encoder (NVENC, 1 s GOP, no B-frames)
 *   source audio capture -> private audio_t -> AAC encoder
 *   both -> ffmpeg_muxer output -> camN.ts
 */
class CameraRecorder {
public:
	CameraRecorder() = default;
	~CameraRecorder();
	CameraRecorder(const CameraRecorder &) = delete;
	CameraRecorder &operator=(const CameraRecorder &) = delete;

	bool start(obs_source_t *source, const std::string &path, int video_kbps, int audio_kbps,
		   const std::string &label, std::string &error);
	void stop();
	bool active() const { return output_ != nullptr; }
	const std::string &encoder_id() const { return encoder_id_; }

private:
	static void audio_capture_cb(void *param, obs_source_t *source, const struct audio_data *audio, bool muted);
	static bool audio_input_cb(void *param, uint64_t start_ts_in, uint64_t end_ts_in, uint64_t *out_ts,
				   uint32_t mixers, struct audio_output_data *mixes);
	void release_all();

	obs_weak_source_t *weak_source_ = nullptr;
	obs_source_t *capture_source_ = nullptr; // source our audio callback is attached to
	obs_view_t *view_ = nullptr;
	video_t *video_ = nullptr;
	audio_t *audio_ = nullptr;
	obs_encoder_t *venc_ = nullptr;
	obs_encoder_t *aenc_ = nullptr;
	obs_output_t *output_ = nullptr;
	std::string encoder_id_;

	std::mutex audio_mtx_;
	size_t channels_ = 2;
	std::deque<float> audio_buf_[MAX_AUDIO_CHANNELS];
};
