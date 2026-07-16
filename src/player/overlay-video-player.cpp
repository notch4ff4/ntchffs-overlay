#ifdef _WIN32

#include "overlay-video-player.h"
#include "overlay-ffmpeg-time-utils.h"
#include "overlay-mp4-box-reader.h"
#include "overlay-string-utils.h"
#include <obs-module.h>
#include <plugin-support.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <chrono>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avstring.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <comdef.h>
#include <timeapi.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")

namespace {

using overlay::util::WideToUtf8;
using overlay::ffmpeg::FramePtsSeconds;
using overlay::ffmpeg::PtsToSeconds;

} // namespace

struct AudioStreamInfo {
	int stream_index = -1;
	// From metadata title, or a generated "Audio N" label.
	std::string name;
	std::string codec_name;
	std::string lang;
	int sample_rate = 0;
	int channels = 0;
};

struct LibAvState {
	AVFormatContext *fmt_ctx = nullptr;
	AVCodecContext *video_codec_ctx = nullptr;
	AVCodecContext *audio_codec_ctx = nullptr;
	AVBufferRef *hw_device_ctx = nullptr;
	AVPixelFormat hw_pix_fmt = AV_PIX_FMT_NONE;
	bool video_hw_decode = false;
	int video_stream_idx = -1;
	int audio_stream_idx = -1;
	std::string video_stream_name;
	std::vector<AudioStreamInfo> audio_streams;
	int current_audio_index = 0;
	AVRational video_time_base = {0, 1};
	AVRational audio_time_base = {0, 1};
	int audio_sample_rate = 44100;
	int audio_channels = 2;

	void Free()
	{
		avcodec_free_context(&video_codec_ctx);
		avcodec_free_context(&audio_codec_ctx);
		av_buffer_unref(&hw_device_ctx);
		video_hw_decode = false;
		hw_pix_fmt = AV_PIX_FMT_NONE;
		avformat_close_input(&fmt_ctx);
		audio_streams.clear();
	}
};

namespace {

static AVPixelFormat GetHwFormat(AVCodecContext *ctx, const AVPixelFormat *pix_fmts)
{
	auto *av = static_cast<LibAvState *>(ctx->opaque);
	for (const AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
		if (*p == av->hw_pix_fmt) {
			return *p;
		}
	}
	return AV_PIX_FMT_NONE;
}

static bool TransferHwFrameToCpu(const AVFrame *hw_frame, AVFrame *cpu_frame)
{
	if (!hw_frame || !cpu_frame) {
		return false;
	}
	if (hw_frame->format != AV_PIX_FMT_CUDA) {
		return av_frame_ref(cpu_frame, hw_frame) >= 0;
	}
	av_frame_unref(cpu_frame);
	if (av_hwframe_transfer_data(cpu_frame, hw_frame, 0) < 0) {
		return false;
	}
	return true;
}

static bool OpenSwVideoDecoder(LibAvState *av, AVStream *stream, const std::string &pathUtf8)
{
	av->video_hw_decode = false;
	const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
	if (!codec) {
		obs_log(LOG_WARNING, "[OverlayVideoPlayer] No decoder for video codec_id=%d (path: %s)",
			stream->codecpar->codec_id, pathUtf8.c_str());
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
		obs_log(LOG_WARNING, "[OverlayVideoPlayer] avcodec_open2 failed for video decoder %s",
			codec->name);
		avcodec_free_context(&ctx);
		return false;
	}

	av->video_codec_ctx = ctx;
	av->video_time_base = stream->time_base;
	return true;
}

static bool OpenHwAv1Decoder(LibAvState *av, AVStream *stream)
{
	const AVCodec *codec = avcodec_find_decoder_by_name("av1_cuvid");
	if (!codec) {
		return false;
	}

	av_buffer_unref(&av->hw_device_ctx);
	if (av_hwdevice_ctx_create(&av->hw_device_ctx, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0) < 0) {
		return false;
	}

	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	if (!ctx) {
		return false;
	}
	avcodec_parameters_to_context(ctx, stream->codecpar);
	ctx->opaque = av;
	ctx->hw_device_ctx = av_buffer_ref(av->hw_device_ctx);
	av->hw_pix_fmt = AV_PIX_FMT_CUDA;
	ctx->get_format = GetHwFormat;
	ctx->thread_count = 1;

	if (avcodec_open2(ctx, codec, nullptr) < 0) {
		avcodec_free_context(&ctx);
		return false;
	}

	av->video_codec_ctx = ctx;
	av->video_time_base = stream->time_base;
	av->video_hw_decode = true;
	return true;
}

static bool OpenVideoDecoder(LibAvState *av, AVStream *stream, const std::string &pathUtf8)
{
	if (stream->codecpar->codec_id == AV_CODEC_ID_AV1 && OpenHwAv1Decoder(av, stream)) {
		return true;
	}
	return OpenSwVideoDecoder(av, stream, pathUtf8);
}

} // namespace

// --- Three-thread playback: packet queue item and state ---
struct PacketQueueItem {
	bool is_flush = false;
	AVPacket pkt{};

	PacketQueueItem() { pkt = {}; }
	~PacketQueueItem() { av_packet_unref(&pkt); }
	PacketQueueItem(PacketQueueItem &&other) noexcept : is_flush(other.is_flush)
	{
		av_packet_move_ref(&pkt, &other.pkt);
	}
	PacketQueueItem &operator=(PacketQueueItem &&other) noexcept
	{
		av_packet_unref(&pkt);
		is_flush = other.is_flush;
		av_packet_move_ref(&pkt, &other.pkt);
		return *this;
	}
	PacketQueueItem(const PacketQueueItem &) = delete;
	PacketQueueItem &operator=(const PacketQueueItem &) = delete;
};

struct ThreeThreadState {
	static constexpr size_t MAX_VIDEO_PACKETS = 128;
	static constexpr size_t MAX_AUDIO_PACKETS = 256;

	std::queue<PacketQueueItem> videoQueue;
	std::queue<PacketQueueItem> audioQueue;
	std::mutex videoMutex;
	std::mutex audioMutex;
	std::condition_variable videoCond;
	std::condition_variable audioCond;

	void clearQueues()
	{
		{
			std::lock_guard<std::mutex> g(videoMutex);
			while (!videoQueue.empty())
				videoQueue.pop();
		}
		{
			std::lock_guard<std::mutex> g(audioMutex);
			while (!audioQueue.empty())
				audioQueue.pop();
		}
	}

	void pushFlushBoth()
	{
		PacketQueueItem vf, af;
		vf.is_flush = true;
		af.is_flush = true;
		{
			std::lock_guard<std::mutex> g(videoMutex);
			videoQueue.push(std::move(vf));
			videoCond.notify_one();
		}
		{
			std::lock_guard<std::mutex> g(audioMutex);
			audioQueue.push(std::move(af));
			audioCond.notify_one();
		}
	}

	void wakeAll()
	{
		videoCond.notify_all();
		audioCond.notify_all();
	}

