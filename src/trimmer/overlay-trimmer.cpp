#ifdef _WIN32

#include "overlay-trimmer.h"
#include "overlay-ffmpeg-time-utils.h"
#include "overlay-ffmpeg-video-decode.h"
#include "overlay-string-utils.h"
#include <obs-module.h>
#include <plugin-support.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
#include <libavutil/avstring.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/samplefmt.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

namespace {

using overlay::util::Utf8ToWide;
using overlay::util::WideToUtf8;
using overlay::ffmpeg::PtsToSeconds;
using overlay::ffmpeg::SecondsToTimestamp;
using overlay::ffmpeg::OpenVideoDecoder;
using overlay::ffmpeg::ReleaseVideoHwDecodeState;
using overlay::ffmpeg::TransferHwFrameToCpu;
using overlay::ffmpeg::VideoHwDecodeState;

struct InterruptCtx {
	std::atomic<bool> *cancel = nullptr;
};

static int av_interrupt_cb(void *opaque)
{
	auto *ctx = static_cast<InterruptCtx *>(opaque);
	if (!ctx || !ctx->cancel) {
		return 0;
	}
	return ctx->cancel->load() ? 1 : 0;
}

struct AvCloser {
	AVFormatContext *in = nullptr;
	AVFormatContext *out = nullptr;
	bool out_opened = false;

	~AvCloser()
	{
		if (out) {
			av_write_trailer(out);
			if (out_opened && !(out->oformat->flags & AVFMT_NOFILE)) {
				avio_closep(&out->pb);
			}
			avformat_free_context(out);
			out = nullptr;
		}
		if (in) {
			avformat_close_input(&in);
			in = nullptr;
		}
	}
};

static void set_err(std::wstring &outError, const wchar_t *prefix, int errnum)
{
	char errbuf[256];
	av_strerror(errnum, errbuf, sizeof(errbuf));
	outError = std::wstring(prefix) + Utf8ToWide(errbuf);
}

static const AVCodec *find_video_encoder(bool useCpuEncoder)
{
	UNUSED_PARAMETER(useCpuEncoder);
	// libx264 is preferred when bundled; any H.264 encoder is acceptable otherwise.
	if (const AVCodec *c = avcodec_find_encoder_by_name("libx264")) {
		return c;
	}
	return avcodec_find_encoder(AV_CODEC_ID_H264);
}

static const AVCodec *find_audio_encoder()
{
	// AAC maximizes MP4 player compatibility.
	return avcodec_find_encoder(AV_CODEC_ID_AAC);
}

static AVRational pick_fps_timebase(const AVStream *st)
{
	AVRational fps = {30, 1};
	if (st) {
		if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0)
			fps = st->avg_frame_rate;
		else if (st->r_frame_rate.num > 0 && st->r_frame_rate.den > 0)
			fps = st->r_frame_rate;
	}
	if (fps.num <= 0 || fps.den <= 0)
		return AVRational{1, 30};
	return av_inv_q(fps);
}

struct CodecCtxDeleter {
	void operator()(AVCodecContext *ctx) const { avcodec_free_context(&ctx); }
};

struct FrameDeleter {
	void operator()(AVFrame *f) const { av_frame_free(&f); }
};

struct PacketDeleter {
	void operator()(AVPacket *p) const { av_packet_free(&p); }
};

