/*
RPG Replay Engine
Copyright (C) 2026 Rapson Production Group

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

/* "RPG Replay Player" OBS source.
 *
 * Reads the camera recordings the engine is writing (MPEG-TS, still
 * growing), decodes them with FFmpeg and outputs video + audio into OBS at
 * the requested playhead and speed. Put this source in your Replay scene.
 */

#include "engine.hpp"
#include "player.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>
#include <media-io/video-io.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>

namespace {

using rpg::AudioMode;
using rpg::Engine;
using rpg::PlaybackState;

constexpr double kFrameDur = 1.0 / rpg::kReplayFps;

std::string av_err(int code)
{
	char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
	av_strerror(code, buf, sizeof(buf));
	return buf;
}

class ReplayPlayer {
public:
	explicit ReplayPlayer(obs_source_t *source) : source_(source)
	{
		thread_ = std::thread([this] { run(); });
	}

	~ReplayPlayer()
	{
		stop_ = true;
		if (thread_.joinable())
			thread_.join();
		close_file();
	}

private:
	enum class Read { Video, Eof, Error };

	/* ---------------- main loop ---------------- */

	void run()
	{
		Engine &eng = Engine::get();
		uint64_t seen_seq = UINT64_MAX;
		std::string cur_path;

		while (!stop_) {
			PlaybackState st = eng.playback();
			rpg::SessionInfo ses = eng.session();
			const int cam = std::clamp(st.camera, 1, rpg::kMaxCameras);
			const std::string path = ses.valid ? ses.cameras[cam - 1].path : std::string();

			if (path.empty()) {
				if (fmt_)
					close_file();
				cur_path.clear();
				eng.set_player_status(ses.valid ? "Camera " + std::to_string(cam) +
									  " was not recorded in this session"
								: "Waiting for a recording (press REC)");
				eng.wait_playback_change(seen_seq, 250);
				continue;
			}

			bool force_seek = false;
			if (path != cur_path) {
				close_file();
				if (!open_file(path)) {
					/* file may not have enough data yet right after REC */
					os_sleep_ms(200);
					continue;
				}
				cur_path = path;
				force_seek = true;
				eng.set_player_status("Playing camera " + std::to_string(cam));
			}

			if (st.seq != seen_seq || force_seek) {
				const bool changed = st.seq != seen_seq;
				seen_seq = st.seq;
				double target = st.seek_pending ? st.seek_to : eng.position();
				if (st.seek_pending || force_seek) {
					const double head = eng.head_seconds();
					target = std::clamp(target, 0.0, std::max(0.0, head - 0.1));
					seek_and_show(target);
					if (st.seek_pending)
						eng.consume_seek(st.seq);
				}
				if (changed || force_seek)
					reanchor(st.speed, st.audio_mode);
			}

			if (!st.playing) {
				eng.wait_playback_change(seen_seq, 100);
				continue;
			}

			/* Live: if playback has fallen behind the record head, catch up. */
			if (st.live && eng.head_seconds() - position_ > 1.0) {
				seek_and_show(eng.head_seconds() - rpg::kLiveDelaySec);
				reanchor(st.speed, st.audio_mode);
			}

			AVFrame *frame = nullptr;
			Read r = read_next_video(&frame);
			if (r == Read::Eof) {
				/* reached the end of what has been written so far */
				clear_eof();
				eng.wait_playback_change(seen_seq, eng.recording() ? 5 : 100);
				continue;
			}
			if (r == Read::Error) {
				eng.set_player_status("Decode error; reopening file");
				close_file();
				cur_path.clear();
				os_sleep_ms(200);
				continue;
			}

			const double t = video_time(frame);
			uint64_t due = due_ns(t);
			uint64_t now = os_gettime_ns();
			if (due + 100000000ULL < now) {
				/* more than 100 ms late (slow seek / hiccup): restart timing here */
				anchor_wall_ = now;
				anchor_t_ = t;
				due = now;
			}

			/* sleep until the frame is due, but react at once to commands */
			bool interrupted = false;
			while (!stop_) {
				now = os_gettime_ns();
				if (now >= due)
					break;
				const uint64_t remain_ms = (due - now) / 1000000ULL;
				if (remain_ms > 2) {
					if (eng.wait_playback_change(seen_seq, (int)std::min<uint64_t>(remain_ms - 1, 50))) {
						interrupted = true;
						break;
					}
				} else {
					os_sleepto_ns(due);
					break;
				}
			}
			if (interrupted)
				continue; // handled at the top of the loop

			output_video(frame);
			position_ = t;
			eng.report_position(t);
		}
	}