	// Returns false on seek/stop so the caller must unref the packet.
	bool pushVideo(OverlayVideoPlayer *player, AVPacket *pkt)
	{
		std::unique_lock<std::mutex> lock(videoMutex);
		videoCond.wait(lock, [this, player] {
			return videoQueue.size() < MAX_VIDEO_PACKETS || player->m_demuxStop.load() ||
			       player->m_seekRequested.load();
		});
		if (player->m_demuxStop.load() || player->m_seekRequested.load())
			return false;
		PacketQueueItem item;
		item.is_flush = false;
		av_packet_move_ref(&item.pkt, pkt);
		videoQueue.push(std::move(item));
		videoCond.notify_one();
		return true;
	}

	bool pushAudio(OverlayVideoPlayer *player, AVPacket *pkt)
	{
		std::unique_lock<std::mutex> lock(audioMutex);
		audioCond.wait(lock, [this, player] {
			return audioQueue.size() < MAX_AUDIO_PACKETS || player->m_demuxStop.load() ||
			       player->m_seekRequested.load();
		});
		if (player->m_demuxStop.load() || player->m_seekRequested.load())
			return false;
		PacketQueueItem item;
		item.is_flush = false;
		av_packet_move_ref(&item.pkt, pkt);
		audioQueue.push(std::move(item));
		audioCond.notify_one();
		return true;
	}

	// Returns false when playback is stopping.
	bool popVideo(OverlayVideoPlayer *player, PacketQueueItem &out)
	{
		std::unique_lock<std::mutex> lock(videoMutex);
		videoCond.wait(lock, [this, player] { return !videoQueue.empty() || player->m_videoStop.load(); });
		if (player->m_videoStop.load())
			return false;
		out = std::move(videoQueue.front());
		videoQueue.pop();
		videoCond.notify_one();
		return true;
	}

	bool popAudio(OverlayVideoPlayer *player, PacketQueueItem &out)
	{
		std::unique_lock<std::mutex> lock(audioMutex);
		audioCond.wait(lock, [this, player] { return !audioQueue.empty() || player->m_audioStop.load(); });
		if (player->m_audioStop.load())
			return false;
		out = std::move(audioQueue.front());
		audioQueue.pop();
		audioCond.notify_one();
		return true;
	}
};

OverlayVideoPlayer::OverlayVideoPlayer()
	: m_initialized(false),
	  m_playing(false),
	  m_duration(0.0),
	  m_currentTime(0.0),
	  m_muted(false),
	  m_volume(1.0),
	  m_trimIn(0.0),
	  m_trimOut(0.0),
	  m_logFrameCount(0),
	  m_lastTick(0),
	  m_lastFrameTick(0),
	  m_lastStallLogTick(0),
	  m_frameWidth(0),
	  m_frameHeight(0),
	  m_sourceWidth(0),
	  m_sourceHeight(0),
	  m_targetMaxWidth(0),
	  m_targetMaxHeight(0),
	  m_videoFps(30.0),
	  m_videoStop(false),
	  m_audioStop(false),
	  m_av(new LibAvState),
	  m_audioClock(0.0),
	  m_playStartTime(0.0),
	  m_demuxStop(false),
	  m_seekTargetSec(0.0),
	  m_seekRequested(false)
{
}

OverlayVideoPlayer::~OverlayVideoPlayer()
{
	Close();
	delete m_av;
	m_av = nullptr;
}

bool OverlayVideoPlayer::Initialize()
{
	if (m_initialized) {
		return true;
	}
	m_initialized = true;
	return true;
}

void OverlayVideoPlayer::Shutdown()
{
	Close();
	m_initialized = false;
}

static bool OpenAvFormat(LibAvState *av, const std::wstring &path)
{
	std::string pathUtf8 = WideToUtf8(path);
	if (pathUtf8.empty()) {
		return false;
	}

	AVFormatContext *fmt_ctx = nullptr;
	AVDictionary *opts = nullptr;
	av_dict_set(&opts, "probesize", "5000000", 0);
	av_dict_set(&opts, "analyzeduration", "2000000", 0);

	int ret = avformat_open_input(&fmt_ctx, pathUtf8.c_str(), nullptr, &opts);
	av_dict_free(&opts);
	if (ret < 0) {
		char errbuf[256];
		av_strerror(ret, errbuf, sizeof(errbuf));
		obs_log(LOG_WARNING, "OverlayVideoPlayer: avformat_open_input failed: %s", errbuf);
		return false;
	}

	ret = avformat_find_stream_info(fmt_ctx, nullptr);
	if (ret < 0) {
		avformat_close_input(&fmt_ctx);
		return false;
	}

	av->fmt_ctx = fmt_ctx;
	av->video_stream_name.clear();

	av->video_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (av->video_stream_idx >= 0) {
		AVStream *stream = fmt_ctx->streams[av->video_stream_idx];
		AVDictionaryEntry *title_tag = av_dict_get(stream->metadata, "title", nullptr, 0);
		if (title_tag && title_tag->value && title_tag->value[0] != '\0') {
			av->video_stream_name = title_tag->value;
		} else {
			// Skip generic MP4 handler_name placeholders like "VideoHandler".
			AVDictionaryEntry *handler = av_dict_get(stream->metadata, "handler_name", nullptr, 0);
			const char *hv = handler ? handler->value : nullptr;
			if (hv && hv[0] != '\0' && av_strcasecmp(hv, "VideoHandler") != 0) {
				av->video_stream_name = hv;
			} else {
				av->video_stream_name = "Video";
			}
		}
		if (!OpenVideoDecoder(av, stream, pathUtf8)) {
			obs_log(LOG_WARNING, "[OverlayVideoPlayer] Failed to open video decoder (path: %s)",
				pathUtf8.c_str());
		}
	}

	av->audio_streams.clear();
	av->audio_stream_idx = -1;
	int audio_number = 0;
	for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
		if (fmt_ctx->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
			continue;
		}
		AVStream *stream = fmt_ctx->streams[i];
		const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
		const char *codec_name = codec ? codec->name : "?";
		std::string lang = "und";
		AVDictionaryEntry *tag = av_dict_get(stream->metadata, "language", nullptr, 0);
		if (tag && tag->value && tag->value[0] != '\0') {
			lang = tag->value;
		}
		std::string name;
		AVDictionaryEntry *title_tag = av_dict_get(stream->metadata, "title", nullptr, 0);
		if (title_tag && title_tag->value && title_tag->value[0] != '\0') {
			name = title_tag->value;
		} else {
			// Skip generic MP4 handler_name placeholders like "SoundHandler".
			AVDictionaryEntry *handler = av_dict_get(stream->metadata, "handler_name", nullptr, 0);
			const char *hv = handler ? handler->value : nullptr;
			if (hv && hv[0] != '\0' && av_strcasecmp(hv, "SoundHandler") != 0 &&
			    av_strcasecmp(hv, "AudioHandler") != 0) {
				name = hv;
			} else {
				audio_number++;
				name = "Audio " + std::to_string(audio_number);
			}
		}
		AudioStreamInfo info;
		info.stream_index = static_cast<int>(i);
		info.name = name;
		info.codec_name = codec_name;
		info.lang = lang;
		info.sample_rate = stream->codecpar->sample_rate > 0 ? stream->codecpar->sample_rate : 0;
		info.channels = stream->codecpar->ch_layout.nb_channels > 0 ? stream->codecpar->ch_layout.nb_channels
									    : 0;
		av->audio_streams.push_back(info);
	}

	// Prefer OBS trak/udta/name atoms (not exposed by FFmpeg mov demuxer).
	{
		const auto boxNames = overlay::mp4::ReadMp4AudioTrackNames(path);
		if (boxNames.size() == av->audio_streams.size()) {
			for (size_t i = 0; i < boxNames.size(); i++) {
				if (boxNames[i].has_value() && !boxNames[i]->empty()) {
					av->audio_streams[i].name = *boxNames[i];
				}
			}
		}
	}

	av->current_audio_index = 0;
	if (!av->audio_streams.empty()) {
		av->audio_stream_idx = av->audio_streams[0].stream_index;
		AVStream *stream = fmt_ctx->streams[av->audio_stream_idx];
		const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
		if (codec) {
			AVCodecContext *ctx = avcodec_alloc_context3(codec);
			if (ctx) {
				avcodec_parameters_to_context(ctx, stream->codecpar);
				ctx->thread_count = 0;
				if (avcodec_open2(ctx, codec, nullptr) >= 0) {
					av->audio_codec_ctx = ctx;
					av->audio_time_base = stream->time_base;
					av->audio_sample_rate = ctx->sample_rate > 0 ? ctx->sample_rate : 44100;
					av->audio_channels = ctx->ch_layout.nb_channels > 0 ? ctx->ch_layout.nb_channels
											    : 2;
					av->audio_streams[0].sample_rate = av->audio_sample_rate;
					av->audio_streams[0].channels = av->audio_channels;
				} else {
					avcodec_free_context(&ctx);
				}
			}
		}
	}

	// Ignore subtitle/data streams while keeping every audio track for switching.
	for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
		if (i != static_cast<unsigned>(av->video_stream_idx)) {
			bool is_audio = false;
			for (const auto &a : av->audio_streams) {
				if (static_cast<unsigned>(a.stream_index) == i) {
					is_audio = true;
					break;
				}
			}
			if (!is_audio) {
				fmt_ctx->streams[i]->discard = AVDISCARD_ALL;
			}
		}
	}

	return av->video_codec_ctx != nullptr;
}