// --- Compress export: in-process MP4 transcode (H.264/AAC) ---
static bool transcode_trim_mp4(const std::wstring &inputPath, const std::wstring &outputPath, double startSeconds,
			       double endSeconds, int crf, bool useCpuEncoder,
			       const std::vector<int> &streamIndicesToKeep, std::atomic<bool> &cancelRequested,
			       std::atomic<double> &progress, std::wstring &outError)
{
	outError.clear();

	std::string inUtf8 = WideToUtf8(inputPath);
	std::string outUtf8 = WideToUtf8(outputPath);
	if (inUtf8.empty() || outUtf8.empty()) {
		outError = L"Invalid input/output path";
		return false;
	}

	InterruptCtx ictx;
	ictx.cancel = &cancelRequested;

	struct InFmtDeleter {
		void operator()(AVFormatContext *c) const
		{
			if (!c)
				return;
			AVFormatContext *tmp = c;
			avformat_close_input(&tmp);
		}
	};
	struct OutFmtDeleter {
		void operator()(AVFormatContext *c) const
		{
			if (!c)
				return;
			if (!(c->oformat->flags & AVFMT_NOFILE) && c->pb) {
				avio_closep(&c->pb);
			}
			avformat_free_context(c);
		}
	};
	struct SwsDeleter {
		void operator()(SwsContext *c) const { sws_freeContext(c); }
	};
	struct SwrDeleter {
		void operator()(SwrContext *c) const { swr_free(&c); }
	};
	struct AudioFifoDeleter {
		void operator()(AVAudioFifo *f) const
		{
			if (f)
				av_audio_fifo_free(f);
		}
	};

	AVFormatContext *ifmt_raw = avformat_alloc_context();
	if (!ifmt_raw) {
		outError = L"FFmpeg: avformat_alloc_context failed";
		return false;
	}
	ifmt_raw->interrupt_callback = AVIOInterruptCB{&av_interrupt_cb, &ictx};

	int ret = avformat_open_input(&ifmt_raw, inUtf8.c_str(), nullptr, nullptr);
	if (ret < 0) {
		set_err(outError, L"FFmpeg: failed to open input: ", ret);
		if (ifmt_raw)
			avformat_free_context(ifmt_raw);
		return false;
	}
	std::unique_ptr<AVFormatContext, InFmtDeleter> ifmt(ifmt_raw);

	ret = avformat_find_stream_info(ifmt.get(), nullptr);
	if (ret < 0) {
		set_err(outError, L"FFmpeg: failed to read stream info: ", ret);
		return false;
	}

	AVFormatContext *ofmt_raw = nullptr;
	ret = avformat_alloc_output_context2(&ofmt_raw, nullptr, "mp4", outUtf8.c_str());
	if (ret < 0 || !ofmt_raw) {
		outError = L"FFmpeg: failed to create output context";
		return false;
	}
	std::unique_ptr<AVFormatContext, OutFmtDeleter> ofmt(ofmt_raw);
	ofmt->interrupt_callback = AVIOInterruptCB{&av_interrupt_cb, &ictx};

	// --- Stream selection ---
	std::vector<bool> keep(static_cast<size_t>(ifmt->nb_streams), true);
	if (!streamIndicesToKeep.empty()) {
		std::fill(keep.begin(), keep.end(), false);
		for (int idx : streamIndicesToKeep) {
			if (idx >= 0 && idx < static_cast<int>(ifmt->nb_streams)) {
				keep[static_cast<size_t>(idx)] = true;
			}
		}
	}

	int video_in = -1;
	std::vector<int> audio_in;
	for (unsigned i = 0; i < ifmt->nb_streams; i++) {
		if (!keep[i])
			continue;
		const AVStream *st = ifmt->streams[i];
		if (!st || !st->codecpar)
			continue;
		if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_in < 0) {
			video_in = static_cast<int>(i);
		} else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
			audio_in.push_back(static_cast<int>(i));
		}
	}
	if (video_in < 0 && audio_in.empty()) {
		outError = L"FFmpeg: no audio/video streams selected";
		return false;
	}

	// --- Decoders ---
	std::unique_ptr<AVCodecContext, CodecCtxDeleter> vdec;
	VideoHwDecodeState vdec_hw;
	struct VideoHwReleaseGuard {
		VideoHwDecodeState *hw = nullptr;
		~VideoHwReleaseGuard()
		{
			if (hw) {
				ReleaseVideoHwDecodeState(hw);
			}
		}
	} vdec_hw_guard{&vdec_hw};
	std::vector<std::unique_ptr<AVCodecContext, CodecCtxDeleter>> adec;

	if (video_in >= 0) {
		AVStream *st = ifmt->streams[video_in];
		AVCodecContext *ctx = nullptr;
		const bool is_av1 = st->codecpar->codec_id == AV_CODEC_ID_AV1;
		if (!OpenVideoDecoder(&ctx, st, &vdec_hw, /*allow_av1_sw_fallback=*/!is_av1)) {
			if (is_av1) {
				outError = L"FFmpeg: AV1 export requires NVIDIA GPU decoder (av1_cuvid)";
			} else {
				outError = L"FFmpeg: video decoder not found";
			}
			return false;
		}
		vdec.reset(ctx);
	}

	for (int idx : audio_in) {
		AVStream *st = ifmt->streams[idx];
		const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
		if (!dec) {
			outError = L"FFmpeg: audio decoder not found";
			return false;
		}
		AVCodecContext *ctx = avcodec_alloc_context3(dec);
		if (!ctx) {
			outError = L"FFmpeg: avcodec_alloc_context3(audio) failed";
			return false;
		}
		avcodec_parameters_to_context(ctx, st->codecpar);
		ret = avcodec_open2(ctx, dec, nullptr);
		if (ret < 0) {
			set_err(outError, L"FFmpeg: avcodec_open2(audio) failed: ", ret);
			avcodec_free_context(&ctx);
			return false;
		}
		adec.emplace_back(ctx);
	}

	auto pick_video_pix_fmt = [&](const AVCodec *enc) -> AVPixelFormat {
		if (!enc || !enc->name)
			return AV_PIX_FMT_YUV420P;
		return AV_PIX_FMT_YUV420P;
	};

	// --- Encoders and output streams ---
	std::unique_ptr<AVCodecContext, CodecCtxDeleter> venc;
	std::vector<std::unique_ptr<AVCodecContext, CodecCtxDeleter>> aenc;
	int video_out = -1;
	std::vector<int> audio_out;
	std::unique_ptr<SwsContext, SwsDeleter> sws;
	std::vector<std::unique_ptr<SwrContext, SwrDeleter>> swr;

	if (video_in >= 0) {
		AVStream *in_st = ifmt->streams[video_in];
		const AVCodec *enc = find_video_encoder(useCpuEncoder);
		if (!enc) {
			outError = L"FFmpeg: H.264 encoder not found in this build";
			return false;
		}

		AVCodecContext *ctx = avcodec_alloc_context3(enc);
		if (!ctx) {
			outError = L"FFmpeg: avcodec_alloc_context3(video enc) failed";
			return false;
		}
		ctx->width = vdec->width > 0 ? vdec->width : static_cast<int>(in_st->codecpar->width);
		ctx->height = vdec->height > 0 ? vdec->height : static_cast<int>(in_st->codecpar->height);
		ctx->time_base = pick_fps_timebase(in_st);
		ctx->framerate = av_inv_q(ctx->time_base);
		ctx->gop_size = 60;
		ctx->max_b_frames = 2;
		ctx->bit_rate = 0;
		ctx->pix_fmt = pick_video_pix_fmt(enc);

		// Only libx264 reliably honors CRF in this build.
		if (enc->name && !strcmp(enc->name, "libx264")) {
			av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
			av_opt_set_int(ctx->priv_data, "crf", crf, 0);
		}

		if (ofmt->oformat->flags & AVFMT_GLOBALHEADER) {
			ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
		}

		ret = avcodec_open2(ctx, enc, nullptr);
		if (ret < 0) {
			set_err(outError, L"FFmpeg: avcodec_open2(video enc) failed: ", ret);
			avcodec_free_context(&ctx);
			return false;
		}
		venc.reset(ctx);

		AVStream *out_st = avformat_new_stream(ofmt.get(), nullptr);
		if (!out_st) {
			outError = L"FFmpeg: avformat_new_stream(video) failed";
			return false;
		}
		video_out = static_cast<int>(out_st->index);
		ret = avcodec_parameters_from_context(out_st->codecpar, venc.get());
		if (ret < 0) {
			outError = L"FFmpeg: avcodec_parameters_from_context(video) failed";
			return false;
		}
		out_st->time_base = venc->time_base;
	}

	const AVCodec *aenc_codec = find_audio_encoder();
	if (!aenc_codec && !adec.empty()) {
		outError = L"FFmpeg: AAC encoder not found";
		return false;
	}
	swr.reserve(adec.size());

	for (size_t i = 0; i < adec.size(); i++) {
		AVCodecContext *dec = adec[i].get();
		AVCodecContext *ctx = avcodec_alloc_context3(aenc_codec);
		if (!ctx) {
			outError = L"FFmpeg: avcodec_alloc_context3(audio enc) failed";
			return false;
		}
		ctx->sample_rate = dec->sample_rate > 0 ? dec->sample_rate : 48000;
		ctx->time_base = AVRational{1, ctx->sample_rate};
		ctx->bit_rate = 128000;
		ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;

		if (dec->ch_layout.nb_channels > 0) {
			av_channel_layout_copy(&ctx->ch_layout, &dec->ch_layout);
		} else {
			av_channel_layout_default(&ctx->ch_layout, 2);
		}

		if (ofmt->oformat->flags & AVFMT_GLOBALHEADER) {
			ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
		}

		ret = avcodec_open2(ctx, aenc_codec, nullptr);
		if (ret < 0) {
			set_err(outError, L"FFmpeg: avcodec_open2(audio enc) failed: ", ret);
			avcodec_free_context(&ctx);
			return false;
		}
		aenc.emplace_back(ctx);

		AVStream *out_st = avformat_new_stream(ofmt.get(), nullptr);
		if (!out_st) {
			outError = L"FFmpeg: avformat_new_stream(audio) failed";
			return false;
		}
		audio_out.push_back(static_cast<int>(out_st->index));
		ret = avcodec_parameters_from_context(out_st->codecpar, ctx);
		if (ret < 0) {
			outError = L"FFmpeg: avcodec_parameters_from_context(audio) failed";
			return false;
		}
		out_st->time_base = ctx->time_base;

		SwrContext *raw = nullptr;
		ret = swr_alloc_set_opts2(&raw, &ctx->ch_layout, ctx->sample_fmt, ctx->sample_rate, &dec->ch_layout,
					  dec->sample_fmt, dec->sample_rate, 0, nullptr);
		if (ret < 0 || !raw) {
			outError = L"FFmpeg: swr_alloc_set_opts2 failed";
			if (raw)
				swr_free(&raw);
			return false;
		}
		ret = swr_init(raw);
		if (ret < 0) {
			set_err(outError, L"FFmpeg: swr_init failed: ", ret);
			swr_free(&raw);
			return false;
		}
		swr.emplace_back(raw);
	}

	if (!(ofmt->oformat->flags & AVFMT_NOFILE)) {
		ret = avio_open(&ofmt->pb, outUtf8.c_str(), AVIO_FLAG_WRITE);
		if (ret < 0) {
			set_err(outError, L"FFmpeg: failed to open output file: ", ret);
			return false;
		}
	}
	AVDictionary *mux_opts = nullptr;
	av_dict_set(&mux_opts, "movflags", "+faststart", 0);
	ret = avformat_write_header(ofmt.get(), &mux_opts);
	av_dict_free(&mux_opts);
	if (ret < 0) {
		set_err(outError, L"FFmpeg: avformat_write_header failed: ", ret);
		return false;
	}

	const double startSec = std::max(0.0, startSeconds);
	if (startSec > 0.0) {
		int64_t seek_ts = static_cast<int64_t>(llround(startSec * static_cast<double>(AV_TIME_BASE)));
		ret = av_seek_frame(ifmt.get(), -1, seek_ts, AVSEEK_FLAG_BACKWARD);
		if (ret >= 0) {
			if (vdec)
				avcodec_flush_buffers(vdec.get());
			for (auto &d : adec)
				avcodec_flush_buffers(d.get());
		}
	}

	const bool hasEnd = (endSeconds > startSeconds) && (endSeconds > 0.0);
	const double totalDuration = hasEnd ? (endSeconds - startSeconds) : 0.0;
	progress.store(0.0);

	auto frame_time = [&](AVFrame *f, int stream_index) -> double {
		int64_t ts = (f->best_effort_timestamp != AV_NOPTS_VALUE) ? f->best_effort_timestamp : f->pts;
		if (ts == AV_NOPTS_VALUE)
			return 0.0;
		AVStream *st = ifmt->streams[stream_index];
		return PtsToSeconds(ts, st->time_base);
	};

	auto write_encoded = [&](AVCodecContext *enc_ctx, AVStream *out_st, int out_index) -> bool {
		while (true) {
			std::unique_ptr<AVPacket, PacketDeleter> op(av_packet_alloc());
			if (!op) {
				outError = L"FFmpeg: av_packet_alloc failed";
				return false;
			}
			ret = avcodec_receive_packet(enc_ctx, op.get());
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
				return true;
			}
			if (ret < 0) {
				set_err(outError, L"FFmpeg: avcodec_receive_packet failed: ", ret);
				return false;
			}
			if (cancelRequested.load()) {
				outError = L"Cancelled";
				return false;
			}
			op->stream_index = out_index;
			av_packet_rescale_ts(op.get(), enc_ctx->time_base, out_st->time_base);
			ret = av_interleaved_write_frame(ofmt.get(), op.get());
			if (ret < 0) {
				set_err(outError, L"FFmpeg: av_interleaved_write_frame failed: ", ret);
				return false;
			}
		}
	};

	// Regenerate monotonic PTS after re-encode (CFR video, sample-count audio).
	int64_t video_pts = 0;
	std::vector<int64_t> audio_pts(aenc.size(), 0);
	bool video_done = (video_in < 0);
	std::vector<bool> audio_done(aenc.size(), false);

	std::vector<std::unique_ptr<AVAudioFifo, AudioFifoDeleter>> audio_fifos;
	audio_fifos.reserve(aenc.size());
	for (size_t i = 0; i < aenc.size(); i++) {
		AVCodecContext *enc = aenc[i].get();
		AVAudioFifo *fifo = av_audio_fifo_alloc(enc->sample_fmt, enc->ch_layout.nb_channels,
						       enc->frame_size > 0 ? enc->frame_size : 1024);
		if (!fifo) {
			outError = L"FFmpeg: av_audio_fifo_alloc failed";
			return false;
		}
		audio_fifos.emplace_back(fifo);
	}

	auto all_done = [&]() -> bool {
		if (!video_done)
			return false;
		for (bool d : audio_done) {
			if (!d)
				return false;
		}
		return true;
	};

	auto handle_video_frame = [&](AVFrame *inFrame) -> bool {
		double t = frame_time(inFrame, video_in);
		if (startSeconds > 0.0 && t + 0.001 < startSeconds)
			return true;
		if (hasEnd && t > endSeconds) {
			video_done = true;
			return true;
		}

		AVFrame *vf = inFrame;
		std::unique_ptr<AVFrame, FrameDeleter> conv;
		const AVPixelFormat srcFmt = static_cast<AVPixelFormat>(inFrame->format);
		if (srcFmt != venc->pix_fmt) {
			if (!sws) {
				SwsContext *raw = sws_getContext(inFrame->width, inFrame->height, srcFmt, venc->width,
								 venc->height, venc->pix_fmt, SWS_BILINEAR, nullptr,
								 nullptr, nullptr);
				if (!raw) {
					outError = L"FFmpeg: sws_getContext failed";
					return false;
				}
				sws.reset(raw);
			}
			conv.reset(av_frame_alloc());
			if (!conv) {
				outError = L"FFmpeg: av_frame_alloc failed";
				return false;
			}
			conv->format = venc->pix_fmt;
			conv->width = venc->width;
			conv->height = venc->height;
			ret = av_frame_get_buffer(conv.get(), 32);
			if (ret < 0) {
				set_err(outError, L"FFmpeg: av_frame_get_buffer failed: ", ret);
				return false;
			}
			sws_scale(sws.get(), vf->data, vf->linesize, 0, vf->height, conv->data, conv->linesize);
			vf = conv.get();
		}

		vf->pts = video_pts++;
		ret = avcodec_send_frame(venc.get(), vf);
		if (ret < 0 && ret != AVERROR(EAGAIN)) {
			set_err(outError, L"FFmpeg: avcodec_send_frame(video) failed: ", ret);
			return false;
		}
		AVStream *out_st = ofmt->streams[video_out];
		if (!write_encoded(venc.get(), out_st, video_out))
			return false;

		if (hasEnd && totalDuration > 0.0) {
			double p = (t - startSeconds) / totalDuration;
			if (p < 0.0)
				p = 0.0;
			if (p > 1.0)
				p = 1.0;
			progress.store(p);
		}
		return true;
	};

	auto encode_audio_chunks = [&](size_t ai) -> bool {
		AVCodecContext *enc = aenc[ai].get();
		AVAudioFifo *fifo = audio_fifos[ai].get();
		const int frame_size = enc->frame_size > 0 ? enc->frame_size : 0;

		while (true) {
			const int avail = av_audio_fifo_size(fifo);
			if (avail <= 0)
				break;
			const int nb_samples = (frame_size > 0) ? frame_size : avail;
			if (frame_size > 0 && avail < frame_size)
				break;

			std::unique_ptr<AVFrame, FrameDeleter> outF(av_frame_alloc());
			if (!outF) {
				outError = L"FFmpeg: av_frame_alloc failed";
				return false;
			}
			outF->format = enc->sample_fmt;
			outF->sample_rate = enc->sample_rate;
			av_channel_layout_copy(&outF->ch_layout, &enc->ch_layout);
			outF->nb_samples = nb_samples;
			ret = av_frame_get_buffer(outF.get(), 0);
			if (ret < 0) {
				set_err(outError, L"FFmpeg: av_frame_get_buffer(audio) failed: ", ret);
				return false;
			}
			if (av_audio_fifo_read(fifo, (void **)outF->data, nb_samples) < nb_samples) {
				outError = L"FFmpeg: av_audio_fifo_read failed";
				return false;
			}
			outF->pts = audio_pts[ai];
			audio_pts[ai] += nb_samples;

			AVStream *out_st = ofmt->streams[audio_out[ai]];
			while (true) {
				ret = avcodec_send_frame(enc, outF.get());
				if (ret == AVERROR(EAGAIN)) {
					if (!write_encoded(enc, out_st, audio_out[ai]))
						return false;
					continue;
				}
				if (ret < 0) {
					set_err(outError, L"FFmpeg: avcodec_send_frame(audio) failed: ", ret);
					return false;
				}
				break;
			}
			if (!write_encoded(enc, out_st, audio_out[ai]))
				return false;
		}
		return true;
	};

	auto flush_audio_fifo = [&](size_t ai) -> bool {
		AVCodecContext *enc = aenc[ai].get();
		AVAudioFifo *fifo = audio_fifos[ai].get();
		const int frame_size = enc->frame_size > 0 ? enc->frame_size : 0;
		int avail = av_audio_fifo_size(fifo);
		if (avail <= 0)
			return true;

		if (frame_size > 0) {
			const int pad = (frame_size - (avail % frame_size)) % frame_size;
			if (pad > 0) {
				std::unique_ptr<AVFrame, FrameDeleter> silence(av_frame_alloc());
				if (!silence) {
					outError = L"FFmpeg: av_frame_alloc failed";
					return false;
				}
				silence->format = enc->sample_fmt;
				silence->sample_rate = enc->sample_rate;
				av_channel_layout_copy(&silence->ch_layout, &enc->ch_layout);
				silence->nb_samples = pad;
				ret = av_frame_get_buffer(silence.get(), 0);
				if (ret < 0) {
					set_err(outError, L"FFmpeg: av_frame_get_buffer(audio pad) failed: ", ret);
					return false;
				}
				ret = av_samples_set_silence(silence->data, 0, pad, enc->ch_layout.nb_channels,
							     enc->sample_fmt);
				if (ret < 0) {
					set_err(outError, L"FFmpeg: av_samples_set_silence failed: ", ret);
					return false;
				}
				if (av_audio_fifo_write(fifo, (void **)silence->data, pad) < pad) {
					outError = L"FFmpeg: av_audio_fifo_write(pad) failed";
					return false;
				}
			}
		}
		return encode_audio_chunks(ai);
	};

	auto handle_audio_frame = [&](size_t ai, AVFrame *inFrame) -> bool {
		int in_idx = audio_in[ai];
		if (in_idx < 0)
			return true;
		double t = frame_time(inFrame, in_idx);

		double frameDur = 0.0;
		if (inFrame->nb_samples > 0 && adec[ai]->sample_rate > 0) {
			frameDur =
				static_cast<double>(inFrame->nb_samples) / static_cast<double>(adec[ai]->sample_rate);
		}
		if (startSeconds > 0.0 && (t + frameDur) < startSeconds - 0.001)
			return true;
		if (hasEnd && t > endSeconds) {
			audio_done[ai] = true;
			audio_in[ai] = -1;
			return true;
		}

		AVCodecContext *enc = aenc[ai].get();
		SwrContext *s = swr[ai].get();

		std::unique_ptr<AVFrame, FrameDeleter> outF(av_frame_alloc());
		if (!outF) {
			outError = L"FFmpeg: av_frame_alloc failed";
			return false;
		}
		outF->sample_rate = enc->sample_rate;
		outF->format = enc->sample_fmt;
		av_channel_layout_copy(&outF->ch_layout, &enc->ch_layout);

		int out_nb = swr_get_out_samples(s, inFrame->nb_samples);
		if (out_nb <= 0)
			return true;
		outF->nb_samples = out_nb;
		ret = av_frame_get_buffer(outF.get(), 0);
		if (ret < 0) {
			set_err(outError, L"FFmpeg: av_frame_get_buffer(audio) failed: ", ret);
			return false;
		}
		int converted =
			swr_convert(s, outF->data, out_nb, (const uint8_t **)inFrame->data, inFrame->nb_samples);
		if (converted < 0) {
			outError = L"FFmpeg: swr_convert failed";
			return false;
		}
		if (converted == 0)
			return true;
		outF->nb_samples = converted;

		if (av_audio_fifo_write(audio_fifos[ai].get(), (void **)outF->data, converted) < converted) {
			outError = L"FFmpeg: av_audio_fifo_write failed";
			return false;
		}
		return encode_audio_chunks(ai);
	};

	auto process_video_frame = [&](AVFrame *frame) -> bool {
		if (vdec_hw.active) {
			std::unique_ptr<AVFrame, FrameDeleter> cpu(av_frame_alloc());
			if (!cpu) {
				outError = L"FFmpeg: av_frame_alloc failed";
				return false;
			}
			if (!TransferHwFrameToCpu(frame, cpu.get())) {
				return true;
			}
			return handle_video_frame(cpu.get());
		}
		return handle_video_frame(frame);
	};

	AVPacket ipkt = {};
	while (true) {
		if (cancelRequested.load()) {
			outError = L"Cancelled";
			return false;
		}
		if (all_done())
			break;

		ret = av_read_frame(ifmt.get(), &ipkt);
		if (ret < 0) {
			break;
		}

		const int si = ipkt.stream_index;
		if (vdec && !video_done && si == video_in) {
			ret = avcodec_send_packet(vdec.get(), &ipkt);
			av_packet_unref(&ipkt);
			if (ret < 0 && ret != AVERROR(EAGAIN)) {
				set_err(outError, L"FFmpeg: avcodec_send_packet(video) failed: ", ret);
				return false;
			}
			while (true) {
				std::unique_ptr<AVFrame, FrameDeleter> f(av_frame_alloc());
				if (!f) {
					outError = L"FFmpeg: av_frame_alloc failed";
					return false;
				}
				ret = avcodec_receive_frame(vdec.get(), f.get());
				if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
					break;
				if (ret < 0) {
					set_err(outError, L"FFmpeg: avcodec_receive_frame(video) failed: ", ret);
					return false;
				}
				if (!process_video_frame(f.get()))
					return false;
			}
			continue;
		}

		bool audio_handled = false;
		for (size_t ai = 0; ai < audio_in.size(); ai++) {
			if (audio_in[ai] < 0 || audio_done[ai])
				continue;
			if (si != audio_in[ai])
				continue;
			audio_handled = true;
			ret = avcodec_send_packet(adec[ai].get(), &ipkt);
			av_packet_unref(&ipkt);
			if (ret < 0 && ret != AVERROR(EAGAIN)) {
				set_err(outError, L"FFmpeg: avcodec_send_packet(audio) failed: ", ret);
				return false;
			}
			while (true) {
				std::unique_ptr<AVFrame, FrameDeleter> af(av_frame_alloc());
				if (!af) {
					outError = L"FFmpeg: av_frame_alloc failed";
					return false;
				}
				ret = avcodec_receive_frame(adec[ai].get(), af.get());
				if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
					break;
				if (ret < 0) {
					set_err(outError, L"FFmpeg: avcodec_receive_frame(audio) failed: ", ret);
					return false;
				}
				if (!handle_audio_frame(ai, af.get()))
					return false;
			}
			break;
		}
		if (!audio_handled) {
			av_packet_unref(&ipkt);
		}
	}

	if (vdec && !video_done) {
		avcodec_send_packet(vdec.get(), nullptr);
		while (true) {
			std::unique_ptr<AVFrame, FrameDeleter> f(av_frame_alloc());
			if (!f) {
				outError = L"FFmpeg: av_frame_alloc failed";
				return false;
			}
			ret = avcodec_receive_frame(vdec.get(), f.get());
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
				break;
			if (ret < 0) {
				set_err(outError, L"FFmpeg: avcodec_receive_frame(video) failed: ", ret);
				return false;
			}
			if (!process_video_frame(f.get()))
				return false;
		}
	}
	for (size_t ai = 0; ai < adec.size(); ai++) {
		if (audio_done[ai])
			continue;
		avcodec_send_packet(adec[ai].get(), nullptr);
		while (true) {
			std::unique_ptr<AVFrame, FrameDeleter> af(av_frame_alloc());
			if (!af) {
				outError = L"FFmpeg: av_frame_alloc failed";
				return false;
			}
			ret = avcodec_receive_frame(adec[ai].get(), af.get());
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
				break;
			if (ret < 0) {
				set_err(outError, L"FFmpeg: avcodec_receive_frame(audio) failed: ", ret);
				return false;
			}
			if (!handle_audio_frame(ai, af.get()))
				return false;
		}
	}

	if (venc) {
		avcodec_send_frame(venc.get(), nullptr);
		AVStream *out_st = ofmt->streams[video_out];
		if (!write_encoded(venc.get(), out_st, video_out))
			return false;
	}
	for (size_t i = 0; i < aenc.size(); i++) {
		if (!flush_audio_fifo(i))
			return false;
	}
	for (size_t i = 0; i < aenc.size(); i++) {
		avcodec_send_frame(aenc[i].get(), nullptr);
		AVStream *out_st = ofmt->streams[audio_out[i]];
		if (!write_encoded(aenc[i].get(), out_st, audio_out[i]))
			return false;
	}

	ret = av_write_trailer(ofmt.get());
	if (ret < 0) {
		set_err(outError, L"FFmpeg: av_write_trailer failed: ", ret);
		return false;
	}

	progress.store(1.0);
	return true;
}

