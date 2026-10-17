#ifdef _WIN32

#include "overlay-trimmer.h"
#include "overlay-ffmpeg-time-utils.h"
#include "overlay-ffmpeg-video-decode.h"
#include "overlay-mp4-box-reader.h"
#include "overlay-string-utils.h"
#include <obs-module.h>
#include <plugin-support.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <limits>
#include <sstream>
#include <thread>
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
using overlay::ffmpeg::FramePtsSeconds;
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

// Copies disposition/metadata and restores OBS audio names from trak/udta/name
// (FFmpeg mov demuxer does not expose them; mov muxer writes title as that atom).
static void apply_output_stream_metadata(AVStream *out_st, const AVStream *in_st,
					 const std::vector<std::optional<std::string>> &mp4AudioNames,
					 size_t audioOrdinal)
{
	if (!out_st || !in_st)
		return;

	out_st->disposition = in_st->disposition;
	av_dict_copy(&out_st->metadata, in_st->metadata, 0);

	if (!in_st->codecpar || in_st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
		return;
	if (audioOrdinal >= mp4AudioNames.size() || !mp4AudioNames[audioOrdinal].has_value())
		return;
	const std::string &name = *mp4AudioNames[audioOrdinal];
	if (!name.empty()) {
		av_dict_set(&out_st->metadata, "title", name.c_str(), 0);
	}
}

static size_t audio_stream_ordinal(const AVFormatContext *ifmt, int streamIndex)
{
	size_t ordinal = 0;
	if (!ifmt)
		return 0;
	for (unsigned i = 0; i < ifmt->nb_streams; i++) {
		const AVStream *st = ifmt->streams[i];
		if (!st || !st->codecpar || st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
			continue;
		if (static_cast<int>(i) == streamIndex)
			return ordinal;
		++ordinal;
	}
	return ordinal;
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
// outWidth/outHeight/outFps: 0 keeps source values.
static bool transcode_trim_mp4(const std::wstring &inputPath, const std::wstring &outputPath, double startSeconds,
			       double endSeconds, int crf, bool useCpuEncoder, int outWidth, int outHeight,
			       double outFps, const char *containerName, const std::vector<int> &streamIndicesToKeep,
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
	const char *muxName = (containerName && containerName[0]) ? containerName : "mp4";
	ret = avformat_alloc_output_context2(&ofmt_raw, nullptr, muxName, outUtf8.c_str());
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

	const std::vector<std::optional<std::string>> mp4AudioNames =
		overlay::mp4::ReadMp4AudioTrackNames(inputPath);

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
		int srcW = vdec->width > 0 ? vdec->width : static_cast<int>(in_st->codecpar->width);
		int srcH = vdec->height > 0 ? vdec->height : static_cast<int>(in_st->codecpar->height);
		if (outWidth > 0)
			srcW = outWidth;
		if (outHeight > 0)
			srcH = outHeight;
		else if (outWidth > 0 && vdec->width > 0 && vdec->height > 0) {
			srcH = std::max(2, (outWidth * vdec->height / vdec->width) & ~1);
			srcW = srcW & ~1;
		}
		srcW = std::max(2, srcW & ~1);
		srcH = std::max(2, srcH & ~1);
		ctx->width = srcW;
		ctx->height = srcH;
		if (outFps > 0.5) {
			AVRational fr = av_d2q(outFps, 100000);
			if (fr.num <= 0 || fr.den <= 0)
				fr = AVRational{30, 1};
			ctx->framerate = fr;
			ctx->time_base = av_inv_q(fr);
		} else {
			ctx->time_base = pick_fps_timebase(in_st);
			ctx->framerate = av_inv_q(ctx->time_base);
		}
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
		apply_output_stream_metadata(out_st, in_st, mp4AudioNames, 0);
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
		apply_output_stream_metadata(out_st, ifmt->streams[audio_in[i]], mp4AudioNames,
					     audio_stream_ordinal(ifmt.get(), audio_in[i]));

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
	double fileDurSec = -1.0;
	if (ifmt->duration > 0 && ifmt->duration != AV_NOPTS_VALUE)
		fileDurSec = static_cast<double>(ifmt->duration) / static_cast<double>(AV_TIME_BASE);
	for (unsigned i = 0; i < ifmt->nb_streams; i++) {
		AVStream *st = ifmt->streams[i];
		if (!st || st->duration <= 0 || st->duration == AV_NOPTS_VALUE)
			continue;
		fileDurSec = std::max(fileDurSec, st->duration * av_q2d(st->time_base));
	}
	const double totalDuration =
		hasEnd ? (endSeconds - startSeconds)
		       : ((fileDurSec > startSeconds) ? (fileDurSec - std::max(0.0, startSeconds)) : 0.0);
	progress.store(0.0);

	auto frame_time = [&](AVFrame *f, int stream_index) -> double {
		return FramePtsSeconds(f, ifmt->streams[stream_index]);
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
		const bool needScale = (srcFmt != venc->pix_fmt) || (inFrame->width != venc->width) ||
				       (inFrame->height != venc->height);
		if (needScale) {
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

		if (totalDuration > 0.0) {
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
static std::vector<int> resolve_stream_keep(const std::wstring &inputPath, const OverlayExportOptions &options)
{
	if (!options.streamIndicesToKeep.empty())
		return options.streamIndicesToKeep;
	if (!options.tracksConfigured)
		return {};

	std::vector<int> keep;
	const std::string inUtf8 = WideToUtf8(inputPath);
	if (inUtf8.empty())
		return keep;

	AVFormatContext *ifmt = nullptr;
	if (avformat_open_input(&ifmt, inUtf8.c_str(), nullptr, nullptr) < 0)
		return keep;
	if (avformat_find_stream_info(ifmt, nullptr) < 0) {
		avformat_close_input(&ifmt);
		return keep;
	}

	int audioOrd = 0;
	for (unsigned i = 0; i < ifmt->nb_streams; i++) {
		AVStream *st = ifmt->streams[i];
		if (!st || !st->codecpar)
			continue;
		const auto t = st->codecpar->codec_type;
		if (t == AVMEDIA_TYPE_VIDEO) {
			if (options.keepVideoTrack)
				keep.push_back(static_cast<int>(i));
		} else if (t == AVMEDIA_TYPE_AUDIO) {
			bool want = false;
			for (int o : options.keepAudioOrdinals) {
				if (o == audioOrd) {
					want = true;
					break;
				}
			}
			if (want)
				keep.push_back(static_cast<int>(i));
			audioOrd++;
		}
	}
	avformat_close_input(&ifmt);
	return keep;
}

static bool remux_trim(const std::wstring &inputPath, const std::wstring &outputPath, double startSeconds,
		       double endSeconds, const std::vector<int> &streamIndicesToKeep,
		       std::atomic<bool> &cancelRequested, std::atomic<double> &progress, std::wstring &outError,
		       bool allowOsCopy = true)
{
	outError.clear();

	std::string inUtf8 = WideToUtf8(inputPath);
	std::string outUtf8 = WideToUtf8(outputPath);
	if (inUtf8.empty() || outUtf8.empty()) {
		outError = L"Invalid input/output path";
		return false;
	}

	const bool noTrimStart = startSeconds <= 0.001;
	const bool noTrimEnd = !(endSeconds > startSeconds && endSeconds > 0.0);

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

	auto keeps_all_av = [&]() -> bool {
		if (streamIndicesToKeep.empty())
			return true;
		for (unsigned i = 0; i < ifmt->nb_streams; i++) {
			AVStream *st = ifmt->streams[i];
			if (!st || !st->codecpar)
				continue;
			const auto t = st->codecpar->codec_type;
			if (t != AVMEDIA_TYPE_VIDEO && t != AVMEDIA_TYPE_AUDIO)
				continue;
			bool listed = false;
			for (int idx : streamIndicesToKeep) {
				if (idx == static_cast<int>(i)) {
					listed = true;
					break;
				}
			}
			if (!listed)
				return false;
		}
		return true;
	};


	double fileDurSec = -1.0;
	if (ifmt->duration > 0 && ifmt->duration != AV_NOPTS_VALUE)
		fileDurSec = static_cast<double>(ifmt->duration) / static_cast<double>(AV_TIME_BASE);
	for (unsigned i = 0; i < ifmt->nb_streams; i++) {
		AVStream *st = ifmt->streams[i];
		if (!st || st->duration <= 0 || st->duration == AV_NOPTS_VALUE)
			continue;
		const double d = st->duration * av_q2d(st->time_base);
		if (d > fileDurSec)
			fileDurSec = d;
	}
	const double endSlack = (fileDurSec > 0.0) ? std::max(0.25, fileDurSec * 0.01) : 0.25;
	const bool coversWholeFile =
		noTrimStart && (noTrimEnd || (fileDurSec > 0.0 && endSeconds + endSlack >= fileDurSec));

	if (allowOsCopy && coversWholeFile && keeps_all_av()) {
		closer.in = nullptr;
		avformat_close_input(&ifmt);
		if (cancelRequested.load()) {
			outError = L"Cancelled";
			return false;
		}
		if (!CopyFileW(inputPath.c_str(), outputPath.c_str(), FALSE)) {
			outError = L"CopyFile failed";
			return false;
		}
		progress.store(1.0);
		return true;
	}

	AVFormatContext *ofmt = nullptr;

	ret = avformat_alloc_output_context2(&ofmt, nullptr, nullptr, outUtf8.c_str());
	if (ret < 0 || !ofmt)
		ret = avformat_alloc_output_context2(&ofmt, nullptr, "mp4", outUtf8.c_str());
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

	const std::vector<std::optional<std::string>> mp4AudioNames =
		overlay::mp4::ReadMp4AudioTrackNames(inputPath);

	std::vector<int> stream_mapping(static_cast<size_t>(ifmt->nb_streams), -1);
	int ref_in_stream = -1;
	size_t audio_ordinal = 0;
	for (unsigned i = 0; i < ifmt->nb_streams; i++) {
		AVStream *in_stream = ifmt->streams[i];
		const bool is_audio =
			in_stream && in_stream->codecpar && in_stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO;
		const size_t this_audio_ordinal = audio_ordinal;
		if (is_audio)
			++audio_ordinal;

		if (!keep[i])
			continue;
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
		apply_output_stream_metadata(out_stream, in_stream, mp4AudioNames, this_audio_ordinal);
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

	// No +faststart here: it forces a full second rewrite pass and makes stream-copy
	// feel slow. Local gallery exports do not need moov-at-front.
	ret = avformat_write_header(ofmt, nullptr);
	if (ret < 0) {
		char errbuf[256];
		av_strerror(ret, errbuf, sizeof(errbuf));
		outError = L"FFmpeg: avformat_write_header failed: " + Utf8ToWide(errbuf);
		return false;
	}

	const bool hasEnd = (endSeconds > startSeconds) && (endSeconds > 0.0);
	const double duration =
		hasEnd ? (endSeconds - startSeconds)
		       : ((fileDurSec > startSeconds) ? (fileDurSec - std::max(0.0, startSeconds)) : 0.0);
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

	// Per-output-stream timestamp repair: OBS/MP4 B-frames often have DTS < PTS near a cut.
	// Clamping every negative stamp to 0 creates duplicate DTS and makes the MP4 muxer
	// return EINVAL — that is exactly what broke Merge (which always remuxes temps).
	const size_t nOutStreams = ofmt->nb_streams;
	std::vector<bool> sawOut(nOutStreams, false);
	std::vector<int64_t> tsOffset(nOutStreams, 0);
	std::vector<int64_t> lastDts(nOutStreams, AV_NOPTS_VALUE);

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
		if (pkt.pts != AV_NOPTS_VALUE)
			pkt.pts -= start_ts_in_tb;
		if (pkt.dts != AV_NOPTS_VALUE)
			pkt.dts -= start_ts_in_tb;

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

		int64_t base = AV_NOPTS_VALUE;
		if (pkt.dts != AV_NOPTS_VALUE)
			base = pkt.dts;
		else if (pkt.pts != AV_NOPTS_VALUE)
			base = pkt.pts;
		else
			base = 0;

		const size_t oi = static_cast<size_t>(out_idx);
		if (!sawOut[oi]) {
			// Shift so the first packet lands at dts>=0 without collapsing B-frame order.
			tsOffset[oi] = (base < 0) ? -base : 0;
			sawOut[oi] = true;
		}
		if (pkt.dts != AV_NOPTS_VALUE)
			pkt.dts += tsOffset[oi];
		if (pkt.pts != AV_NOPTS_VALUE)
			pkt.pts += tsOffset[oi];

		if (pkt.dts == AV_NOPTS_VALUE) {
			pkt.dts = (lastDts[oi] != AV_NOPTS_VALUE) ? (lastDts[oi] + 1) : 0;
		}
		if (lastDts[oi] != AV_NOPTS_VALUE && pkt.dts <= lastDts[oi])
			pkt.dts = lastDts[oi] + 1;
		if (pkt.pts == AV_NOPTS_VALUE)
			pkt.pts = pkt.dts;
		if (pkt.pts < pkt.dts)
			pkt.pts = pkt.dts;

		const int64_t writtenDts = pkt.dts;
		ret = av_interleaved_write_frame(ofmt, &pkt);
		av_packet_unref(&pkt);
		if (ret < 0) {
			char errbuf[256];
			av_strerror(ret, errbuf, sizeof(errbuf));
			outError = L"FFmpeg: write failed: " + Utf8ToWide(errbuf);
			return false;
		}
		lastDts[oi] = writtenDts;

		wrote_any = true;

		if (in_idx == ref_in_stream && duration > 0.0) {
			double p = (t - startSeconds) / duration;
			if (p < 0.0)
				p = 0.0;
			if (p > 1.0)
				p = 1.0;
			progress.store(p);
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

static void build_gif_palette(uint32_t *palette, int colors)
{
	colors = std::clamp(colors, 2, 256);
	palette[0] = 0x00000000; // transparent-ish black reserved
	int filled = 1;
	const int levels = std::max(2, static_cast<int>(std::ceil(std::cbrt(static_cast<double>(colors - 1)))));
	for (int r = 0; r < levels && filled < colors; r++) {
		for (int g = 0; g < levels && filled < colors; g++) {
			for (int b = 0; b < levels && filled < colors; b++) {
				const int rr = (r * 255) / std::max(1, levels - 1);
				const int gg = (g * 255) / std::max(1, levels - 1);
				const int bb = (b * 255) / std::max(1, levels - 1);
				palette[filled++] = (0xFFu << 24) | (rr << 16) | (gg << 8) | bb;
			}
		}
	}
	while (filled < colors) {
		palette[filled] = palette[filled - 1];
		filled++;
	}
	for (int i = colors; i < 256; i++)
		palette[i] = 0;
}

static uint8_t nearest_palette_index(const uint32_t *palette, int colors, int r, int g, int b)
{
	int best = 0;
	int bestDist = INT_MAX;
	for (int i = 0; i < colors; i++) {
		const int pr = (palette[i] >> 16) & 0xFF;
		const int pg = (palette[i] >> 8) & 0xFF;
		const int pb = palette[i] & 0xFF;
		const int dr = r - pr;
		const int dg = g - pg;
		const int db = b - pb;
		const int dist = dr * dr + dg * dg + db * db;
		if (dist < bestDist) {
			bestDist = dist;
			best = i;
		}
	}
	return static_cast<uint8_t>(best);
}

static void rgb_to_pal8(const uint8_t *rgb, int width, int height, int rgbLinesize, uint8_t *dst, int dstLinesize,
			const uint32_t *palette, int colors, int dither)
{
	std::vector<int> errR, errG, errB;
	if (dither == 2) {
		errR.assign(static_cast<size_t>(width + 2), 0);
		errG.assign(static_cast<size_t>(width + 2), 0);
		errB.assign(static_cast<size_t>(width + 2), 0);
	}
	static const int bayer4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

	for (int y = 0; y < height; y++) {
		std::vector<int> nextR, nextG, nextB;
		if (dither == 2) {
			nextR.assign(static_cast<size_t>(width + 2), 0);
			nextG.assign(static_cast<size_t>(width + 2), 0);
			nextB.assign(static_cast<size_t>(width + 2), 0);
		}
		const uint8_t *srcRow = rgb + y * rgbLinesize;
		uint8_t *dstRow = dst + y * dstLinesize;
		for (int x = 0; x < width; x++) {
			int r = srcRow[x * 3 + 0];
			int g = srcRow[x * 3 + 1];
			int b = srcRow[x * 3 + 2];
			if (dither == 1) {
				const int th = bayer4[y & 3][x & 3] - 8;
				r = std::clamp(r + th * 2, 0, 255);
				g = std::clamp(g + th * 2, 0, 255);
				b = std::clamp(b + th * 2, 0, 255);
			} else if (dither == 2) {
				r = std::clamp(r + errR[static_cast<size_t>(x + 1)] / 16, 0, 255);
				g = std::clamp(g + errG[static_cast<size_t>(x + 1)] / 16, 0, 255);
				b = std::clamp(b + errB[static_cast<size_t>(x + 1)] / 16, 0, 255);
			}
			const uint8_t idx = nearest_palette_index(palette, colors, r, g, b);
			dstRow[x] = idx;
			if (dither == 2) {
				const int pr = (palette[idx] >> 16) & 0xFF;
				const int pg = (palette[idx] >> 8) & 0xFF;
				const int pb = palette[idx] & 0xFF;
				const int er = r - pr;
				const int eg = g - pg;
				const int eb = b - pb;
				errR[static_cast<size_t>(x + 2)] += er * 7;
				errG[static_cast<size_t>(x + 2)] += eg * 7;
				errB[static_cast<size_t>(x + 2)] += eb * 7;
				nextR[static_cast<size_t>(x)] += er * 3;
				nextG[static_cast<size_t>(x)] += eg * 3;
				nextB[static_cast<size_t>(x)] += eb * 3;
				nextR[static_cast<size_t>(x + 1)] += er * 5;
				nextG[static_cast<size_t>(x + 1)] += eg * 5;
				nextB[static_cast<size_t>(x + 1)] += eb * 5;
				nextR[static_cast<size_t>(x + 2)] += er * 1;
				nextG[static_cast<size_t>(x + 2)] += eg * 1;
				nextB[static_cast<size_t>(x + 2)] += eb * 1;
			}
		}
		if (dither == 2) {
			errR.swap(nextR);
			errG.swap(nextG);
			errB.swap(nextB);
		}
	}
}

static bool gif_trim_export(const std::wstring &inputPath, const std::wstring &outputPath, double startSeconds,
			    double endSeconds, const OverlayExportOptions &options, std::atomic<bool> &cancelRequested,
			    std::atomic<double> &progress, std::wstring &outError)
{
	outError.clear();
	std::string inUtf8 = WideToUtf8(inputPath);
	std::string outUtf8 = WideToUtf8(outputPath);
	if (inUtf8.empty() || outUtf8.empty()) {
		outError = L"Invalid input/output path";
		return false;
	}
	if (GetFileAttributesW(inputPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
		outError = L"Input file not found: " + inputPath;
		return false;
	}

	InterruptCtx ictx;
	ictx.cancel = &cancelRequested;

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
	struct InFmtDeleter {
		void operator()(AVFormatContext *c) const
		{
			if (c)
				avformat_close_input(&c);
		}
	};
	std::unique_ptr<AVFormatContext, InFmtDeleter> ifmt(ifmt_raw);
	ret = avformat_find_stream_info(ifmt.get(), nullptr);
	if (ret < 0) {
		set_err(outError, L"FFmpeg: failed to read stream info: ", ret);
		return false;
	}

	int video_in = av_find_best_stream(ifmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (video_in < 0) {
		outError = L"FFmpeg: no video stream for GIF export";
		return false;
	}

	AVStream *in_st = ifmt->streams[video_in];
	AVCodecContext *vdec_raw = nullptr;
	VideoHwDecodeState vdec_hw;
	struct VideoHwReleaseGuard {
		VideoHwDecodeState *hw = nullptr;
		~VideoHwReleaseGuard()
		{
			if (hw)
				ReleaseVideoHwDecodeState(hw);
		}
	} vdec_hw_guard{&vdec_hw};
	const bool is_av1 = in_st->codecpar->codec_id == AV_CODEC_ID_AV1;
	if (!OpenVideoDecoder(&vdec_raw, in_st, &vdec_hw, !is_av1)) {
		outError = is_av1 ? L"FFmpeg: AV1 GIF export requires NVIDIA GPU decoder (av1_cuvid)"
				  : L"FFmpeg: video decoder not found";
		return false;
	}
	std::unique_ptr<AVCodecContext, CodecCtxDeleter> vdec(vdec_raw);

	int srcW = vdec->width > 0 ? vdec->width : in_st->codecpar->width;
	int srcH = vdec->height > 0 ? vdec->height : in_st->codecpar->height;
	int dstW = options.width > 0 ? options.width : srcW;
	int dstH = options.height > 0 ? options.height : 0;
	if (dstH <= 0 && srcW > 0)
		dstH = std::max(2, (dstW * srcH / srcW));
	if (dstH <= 0)
		dstH = srcH;
	dstW = std::max(2, dstW);
	dstH = std::max(2, dstH);

	double fps = options.fps > 0.5 ? options.fps : 15.0;
	AVRational fr = av_d2q(fps, 100000);
	if (fr.num <= 0 || fr.den <= 0)
		fr = AVRational{15, 1};

	const AVCodec *gifEnc = avcodec_find_encoder(AV_CODEC_ID_GIF);
	if (!gifEnc) {
		outError = L"FFmpeg: GIF encoder not found in this build";
		return false;
	}

	AVFormatContext *ofmt_raw = nullptr;
	ret = avformat_alloc_output_context2(&ofmt_raw, nullptr, "gif", outUtf8.c_str());
	if (ret < 0 || !ofmt_raw) {
		outError = L"FFmpeg: failed to create GIF output context";
		return false;
	}
	struct OutFmtDeleter {
		void operator()(AVFormatContext *c) const
		{
			if (!c)
				return;
			if (!(c->oformat->flags & AVFMT_NOFILE) && c->pb)
				avio_closep(&c->pb);
			avformat_free_context(c);
		}
	};
	std::unique_ptr<AVFormatContext, OutFmtDeleter> ofmt(ofmt_raw);
	ofmt->interrupt_callback = AVIOInterruptCB{&av_interrupt_cb, &ictx};

	AVCodecContext *venc_raw = avcodec_alloc_context3(gifEnc);
	if (!venc_raw) {
		outError = L"FFmpeg: avcodec_alloc_context3(gif) failed";
		return false;
	}
	std::unique_ptr<AVCodecContext, CodecCtxDeleter> venc(venc_raw);
	venc->width = dstW;
	venc->height = dstH;
	venc->time_base = av_inv_q(fr);
	venc->framerate = fr;
	venc->pix_fmt = AV_PIX_FMT_PAL8;
	if (ofmt->oformat->flags & AVFMT_GLOBALHEADER)
		venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	ret = avcodec_open2(venc.get(), gifEnc, nullptr);
	if (ret < 0) {
		set_err(outError, L"FFmpeg: avcodec_open2(gif) failed: ", ret);
		return false;
	}

	AVStream *out_st = avformat_new_stream(ofmt.get(), nullptr);
	if (!out_st) {
		outError = L"FFmpeg: avformat_new_stream(gif) failed";
		return false;
	}
	ret = avcodec_parameters_from_context(out_st->codecpar, venc.get());
	if (ret < 0) {
		outError = L"FFmpeg: avcodec_parameters_from_context(gif) failed";
		return false;
	}
	out_st->time_base = venc->time_base;

	if (!(ofmt->oformat->flags & AVFMT_NOFILE)) {
		ret = avio_open(&ofmt->pb, outUtf8.c_str(), AVIO_FLAG_WRITE);
		if (ret < 0) {
			set_err(outError, L"FFmpeg: failed to open GIF output: ", ret);
			return false;
		}
	}
	AVDictionary *mux_opts = nullptr;
	if (options.gifLoop == 0)
		av_dict_set(&mux_opts, "loop", "0", 0);
	else {
		char loopBuf[16];
		snprintf(loopBuf, sizeof(loopBuf), "%d", std::max(0, options.gifLoop - 1));
		av_dict_set(&mux_opts, "loop", loopBuf, 0);
	}
	ret = avformat_write_header(ofmt.get(), &mux_opts);
	av_dict_free(&mux_opts);
	if (ret < 0) {
		set_err(outError, L"FFmpeg: avformat_write_header(gif) failed: ", ret);
		return false;
	}

	uint32_t palette[256];
	const int colors = std::clamp(options.gifColors, 2, 256);
	build_gif_palette(palette, colors);

	struct SwsDeleter {
		void operator()(SwsContext *c) const { sws_freeContext(c); }
	};
	std::unique_ptr<SwsContext, SwsDeleter> sws;

	const double startSec = std::max(0.0, startSeconds);
	const bool hasEnd = (endSeconds > startSeconds) && (endSeconds > 0.0);
	double fileDurSec = -1.0;
	if (ifmt->duration > 0 && ifmt->duration != AV_NOPTS_VALUE)
		fileDurSec = static_cast<double>(ifmt->duration) / static_cast<double>(AV_TIME_BASE);
	for (unsigned i = 0; i < ifmt->nb_streams; i++) {
		AVStream *st = ifmt->streams[i];
		if (!st || st->duration <= 0 || st->duration == AV_NOPTS_VALUE)
			continue;
		fileDurSec = std::max(fileDurSec, st->duration * av_q2d(st->time_base));
	}
	const double totalDuration =
		hasEnd ? (endSeconds - startSeconds)
		       : ((fileDurSec > startSec) ? (fileDurSec - startSec) : 0.0);
	if (startSec > 0.0) {
		int64_t seek_ts = static_cast<int64_t>(llround(startSec * static_cast<double>(AV_TIME_BASE)));
		if (av_seek_frame(ifmt.get(), -1, seek_ts, AVSEEK_FLAG_BACKWARD) >= 0)
			avcodec_flush_buffers(vdec.get());
	}

	progress.store(0.0);
	int64_t framePts = 0;
	double nextFrameT = startSec;
	const double frameDur = av_q2d(av_inv_q(fr));

	std::unique_ptr<AVPacket, PacketDeleter> pkt(av_packet_alloc());
	std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
	std::unique_ptr<AVFrame, FrameDeleter> rgb(av_frame_alloc());
	std::unique_ptr<AVFrame, FrameDeleter> pal(av_frame_alloc());
	if (!pkt || !frame || !rgb || !pal) {
		outError = L"FFmpeg: av_frame_alloc failed";
		return false;
	}
	rgb->format = AV_PIX_FMT_RGB24;
	rgb->width = dstW;
	rgb->height = dstH;
	pal->format = AV_PIX_FMT_PAL8;
	pal->width = dstW;
	pal->height = dstH;
	if (av_frame_get_buffer(rgb.get(), 32) < 0 || av_frame_get_buffer(pal.get(), 32) < 0) {
		outError = L"FFmpeg: av_frame_get_buffer(gif) failed";
		return false;
	}
	memcpy(pal->data[1], palette, sizeof(palette));

	auto write_gif_frame = [&](AVFrame *srcCpu) -> bool {
		if (cancelRequested.load()) {
			outError = L"Cancelled";
			return false;
		}
		const double t = FramePtsSeconds(srcCpu, in_st);
		if (t + 0.001 < nextFrameT)
			return true;
		if (hasEnd && t > endSeconds)
			return true;

		AVFrame *use = srcCpu;
		std::unique_ptr<AVFrame, FrameDeleter> transferred;
		if (vdec_hw.active || srcCpu->format == AV_PIX_FMT_CUDA) {
			transferred.reset(av_frame_alloc());
			if (!transferred || !TransferHwFrameToCpu(srcCpu, transferred.get())) {
				outError = L"FFmpeg: failed to transfer HW frame for GIF";
				return false;
			}
			use = transferred.get();
		}

		const AVPixelFormat srcFmt = static_cast<AVPixelFormat>(use->format);
		if (!sws) {
			SwsContext *raw = sws_getContext(use->width, use->height, srcFmt, dstW, dstH, AV_PIX_FMT_RGB24,
							 SWS_BILINEAR, nullptr, nullptr, nullptr);
			if (!raw) {
				outError = L"FFmpeg: sws_getContext(gif) failed";
				return false;
			}
			sws.reset(raw);
		}
		sws_scale(sws.get(), use->data, use->linesize, 0, use->height, rgb->data, rgb->linesize);
		rgb_to_pal8(rgb->data[0], dstW, dstH, rgb->linesize[0], pal->data[0], pal->linesize[0], palette, colors,
			    options.gifDither);
		memcpy(pal->data[1], palette, sizeof(palette));
		pal->pts = framePts++;
		ret = avcodec_send_frame(venc.get(), pal.get());
		if (ret < 0) {
			set_err(outError, L"FFmpeg: avcodec_send_frame(gif) failed: ", ret);
			return false;
		}
		while (true) {
			std::unique_ptr<AVPacket, PacketDeleter> op(av_packet_alloc());
			ret = avcodec_receive_packet(venc.get(), op.get());
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
				break;
			if (ret < 0) {
				set_err(outError, L"FFmpeg: avcodec_receive_packet(gif) failed: ", ret);
				return false;
			}
			op->stream_index = out_st->index;
			av_packet_rescale_ts(op.get(), venc->time_base, out_st->time_base);
			ret = av_interleaved_write_frame(ofmt.get(), op.get());
			if (ret < 0) {
				set_err(outError, L"FFmpeg: write GIF frame failed: ", ret);
				return false;
			}
		}
		nextFrameT += frameDur;
		if (totalDuration > 0.0) {
			double p = (t - startSeconds) / totalDuration;
			progress.store(std::clamp(p, 0.0, 1.0));
		}
		return true;
	};

	bool eof = false;
	while (!eof) {
		if (cancelRequested.load()) {
			outError = L"Cancelled";
			return false;
		}
		ret = av_read_frame(ifmt.get(), pkt.get());
		if (ret == AVERROR_EOF) {
			eof = true;
			avcodec_send_packet(vdec.get(), nullptr);
		} else if (ret < 0) {
			set_err(outError, L"FFmpeg: av_read_frame failed: ", ret);
			return false;
		} else {
			if (pkt->stream_index != video_in) {
				av_packet_unref(pkt.get());
				continue;
			}
			ret = avcodec_send_packet(vdec.get(), pkt.get());
			av_packet_unref(pkt.get());
			if (ret < 0 && ret != AVERROR(EAGAIN)) {
				set_err(outError, L"FFmpeg: avcodec_send_packet failed: ", ret);
				return false;
			}
		}

		while (true) {
			ret = avcodec_receive_frame(vdec.get(), frame.get());
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
				break;
			if (ret < 0) {
				set_err(outError, L"FFmpeg: avcodec_receive_frame failed: ", ret);
				return false;
			}
			const double t = FramePtsSeconds(frame.get(), in_st);
			if (hasEnd && t > endSeconds) {
				eof = true;
				break;
			}
			if (!write_gif_frame(frame.get()))
				return false;
			av_frame_unref(frame.get());
		}
	}

	avcodec_send_frame(venc.get(), nullptr);
	while (true) {
		std::unique_ptr<AVPacket, PacketDeleter> op(av_packet_alloc());
		ret = avcodec_receive_packet(venc.get(), op.get());
		if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
			break;
		if (ret < 0) {
			set_err(outError, L"FFmpeg: flush GIF failed: ", ret);
			return false;
		}
		op->stream_index = out_st->index;
		av_packet_rescale_ts(op.get(), venc->time_base, out_st->time_base);
		ret = av_interleaved_write_frame(ofmt.get(), op.get());
		if (ret < 0) {
			set_err(outError, L"FFmpeg: write GIF trailer frame failed: ", ret);
			return false;
		}
	}
	ret = av_write_trailer(ofmt.get());
	if (ret < 0) {
		set_err(outError, L"FFmpeg: av_write_trailer(gif) failed: ", ret);
		return false;
	}
	progress.store(1.0);
	return true;
}

static std::wstring make_temp_path(const std::wstring &prefix, const std::wstring &ext)
{
	wchar_t tempDir[MAX_PATH];
	DWORD n = GetTempPathW(MAX_PATH, tempDir);
	if (n == 0 || n > MAX_PATH)
		return L"";
	wchar_t tempFile[MAX_PATH];
	if (GetTempFileNameW(tempDir, prefix.c_str(), 0, tempFile) == 0)
		return L"";
	std::wstring path = tempFile;
	DeleteFileW(path.c_str());
	if (!ext.empty()) {
		if (path.size() >= 4 && path.substr(path.size() - 4) == L".tmp")
			path = path.substr(0, path.size() - 4);
		path += ext;
	}
	return path;
}

static bool remux_concat_files(const std::vector<std::wstring> &inputs, const std::wstring &outputPath,
			       std::atomic<bool> &cancelRequested, std::wstring &outError)
{
	outError.clear();
	if (inputs.empty()) {
		outError = L"Merge concat: no inputs";
		return false;
	}
	if (inputs.size() == 1) {
		if (!CopyFileW(inputs[0].c_str(), outputPath.c_str(), FALSE)) {
			outError = L"Merge concat: failed to copy single segment";
			return false;
		}
		return true;
	}

	InterruptCtx ictx;
	ictx.cancel = &cancelRequested;

	AVFormatContext *ref_in = nullptr;
	int ret = avformat_open_input(&ref_in, WideToUtf8(inputs[0]).c_str(), nullptr, nullptr);
	if (ret < 0) {
		set_err(outError, L"FFmpeg: merge open first segment failed: ", ret);
		return false;
	}
	struct RefCloser {
		AVFormatContext *c = nullptr;
		~RefCloser()
		{
			if (c)
				avformat_close_input(&c);
		}
	} refCloser{ref_in};

	ret = avformat_find_stream_info(ref_in, nullptr);
	if (ret < 0) {
		set_err(outError, L"FFmpeg: merge first segment info failed: ", ret);
		return false;
	}

	AVFormatContext *ofmt = nullptr;
	ret = avformat_alloc_output_context2(&ofmt, nullptr, "mp4", WideToUtf8(outputPath).c_str());
	if (ret < 0 || !ofmt) {
		outError = L"FFmpeg: failed to create merge concat output";
		return false;
	}
	struct OutCloser {
		AVFormatContext *c = nullptr;
		bool opened = false;
		bool header = false;
		~OutCloser()
		{
			if (!c)
				return;
			if (header)
				av_write_trailer(c);
			if (opened && !(c->oformat->flags & AVFMT_NOFILE) && c->pb)
				avio_closep(&c->pb);
			avformat_free_context(c);
		}
	} outCloser{ofmt, false, false};
	ofmt->interrupt_callback = AVIOInterruptCB{&av_interrupt_cb, &ictx};

	struct OutSlot {
		AVMediaType type = AVMEDIA_TYPE_UNKNOWN;
		int ordinal = 0; 
	};
	std::vector<OutSlot> outSlots;
	int videoOrd = 0;
	int audioOrd = 0;
	for (unsigned i = 0; i < ref_in->nb_streams; i++) {
		AVStream *in_st = ref_in->streams[i];
		if (!in_st || !in_st->codecpar)
			continue;
		const AVMediaType t = in_st->codecpar->codec_type;
		if (t != AVMEDIA_TYPE_VIDEO && t != AVMEDIA_TYPE_AUDIO)
			continue;
		AVStream *out_st = avformat_new_stream(ofmt, nullptr);
		if (!out_st) {
			outError = L"FFmpeg: avformat_new_stream(merge concat) failed";
			return false;
		}
		ret = avcodec_parameters_copy(out_st->codecpar, in_st->codecpar);
		if (ret < 0) {
			outError = L"FFmpeg: avcodec_parameters_copy(merge concat) failed";
			return false;
		}
		out_st->codecpar->codec_tag = 0;
		out_st->time_base = in_st->time_base;
		OutSlot slot;
		slot.type = t;
		slot.ordinal = (t == AVMEDIA_TYPE_VIDEO) ? videoOrd++ : audioOrd++;
		outSlots.push_back(slot);
	}
	if (outSlots.empty()) {
		outError = L"FFmpeg: merge concat: first segment has no A/V streams";
		return false;
	}

	if (!(ofmt->oformat->flags & AVFMT_NOFILE)) {
		ret = avio_open(&ofmt->pb, WideToUtf8(outputPath).c_str(), AVIO_FLAG_WRITE);
		if (ret < 0) {
			set_err(outError, L"FFmpeg: open merge concat output failed: ", ret);
			return false;
		}
		outCloser.opened = true;
	}

	ret = avformat_write_header(ofmt, nullptr);
	if (ret < 0) {
		set_err(outError, L"FFmpeg: merge concat write_header failed: ", ret);
		return false;
	}
	outCloser.header = true;

	const size_t nOut = outSlots.size();
	std::vector<int64_t> nextDts(nOut, 0);
	std::vector<int64_t> lastDts(nOut, AV_NOPTS_VALUE);

	AVPacket *pkt = av_packet_alloc();
	if (!pkt) {
		outError = L"FFmpeg: av_packet_alloc failed (merge concat)";
		return false;
	}
	struct PktFree {
		AVPacket **p;
		~PktFree()
		{
			if (p && *p)
				av_packet_free(p);
		}
	} pktFree{&pkt};

	for (size_t fileIdx = 0; fileIdx < inputs.size(); fileIdx++) {
		if (cancelRequested.load()) {
			outError = L"Cancelled";
			return false;
		}

		AVFormatContext *ifmt = nullptr;
		ret = avformat_open_input(&ifmt, WideToUtf8(inputs[fileIdx]).c_str(), nullptr, nullptr);
		if (ret < 0) {
			set_err(outError, L"FFmpeg: merge open segment failed: ", ret);
			return false;
		}
		struct InCloser {
			AVFormatContext *c = nullptr;
			~InCloser()
			{
				if (c)
					avformat_close_input(&c);
			}
		} inCloser{ifmt};
		ifmt->interrupt_callback = AVIOInterruptCB{&av_interrupt_cb, &ictx};

		ret = avformat_find_stream_info(ifmt, nullptr);
		if (ret < 0) {
			set_err(outError, L"FFmpeg: merge segment info failed: ", ret);
			return false;
		}

		std::vector<int> inToOut(static_cast<size_t>(ifmt->nb_streams), -1);
		int inVideoOrd = 0;
		int inAudioOrd = 0;
		for (unsigned i = 0; i < ifmt->nb_streams; i++) {
			AVStream *in_st = ifmt->streams[i];
			if (!in_st || !in_st->codecpar)
				continue;
			const AVMediaType t = in_st->codecpar->codec_type;
			if (t != AVMEDIA_TYPE_VIDEO && t != AVMEDIA_TYPE_AUDIO)
				continue;
			const int ord = (t == AVMEDIA_TYPE_VIDEO) ? inVideoOrd++ : inAudioOrd++;
			for (size_t o = 0; o < outSlots.size(); o++) {
				if (outSlots[o].type == t && outSlots[o].ordinal == ord) {
					AVStream *out_st = ofmt->streams[o];
					if (out_st->codecpar->codec_id != in_st->codecpar->codec_id) {
						outError = L"Merge concat: codec mismatch across segments (stream copy)";
						return false;
					}
					inToOut[i] = static_cast<int>(o);
					break;
				}
			}
		}

		std::vector<bool> sawStream(nOut, false);
		std::vector<int64_t> fileOffset(nOut, 0);

		while (true) {
			if (cancelRequested.load()) {
				outError = L"Cancelled";
				return false;
			}
			ret = av_read_frame(ifmt, pkt);
			if (ret == AVERROR_EOF)
				break;
			if (ret < 0) {
				set_err(outError, L"FFmpeg: merge concat read failed: ", ret);
				return false;
			}

			const int inIdx = pkt->stream_index;
			if (inIdx < 0 || inIdx >= static_cast<int>(inToOut.size()) ||
			    inToOut[static_cast<size_t>(inIdx)] < 0) {
				av_packet_unref(pkt);
				continue;
			}
			const int outIdx = inToOut[static_cast<size_t>(inIdx)];
			AVStream *in_st = ifmt->streams[inIdx];
			AVStream *out_st = ofmt->streams[outIdx];

			av_packet_rescale_ts(pkt, in_st->time_base, out_st->time_base);
			pkt->stream_index = outIdx;
			pkt->pos = -1;

			int64_t base = AV_NOPTS_VALUE;
			if (pkt->dts != AV_NOPTS_VALUE)
				base = pkt->dts;
			else if (pkt->pts != AV_NOPTS_VALUE)
				base = pkt->pts;
			else
				base = 0;

			if (!sawStream[static_cast<size_t>(outIdx)]) {
				fileOffset[static_cast<size_t>(outIdx)] =
					nextDts[static_cast<size_t>(outIdx)] - base;
				sawStream[static_cast<size_t>(outIdx)] = true;
			}
			const int64_t off = fileOffset[static_cast<size_t>(outIdx)];
			if (pkt->dts != AV_NOPTS_VALUE)
				pkt->dts += off;
			if (pkt->pts != AV_NOPTS_VALUE)
				pkt->pts += off;

			if (pkt->dts == AV_NOPTS_VALUE) {
				pkt->dts = (lastDts[static_cast<size_t>(outIdx)] != AV_NOPTS_VALUE)
						   ? (lastDts[static_cast<size_t>(outIdx)] + 1)
						   : nextDts[static_cast<size_t>(outIdx)];
			}
			if (lastDts[static_cast<size_t>(outIdx)] != AV_NOPTS_VALUE &&
			    pkt->dts <= lastDts[static_cast<size_t>(outIdx)]) {
				pkt->dts = lastDts[static_cast<size_t>(outIdx)] + 1;
			}
			if (pkt->pts == AV_NOPTS_VALUE)
				pkt->pts = pkt->dts;
			if (pkt->pts < pkt->dts)
				pkt->pts = pkt->dts;

			const int64_t writtenDts = pkt->dts;
			int64_t endTs = writtenDts;
			if (pkt->duration > 0)
				endTs = writtenDts + pkt->duration;
			if (pkt->pts != AV_NOPTS_VALUE) {
				const int64_t ptsEnd = pkt->pts + (pkt->duration > 0 ? pkt->duration : 0);
				if (ptsEnd > endTs)
					endTs = ptsEnd;
			}

			ret = av_interleaved_write_frame(ofmt, pkt);
			av_packet_unref(pkt);
			if (ret < 0) {
				set_err(outError, L"FFmpeg: merge write failed: ", ret);
				return false;
			}

			lastDts[static_cast<size_t>(outIdx)] = writtenDts;
			if (endTs + 1 > nextDts[static_cast<size_t>(outIdx)])
				nextDts[static_cast<size_t>(outIdx)] = endTs + 1;
		}

		for (size_t o = 0; o < nOut; o++) {
			if (!sawStream[o] && lastDts[o] != AV_NOPTS_VALUE)
				nextDts[o] = lastDts[o] + 1;
			else if (!sawStream[o] && nextDts[o] < 1)
				nextDts[o] = 0;
		}
	}

	outCloser.header = false;
	ret = av_write_trailer(ofmt);
	if (ret < 0) {
		set_err(outError, L"FFmpeg: merge concat trailer failed: ", ret);
		return false;
	}
	return true;
}

static bool export_one(const std::wstring &inputPath, const std::wstring &outputPath, double startSeconds,
		       double endSeconds, const OverlayExportOptions &options, std::atomic<bool> &cancelRequested,
		       std::atomic<double> &progress, std::wstring &outError, bool allowOsCopy = true)
{
	outError.clear();
	if (outputPath.empty()) {
		outError = L"Output path is empty";
		return false;
	}
	if (GetFileAttributesW(inputPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
		outError = L"Input file not found: " + inputPath;
		return false;
	}

	std::vector<int> streams = resolve_stream_keep(inputPath, options);
	if (options.tracksConfigured && streams.empty() && options.streamIndicesToKeep.empty()) {
		if (!options.keepVideoTrack && options.keepAudioOrdinals.empty()) {
			outError = L"No tracks selected";
			return false;
		}
	}

	if (options.mode == RebuildMode::Copy) {
		return remux_trim(inputPath, outputPath, startSeconds, endSeconds, streams, cancelRequested, progress,
				  outError, allowOsCopy);
	}
	if (options.IsGif()) {
		return gif_trim_export(inputPath, outputPath, startSeconds, endSeconds, options, cancelRequested,
				       progress, outError);
	}
	int crf = options.crf;
	if (crf < 18)
		crf = 18;
	if (crf > 51)
		crf = 51;
	return transcode_trim_mp4(inputPath, outputPath, startSeconds, endSeconds, crf, options.useCpuEncoder,
				  options.width, options.height, options.fps, options.ContainerName(), streams,
				  cancelRequested, progress, outError);
}

static double merge_segment_duration(const OverlayExportSegment &seg)
{
	if (seg.endSeconds > seg.startSeconds && seg.endSeconds > 0.0)
		return seg.endSeconds - seg.startSeconds;
	return 1.0;
}

// Maps a nested 0..1 progress into [base, base+scale] on the outer atomic.
struct ProgressMapBridge {
	std::atomic<double> &dst;
	std::atomic<double> src{0.0};
	std::atomic<bool> stop{false};
	std::thread worker;

	ProgressMapBridge(std::atomic<double> &d, double base, double scale) : dst(d)
	{
		worker = std::thread([this, base, scale]() {
			while (!stop.load(std::memory_order_relaxed)) {
				const double p = std::clamp(src.load(std::memory_order_relaxed), 0.0, 1.0);
				dst.store(base + scale * p, std::memory_order_relaxed);
				Sleep(40);
			}
			const double p = std::clamp(src.load(std::memory_order_relaxed), 0.0, 1.0);
			dst.store(base + scale * p, std::memory_order_relaxed);
		});
	}

	~ProgressMapBridge()
	{
		stop.store(true, std::memory_order_relaxed);
		if (worker.joinable())
			worker.join();
	}

	ProgressMapBridge(const ProgressMapBridge &) = delete;
	ProgressMapBridge &operator=(const ProgressMapBridge &) = delete;
};

static bool merge_export_run(const std::vector<OverlayExportSegment> &segments, const std::wstring &outputPath,
			     const OverlayExportOptions &options, std::atomic<bool> &cancelRequested,
			     std::atomic<double> &progress, std::wstring &outError)
{
	outError.clear();
	if (segments.empty()) {
		outError = L"Merge export: no segments";
		return false;
	}
	if (outputPath.empty()) {
		outError = L"Output path is empty";
		return false;
	}

	// Segment prep ~20%, concat → 25%, final rebuild ~75%.
	constexpr double kSegShare = 0.20;
	constexpr double kConcatShare = 0.05;
	constexpr double kFinalShare = 0.75;

	double totalSegDur = 0.0;
	for (const auto &seg : segments)
		totalSegDur += merge_segment_duration(seg);
	if (totalSegDur <= 0.0)
		totalSegDur = static_cast<double>(segments.size());

	std::vector<std::wstring> temps;
	temps.reserve(segments.size());
	double segBase = 0.0;

	for (size_t i = 0; i < segments.size(); i++) {
		if (cancelRequested.load()) {
			outError = L"Cancelled";
			for (const auto &t : temps)
				DeleteFileW(t.c_str());
			return false;
		}
		const auto &seg = segments[i];
		if (GetFileAttributesW(seg.inputPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
			outError = L"Input file not found: " + seg.inputPath;
			for (const auto &t : temps)
				DeleteFileW(t.c_str());
			return false;
		}
		std::wstring temp = make_temp_path(L"ovm", L".mp4");
		if (temp.empty()) {
			outError = L"Failed to create temp file for merge";
			for (const auto &t : temps)
				DeleteFileW(t.c_str());
			return false;
		}
		std::wstring err;

		OverlayExportOptions copyOpts = options;
		copyOpts.mode = RebuildMode::Copy;
		copyOpts.streamIndicesToKeep = resolve_stream_keep(seg.inputPath, options);
		copyOpts.tracksConfigured = false;
		if (options.tracksConfigured && copyOpts.streamIndicesToKeep.empty() &&
		    !options.keepVideoTrack && options.keepAudioOrdinals.empty()) {
			outError = L"Merge segment " + std::to_wstring(i + 1) + L": no tracks selected";
			DeleteFileW(temp.c_str());
			for (const auto &t : temps)
				DeleteFileW(t.c_str());
			return false;
		}

		const double segScale = kSegShare * (merge_segment_duration(seg) / totalSegDur);
		{
			ProgressMapBridge bridge(progress, segBase, segScale);
			if (!export_one(seg.inputPath, temp, seg.startSeconds, seg.endSeconds, copyOpts,
					cancelRequested, bridge.src, err, /*allowOsCopy=*/false)) {
				outError = L"Merge segment " + std::to_wstring(i + 1) + L" failed: " + err;
				DeleteFileW(temp.c_str());
				for (const auto &t : temps)
					DeleteFileW(t.c_str());
				return false;
			}
		}
		temps.push_back(temp);
		segBase += segScale;
		progress.store(segBase);
	}

	std::wstring concatTemp = make_temp_path(L"ovc", L".mp4");
	if (concatTemp.empty()) {
		outError = L"Failed to create merge concat temp";
		for (const auto &t : temps)
			DeleteFileW(t.c_str());
		return false;
	}

	std::wstring err;
	if (!remux_concat_files(temps, concatTemp, cancelRequested, err)) {
		outError = err.empty() ? L"Merge concat failed" : err;
		DeleteFileW(concatTemp.c_str());
		for (const auto &t : temps)
			DeleteFileW(t.c_str());
		return false;
	}

	const double finalBase = kSegShare + kConcatShare;
	progress.store(finalBase);

	OverlayExportOptions finalOpts = options;
	finalOpts.streamIndicesToKeep.clear();
	finalOpts.tracksConfigured = false;

	bool ok = false;
	{
		ProgressMapBridge bridge(progress, finalBase, kFinalShare);
		ok = export_one(concatTemp, outputPath, 0.0, 0.0, finalOpts, cancelRequested, bridge.src, err,
				/*allowOsCopy=*/true);
	}
	if (!ok)
		outError = err.empty() ? L"Merge final rebuild failed" : (L"Merge final rebuild failed: " + err);

	DeleteFileW(concatTemp.c_str());
	for (const auto &t : temps)
		DeleteFileW(t.c_str());
	if (ok)
		progress.store(1.0);
	return ok;
}

bool overlay_export_run(const std::wstring &inputPath, const std::wstring &outputPath, double startSeconds,
			double endSeconds, const OverlayExportOptions &options, std::atomic<bool> &cancelRequested,
			std::atomic<double> &progress, std::wstring &outError)
{
	return export_one(inputPath, outputPath, startSeconds, endSeconds, options, cancelRequested, progress,
			  outError);
}

bool overlay_export_merge_run(const std::vector<OverlayExportSegment> &segments, const std::wstring &outputPath,
			      const OverlayExportOptions &options, std::atomic<bool> &cancelRequested,
			      std::atomic<double> &progress, std::wstring &outError)
{
	return merge_export_run(segments, outputPath, options, cancelRequested, progress, outError);
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
		obs_log(LOG_WARNING,
			"[Trim] keep-last no-op path=%s keep=%.3f span=%.3f (returning success without writing output)",
			overlay::util::WideToUtf8(inputPath).c_str(), keepSeconds, file_duration_sec);
		return true;
	}

	const double startSeconds = std::max(playback_start_sec, playback_end_sec - keepSeconds);
	obs_log(LOG_INFO,
		"[Trim] keep-last path=%s playback=[%.3f..%.3f] span=%.3f keep=%.3f start=%.3f",
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
	if (m_status.load() != Status::Trimming && m_worker.joinable()) {
		m_worker.join();
	}
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

bool OverlayTrimmer::BeginWorker()
{
	if (m_status.load() == Status::Trimming) {
		return false;
	}
	if (m_worker.joinable()) {
		m_worker.join();
	}
	Reset();
	m_status.store(Status::Trimming);
	return true;
}

bool OverlayTrimmer::StartTrim(const std::wstring &inputPath, const std::wstring &outputPath, double startSeconds,
			       double endSeconds)
{
	if (!BeginWorker())
		return false;
	m_worker = std::thread(&OverlayTrimmer::RunTrim, this, inputPath, outputPath, startSeconds, endSeconds);
	return true;
}

bool OverlayTrimmer::StartExport(const std::wstring &inputPath, const std::wstring &outputPath, double startSeconds,
				 double endSeconds, bool compress, int crf, const std::vector<int> &streamIndicesToKeep,
				 const std::wstring &customFfmpegArgs, bool useCpuEncoder)
{
	if (!BeginWorker())
		return false;
	if (crf < 18)
		crf = 18;
	if (crf > 51)
		crf = 51;
	m_worker = std::thread(&OverlayTrimmer::RunExport, this, inputPath, outputPath, startSeconds, endSeconds,
			       compress, crf, streamIndicesToKeep, customFfmpegArgs, useCpuEncoder);
	return true;
}

bool OverlayTrimmer::StartRebuildExport(const std::wstring &inputPath, const std::wstring &outputPath,
					double startSeconds, double endSeconds, const OverlayExportOptions &options)
{
	if (!BeginWorker())
		return false;
	m_worker = std::thread(&OverlayTrimmer::RunRebuildExport, this, inputPath, outputPath, startSeconds, endSeconds,
			       options);
	return true;
}

bool OverlayTrimmer::StartMergeExport(const std::vector<OverlayExportSegment> &segments, const std::wstring &outputPath,
				      const OverlayExportOptions &options)
{
	if (!BeginWorker())
		return false;
	m_worker = std::thread(&OverlayTrimmer::RunMergeExport, this, segments, outputPath, options);
	return true;
}

bool OverlayTrimmer::StartTranscode(const std::wstring &inputPath, const std::wstring &outputPath, int crf)
{
	if (!BeginWorker())
		return false;
	if (crf < 18)
		crf = 18;
	if (crf > 51)
		crf = 51;
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
	OverlayExportOptions opts;
	opts.mode = compress ? RebuildMode::Reencode : RebuildMode::Copy;
	opts.crf = crf;
	opts.streamIndicesToKeep = streamIndicesToKeep;
	opts.useCpuEncoder = useCpuEncoder;
	if (!customFfmpegArgs.empty()) {
		m_lastError = L"Custom ffmpeg args are not supported (in-process mode)";
		m_status.store(Status::Error);
		obs_log(LOG_ERROR, "Gallery export failed: custom ffmpeg args are not supported");
		return;
	}
	RunRebuildExport(inputPath, outputPath, startSeconds, endSeconds, opts);
}

void OverlayTrimmer::RunRebuildExport(const std::wstring &inputPath, const std::wstring &outputPath,
				      double startSeconds, double endSeconds, OverlayExportOptions options)
{
	if (outputPath.empty()) {
		m_lastError = L"Output path is empty";
		m_status.store(Status::Error);
		obs_log(LOG_ERROR, "Gallery export failed: output path is empty");
		return;
	}

	m_cancelRequested.store(false);
	m_progress.store(0.0);
	m_exportTotalDuration = (endSeconds > startSeconds) ? (endSeconds - startSeconds) : 0.0;

	std::wstring err;
	bool ok = overlay_export_run(inputPath, outputPath, startSeconds, endSeconds, options, m_cancelRequested,
				     m_progress, err);
	if (ok) {
		m_lastOutput = outputPath;
		m_status.store(Status::Success);
	} else {
		m_lastError = err.empty() ? L"Export failed" : err;
		m_status.store(Status::Error);
		if (err == L"Cancelled") {
			obs_log(LOG_INFO, "Gallery export cancelled (input: %s)", WideToUtf8(inputPath).c_str());
		} else {
			obs_log(LOG_ERROR, "Gallery export failed: %s (input: %s, output: %s, range: %.3f-%.3f s, mode: %d)",
				WideToUtf8(m_lastError).c_str(), WideToUtf8(inputPath).c_str(),
				WideToUtf8(outputPath).c_str(), startSeconds, endSeconds,
				RebuildModeToInt(options.mode));
		}
	}
}

void OverlayTrimmer::RunMergeExport(std::vector<OverlayExportSegment> segments, std::wstring outputPath,
				    OverlayExportOptions options)
{
	if (outputPath.empty()) {
		m_lastError = L"Output path is empty";
		m_status.store(Status::Error);
		return;
	}
	m_cancelRequested.store(false);
	m_progress.store(0.0);
	m_exportTotalDuration = 0.0;
	for (const auto &s : segments) {
		if (s.endSeconds > s.startSeconds)
			m_exportTotalDuration += (s.endSeconds - s.startSeconds);
	}

	std::wstring err;
	bool ok = overlay_export_merge_run(segments, outputPath, options, m_cancelRequested, m_progress, err);
	if (ok) {
		m_lastOutput = outputPath;
		m_status.store(Status::Success);
	} else {
		m_lastError = err.empty() ? L"Merge export failed" : err;
		m_status.store(Status::Error);
		obs_log(err == L"Cancelled" ? LOG_INFO : LOG_ERROR, "Gallery merge export: %s",
			WideToUtf8(m_lastError).c_str());
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