// Used when switching audio tracks without reopening the file.
static bool OpenAudioDecoderForStream(LibAvState *av, int stream_index)
{
	if (!av->fmt_ctx || stream_index < 0 || stream_index >= static_cast<int>(av->fmt_ctx->nb_streams)) {
		return false;
	}
	AVStream *stream = av->fmt_ctx->streams[stream_index];
	if (stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
		return false;
	}
	avcodec_free_context(&av->audio_codec_ctx);
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
	if (avcodec_open2(ctx, codec, nullptr) < 0) {
		avcodec_free_context(&ctx);
		return false;
	}
	av->audio_codec_ctx = ctx;
	av->audio_time_base = stream->time_base;
	av->audio_sample_rate = ctx->sample_rate > 0 ? ctx->sample_rate : 44100;
	av->audio_channels = ctx->ch_layout.nb_channels > 0 ? ctx->ch_layout.nb_channels : 2;
	return true;
}

bool OverlayVideoPlayer::Open(const std::wstring &path)
{
	Close();
	if (!m_initialized && !Initialize()) {
		return false;
	}
	m_openPath = path;
	m_duration.store(0.0);
	m_currentTime.store(0.0);
	m_trimIn = 0.0;
	m_trimOut = 0.0;
	m_logFrameCount = 0;
	m_lastFrameTick.store(GetTickCount64());
	m_lastStallLogTick = 0;
	m_frameData.clear();

	if (!OpenAvFormat(m_av, path)) {
		return false;
	}

	if (m_av->fmt_ctx->duration != AV_NOPTS_VALUE) {
		m_duration.store(static_cast<double>(m_av->fmt_ctx->duration) / AV_TIME_BASE);
	}
	if (m_av->video_codec_ctx) {
		m_sourceWidth = m_av->video_codec_ctx->width;
		m_sourceHeight = m_av->video_codec_ctx->height;
		AVStream *stream = m_av->fmt_ctx->streams[m_av->video_stream_idx];
		if (stream->avg_frame_rate.den > 0) {
			m_videoFps = av_q2d(stream->avg_frame_rate);
		} else if (stream->r_frame_rate.den > 0) {
			m_videoFps = av_q2d(stream->r_frame_rate);
		}
	}
	if (m_sourceWidth <= 0 || m_sourceHeight <= 0) {
		m_sourceWidth = 1280;
		m_sourceHeight = 720;
	}
	if (m_videoFps < 1.0) {
		m_videoFps = 30.0;
	}

	UpdateOutputSize();

	// Show a still frame immediately while playback threads are idle.
	StartDecodeThread(0.0, true);
	return true;
}

void OverlayVideoPlayer::Close()
{
	StopPlayback();
	m_av->Free();
	m_frameData.clear();
	m_frameWidth = 0;
	m_frameHeight = 0;
	m_playing = false;
	m_duration.store(0.0);
	m_currentTime.store(0.0);
	m_trimIn = 0.0;
	m_trimOut = 0.0;
	m_openPath.clear();
	m_sourceWidth = 0;
	m_sourceHeight = 0;
	m_targetMaxWidth = 0;
	m_targetMaxHeight = 0;
}

void OverlayVideoPlayer::SetPlaying(bool playing)
{
	if (playing == m_playing) {
		return;
	}
	m_playing = playing;
	if (m_playing) {
		StartPlayback(m_currentTime.load());
	} else {
		StopPlayback();
	}
}

void OverlayVideoPlayer::TogglePlaying()
{
	SetPlaying(!m_playing);
}

void OverlayVideoPlayer::Seek(double seconds)
{
	double duration = m_duration.load();
	if (duration > 0.0 && seconds > duration) {
		seconds = duration;
	}
	if (seconds < 0.0) {
		seconds = 0.0;
	}
	m_currentTime.store(seconds);
	if (m_playing && m_tt) {
		m_seekTargetSec.store(seconds);
		m_seekRequested.store(true);
		m_tt->wakeAll();
	} else if (m_playing) {
		StartPlayback(seconds);
	} else {
		StopPlayback();
		StartDecodeThread(seconds, true);
	}
}

void OverlayVideoPlayer::SeekOneFrameForward()
{
	double t = m_currentTime.load();
	double step = (m_videoFps > 0.0) ? (1.0 / m_videoFps) : (1.0 / 30.0);
	Seek(t + step);
}

void OverlayVideoPlayer::SeekOneFrameBackward()
{
	double t = m_currentTime.load();
	double step = (m_videoFps > 0.0) ? (1.0 / m_videoFps) : (1.0 / 30.0);
	Seek((t > step) ? (t - step) : 0.0);
}