// --- Stream-copy trim/remux ---
static bool remux_trim(const std::wstring &inputPath, const std::wstring &outputPath, double startSeconds,
		       double endSeconds, const std::vector<int> &streamIndicesToKeep,
		       std::atomic<bool> &cancelRequested, std::atomic<double> &progress, std::wstring &outError)
{
	outError.clear();

	std::string inUtf8 = WideToUtf8(inputPath);
	std::string outUtf8 = WideToUtf8(outputPath);
	if (inUtf8.empty() || outUtf8.empty()) {
		outError = L"Invalid input/output path";
		return false;
	}

	InterruptCtx ictx;
	ictx.cancel = &cancelRequested;

	AvCloser closer;
	AVFormatContext *ifmt = nullptr;
	ifmt = avformat_alloc_context();
	if (!ifmt) {
		outError = L"FFmpeg: avformat_alloc_context failed";
		return false;
	}
	ifmt->interrupt_callback = AVIOInterruptCB{&av_interrupt_cb, &ictx};

	int ret = avformat_open_input(&ifmt, inUtf8.c_str(), nullptr, nullptr);
	if (ret < 0) {
		char errbuf[256];
		av_strerror(ret, errbuf, sizeof(errbuf));
		outError = L"FFmpeg: failed to open input: " + Utf8ToWide(errbuf);
		avformat_free_context(ifmt);
		return false;
	}
	closer.in = ifmt;

	ret = avformat_find_stream_info(ifmt, nullptr);
	if (ret < 0) {
		char errbuf[256];
		av_strerror(ret, errbuf, sizeof(errbuf));
		outError = L"FFmpeg: failed to read stream info: " + Utf8ToWide(errbuf);
		return false;
	}

	AVFormatContext *ofmt = nullptr;
	ret = avformat_alloc_output_context2(&ofmt, nullptr, nullptr, outUtf8.c_str());
	if (ret < 0 || !ofmt) {
		outError = L"FFmpeg: failed to create output context";
		return false;
	}
	closer.out = ofmt;
	ofmt->interrupt_callback = AVIOInterruptCB{&av_interrupt_cb, &ictx};

	std::vector<bool> keep(static_cast<size_t>(ifmt->nb_streams), true);
	if (!streamIndicesToKeep.empty()) {
		std::fill(keep.begin(), keep.end(), false);
		for (int idx : streamIndicesToKeep) {
			if (idx >= 0 && idx < static_cast<int>(ifmt->nb_streams)) {
				keep[static_cast<size_t>(idx)] = true;
			}
		}
	}

	std::vector<int> stream_mapping(static_cast<size_t>(ifmt->nb_streams), -1);
	int ref_in_stream = -1;
	for (unsigned i = 0; i < ifmt->nb_streams; i++) {
		if (!keep[i])
			continue;
		AVStream *in_stream = ifmt->streams[i];
		if (!in_stream || !in_stream->codecpar)
			continue;

		// Video timestamps drive progress and end detection when present.
		if (ref_in_stream < 0 && in_stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			ref_in_stream = static_cast<int>(i);
		}

		AVStream *out_stream = avformat_new_stream(ofmt, nullptr);
		if (!out_stream) {
			outError = L"FFmpeg: avformat_new_stream failed";
			return false;
		}
		stream_mapping[i] = static_cast<int>(out_stream->index);

		ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
		if (ret < 0) {
			outError = L"FFmpeg: avcodec_parameters_copy failed";
			return false;
		}
		out_stream->codecpar->codec_tag = 0;
		out_stream->time_base = in_stream->time_base;
	}

	if (ref_in_stream < 0) {
		for (unsigned i = 0; i < ifmt->nb_streams; i++) {
			if (keep[i] && stream_mapping[i] >= 0) {
				ref_in_stream = static_cast<int>(i);
				break;
			}
		}
	}

	if (ref_in_stream < 0) {
		outError = L"FFmpeg: no streams selected";
		return false;
	}

	if (!(ofmt->oformat->flags & AVFMT_NOFILE)) {
		ret = avio_open(&ofmt->pb, outUtf8.c_str(), AVIO_FLAG_WRITE);
		if (ret < 0) {
			char errbuf[256];
			av_strerror(ret, errbuf, sizeof(errbuf));
			outError = L"FFmpeg: failed to open output file: " + Utf8ToWide(errbuf);
			return false;
		}
		closer.out_opened = true;
	}

	AVDictionary *mux_opts = nullptr;
	// faststart moves moov to the file head for quicker MP4 playback.
	if (ofmt->oformat && ofmt->oformat->name &&
	    (!strcmp(ofmt->oformat->name, "mp4") || !strcmp(ofmt->oformat->name, "mov"))) {
		av_dict_set(&mux_opts, "movflags", "+faststart", 0);
	}

	ret = avformat_write_header(ofmt, &mux_opts);
	av_dict_free(&mux_opts);
	if (ret < 0) {
		char errbuf[256];
		av_strerror(ret, errbuf, sizeof(errbuf));
		outError = L"FFmpeg: avformat_write_header failed: " + Utf8ToWide(errbuf);
		return false;
	}

	const bool hasEnd = (endSeconds > startSeconds) && (endSeconds > 0.0);
	const double duration = hasEnd ? (endSeconds - startSeconds) : 0.0;
	progress.store(0.0);

	// Stream copy usually begins on the nearest keyframe after seek.
	if (startSeconds > 0.0) {
		int64_t seek_ts = static_cast<int64_t>(llround(startSeconds * static_cast<double>(AV_TIME_BASE)));
		ret = av_seek_frame(ifmt, -1, seek_ts, AVSEEK_FLAG_BACKWARD);
		if (ret >= 0) {
			avformat_flush(ifmt);
		}
	}

	AVPacket pkt = {};

	const double eps = 0.001;

	// After AVSEEK_FLAG_BACKWARD, align to the nearest preceding keyframe so the kept
	// segment is as close as possible to the requested duration (lossless stream copy).
	double cutStartSeconds = std::max(0.0, startSeconds);
	bool needKeyframeAlign = (cutStartSeconds > 0.0);
	bool waitingForFirstKeyframe = false;
	if (needKeyframeAlign) {
		AVStream *refStream = ifmt->streams[ref_in_stream];
		if (refStream && refStream->codecpar && refStream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			waitingForFirstKeyframe = true;
		}
	}

	// End each stream independently so trailing audio is not dropped early.
	std::vector<bool> stream_done(static_cast<size_t>(ifmt->nb_streams), false);
	auto all_done = [&]() -> bool {
		for (unsigned i = 0; i < ifmt->nb_streams; i++) {
			if (stream_mapping[i] < 0)
				continue;
			if (!stream_done[i])
				return false;
		}
		return true;
	};

	bool wrote_any = false;
	while (true) {
		if (cancelRequested.load()) {
			outError = L"Cancelled";
			return false;
		}

		ret = av_read_frame(ifmt, &pkt);
		if (ret < 0) {
			break;
		}

		const int in_idx = pkt.stream_index;
		if (in_idx < 0 || in_idx >= static_cast<int>(ifmt->nb_streams) ||
		    stream_mapping[static_cast<size_t>(in_idx)] < 0) {
			av_packet_unref(&pkt);
			continue;
		}

		AVStream *in_stream = ifmt->streams[in_idx];
		int64_t ts = (pkt.pts != AV_NOPTS_VALUE) ? pkt.pts : pkt.dts;
		double t = PtsToSeconds(ts, in_stream->time_base);

		// Start on the first keyframe after the backward seek (at or before startSeconds).
		if (waitingForFirstKeyframe) {
			if (in_idx == ref_in_stream && (pkt.flags & AV_PKT_FLAG_KEY)) {
				cutStartSeconds = t;
				waitingForFirstKeyframe = false;
			} else {
				av_packet_unref(&pkt);
				continue;
			}
		}

		if (cutStartSeconds > 0.0 && (t + eps) < cutStartSeconds) {
			av_packet_unref(&pkt);
			continue;
		}

		if (hasEnd && wrote_any && (t - eps) > endSeconds) {
			stream_done[static_cast<size_t>(in_idx)] = true;
			av_packet_unref(&pkt);
			if (all_done()) {
				break;
			}
			continue;
		}

		const int out_idx = stream_mapping[static_cast<size_t>(in_idx)];
		AVStream *out_stream = ofmt->streams[out_idx];

		const int64_t start_ts_in_tb = SecondsToTimestamp(cutStartSeconds, in_stream->time_base);
		if (pkt.pts != AV_NOPTS_VALUE) {
			pkt.pts -= start_ts_in_tb;
		}
		if (pkt.dts != AV_NOPTS_VALUE) {
			pkt.dts -= start_ts_in_tb;
		}

		if (pkt.pts != AV_NOPTS_VALUE && pkt.pts < 0)
			pkt.pts = 0;
		if (pkt.dts != AV_NOPTS_VALUE && pkt.dts < 0)
			pkt.dts = 0;

		if (pkt.pts != AV_NOPTS_VALUE) {
			pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, out_stream->time_base,
						   (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
		}
		if (pkt.dts != AV_NOPTS_VALUE) {
			pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base, out_stream->time_base,
						   (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
		}
		pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base, out_stream->time_base);
		pkt.pos = -1;
		pkt.stream_index = out_idx;

		ret = av_interleaved_write_frame(ofmt, &pkt);
		av_packet_unref(&pkt);
		if (ret < 0) {
			char errbuf[256];
			av_strerror(ret, errbuf, sizeof(errbuf));
			outError = L"FFmpeg: write failed: " + Utf8ToWide(errbuf);
			return false;
		}

		wrote_any = true;

		if (hasEnd && in_idx == ref_in_stream && duration > 0.0) {
			double p = (t - startSeconds) / duration;
			if (p < 0.0)
				p = 0.0;
			if (p > 1.0)
				p = 1.0;
			progress.store(p);
		} else if (!hasEnd) {
			progress.store(0.0);
		}
	}

	if (!wrote_any && cancelRequested.load()) {
		outError = L"Cancelled";
		return false;
	}

	if (!outError.empty()) {
		return false;
	}

	progress.store(1.0);
	return true;
}

} // namespace