	/* ---------------- timing ---------------- */

	void reanchor(double speed, AudioMode mode)
	{
		speed_ = speed > 0.0 ? speed : 1.0;
		anchor_wall_ = os_gettime_ns();
		anchor_t_ = position_;
		if (mode != audio_mode_ || std::fabs(speed_ - filter_speed_) > 1e-6)
			reset_audio_filter();
		audio_mode_ = mode;
	}

	uint64_t due_ns(double t) const
	{
		const double dt = (t - anchor_t_) / speed_;
		const double ns = (double)anchor_wall_ + dt * 1e9;
		return ns <= 0.0 ? 0 : (uint64_t)ns;
	}

	/* ---------------- file / decoders ---------------- */

	bool open_file(const std::string &path)
	{
		AVFormatContext *fmt = nullptr;
		int ret = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr);
		if (ret < 0) {
			Engine::get().set_player_status("Waiting for recording data (" + av_err(ret) + ")");
			return false;
		}
		fmt->max_analyze_duration = 500000; // 0.5 s is plenty for our own files
		ret = avformat_find_stream_info(fmt, nullptr);
		if (ret < 0) {
			avformat_close_input(&fmt);
			return false;
		}
		const int vidx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
		if (vidx < 0) {
			avformat_close_input(&fmt);
			return false;
		}
		const int aidx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, vidx, nullptr, 0);

		fmt_ = fmt;
		vidx_ = vidx;
		vtb_ = av_q2d(fmt->streams[vidx]->time_base);
		base_pts_ = fmt->streams[vidx]->start_time != AV_NOPTS_VALUE ? fmt->streams[vidx]->start_time : 0;