int OverlayVideoPlayer::GetAudioTrackCount() const
{
	return static_cast<int>(m_av->audio_streams.size());
}

void OverlayVideoPlayer::GetAudioTrackInfo(int index, AudioTrackInfo &out) const
{
	out = {};
	if (index < 0 || index >= static_cast<int>(m_av->audio_streams.size())) {
		return;
	}
	const AudioStreamInfo &s = m_av->audio_streams[static_cast<size_t>(index)];
	out.streamIndex = s.stream_index;
	out.name = s.name;
	out.codecName = s.codec_name;
	out.lang = s.lang;
	out.sampleRate = s.sample_rate;
	out.channels = s.channels;
}

int OverlayVideoPlayer::GetCurrentAudioTrackIndex() const
{
	return m_av->current_audio_index;
}

void OverlayVideoPlayer::SetCurrentAudioTrack(int index)
{
	if (index < 0 || index >= static_cast<int>(m_av->audio_streams.size())) {
		return;
	}
	if (index == m_av->current_audio_index) {
		return;
	}
	double t = m_currentTime.load();
	bool wasPlaying = m_playing;
	StopPlayback();
	if (!OpenAudioDecoderForStream(m_av, m_av->audio_streams[static_cast<size_t>(index)].stream_index)) {
		return;
	}
	m_av->current_audio_index = index;
	m_av->audio_stream_idx = m_av->audio_streams[static_cast<size_t>(index)].stream_index;
	if (wasPlaying) {
		StartPlayback(t);
	} else {
		m_currentTime.store(t);
		StartDecodeThread(t, true);
	}
}

int OverlayVideoPlayer::GetVideoStreamIndex() const
{
	return m_av->video_stream_idx;
}

std::string OverlayVideoPlayer::GetVideoStreamName() const
{
	if (!m_av->video_stream_name.empty()) {
		return m_av->video_stream_name;
	}
	return "Video";
}

void OverlayVideoPlayer::SetMuted(bool muted)
{
	m_muted.store(muted);
}

void OverlayVideoPlayer::SetVolume(double volume)
{
	m_volume.store((std::max)(0.0, (std::min)(1.0, volume)));
}

void OverlayVideoPlayer::SetTargetSize(int maxWidth, int maxHeight)
{
	if (maxWidth < 0)
		maxWidth = 0;
	if (maxHeight < 0)
		maxHeight = 0;
	if (m_targetMaxWidth == maxWidth && m_targetMaxHeight == maxHeight) {
		return;
	}
	int srcW = m_sourceWidth > 0 ? m_sourceWidth : 1280;
	int srcH = m_sourceHeight > 0 ? m_sourceHeight : 720;
	int maxW = maxWidth > 0 ? maxWidth : 960;
	int maxH = maxHeight > 0 ? maxHeight : 540;
	double scale = (std::min)(1.0, (std::min)(static_cast<double>(maxW) / srcW, static_cast<double>(maxH) / srcH));
	int outW = (std::max)(16, static_cast<int>(srcW * scale)) & ~1;
	int outH = (std::max)(16, static_cast<int>(srcH * scale)) & ~1;

	const bool sizeChanged = (outW != m_frameWidth) || (outH != m_frameHeight);
	m_targetMaxWidth = maxWidth;
	m_targetMaxHeight = maxHeight;
	if (!sizeChanged) {
		return;
	}

	double t = m_currentTime.load();
	bool wasPlaying = m_playing;
	StopPlayback();

	{
		std::lock_guard<std::mutex> lock(m_frameMutex);
		m_frameWidth = outW;
		m_frameHeight = outH;
		m_frameData.clear();
	}

	if (wasPlaying) {
		m_playing = true;
		StartPlayback(t);
	} else if (!m_openPath.empty()) {
		StartDecodeThread(t, true);
	}
}

bool OverlayVideoPlayer::Update()
{
	if (!m_playing) {
		return false;
	}
	ULONGLONG now = GetTickCount64();
	ULONGLONG last = m_lastFrameTick.load();
	if (last > 0) {
		ULONGLONG stallMs = now - last;
		if (stallMs > 700 && (m_lastStallLogTick == 0 || (now - m_lastStallLogTick) > 2000)) {
			m_lastStallLogTick = now;
			obs_log(LOG_INFO, "OverlayVideoPlayer: video stall %llums",
				static_cast<unsigned long long>(stallMs));
		}
	}
	return true;
}

bool OverlayVideoPlayer::GetFrameSnapshot(std::vector<uint8_t> &out, int &width, int &height) const
{
	std::lock_guard<std::mutex> lock(m_frameMutex);
	if (m_frameData.empty() || m_frameWidth <= 0 || m_frameHeight <= 0) {
		return false;
	}
	out = m_frameData;
	width = m_frameWidth;
	height = m_frameHeight;
	return true;
}

bool OverlayVideoPlayer::AcquireFrame(FrameView &out) const
{
	out = FrameView{};
	std::unique_lock<std::mutex> lock(m_frameMutex);
	if (m_frameData.empty() || m_frameWidth <= 0 || m_frameHeight <= 0) {
		return false;
	}
	out.data = m_frameData.data();
	out.width = m_frameWidth;
	out.height = m_frameHeight;
	out.lock = std::move(lock);
	return true;
}

void OverlayVideoPlayer::StartPlayback(double startTime)
{
	StopPlayback();
	m_playStartTime = startTime;
	m_audioClock = startTime;
	StartThreeThreadPlayback(startTime);
}

void OverlayVideoPlayer::StopPlayback()
{
	StopThreeThreadPlayback();
	m_videoStop.store(true);
	m_audioStop.store(true);
	if (m_decodeThread.joinable()) {
		m_decodeThread.join();
	}
	m_videoStop.store(false);
	m_audioStop.store(false);
	StopAudioPlayback();
}

void OverlayVideoPlayer::StopThreeThreadPlayback()
{
	m_demuxStop.store(true);
	m_videoStop.store(true);
	m_audioStop.store(true);
	if (m_tt) {
		m_tt->wakeAll();
	}
	if (m_demuxThread.joinable()) {
		m_demuxThread.join();
	}
	if (m_videoThread.joinable()) {
		m_videoThread.join();
	}
	if (m_audioDecodeThread.joinable()) {
		m_audioDecodeThread.join();
	}
	m_demuxStop.store(false);
	m_videoStop.store(false);
	m_audioStop.store(false);
	m_tt.reset();
}