bool overlay_replay_probe_playback_range(const std::wstring &inputPath, double &outStartSeconds,
					 double &outEndSeconds, std::wstring &outError,
					 int *outRefStreamIndex, int *outVideoPacketCount,
					 double *outContainerDurationSeconds)
{
	outStartSeconds = 0.0;
	outEndSeconds = 0.0;
	outError.clear();
	if (outRefStreamIndex)
		*outRefStreamIndex = -1;
	if (outVideoPacketCount)
		*outVideoPacketCount = 0;
	if (outContainerDurationSeconds)
		*outContainerDurationSeconds = -1.0;

	const std::string inUtf8 = WideToUtf8(inputPath);
	if (inUtf8.empty()) {
		outError = L"Invalid input path";
		return false;
	}

	AVFormatContext *ifmt = avformat_alloc_context();
	if (!ifmt) {
		outError = L"FFmpeg: avformat_alloc_context failed";
		return false;
	}

	int ret = avformat_open_input(&ifmt, inUtf8.c_str(), nullptr, nullptr);
	if (ret < 0) {
		char errbuf[256];
		av_strerror(ret, errbuf, sizeof(errbuf));
		outError = L"FFmpeg: failed to open input: " + Utf8ToWide(errbuf);
		avformat_free_context(ifmt);
		return false;
	}

	ret = avformat_find_stream_info(ifmt, nullptr);
	if (ret < 0) {
		char errbuf[256];
		av_strerror(ret, errbuf, sizeof(errbuf));
		outError = L"FFmpeg: failed to read stream info: " + Utf8ToWide(errbuf);
		avformat_close_input(&ifmt);
		return false;
	}

	int ref_stream = -1;
	for (unsigned i = 0; i < ifmt->nb_streams; i++) {
		AVStream *st = ifmt->streams[i];
		if (st && st->codecpar && st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			ref_stream = static_cast<int>(i);
			break;
		}
	}
	if (ref_stream < 0) {
		for (unsigned i = 0; i < ifmt->nb_streams; i++) {
			if (ifmt->streams[i]) {
				ref_stream = static_cast<int>(i);
				break;
			}
		}
	}

	double min_pts = std::numeric_limits<double>::infinity();
	double max_pts = 0.0;
	bool found_packet = false;
	int video_packet_count = 0;

	if (ref_stream >= 0) {
		if (outRefStreamIndex)
			*outRefStreamIndex = ref_stream;

		AVPacket pkt = {};
		while ((ret = av_read_frame(ifmt, &pkt)) >= 0) {
			if (pkt.stream_index != ref_stream) {
				av_packet_unref(&pkt);
				continue;
			}

			++video_packet_count;

			AVStream *st = ifmt->streams[ref_stream];
			int64_t ts = (pkt.pts != AV_NOPTS_VALUE) ? pkt.pts : pkt.dts;
			if (ts == AV_NOPTS_VALUE) {
				av_packet_unref(&pkt);
				continue;
			}

			const double t = PtsToSeconds(ts, st->time_base);
			double packet_end = t;
			if (pkt.duration > 0) {
				packet_end += static_cast<double>(pkt.duration) * av_q2d(st->time_base);
			}

			min_pts = std::min(min_pts, t);
			max_pts = std::max(max_pts, packet_end);
			found_packet = true;
			av_packet_unref(&pkt);
		}
		av_packet_unref(&pkt);
	}

	if (outVideoPacketCount)
		*outVideoPacketCount = video_packet_count;

	const double container_duration_sec =
		(ifmt->duration != AV_NOPTS_VALUE && ifmt->duration > 0)
			? static_cast<double>(ifmt->duration) / static_cast<double>(AV_TIME_BASE)
			: -1.0;
	if (outContainerDurationSeconds)
		*outContainerDurationSeconds = container_duration_sec;

	if (found_packet && max_pts > min_pts) {
		outStartSeconds = min_pts;
		outEndSeconds = max_pts;
		avformat_close_input(&ifmt);
		return true;
	}

	if (container_duration_sec > 0.0) {
		outStartSeconds = 0.0;
		outEndSeconds = container_duration_sec;
		avformat_close_input(&ifmt);
		return true;
	}

	double max_stream_end = 0.0;
	for (unsigned i = 0; i < ifmt->nb_streams; i++) {
		AVStream *st = ifmt->streams[i];
		if (!st || !st->codecpar)
			continue;
		if (st->duration > 0) {
			const double stream_duration = static_cast<double>(st->duration) * av_q2d(st->time_base);
			max_stream_end = std::max(max_stream_end, stream_duration);
		}
	}

	avformat_close_input(&ifmt);

	if (max_stream_end <= 0.0) {
		outError = L"FFmpeg: duration not found";
		return false;
	}

	outStartSeconds = 0.0;
	outEndSeconds = max_stream_end;
	return true;
}