		vdec_ = open_decoder(fmt->streams[vidx], true);
		if (!vdec_) {
			close_file();
			return false;
		}
		if (aidx >= 0) {
			adec_ = open_decoder(fmt->streams[aidx], false);
			if (adec_) {
				aidx_ = aidx;
				atb_ = av_q2d(fmt->streams[aidx]->time_base);
			}
		}
		pkt_ = av_packet_alloc();
		frame_ = av_frame_alloc();
		aframe_ = av_frame_alloc();
		last_frame_ = av_frame_alloc();
		obs_log(LOG_INFO, "Replay player opened %s", path.c_str());
		return true;
	}

	static AVCodecContext *open_decoder(AVStream *st, bool video)
	{
		const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
		if (!codec)
			return nullptr;
		AVCodecContext *ctx = avcodec_alloc_context3(codec);
		if (!ctx)
			return nullptr;
		if (avcodec_parameters_to_context(ctx, st->codecpar) < 0) {
			avcodec_free_context(&ctx);
			return nullptr;
		}
		ctx->pkt_timebase = st->time_base;
		if (video) {
			ctx->thread_count = 4;
			ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
		}
		if (avcodec_open2(ctx, codec, nullptr) < 0) {
			avcodec_free_context(&ctx);
			return nullptr;
		}
		return ctx;
	}

	void close_file()
	{
		reset_audio_filter();
		if (vdec_)
			avcodec_free_context(&vdec_);
		if (adec_)
			avcodec_free_context(&adec_);
		if (fmt_)
			avformat_close_input(&fmt_);
		av_packet_free(&pkt_);
		av_frame_free(&frame_);
		av_frame_free(&aframe_);
		av_frame_free(&last_frame_);
		vidx_ = aidx_ = -1;
	}

	void clear_eof()
	{
		if (fmt_ && fmt_->pb)
			fmt_->pb->eof_reached = 0;
	}

	double video_time(const AVFrame *f) const
	{
		int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
		if (pts == AV_NOPTS_VALUE)
			return position_;
		return (double)(pts - base_pts_) * vtb_;
	}

	/* Reads packets until one decoded video frame is ready. Audio packets
	 * met on the way are decoded and sent to OBS (unless suppressed). */
	Read read_next_video(AVFrame **out)
	{
		for (;;) {
			int ret = avcodec_receive_frame(vdec_, frame_);
			if (ret == 0) {
				av_frame_unref(last_frame_);
				av_frame_ref(last_frame_, frame_);
				*out = frame_;
				return Read::Video;
			}
			if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
				return Read::Error;

			ret = av_read_frame(fmt_, pkt_);
			if (ret == AVERROR_EOF || (ret < 0 && fmt_->pb && avio_feof(fmt_->pb)))
				return Read::Eof;
			if (ret == AVERROR(EAGAIN))
				return Read::Eof;
			if (ret < 0)
				return Read::Error;

			if (pkt_->stream_index == vidx_) {
				ret = avcodec_send_packet(vdec_, pkt_);
				av_packet_unref(pkt_);
				if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_INVALIDDATA)
					return Read::Error;
			} else if (pkt_->stream_index == aidx_ && adec_) {
				if (!suppress_audio_)
					decode_audio_packet(pkt_);
				av_packet_unref(pkt_);
			} else {
				av_packet_unref(pkt_);
			}
		}
	}

	/* Jump to `target` (session seconds) and show the frame at that moment. */
	void seek_and_show(double target)
	{
		if (!fmt_)
			return;
		target = std::max(0.0, target);
		const int64_t ts = base_pts_ + (int64_t)(target / vtb_);
		int ret = av_seek_frame(fmt_, vidx_, ts, AVSEEK_FLAG_BACKWARD);
		if (ret < 0)
			ret = avformat_seek_file(fmt_, vidx_, INT64_MIN, ts, ts, 0);
		if (ret < 0)
			obs_log(LOG_DEBUG, "seek to %.3f failed: %s", target, av_err(ret).c_str());
		clear_eof();
		avcodec_flush_buffers(vdec_);
		if (adec_)
			avcodec_flush_buffers(adec_);
		reset_audio_filter();

		suppress_audio_ = true;
		bool shown = false;
		for (int guard = 0; guard < 2000 && !stop_; guard++) {
			AVFrame *f = nullptr;
			Read r = read_next_video(&f);
			if (r == Read::Video) {
				const double t = video_time(f);
				if (t + kFrameDur * 0.5 >= target) {
					output_video(f);
					position_ = t;
					shown = true;
					break;
				}
				continue;
			}
			if (r == Read::Eof && last_frame_ && last_frame_->data[0]) {
				/* target is past what is written: show the newest frame */
				output_video(last_frame_);
				position_ = video_time(last_frame_);
				shown = true;
			}
			break;
		}
		suppress_audio_ = false;
		clear_eof();
		if (shown)
			Engine::get().report_position(position_);
	}

	/* ---------------- video out ---------------- */

	void output_video(AVFrame *f)
	{
		enum video_format format;
		bool full_range = f->color_range == AVCOL_RANGE_JPEG;
		switch (f->format) {
		case AV_PIX_FMT_YUV420P:
			format = VIDEO_FORMAT_I420;
			break;
		case AV_PIX_FMT_YUVJ420P:
			format = VIDEO_FORMAT_I420;
			full_range = true;
			break;
		case AV_PIX_FMT_NV12:
			format = VIDEO_FORMAT_NV12;
			break;
		case AV_PIX_FMT_YUV444P:
			format = VIDEO_FORMAT_I444;
			break;
		default:
			if (!warned_format_) {
				obs_log(LOG_WARNING, "replay player: unsupported pixel format %d", f->format);
				warned_format_ = true;
			}
			return;
		}

		struct obs_source_frame out = {};
		out.width = (uint32_t)f->width;
		out.height = (uint32_t)f->height;
		out.format = format;
		out.timestamp = os_gettime_ns();
		out.full_range = full_range;
		for (int i = 0; i < 3; i++) {
			out.data[i] = f->data[i];
			out.linesize[i] = (uint32_t)f->linesize[i];
		}
		const enum video_colorspace cs = f->colorspace == AVCOL_SPC_BT470BG || f->colorspace == AVCOL_SPC_SMPTE170M
							 ? VIDEO_CS_601
							 : VIDEO_CS_709;
		video_format_get_parameters_for_format(cs, full_range ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL, format,
						       out.color_matrix, out.color_range_min, out.color_range_max);
		obs_source_output_video(source_, &out);
	}

	/* ---------------- audio out ---------------- */

	static enum speaker_layout speakers_for(int channels)
	{
		switch (channels) {
		case 1:
			return SPEAKERS_MONO;
		case 2:
			return SPEAKERS_STEREO;
		case 3:
			return SPEAKERS_2POINT1;
		case 4:
			return SPEAKERS_4POINT0;
		case 5:
			return SPEAKERS_4POINT1;
		case 6:
			return SPEAKERS_5POINT1;
		case 8:
			return SPEAKERS_7POINT1;
		default:
			return SPEAKERS_UNKNOWN;
		}
	}

	void decode_audio_packet(AVPacket *pkt)
	{
		if (avcodec_send_packet(adec_, pkt) < 0)
			return;
		while (avcodec_receive_frame(adec_, aframe_) == 0) {
			handle_audio_frame(aframe_);
			av_frame_unref(aframe_);
		}
	}

	double audio_time(const AVFrame *f) const
	{
		int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
		if (pts == AV_NOPTS_VALUE)
			return position_;
		return (double)pts * atb_ - (double)base_pts_ * vtb_;
	}

	void handle_audio_frame(AVFrame *f)
	{
		if (f->format != AV_SAMPLE_FMT_FLTP)
			return; // AAC decoder output; anything else is unexpected
		const double t = audio_time(f);
		if (t + 0.05 < anchor_t_)
			return; // from before the playhead (seek pre-roll)

		const bool normal_speed = std::fabs(speed_ - 1.0) < 1e-6;
		if (!normal_speed && audio_mode_ == AudioMode::Mute)
			return;

		if (normal_speed || audio_mode_ == AudioMode::FollowSpeed) {
			/* Follow speed: hand OBS the samples at rate*speed. OBS resamples to
			 * its own rate, which slows the audio and drops the pitch. */
			push_audio(f, (uint32_t)std::lround(f->sample_rate * speed_), due_ns(t));
			return;
		}

		/* Preserve pitch: time-stretch with FFmpeg's atempo filter. */
		if (!ensure_audio_filter(f))
			return;
		if (filter_samples_out_ == 0 && !filter_started_) {
			filter_start_wall_ = due_ns(t);
			filter_started_ = true;
		}
		if (av_buffersrc_add_frame_flags(filter_src_, f, AV_BUFFERSRC_FLAG_KEEP_REF) < 0)
			return;
		AVFrame *o = av_frame_alloc();
		while (av_buffersink_get_frame(filter_sink_, o) >= 0) {
			const uint64_t ts = filter_start_wall_ + (uint64_t)((double)filter_samples_out_ * 1e9 /
									    (double)o->sample_rate);
			push_audio(o, (uint32_t)o->sample_rate, ts);
			filter_samples_out_ += (uint64_t)o->nb_samples;
			av_frame_unref(o);
		}
		av_frame_free(&o);
	}

	void push_audio(const AVFrame *f, uint32_t rate, uint64_t ts)
	{
		const int channels = f->ch_layout.nb_channels;
		struct obs_source_audio a = {};
		for (int i = 0; i < channels && i < MAX_AV_PLANES; i++)
			a.data[i] = f->extended_data[i];
		a.frames = (uint32_t)f->nb_samples;
		a.speakers = speakers_for(channels);
		a.format = AUDIO_FORMAT_FLOAT_PLANAR;
		a.samples_per_sec = rate;
		a.timestamp = ts;
		if (a.speakers != SPEAKERS_UNKNOWN && rate > 0)
			obs_source_output_audio(source_, &a);
	}

	bool ensure_audio_filter(const AVFrame *f)
	{
		if (filter_graph_)
			return true;

		filter_graph_ = avfilter_graph_alloc();
		if (!filter_graph_)
			return false;

		char layout[64] = {0};
		av_channel_layout_describe(&f->ch_layout, layout, sizeof(layout));
		char args[256];
		snprintf(args, sizeof(args), "time_base=1/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
			 f->sample_rate, f->sample_rate, av_get_sample_fmt_name((AVSampleFormat)f->format), layout);

		/* atempo accepts 0.5..2.0 per stage, so chain stages for 25% */
		std::string chain;
		double remaining = speed_;
		while (remaining < 0.5 - 1e-9) {
			chain += "atempo=0.5,";
			remaining /= 0.5;
		}
		char last[32];
		snprintf(last, sizeof(last), "atempo=%.4f", remaining);
		chain += last;

		AVFilterInOut *outputs = avfilter_inout_alloc();
		AVFilterInOut *inputs = avfilter_inout_alloc();
		bool ok = avfilter_graph_create_filter(&filter_src_, avfilter_get_by_name("abuffer"), "in", args,
						       nullptr, filter_graph_) >= 0 &&
			  avfilter_graph_create_filter(&filter_sink_, avfilter_get_by_name("abuffersink"), "out",
						       nullptr, nullptr, filter_graph_) >= 0;
		if (ok) {
			const enum AVSampleFormat fmts[] = {AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_NONE};
			ok = av_opt_set_int_list(filter_sink_, "sample_fmts", fmts, AV_SAMPLE_FMT_NONE,
						 AV_OPT_SEARCH_CHILDREN) >= 0;
		}
		if (ok) {
			outputs->name = av_strdup("in");
			outputs->filter_ctx = filter_src_;
			outputs->pad_idx = 0;
			outputs->next = nullptr;
			inputs->name = av_strdup("out");
			inputs->filter_ctx = filter_sink_;
			inputs->pad_idx = 0;
			inputs->next = nullptr;
			ok = avfilter_graph_parse_ptr(filter_graph_, chain.c_str(), &inputs, &outputs, nullptr) >= 0 &&
			     avfilter_graph_config(filter_graph_, nullptr) >= 0;
		}
		avfilter_inout_free(&inputs);
		avfilter_inout_free(&outputs);
		if (!ok) {
			obs_log(LOG_WARNING, "replay player: could not build pitch-preserving filter (%s)", chain.c_str());
			reset_audio_filter();
			return false;
		}
		filter_speed_ = speed_;
		filter_samples_out_ = 0;
		filter_started_ = false;
		return true;
	}

	void reset_audio_filter()
	{
		if (filter_graph_)
			avfilter_graph_free(&filter_graph_);
		filter_src_ = nullptr;
		filter_sink_ = nullptr;
		filter_speed_ = -1.0;
		filter_samples_out_ = 0;
		filter_started_ = false;
	}

	/* ---------------- members ---------------- */

	obs_source_t *source_;
	std::thread thread_;
	std::atomic<bool> stop_{false};

	AVFormatContext *fmt_ = nullptr;
	AVCodecContext *vdec_ = nullptr;
	AVCodecContext *adec_ = nullptr;
	AVPacket *pkt_ = nullptr;
	AVFrame *frame_ = nullptr;
	AVFrame *aframe_ = nullptr;
	AVFrame *last_frame_ = nullptr;
	int vidx_ = -1;
	int aidx_ = -1;
	double vtb_ = 1.0 / 90000.0;
	double atb_ = 1.0 / 90000.0;
	int64_t base_pts_ = 0;

	double position_ = 0.0;
	double speed_ = 1.0;
	uint64_t anchor_wall_ = 0;
	double anchor_t_ = 0.0;
	AudioMode audio_mode_ = AudioMode::FollowSpeed;
	bool suppress_audio_ = false;
	bool warned_format_ = false;

	AVFilterGraph *filter_graph_ = nullptr;
	AVFilterContext *filter_src_ = nullptr;
	AVFilterContext *filter_sink_ = nullptr;
	double filter_speed_ = -1.0;
	uint64_t filter_samples_out_ = 0;
	uint64_t filter_start_wall_ = 0;
	bool filter_started_ = false;
};

const char *player_get_name(void *)
{
	return obs_module_text("RPGReplay.Player");
}

void *player_create(obs_data_t *, obs_source_t *source)
{
	return new ReplayPlayer(source);
}

void player_destroy(void *data)
{
	delete static_cast<ReplayPlayer *>(data);
}

} // namespace

void rpg_register_player_source()
{
	struct obs_source_info info = {};
	info.id = "rpg_replay_player";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.icon_type = OBS_ICON_TYPE_MEDIA;
	info.get_name = player_get_name;
	info.create = player_create;
	info.destroy = player_destroy;
	obs_register_source(&info);
}