void OverlayVideoPlayer::StartThreeThreadPlayback(double startTime)
{
	StopThreeThreadPlayback();
	if (m_openPath.empty() || !m_av->video_codec_ctx) {
		return;
	}

	UpdateOutputSize();
	m_playStartTime = startTime;
	m_audioClock = startTime;
	m_seekTargetSec.store(startTime);
	m_seekRequested.store(false);
	m_demuxStop.store(false);
	m_videoStop.store(false);
	m_audioStop.store(false);

	// Decoders are empty here, so seek alone is enough before threads start.
	AVFormatContext *fmt_ctx = m_av->fmt_ctx;
	int64_t seek_target = static_cast<int64_t>(startTime * AV_TIME_BASE);
	if (av_seek_frame(fmt_ctx, -1, seek_target, AVSEEK_FLAG_BACKWARD) < 0) {
		av_seek_frame(fmt_ctx, 0, 0, AVSEEK_FLAG_BACKWARD);
	}

	m_tt = std::make_unique<ThreeThreadState>();

	bool useAudio = (m_av->audio_codec_ctx != nullptr);
	if (useAudio) {
		// Output is resampled to stereo S16; WASAPI must match or shared-mode init may fail.
		StartAudioThread(m_av->audio_sample_rate, 2);
	}

	LibAvState *av = m_av;
	const int video_idx = av->video_stream_idx;
	const int audio_idx = av->audio_stream_idx;
	const double startTimeSec = startTime;

	m_demuxThread = std::thread([this, av, video_idx, audio_idx, useAudio, seek_target, startTimeSec]() {
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
		AVFormatContext *fmt_ctx = av->fmt_ctx;
		AVPacket *pkt = av_packet_alloc();
		if (!pkt)
			return;

		while (!m_demuxStop.load()) {
			if (m_seekRequested.load()) {
				double targetSec = m_seekTargetSec.load();
				m_tt->clearQueues();
				int64_t ts = static_cast<int64_t>(targetSec * AV_TIME_BASE);
				if (av_seek_frame(fmt_ctx, -1, ts, AVSEEK_FLAG_BACKWARD) < 0) {
					av_seek_frame(fmt_ctx, 0, 0, AVSEEK_FLAG_BACKWARD);
				}
				m_playStartTime = targetSec;
				m_audioClock = targetSec;
				m_tt->pushFlushBoth();
				m_seekRequested.store(false);
				continue;
			}

			int ret = av_read_frame(fmt_ctx, pkt);
			if (ret < 0) {
				if (ret == AVERROR_EOF) {
					if (av_seek_frame(fmt_ctx, -1, seek_target, AVSEEK_FLAG_BACKWARD) < 0) {
						av_seek_frame(fmt_ctx, 0, 0, AVSEEK_FLAG_BACKWARD);
					}
					m_tt->pushFlushBoth();
				}
				av_packet_unref(pkt);
				continue;
			}

			if (pkt->stream_index == video_idx) {
				if (!m_tt->pushVideo(this, pkt))
					av_packet_unref(pkt);
			} else if (pkt->stream_index == audio_idx && useAudio) {
				if (!m_tt->pushAudio(this, pkt))
					av_packet_unref(pkt);
			} else {
				av_packet_unref(pkt);
			}
		}

		av_packet_free(&pkt);
	});

	m_videoThread = std::thread([this, av]() {
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
		AVCodecContext *video_ctx = av->video_codec_ctx;
		if (!video_ctx)
			return;

		AVFrame *frame = av_frame_alloc();
		AVFrame *cpu_frame = av_frame_alloc();
		if (!frame || !cpu_frame) {
			av_frame_free(&frame);
			av_frame_free(&cpu_frame);
			return;
		}

		SwsContext *sws = nullptr;
		int swsSrcW = 0, swsSrcH = 0, swsDstW = 0, swsDstH = 0;
		AVPixelFormat swsSrcFmt = AV_PIX_FMT_NONE;
		const size_t frameSize = static_cast<size_t>(m_frameWidth) * m_frameHeight * 4;
		std::vector<uint8_t> buffer(frameSize);
		auto startTick = std::chrono::steady_clock::now();
		const double displayFps = 60.0;
		const double minFrameInterval = (m_videoFps > displayFps) ? (1.0 / displayFps) : 0.0;
		double lastDisplayedElapsed = -1.0;
		bool acceptNextFrame = true;
		AVStream *video_stream = av->fmt_ctx->streams[av->video_stream_idx];

		while (!m_videoStop.load() && m_tt) {
			PacketQueueItem item;
			if (!m_tt->popVideo(this, item))
				break;

			if (item.is_flush) {
				avcodec_flush_buffers(video_ctx);
				startTick = std::chrono::steady_clock::now();
				lastDisplayedElapsed = -1.0;
				acceptNextFrame = true;
				continue;
			}

			int ret = avcodec_send_packet(video_ctx, &item.pkt);
			if (ret != 0 && ret != AVERROR(EAGAIN))
				continue;

			while (avcodec_receive_frame(video_ctx, frame) == 0) {
				AVFrame *display_frame = frame;
				if (av->video_hw_decode) {
					if (!TransferHwFrameToCpu(frame, cpu_frame))
						continue;
					display_frame = cpu_frame;
				}
				double ptsSec = FramePtsSeconds(frame, video_stream);
				// Keyframe seeks can land early; suppress frames before the target time.
				double seekTarget = m_playStartTime;
				const double halfFrame = (m_videoFps > 0.0) ? (0.5 / m_videoFps) : 0.017;
				if (acceptNextFrame) {
					acceptNextFrame = false;
				} else if (ptsSec < seekTarget - halfFrame) {
					continue;
				}

				m_currentTime.store(ptsSec);

				int srcW = display_frame->width, srcH = display_frame->height;
				AVPixelFormat srcFmt = static_cast<AVPixelFormat>(display_frame->format);
				if (!sws || swsSrcW != srcW || swsSrcH != srcH || swsSrcFmt != srcFmt ||
				    swsDstW != m_frameWidth || swsDstH != m_frameHeight) {
					sws_freeContext(sws);
					sws = sws_getContext(srcW, srcH, srcFmt, m_frameWidth, m_frameHeight,
							     AV_PIX_FMT_BGRA, SWS_FAST_BILINEAR, nullptr, nullptr,
							     nullptr);
					if (sws) {
						swsSrcW = srcW;
						swsSrcH = srcH;
						swsSrcFmt = srcFmt;
						swsDstW = m_frameWidth;
						swsDstH = m_frameHeight;
					}
				}
				if (sws) {
					uint8_t *dst[1] = {buffer.data()};
					int dstStride[1] = {m_frameWidth * 4};
					sws_scale(sws, display_frame->data, display_frame->linesize, 0, display_frame->height,
						  dst, dstStride);
					{
						std::lock_guard<std::mutex> lock(m_frameMutex);
						m_frameData.swap(buffer);
					}
					buffer.resize(frameSize);
				}
				m_lastFrameTick.store(GetTickCount64());

				if (!m_playing)
					break;
				double playStart = m_playStartTime;
				double elapsed =
					std::chrono::duration<double>(std::chrono::steady_clock::now() - startTick)
						.count();
				double targetElapsed = ptsSec - playStart;
				if (targetElapsed > elapsed + 1.0)
					continue;
				if (minFrameInterval > 0.0 && lastDisplayedElapsed >= 0.0 &&
				    targetElapsed < lastDisplayedElapsed + minFrameInterval) {
					continue;
				}
				if (targetElapsed > 0.0) {
					double sleepSec = targetElapsed - elapsed;
					if (sleepSec > 0.0) {
						const double maxSleep = 0.05;
						while (sleepSec > maxSleep && !m_videoStop.load() && m_playing) {
							std::this_thread::sleep_for(
								std::chrono::duration<double>(maxSleep));
							elapsed = std::chrono::duration<double>(
									  std::chrono::steady_clock::now() - startTick)
									  .count();
							sleepSec = targetElapsed - elapsed;
						}
						if (sleepSec > 0.0 && sleepSec <= maxSleep) {
							std::this_thread::sleep_for(
								std::chrono::duration<double>(sleepSec));
						}
					}
				}
				lastDisplayedElapsed = targetElapsed;

				double duration = m_duration.load();
				if (duration > 0.0 && m_currentTime.load() >= duration)
					break;
			}
		}

		sws_freeContext(sws);
		av_frame_free(&cpu_frame);
		av_frame_free(&frame);
	});

	m_audioDecodeThread = std::thread([this, av, useAudio]() {
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
		AVCodecContext *audio_ctx = av->audio_codec_ctx;
		if (!audio_ctx || !useAudio)
			return;

		SwrContext *swr = nullptr;
		AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
		swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_S16, av->audio_sample_rate, &audio_ctx->ch_layout,
				    audio_ctx->sample_fmt, audio_ctx->sample_rate, 0, NULL);
		if (!swr || swr_init(swr) < 0) {
			swr_free(&swr);
			return;
		}

		AVFrame *aframe = av_frame_alloc();
		if (!aframe) {
			swr_free(&swr);
			return;
		}

		std::vector<uint8_t> audioAccum;
		const size_t chunkSize = av->audio_sample_rate * 4 / 50;

		while (!m_audioStop.load() && m_tt) {
			PacketQueueItem item;
			if (!m_tt->popAudio(this, item))
				break;

			if (item.is_flush) {
				avcodec_flush_buffers(audio_ctx);
				audioAccum.clear();
				{
					std::lock_guard<std::mutex> lock(m_audioQueueMutex);
					while (!m_audioQueue.empty())
						m_audioQueue.pop();
					m_audioQueueCond.notify_all();
				}
				continue;
			}

			int ret = avcodec_send_packet(audio_ctx, &item.pkt);
			if (ret != 0 && ret != AVERROR(EAGAIN))
				continue;

			while (avcodec_receive_frame(audio_ctx, aframe) == 0) {
				double audioPtsSec = PtsToSeconds(aframe->pts, av->audio_time_base);
				if (audioPtsSec < m_playStartTime)
					continue;

				int outSamples = swr_get_out_samples(swr, aframe->nb_samples);
				std::vector<uint8_t> outBuf(static_cast<size_t>(outSamples) * 4);
				uint8_t *outPtr = outBuf.data();
				swr_convert(swr, &outPtr, outSamples, (const uint8_t **)aframe->data,
					    aframe->nb_samples);
				audioAccum.insert(audioAccum.end(), outBuf.begin(), outBuf.end());

				while (audioAccum.size() >= chunkSize) {
					std::vector<uint8_t> chunk(audioAccum.begin(), audioAccum.begin() + chunkSize);
					audioAccum.erase(audioAccum.begin(), audioAccum.begin() + chunkSize);
					{
						std::unique_lock<std::mutex> lock(m_audioQueueMutex);
						while (m_audioQueue.size() >= 48 && !m_videoStop.load() &&
						       !m_audioStop.load()) {
							m_audioQueueCond.wait_for(lock, std::chrono::milliseconds(20));
						}
						if (m_videoStop.load() || m_audioStop.load())
							break;
						m_audioQueue.push(std::move(chunk));
						m_audioQueueCond.notify_one();
					}
				}
			}
		}

		if (!audioAccum.empty()) {
			std::unique_lock<std::mutex> lock(m_audioQueueMutex);
			while (m_audioQueue.size() >= 48 && !m_audioStop.load()) {
				m_audioQueueCond.wait_for(lock, std::chrono::milliseconds(20));
			}
			if (m_audioQueue.size() < 48) {
				m_audioQueue.push(std::move(audioAccum));
				m_audioQueueCond.notify_one();
			}
		}

		av_frame_free(&aframe);
		swr_free(&swr);
	});
}