bool overlay_replay_probe_duration_seconds(const std::wstring &inputPath, double &outDurationSeconds,
					   std::wstring &outError)
{
	double start_seconds = 0.0;
	double end_seconds = 0.0;
	if (!overlay_replay_probe_playback_range(inputPath, start_seconds, end_seconds, outError)) {
		return false;
	}
	outDurationSeconds = std::max(0.0, end_seconds - start_seconds);
	return outDurationSeconds > 0.0;
}

bool overlay_replay_lossless_trim(const std::wstring &inputPath, const std::wstring &outputPath,
				  double startSeconds, std::wstring &outError)
{
	std::atomic<bool> cancelRequested{false};
	std::atomic<double> progress{0.0};
	return remux_trim(inputPath, outputPath, startSeconds, 0.0, {}, cancelRequested, progress, outError);
}

bool overlay_replay_lossless_trim_keep_last(const std::wstring &inputPath, const std::wstring &outputPath,
					    double keepSeconds, std::wstring &outError)
{
	outError.clear();
	if (keepSeconds <= 0.0) {
		outError = L"Invalid keep duration";
		return false;
	}

	double playback_start_sec = 0.0;
	double playback_end_sec = 0.0;
	if (!overlay_replay_probe_playback_range(inputPath, playback_start_sec, playback_end_sec, outError)) {
		return false;
	}

	const double file_duration_sec = std::max(0.0, playback_end_sec - playback_start_sec);

	constexpr double kTailEpsilon = 0.05;
	if (keepSeconds + kTailEpsilon >= file_duration_sec) {
		return true;
	}

	const double startSeconds = std::max(playback_start_sec, playback_end_sec - keepSeconds);
	obs_log(LOG_INFO,
		"[SmartReplay] trim keep-last path=%s playback=[%.3f..%.3f] span=%.3f keep=%.3f start=%.3f",
		overlay::util::WideToUtf8(inputPath).c_str(), playback_start_sec, playback_end_sec,
		file_duration_sec, keepSeconds, startSeconds);
	return overlay_replay_lossless_trim(inputPath, outputPath, startSeconds, outError);
}

