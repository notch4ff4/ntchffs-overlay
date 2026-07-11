#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

namespace overlay::ffmpeg {

struct VideoHwDecodeState {
	AVBufferRef *device_ctx = nullptr;
	AVPixelFormat pix_fmt = AV_PIX_FMT_NONE;
	bool active = false;

	// Outlives the codec context that uses get_format.
	AVPixelFormat get_format_target = AV_PIX_FMT_NONE;
};

inline void ReleaseVideoHwDecodeState(VideoHwDecodeState *hw)
{
	if (!hw) {
		return;
	}
	av_buffer_unref(&hw->device_ctx);
	hw->pix_fmt = AV_PIX_FMT_NONE;
	hw->active = false;
	hw->get_format_target = AV_PIX_FMT_NONE;
}

namespace detail {

inline AVPixelFormat HwGetFormat(AVCodecContext *ctx, const AVPixelFormat *pix_fmts)
{
	auto *target = static_cast<AVPixelFormat *>(ctx->opaque);
	if (!target) {
		return AV_PIX_FMT_NONE;
	}
	for (const AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
		if (*p == *target) {
			return *p;
		}
	}
	return AV_PIX_FMT_NONE;
}

} // namespace detail

inline bool TransferHwFrameToCpu(const AVFrame *hw_frame, AVFrame *cpu_frame)
{
	if (!hw_frame || !cpu_frame) {
		return false;
	}
	if (hw_frame->format != AV_PIX_FMT_CUDA) {
		return av_frame_ref(cpu_frame, hw_frame) >= 0;
	}
	av_frame_unref(cpu_frame);
	return av_hwframe_transfer_data(cpu_frame, hw_frame, 0) >= 0;
}

// Opens a video decoder for stream. For AV1, prefers av1_cuvid when allow_av1_sw_fallback is false.
inline bool OpenVideoDecoder(AVCodecContext **out_ctx, AVStream *stream, VideoHwDecodeState *hw,
			     bool allow_av1_sw_fallback = true)
{
	if (!out_ctx || !stream || !stream->codecpar) {
		return false;
	}
	*out_ctx = nullptr;
	if (hw) {
		ReleaseVideoHwDecodeState(hw);
	}

	const bool is_av1 = stream->codecpar->codec_id == AV_CODEC_ID_AV1;
	if (is_av1) {
		const AVCodec *codec = avcodec_find_decoder_by_name("av1_cuvid");
		if (codec && hw && av_hwdevice_ctx_create(&hw->device_ctx, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0) >= 0) {
			AVCodecContext *ctx = avcodec_alloc_context3(codec);
			if (!ctx) {
				return false;
			}
			avcodec_parameters_to_context(ctx, stream->codecpar);
			hw->get_format_target = AV_PIX_FMT_CUDA;
			hw->pix_fmt = AV_PIX_FMT_CUDA;
			ctx->opaque = &hw->get_format_target;
			ctx->get_format = detail::HwGetFormat;
			ctx->hw_device_ctx = av_buffer_ref(hw->device_ctx);
			ctx->thread_count = 1;
			if (avcodec_open2(ctx, codec, nullptr) >= 0) {
				hw->active = true;
				*out_ctx = ctx;
				return true;
			}
			avcodec_free_context(&ctx);
			ReleaseVideoHwDecodeState(hw);
		}
		if (!allow_av1_sw_fallback) {
			return false;
		}
	}

	const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
	if (!codec) {
		return false;
	}
	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	if (!ctx) {
		return false;
	}
	avcodec_parameters_to_context(ctx, stream->codecpar);
	ctx->thread_count = 0;
	ctx->thread_type = FF_THREAD_FRAME;
	if (avcodec_open2(ctx, codec, nullptr) < 0) {
		avcodec_free_context(&ctx);
		return false;
	}
	*out_ctx = ctx;
	return true;
}

} // namespace overlay::ffmpeg