void OverlayVideoPlayer::StartDecodeThread(double startTime, bool singleFrame)
{
	if (m_openPath.empty() || !m_av->video_codec_ctx) {
		return;
	}

	UpdateOutputSize();

	m_videoStop.store(true);
	m_audioStop.store(true);
	if (m_decodeThread.joinable()) {
		m_decodeThread.join();
	}
	m_videoStop.store(false);
	m_audioStop.store(false);
	m_decodeThread = std::thread([this, startTime, singleFrame]() {
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
		AVFormatContext *fmt_ctx = m_av->fmt_ctx;
		AVCodecContext *video_ctx = m_av->video_codec_ctx;
		AVCodecContext *audio_ctx = m_av->audio_codec_ctx;
		int video_idx = m_av->video_stream_idx;
		int audio_idx = m_av->audio_stream_idx;

		int64_t seek_target = static_cast<int64_t>(startTime * AV_TIME_BASE);
		if (av_seek_frame(fmt_ctx, -1, seek_target, AVSEEK_FLAG_BACKWARD) < 0) {
			av_seek_frame(fmt_ctx, 0, 0, AVSEEK_FLAG_BACKWARD);
		}
		if (video_ctx)
			avcodec_flush_buffers(video_ctx);
		if (audio_ctx)
			avcodec_flush_buffers(audio_ctx);

		AVPacket *pkt = av_packet_alloc();
		AVFrame *frame = av_frame_alloc();
		AVFrame *cpu_frame = av_frame_alloc();
		if (!pkt || !frame || !cpu_frame) {
			av_packet_free(&pkt);
			av_frame_free(&frame);
			av_frame_free(&cpu_frame);
			return;
		}

		SwsContext *sws = nullptr;
		SwrContext *swr = nullptr;
		int swsSrcW = 0, swsSrcH = 0, swsDstW = 0, swsDstH = 0;
		AVPixelFormat swsSrcFmt = AV_PIX_FMT_NONE;
		std::vector<uint8_t> audioAccum;
		const size_t frameSize = static_cast<size_t>(m_frameWidth) * m_frameHeight * 4;
		std::vector<uint8_t> buffer(frameSize);
		auto startTick = std::chrono::steady_clock::now();
		const double startTimeSec = startTime;
		const double displayFps = 60.0;
		const double minFrameInterval = (m_videoFps > displayFps) ? (1.0 / displayFps) : 0.0;
		double lastDisplayedElapsed = -1.0;
		bool acceptNextFrame = true;
		AVStream *video_stream = m_av->fmt_ctx->streams[video_idx];

		// Mute is applied in the WASAPI loop, but audio still decodes during playback.
	bool useAudio = !singleFrame && audio_ctx;
		if (useAudio) {
			AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
			swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_S16, m_av->audio_sample_rate,
					    &audio_ctx->ch_layout, audio_ctx->sample_fmt, audio_ctx->sample_rate, 0,
					    NULL);
			if (swr && swr_init(swr) >= 0) {
				StartAudioThread(m_av->audio_sample_rate, 2);
			} else {
				swr_free(&swr);
				swr = nullptr;
				useAudio = false;
			}
		}

		const size_t chunkSize = m_av->audio_sample_rate * 4 / 50;
		bool gotFirstFrame = false;

		while (!m_videoStop.load() && !m_audioStop.load()) {
			int ret = av_read_frame(fmt_ctx, pkt);
			if (ret < 0) {
				if (ret == AVERROR_EOF) {
					if (singleFrame)
						break;
					av_seek_frame(fmt_ctx, -1, seek_target, AVSEEK_FLAG_BACKWARD);
					if (video_ctx)
						avcodec_flush_buffers(video_ctx);
					if (audio_ctx)
						avcodec_flush_buffers(audio_ctx);
					startTick = std::chrono::steady_clock::now();
					acceptNextFrame = true;
				}
				av_packet_unref(pkt);
				continue;
			}

			if (pkt->stream_index == video_idx && video_ctx) {
				if (singleFrame && gotFirstFrame) {
					av_packet_unref(pkt);
					continue;
				}
				ret = avcodec_send_packet(video_ctx, pkt);
				av_packet_unref(pkt);
				if (ret != 0 && ret != AVERROR(EAGAIN))
					continue;
				if (ret == 0 || ret == AVERROR(EAGAIN)) {
					while (avcodec_receive_frame(video_ctx, frame) == 0) {
						AVFrame *display_frame = frame;
						if (m_av->video_hw_decode) {
							if (!TransferHwFrameToCpu(frame, cpu_frame))
								continue;
							display_frame = cpu_frame;
						}
						double ptsSec = FramePtsSeconds(frame, video_stream);
						// Keyframe seeks can land early; suppress frames before the target time.
						const double halfFrame = (m_videoFps > 0.0) ? (0.5 / m_videoFps)
											    : 0.017;
						if (acceptNextFrame) {
							acceptNextFrame = false;
						} else if (ptsSec < startTimeSec - halfFrame) {
							continue;
						}

						m_currentTime.store(ptsSec);

						int srcW = display_frame->width, srcH = display_frame->height;
						AVPixelFormat srcFmt = static_cast<AVPixelFormat>(display_frame->format);
						if (!sws || swsSrcW != srcW || swsSrcH != srcH || swsSrcFmt != srcFmt ||
						    swsDstW != m_frameWidth || swsDstH != m_frameHeight) {
							sws_freeContext(sws);
							sws = sws_getContext(srcW, srcH, srcFmt, m_frameWidth,
									     m_frameHeight, AV_PIX_FMT_BGRA,
									     SWS_FAST_BILINEAR, nullptr, nullptr,
									     nullptr);
							if (sws) {
								swsSrcW = srcW;
								swsSrcH = srcH;
								swsSrcFmt = srcFmt;
								swsDstW = m_frameWidth;
								swsDstH = m_frameHeight;
							}
						}
						if (sws) {
							uint8_t *dst[1] = {buffer.data()};
							int dstStride[1] = {m_frameWidth * 4};
							sws_scale(sws, display_frame->data, display_frame->linesize, 0,
								  display_frame->height, dst, dstStride);
							{
								std::lock_guard<std::mutex> lock(m_frameMutex);
								m_frameData.swap(buffer);
							}
							buffer.resize(frameSize);
						}
						m_lastFrameTick.store(GetTickCount64());

						if (singleFrame) {
							gotFirstFrame = true;
							goto cleanup;
						}

						if (!m_playing)
							break;
						double elapsed = std::chrono::duration<double>(
									 std::chrono::steady_clock::now() - startTick)
									 .count();
						double targetElapsed = ptsSec - startTimeSec;
						if (targetElapsed > elapsed + 1.0) {
							continue;
						}
						// Cap display rate at 60 fps when the source exceeds it.
						if (minFrameInterval > 0.0 && lastDisplayedElapsed >= 0.0 &&
						    targetElapsed < lastDisplayedElapsed + minFrameInterval) {
							continue;
						}
						// Prefer feeding audio when the queue runs low during video pacing.
						if (useAudio) {
							std::lock_guard<std::mutex> lock(m_audioQueueMutex);
							if (m_audioQueue.size() < 4) {
								lastDisplayedElapsed = targetElapsed;
								continue;
							}
						}
						if (targetElapsed > 0.0) {
							double sleepSec = targetElapsed - elapsed;
							if (sleepSec > 0.0) {
								const double maxSleep = 0.05;
								while (sleepSec > maxSleep && !m_videoStop.load() &&
								       m_playing) {
									std::this_thread::sleep_for(
										std::chrono::duration<double>(
											maxSleep));
									elapsed =
										std::chrono::duration<double>(
											std::chrono::steady_clock::now() -
											startTick)
											.count();
									sleepSec = targetElapsed - elapsed;
								}
								if (sleepSec > 0.0 && sleepSec <= maxSleep) {
									std::this_thread::sleep_for(
										std::chrono::duration<double>(
											sleepSec));
								}
							}
						}
						lastDisplayedElapsed = targetElapsed;

						double duration = m_duration.load();
						if (duration > 0.0 && m_currentTime.load() >= duration)
							goto cleanup;
					}
				}
			} else if (pkt->stream_index == audio_idx && audio_ctx && useAudio && swr) {
				ret = avcodec_send_packet(audio_ctx, pkt);
				av_packet_unref(pkt);
				if (ret == 0 || ret == AVERROR(EAGAIN)) {
					AVFrame *aframe = av_frame_alloc();
					if (aframe && avcodec_receive_frame(audio_ctx, aframe) == 0) {
						double audioPtsSec = PtsToSeconds(aframe->pts, m_av->audio_time_base);
						if (audioPtsSec < startTimeSec) {
							av_frame_free(&aframe);
							continue;
						}
						int outSamples = swr_get_out_samples(swr, aframe->nb_samples);
						std::vector<uint8_t> outBuf(outSamples * 4);
						uint8_t *outPtr = outBuf.data();
						swr_convert(swr, &outPtr, outSamples, (const uint8_t **)aframe->data,
							    aframe->nb_samples);
						audioAccum.insert(audioAccum.end(), outBuf.begin(), outBuf.end());
						while (audioAccum.size() >= chunkSize) {
							std::vector<uint8_t> chunk(audioAccum.begin(),
										   audioAccum.begin() + chunkSize);
							audioAccum.erase(audioAccum.begin(),
									 audioAccum.begin() + chunkSize);
							{
								std::unique_lock<std::mutex> lock(m_audioQueueMutex);
								while (m_audioQueue.size() >= 48 &&
								       !m_videoStop.load() && !m_audioStop.load()) {
									m_audioQueueCond.wait_for(
										lock, std::chrono::milliseconds(20));
								}
								if (m_videoStop.load() || m_audioStop.load())
									break;
								m_audioQueue.push(std::move(chunk));
								m_audioQueueCond.notify_one();
							}
						}
						av_frame_free(&aframe);
					}
				}
			} else {
				av_packet_unref(pkt);
			}
		}

	cleanup:
		if (!audioAccum.empty()) {
			std::unique_lock<std::mutex> lock(m_audioQueueMutex);
			while (m_audioQueue.size() >= 48 && !m_audioStop.load()) {
				m_audioQueueCond.wait_for(lock, std::chrono::milliseconds(20));
			}
			if (m_audioQueue.size() < 48) {
				m_audioQueue.push(std::move(audioAccum));
				m_audioQueueCond.notify_one();
			}
		}
		sws_freeContext(sws);
		swr_free(&swr);
		StopAudioPlayback();
		av_packet_free(&pkt);
		av_frame_free(&cpu_frame);
		av_frame_free(&frame);
	});
}