OverlayTrimmer::OverlayTrimmer()
	: m_status(Status::Idle),
	  m_progress(0.0),
	  m_cancelRequested(false),
	  m_exportTotalDuration(0.0)
{
}

OverlayTrimmer::~OverlayTrimmer()
{
	if (m_worker.joinable()) {
		m_worker.join();
	}
}

void OverlayTrimmer::Reset()
{
	m_lastError.clear();
	m_lastOutput.clear();
	m_status.store(Status::Idle);
	m_progress.store(0.0);
	m_exportTotalDuration = 0.0;
}

std::wstring OverlayTrimmer::GetProgressText() const
{
	double p = m_progress.load();
	if (m_exportTotalDuration <= 0.0) {
		int pc = static_cast<int>(p * 100.0 + 0.5);
		if (pc > 100)
			pc = 100;
		return std::to_wstring(pc) + L"%";
	}
	int currentSec = static_cast<int>(p * m_exportTotalDuration + 0.5);
	int totalSec = static_cast<int>(m_exportTotalDuration + 0.5);
	int cMin = currentSec / 60, cSec = currentSec % 60;
	int tMin = totalSec / 60, tSec = totalSec % 60;
	wchar_t buf[32];
	swprintf(buf, 32, L"%d:%02d / %d:%02d", cMin, cSec, tMin, tSec);
	return std::wstring(buf);
}

