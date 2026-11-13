/*
RPG Replay Engine
Copyright (C) 2026 Rapson Production Group

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "recorder.hpp"

#include <plugin-support.h>
#include <util/platform.h>

#include <cstring>

namespace {

bool encoder_available(const char *id)
{
	const char *type = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &type); i++) {
		if (type && strcmp(type, id) == 0)
			return true;
	}
	return false;
}

/* Prefer NVENC (texture-based in OBS 31+, jim_nvenc in OBS 30), then x264. */
const char *pick_video_encoder()
{
	static const char *const candidates[] = {"obs_nvenc_h264_tex", "jim_nvenc", "ffmpeg_nvenc", "obs_x264"};
	for (const char *id : candidates) {
		if (encoder_available(id))
			return id;
	}
	return nullptr;
}

obs_data_t *video_encoder_settings(const char *id, int kbps)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "rate_control", "CBR");
	obs_data_set_int(s, "bitrate", kbps);
	/* 1 s keyframe interval and no B-frames: fast, frame-accurate seeking. */
	obs_data_set_int(s, "keyint_sec", 1);
	obs_data_set_int(s, "bf", 0);
	if (strcmp(id, "obs_x264") == 0) {
		obs_data_set_string(s, "preset", "veryfast");
		obs_data_set_string(s, "profile", "high");
		obs_data_set_string(s, "x264opts", "bframes=0");
	} else {
		obs_data_set_string(s, "preset2", "p4");
		obs_data_set_string(s, "tune", "ll");
		obs_data_set_string(s, "multipass", "disabled");
		obs_data_set_string(s, "profile", "high");
		obs_data_set_bool(s, "lookahead", false);
		obs_data_set_bool(s, "psycho_aq", false);
	}
	return s;
}

} // namespace

CameraRecorder::~CameraRecorder()
{
	stop();
}

bool CameraRecorder::start(obs_source_t *source, const std::string &path, int video_kbps, int audio_kbps,
			   const std::string &label, std::string &error)
{
	stop();

	if (!source) {
		error = "camera source not found";
		return false;
	}

	uint32_t width = obs_source_get_width(source);
	uint32_t height = obs_source_get_height(source);
	if (width == 0 || height == 0) {
		error = "source '" + std::string(obs_source_get_name(source)) +
			"' has no video yet (is the camera/NDI/SRT feed connected?)";
		return false;
	}
	/* encoders need even dimensions */
	width &= ~1u;
	height &= ~1u;

	const char *enc_id = pick_video_encoder();
	if (!enc_id) {
		error = "no H.264 encoder available";
		return false;
	}
	encoder_id_ = enc_id;

	weak_source_ = obs_source_get_weak_source(source);

	/* --- video: render just this source into its own video mix --- */
	struct obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	ovi.base_width = width;
	ovi.base_height = height;
	ovi.output_width = width;
	ovi.output_height = height;

	view_ = obs_view_create();
	obs_view_set_source(view_, 0, source);
	video_ = obs_view_add2(view_, &ovi);
	if (!video_) {
		error = "could not create a video mix for the camera";
		release_all();
		return false;
	}

	/* --- audio: this source's own audio, not the program mix --- */
	struct obs_audio_info oai = {};
	obs_get_audio_info(&oai);
	channels_ = get_audio_channels(oai.speakers);
	if (channels_ == 0 || channels_ > MAX_AUDIO_CHANNELS)
		channels_ = 2;

	struct audio_output_info aoi = {};
	std::string audio_name = label + " audio";
	aoi.name = audio_name.c_str();
	aoi.samples_per_sec = oai.samples_per_sec;
	aoi.format = AUDIO_FORMAT_FLOAT_PLANAR;
	aoi.speakers = oai.speakers;
	aoi.input_callback = audio_input_cb;
	aoi.input_param = this;
	if (audio_output_open(&audio_, &aoi) != AUDIO_OUTPUT_SUCCESS) {
		audio_ = nullptr;
		error = "could not open audio output for the camera";
		release_all();
		return false;
	}
	capture_source_ = obs_source_get_ref(source);
	obs_source_add_audio_capture_callback(capture_source_, audio_capture_cb, this);

	/* --- encoders --- */
	obs_data_t *vs = video_encoder_settings(enc_id, video_kbps);
	venc_ = obs_video_encoder_create(enc_id, (label + " video").c_str(), vs, nullptr);
	obs_data_release(vs);
	if (!venc_) {
		error = std::string("could not create encoder ") + enc_id;
		release_all();
		return false;
	}
	obs_encoder_set_video(venc_, video_);

	obs_data_t *as = obs_data_create();
	obs_data_set_int(as, "bitrate", audio_kbps);
	aenc_ = obs_audio_encoder_create("ffmpeg_aac", (label + " aac").c_str(), as, 0, nullptr);
	obs_data_release(as);
	if (!aenc_) {
		error = "could not create AAC encoder";
		release_all();
		return false;
	}
	obs_encoder_set_audio(aenc_, audio_);

	/* --- file output (MPEG-TS so it is readable while recording) --- */
	obs_data_t *os = obs_data_create();
	obs_data_set_string(os, "path", path.c_str());
	output_ = obs_output_create("ffmpeg_muxer", (label + " recorder").c_str(), os, nullptr);
	obs_data_release(os);
	if (!output_) {
		error = "could not create file output";
		release_all();
		return false;
	}
	obs_output_set_video_encoder(output_, venc_);
	obs_output_set_audio_encoder(output_, aenc_, 0);

	if (!obs_output_start(output_)) {
		const char *why = obs_output_get_last_error(output_);
		error = std::string("recording failed to start") + (why ? std::string(": ") + why : "");
		release_all();
		return false;
	}

	obs_log(LOG_INFO, "[%s] recording '%s' %ux%u with %s to %s", label.c_str(), obs_source_get_name(source),
		width, height, enc_id, path.c_str());
	return true;
}