void OverlayVideoPlayer::StartAudioThread(int sampleRate, int channels)
{
	StopAudioPlayback();
	m_audioStop.store(false);
	m_audioThread = std::thread([this, sampleRate, channels]() {
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
		timeBeginPeriod(1);
		HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
		if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
			timeEndPeriod(1);
			return;
		}

		IMMDeviceEnumerator *enumerator = NULL;
		IMMDevice *device = NULL;
		IAudioClient *client = NULL;
		IAudioRenderClient *render = NULL;
		WAVEFORMATEX wfx = {};
		REFERENCE_TIME bufDuration = 1000000;
		UINT32 bufferFrames = 0;
		const size_t bytesPerFrame = static_cast<size_t>(channels) * 2;
		std::vector<uint8_t> pending;
		size_t pendingOffset = 0;

		hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
				      (void **)&enumerator);
		if (FAILED(hr))
			goto wasapi_cleanup;

		hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
		if (FAILED(hr))
			goto wasapi_cleanup;

		hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void **)&client);
		if (FAILED(hr))
			goto wasapi_cleanup;

		wfx.wFormatTag = WAVE_FORMAT_PCM;
		wfx.nChannels = static_cast<WORD>(channels);
		wfx.nSamplesPerSec = sampleRate;
		wfx.wBitsPerSample = 16;
		wfx.nBlockAlign = wfx.nChannels * 2;
		wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
		wfx.cbSize = 0;

		hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, bufDuration, 0, &wfx, NULL);
		if (FAILED(hr)) {
			obs_log(LOG_WARNING, "[OverlayVideoPlayer] WASAPI Initialize failed: 0x%08lX",
				(unsigned long)hr);
			goto wasapi_cleanup;
		}

		hr = client->GetService(__uuidof(IAudioRenderClient), (void **)&render);
		if (FAILED(hr))
			goto wasapi_cleanup;

		{
			std::unique_lock<std::mutex> lock(m_audioQueueMutex);
			m_audioQueueCond.wait_for(lock, std::chrono::milliseconds(500),
						  [this] { return m_audioQueue.size() >= 8 || m_audioStop.load(); });
		}

		hr = client->Start();
		if (FAILED(hr)) {
			obs_log(LOG_WARNING, "[OverlayVideoPlayer] WASAPI Start failed: 0x%08lX", (unsigned long)hr);
			goto wasapi_cleanup;
		}

		client->GetBufferSize(&bufferFrames);

		while (!m_audioStop.load()) {
			UINT32 padding = 0;
			client->GetCurrentPadding(&padding);
			UINT32 available = bufferFrames - padding;
			if (available < 128) {
				Sleep(5);
				continue;
			}

			while (available > 0 && !m_audioStop.load()) {
				if (pendingOffset >= pending.size()) {
					{
						std::unique_lock<std::mutex> lock(m_audioQueueMutex);
						m_audioQueueCond.wait_for(lock, std::chrono::milliseconds(50), [this] {
							return !m_audioQueue.empty() || m_audioStop.load();
						});
					}
					if (m_audioStop.load())
						break;
					{
						std::lock_guard<std::mutex> lock(m_audioQueueMutex);
						if (m_audioQueue.empty())
							break;
						pending = std::move(m_audioQueue.front());
						m_audioQueue.pop();
						m_audioQueueCond.notify_one();
					}
					pendingOffset = 0;
				}
				if (pendingOffset >= pending.size())
					break;

				size_t bytesAvail = pending.size() - pendingOffset;
				UINT32 framesToWrite = static_cast<UINT32>(
					(std::min)(bytesAvail / bytesPerFrame, static_cast<size_t>(available)));
				if (framesToWrite == 0)
					break;

				BYTE *wasapiBuf = NULL;
				hr = render->GetBuffer(framesToWrite, &wasapiBuf);
				if (FAILED(hr))
					break;

				const bool muted = m_muted.load();
				double vol = m_volume.load();
				if (muted || vol <= 0.0) {
					memset(wasapiBuf, 0, framesToWrite * bytesPerFrame);
				} else if (vol >= 1.0) {
					memcpy(wasapiBuf, pending.data() + pendingOffset,
					       framesToWrite * bytesPerFrame);
				} else {
					const int16_t *src =
						reinterpret_cast<const int16_t *>(pending.data() + pendingOffset);
					int16_t *dst = reinterpret_cast<int16_t *>(wasapiBuf);
					const size_t n = static_cast<size_t>(framesToWrite) * channels;
					for (size_t i = 0; i < n; i++) {
						int32_t s = static_cast<int32_t>(static_cast<double>(src[i]) * vol);
						dst[i] = static_cast<int16_t>((std::max)(-32768, (std::min)(32767, s)));
					}
				}
				render->ReleaseBuffer(framesToWrite, 0);

				pendingOffset += framesToWrite * bytesPerFrame;
				available -= framesToWrite;
			}
		}

		if (client)
			client->Stop();

	wasapi_cleanup:
		if (render)
			render->Release();
		if (client)
			client->Release();
		if (device)
			device->Release();
		if (enumerator)
			enumerator->Release();
		CoUninitialize();
		timeEndPeriod(1);
	});
}

void OverlayVideoPlayer::StopAudioPlayback()
{
	m_audioStop.store(true);
	m_audioQueueCond.notify_all();
	if (m_audioThread.joinable()) {
		m_audioThread.join();
	}
	{
		std::lock_guard<std::mutex> lock(m_audioQueueMutex);
		while (!m_audioQueue.empty())
			m_audioQueue.pop();
	}
}

void OverlayVideoPlayer::UpdateOutputSize()
{
	int srcW = m_sourceWidth > 0 ? m_sourceWidth : 1280;
	int srcH = m_sourceHeight > 0 ? m_sourceHeight : 720;
	int maxW = m_targetMaxWidth > 0 ? m_targetMaxWidth : 960;
	int maxH = m_targetMaxHeight > 0 ? m_targetMaxHeight : 540;
	double scale = (std::min)(1.0, (std::min)(static_cast<double>(maxW) / srcW, static_cast<double>(maxH) / srcH));
	int outW = (std::max)(16, static_cast<int>(srcW * scale)) & ~1;
	int outH = (std::max)(16, static_cast<int>(srcH * scale)) & ~1;
	m_frameWidth = outW;
	m_frameHeight = outH;
}

#endif // _WIN32