void OverlayTrimmer::RequestCancel()
{
	m_cancelRequested.store(true);
}

bool OverlayTrimmer::StartTrim(const std::wstring &inputPath, const std::wstring &outputPath, double startSeconds,
			       double endSeconds)
{
	if (m_status.load() == Status::Trimming) {
		return false;
	}
	if (m_worker.joinable()) {
		m_worker.join();
	}
	Reset();
	m_status.store(Status::Trimming);
	m_worker = std::thread(&OverlayTrimmer::RunTrim, this, inputPath, outputPath, startSeconds, endSeconds);
	return true;
}

bool OverlayTrimmer::StartExport(const std::wstring &inputPath, const std::wstring &outputPath, double startSeconds,
				 double endSeconds, bool compress, int crf, const std::vector<int> &streamIndicesToKeep,
				 const std::wstring &customFfmpegArgs, bool useCpuEncoder)
{
	if (m_status.load() == Status::Trimming) {
		return false;
	}
	if (m_worker.joinable()) {
		m_worker.join();
	}
	Reset();
	m_status.store(Status::Trimming);
	if (crf < 18)
		crf = 18;
	if (crf > 32)
		crf = 32;
	m_worker = std::thread(&OverlayTrimmer::RunExport, this, inputPath, outputPath, startSeconds, endSeconds,
			       compress, crf, streamIndicesToKeep, customFfmpegArgs, useCpuEncoder);
	return true;
}