void CameraRecorder::stop()
{
	if (output_ && obs_output_active(output_)) {
		obs_output_stop(output_);
		/* obs_output_stop is asynchronous; wait for the muxer to finish so
		 * the file is complete before encoders/mixes go away. */
		for (int i = 0; i < 500 && obs_output_active(output_); i++)
			os_sleep_ms(10);
		if (obs_output_active(output_)) {
			obs_log(LOG_WARNING, "recorder did not stop in 5 s, forcing");
			obs_output_force_stop(output_);
		}
	}
	release_all();
}

void CameraRecorder::release_all()
{
	if (capture_source_) {
		obs_source_remove_audio_capture_callback(capture_source_, audio_capture_cb, this);
		obs_source_release(capture_source_);
		capture_source_ = nullptr;
	}
	if (output_) {
		obs_output_release(output_);
		output_ = nullptr;
	}
	if (venc_) {
		obs_encoder_release(venc_);
		venc_ = nullptr;
	}
	if (aenc_) {
		obs_encoder_release(aenc_);
		aenc_ = nullptr;
	}
	if (audio_) {
		audio_output_close(audio_);
		audio_ = nullptr;
	}
	if (view_) {
		obs_view_set_source(view_, 0, nullptr);
		obs_view_remove(view_);
		obs_view_destroy(view_);
		view_ = nullptr;
		video_ = nullptr;
	}
	if (weak_source_) {
		obs_weak_source_release(weak_source_);
		weak_source_ = nullptr;
	}
	std::lock_guard<std::mutex> lock(audio_mtx_);
	for (auto &buf : audio_buf_)
		buf.clear();
}

/* Called on the source's thread whenever it produces audio (whether or not
 * its scene is live). Data is already in OBS's float-planar output format. */
void CameraRecorder::audio_capture_cb(void *param, obs_source_t *, const struct audio_data *audio, bool)
{
	auto *self = static_cast<CameraRecorder *>(param);
	if (!audio || audio->frames == 0)
		return;

	std::lock_guard<std::mutex> lock(self->audio_mtx_);
	for (size_t ch = 0; ch < self->channels_; ch++) {
		const float *in = reinterpret_cast<const float *>(audio->data[ch]);
		auto &buf = self->audio_buf_[ch];
		if (in)
			buf.insert(buf.end(), in, in + audio->frames);
		else
			buf.insert(buf.end(), audio->frames, 0.0f);
	}
}

/* Called on the audio thread for every 1024-frame block of our private
 * audio output. Feeds buffered source audio, or silence if none arrived. */
bool CameraRecorder::audio_input_cb(void *param, uint64_t start_ts_in, uint64_t, uint64_t *out_ts, uint32_t mixers,
				    struct audio_output_data *mixes)
{
	auto *self = static_cast<CameraRecorder *>(param);
	*out_ts = start_ts_in;
	if ((mixers & 1) == 0)
		return true;

	std::lock_guard<std::mutex> lock(self->audio_mtx_);

	/* keep latency bounded (~250 ms) if the source runs fast */
	const size_t max_buffered = AUDIO_OUTPUT_FRAMES * 12;
	for (size_t ch = 0; ch < self->channels_; ch++) {
		auto &buf = self->audio_buf_[ch];
		if (buf.size() > max_buffered)
			buf.erase(buf.begin(), buf.begin() + (buf.size() - AUDIO_OUTPUT_FRAMES * 2));
	}

	for (size_t ch = 0; ch < self->channels_; ch++) {
		float *out = mixes[0].data[ch];
		if (!out)
			continue;
		auto &buf = self->audio_buf_[ch];
		const size_t n = buf.size() < AUDIO_OUTPUT_FRAMES ? buf.size() : AUDIO_OUTPUT_FRAMES;
		for (size_t i = 0; i < n; i++)
			out[i] = buf[i];
		for (size_t i = n; i < AUDIO_OUTPUT_FRAMES; i++)
			out[i] = 0.0f;
		buf.erase(buf.begin(), buf.begin() + n);
	}
	return true;
}