bool OverlayTrimmer::StartTranscode(const std::wstring &inputPath, const std::wstring &outputPath, int crf)
{
	if (m_status.load() == Status::Trimming) {
		return false;
	}
	if (m_worker.joinable()) {
		m_worker.join();
	}
	Reset();
	m_status.store(Status::Trimming);
	if (crf < 18)
		crf = 18;
	if (crf > 32)
		crf = 32;
	m_worker = std::thread(&OverlayTrimmer::RunTranscode, this, inputPath, outputPath, crf);
	return true;
}

void OverlayTrimmer::RunTrim(const std::wstring &inputPath, const std::wstring &outputPath, double startSeconds,
			     double endSeconds)
{
	m_cancelRequested.store(false);
	m_progress.store(0.0);
	m_exportTotalDuration = (endSeconds > startSeconds) ? (endSeconds - startSeconds) : 0.0;

	std::wstring err;
	bool ok = remux_trim(inputPath, outputPath, startSeconds, endSeconds,
			     /*streamIndicesToKeep=*/{}, m_cancelRequested, m_progress, err);
	if (ok) {
		m_lastOutput = outputPath;
		m_status.store(Status::Success);
	} else {
		m_lastError = err.empty() ? L"Trim failed" : err;
		m_status.store(Status::Error);
	}
}

void OverlayTrimmer::RunExport(const std::wstring &inputPath, const std::wstring &outputPath, double startSeconds,
			       double endSeconds, bool compress, int crf, const std::vector<int> &streamIndicesToKeep,
			       const std::wstring &customFfmpegArgs, bool useCpuEncoder)
{
	if (outputPath.empty()) {
		m_lastError = L"Output path is empty";
		m_status.store(Status::Error);
		obs_log(LOG_ERROR, "Gallery export failed: output path is empty");
		return;
	}

	m_cancelRequested.store(false);
	m_progress.store(0.0);
	double totalDuration = (endSeconds > startSeconds) ? (endSeconds - startSeconds) : 0.0;
	m_exportTotalDuration = totalDuration;

	if (!customFfmpegArgs.empty()) {
		// In-process mode cannot spawn external ffmpeg with custom args.
		m_lastError = L"Custom ffmpeg args are not supported (in-process mode)";
		m_status.store(Status::Error);
		obs_log(LOG_ERROR, "Gallery export failed: custom ffmpeg args are not supported");
		return;
	}

	std::wstring err;
	bool ok = false;
	if (!compress) {
		ok = remux_trim(inputPath, outputPath, startSeconds, endSeconds, streamIndicesToKeep, m_cancelRequested,
				m_progress, err);
	} else {
		ok = transcode_trim_mp4(inputPath, outputPath, startSeconds, endSeconds, crf, useCpuEncoder,
					streamIndicesToKeep, m_cancelRequested, m_progress, err);
	}
	if (ok) {
		m_lastOutput = outputPath;
		m_status.store(Status::Success);
	} else {
		m_lastError = err.empty() ? L"Export failed" : err;
		m_status.store(Status::Error);
		if (err == L"Cancelled") {
			obs_log(LOG_INFO, "Gallery export cancelled (input: %s)", WideToUtf8(inputPath).c_str());
		} else {
			obs_log(LOG_ERROR,
				"Gallery export failed: %s (input: %s, output: %s, range: %.3f-%.3f s, compress: %s, crf: %d)",
				WideToUtf8(m_lastError).c_str(), WideToUtf8(inputPath).c_str(),
				WideToUtf8(outputPath).c_str(), startSeconds, endSeconds, compress ? "yes" : "no", crf);
		}
	}
}

void OverlayTrimmer::RunTranscode(const std::wstring &inputPath, const std::wstring &outputPath, int crf)
{
	UNUSED_PARAMETER(crf);

	m_cancelRequested.store(false);
	m_progress.store(0.0);
	m_exportTotalDuration = 0.0;

	// Legacy transcode API now remuxes the full file in-process.
	std::wstring err;
	bool ok = remux_trim(inputPath, outputPath, /*startSeconds=*/0.0, /*endSeconds=*/0.0,
			     /*streams=*/{}, m_cancelRequested, m_progress, err);
	if (ok) {
		m_lastOutput = outputPath;
		m_status.store(Status::Success);
	} else {
		m_lastError = err.empty() ? L"Transcode failed" : err;
		m_status.store(Status::Error);
	}
}

#endif // _WIN32
