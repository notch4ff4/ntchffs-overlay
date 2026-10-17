/*
Clip-duration save flow adapted from Replay Buffer Pro by @JoshuaPotter
(https://github.com/JoshuaPotter/replay-buffer-pro), GPL-2.0-or-later.

Permission to adapt the code/logic was granted by the author in
https://github.com/JoshuaPotter/replay-buffer-pro/issues/30
("Hey, feel free to use what you'd like" — @JoshuaPotter, 2026-08-07).
*/

#include "overlay-clip-save.h"

#ifdef ENABLE_FRONTEND_API

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>
#include <util/config-file.h>

#ifdef ENABLE_QT
#include <obs-frontend-api.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "overlay-smart-replay.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "overlay-trimmer.h"
#include "overlay-string-utils.h"
#endif

static std::atomic<int> g_pending_clip_seconds{0};
static std::mutex g_clip_trim_mutex;

static int clamp_clip_seconds(int seconds)
{
	if (seconds < OVERLAY_CLIP_SAVE_MIN_SECONDS)
		return OVERLAY_CLIP_SAVE_MIN_SECONDS;
	if (seconds > OVERLAY_CLIP_SAVE_MAX_SECONDS)
		return OVERLAY_CLIP_SAVE_MAX_SECONDS;
	return seconds;
}

static const char *replay_buffer_config_section(config_t *config)
{
	const char *mode = config_get_string(config, "Output", "Mode");
	if (mode && strcmp(mode, "Advanced") == 0)
		return "AdvOut";
	return "SimpleOutput";
}

int overlay_clip_save_get_buffer_length(void)
{
	config_t *config = obs_frontend_get_profile_config();
	if (!config)
		return -1;

	const char *section = replay_buffer_config_section(config);
	return static_cast<int>(config_get_uint(config, section, "RecRBTime"));
}

bool overlay_clip_save_set_buffer_length(int seconds)
{
	seconds = clamp_clip_seconds(seconds);

	config_t *config = obs_frontend_get_profile_config();
	if (!config)
		return false;

	if (obs_frontend_replay_buffer_active()) {
		obs_log(LOG_WARNING, "[ClipSave] cannot change buffer length while replay buffer is active");
		return false;
	}

	const char *section = replay_buffer_config_section(config);
	const uint64_t current = config_get_uint(config, section, "RecRBTime");
	if (current == static_cast<uint64_t>(seconds))
		return true;

	config_set_uint(config, section, "RecRBTime", static_cast<uint64_t>(seconds));
	config_save(config);

	if (obs_output_t *replay_output = obs_frontend_get_replay_buffer_output()) {
		obs_data_t *settings = obs_output_get_settings(replay_output);
		if (settings) {
			obs_data_set_int(settings, "max_time_sec", seconds);
			obs_output_update(replay_output, settings);
			obs_data_release(settings);
		}
		obs_output_release(replay_output);
	}

	obs_frontend_save();
	obs_log(LOG_INFO, "[ClipSave] buffer length set to %d s", seconds);
	return true;
}

bool overlay_clip_save_request(int seconds)
{
	const int requested = seconds;
	const int buffer_len = overlay_clip_save_get_buffer_length();
	const bool keep_last = overlay_smart_replay_clip_uses_keep_last();
	const int pending_before = g_pending_clip_seconds.load();

	obs_log(LOG_INFO,
		"[ClipSave] request begin requested=%d s clamped_later keep_last_mode=%s buffer_len=%d pending_before=%d replay_active=%s",
		requested, keep_last ? "yes" : "no", buffer_len, pending_before,
		obs_frontend_replay_buffer_active() ? "yes" : "no");

	if (seconds <= 0) {
		obs_log(LOG_WARNING, "[ClipSave] request rejected: non-positive duration (%d)", requested);
		return false;
	}

	if (!obs_frontend_replay_buffer_active()) {
		obs_log(LOG_WARNING, "[ClipSave] request rejected: replay buffer inactive");
		return false;
	}

	seconds = clamp_clip_seconds(seconds);
	if (seconds != requested) {
		obs_log(LOG_INFO, "[ClipSave] duration clamped %d -> %d s", requested, seconds);
	}

	if (keep_last) {
		if (buffer_len > 0 && seconds > buffer_len) {
			obs_log(LOG_WARNING, "[ClipSave] request rejected: %d s > buffer %d s", seconds, buffer_len);
			return false;
		}

		int expected = 0;
		if (!g_pending_clip_seconds.compare_exchange_strong(expected, seconds)) {
			obs_log(LOG_WARNING, "[ClipSave] request rejected: pending clip already set (%d s)", expected);
			return false;
		}
		obs_log(LOG_INFO,
			"[ClipSave] pending keep-last set to %d s; calling obs_frontend_replay_buffer_save()",
			seconds);
	} else {
		g_pending_clip_seconds.store(0);
		obs_log(LOG_WARNING,
			"[ClipSave] keep_last_mode=no: preset duration %d s will NOT trim to that length; using Smart Replay timestamp-trim path instead",
			seconds);
	}

	// Advance Smart Replay timestamp-trim timeline so later full saves trim correctly.
	const bool noted = overlay_smart_replay_note_save_request();
	obs_log(LOG_INFO, "[ClipSave] smart_replay_note_save_request=%s", noted ? "ok" : "failed/skipped");
	obs_frontend_replay_buffer_save();
	return true;
}

#ifdef _WIN32
static uint64_t get_file_size_bytes(const std::wstring &path)
{
	WIN32_FILE_ATTRIBUTE_DATA attrs = {};
	if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attrs))
		return 0;
	ULARGE_INTEGER size;
	size.LowPart = attrs.nFileSizeLow;
	size.HighPart = attrs.nFileSizeHigh;
	return size.QuadPart;
}

static void log_clip_probe(const char *label, const std::wstring &path)
{
	double start_sec = 0.0;
	double end_sec = 0.0;
	int ref_stream = -1;
	int video_packets = 0;
	double container_duration_sec = -1.0;
	std::wstring err;
	const std::string path_utf8 = overlay::util::WideToUtf8(path);
	if (!overlay_replay_probe_playback_range(path, start_sec, end_sec, err, &ref_stream, &video_packets,
						 &container_duration_sec)) {
		obs_log(LOG_WARNING, "[ClipSave] %s probe failed path=%s size=%llu error=%s", label,
			path_utf8.c_str(), static_cast<unsigned long long>(get_file_size_bytes(path)),
			overlay::util::WideToUtf8(err.empty() ? L"unknown" : err).c_str());
		return;
	}

	obs_log(LOG_INFO,
		"[ClipSave] %s path=%s size=%llu bytes container_duration=%.3f playback=[%.3f..%.3f] span=%.3f s ref_stream=%d video_packets=%d",
		label, path_utf8.c_str(), static_cast<unsigned long long>(get_file_size_bytes(path)),
		container_duration_sec, start_sec, end_sec, std::max(0.0, end_sec - start_sec), ref_stream,
		video_packets);
}

static bool replace_file_with_trimmed(const std::wstring &originalPath, const std::wstring &trimmedPath)
{
	const std::string original_utf8 = overlay::util::WideToUtf8(originalPath);
	const std::string trimmed_utf8 = overlay::util::WideToUtf8(trimmedPath);
	const uint64_t original_size = get_file_size_bytes(originalPath);
	const uint64_t trimmed_size = get_file_size_bytes(trimmedPath);

	obs_log(LOG_INFO,
		"[ClipSave] replace begin original=%s (%llu bytes) trimmed=%s (%llu bytes)",
		original_utf8.c_str(), static_cast<unsigned long long>(original_size), trimmed_utf8.c_str(),
		static_cast<unsigned long long>(trimmed_size));

	if (trimmed_size == 0) {
		obs_log(LOG_ERROR,
			"[ClipSave] replace aborted: trimmed file missing or empty path=%s",
			trimmed_utf8.c_str());
		return false;
	}

	if (!DeleteFileW(originalPath.c_str())) {
		const DWORD err = GetLastError();
		obs_log(LOG_ERROR, "[ClipSave] failed to delete original file path=%s winerr=%lu",
			original_utf8.c_str(), static_cast<unsigned long>(err));
		return false;
	}
	if (!MoveFileW(trimmedPath.c_str(), originalPath.c_str())) {
		const DWORD err = GetLastError();
		obs_log(LOG_ERROR, "[ClipSave] failed to move trimmed file into place from=%s to=%s winerr=%lu",
			trimmed_utf8.c_str(), original_utf8.c_str(), static_cast<unsigned long>(err));
		return false;
	}

	obs_log(LOG_INFO, "[ClipSave] replace done path=%s final_size=%llu bytes", original_utf8.c_str(),
		static_cast<unsigned long long>(get_file_size_bytes(originalPath)));
	return true;
}

static void trim_clip_keep_last_async(const std::wstring &inputPath, double keepSeconds)
{
	obs_log(LOG_INFO, "[ClipSave] trim worker thread spawning keep=%.3f s path=%s", keepSeconds,
		overlay::util::WideToUtf8(inputPath).c_str());

	std::thread([inputPath, keepSeconds]() {
		const std::string input_utf8 = overlay::util::WideToUtf8(inputPath);
		obs_log(LOG_INFO, "[ClipSave] trim worker entered path=%s keep=%.3f s (waiting for trim mutex)",
			input_utf8.c_str(), keepSeconds);

		std::lock_guard<std::mutex> lock(g_clip_trim_mutex);

		const uint64_t trim_start_ns = os_gettime_ns();
		const uint64_t size_bytes = get_file_size_bytes(inputPath);
		obs_log(LOG_INFO, "[ClipSave] trim worker start path=%s keep=%.3f s size=%llu bytes",
			input_utf8.c_str(), keepSeconds, static_cast<unsigned long long>(size_bytes));

		double playback_start_sec = 0.0;
		double playback_end_sec = 0.0;
		int ref_stream = -1;
		int video_packets = 0;
		double container_duration_sec = -1.0;
		std::wstring probe_err;

		obs_log(LOG_INFO,
			"[ClipSave] pre-trim probe begin path=%s (full packet scan; large files can take a while)",
			input_utf8.c_str());
		const uint64_t probe_start_ns = os_gettime_ns();
		if (!overlay_replay_probe_playback_range(inputPath, playback_start_sec, playback_end_sec, probe_err,
							 &ref_stream, &video_packets, &container_duration_sec)) {
			obs_log(LOG_WARNING, "[ClipSave] trim probe failed path=%s error=%s", input_utf8.c_str(),
				overlay::util::WideToUtf8(probe_err.empty() ? L"unknown" : probe_err).c_str());
			return;
		}
		const double probe_elapsed_sec =
			static_cast<double>(os_gettime_ns() - probe_start_ns) / 1000000000.0;

		const double file_duration_sec = std::max(0.0, playback_end_sec - playback_start_sec);
		const double trim_start_sec = std::max(playback_start_sec, playback_end_sec - keepSeconds);
		const double trim_remove_sec = std::max(0.0, file_duration_sec - keepSeconds);
		constexpr double kTailEpsilon = 0.05;

		obs_log(LOG_INFO,
			"[ClipSave] pre-trim probe done path=%s size=%llu bytes container_duration=%.3f playback=[%.3f..%.3f] span=%.3f s ref_stream=%d video_packets=%d probe_time=%.3f s",
			input_utf8.c_str(), static_cast<unsigned long long>(size_bytes), container_duration_sec,
			playback_start_sec, playback_end_sec, file_duration_sec, ref_stream, video_packets,
			probe_elapsed_sec);

		obs_log(LOG_INFO,
			"[ClipSave] trim plan path=%s keep=%.3f trim_start=%.3f remove=%.3f",
			input_utf8.c_str(), keepSeconds, trim_start_sec, trim_remove_sec);

		if (keepSeconds + kTailEpsilon >= file_duration_sec) {
			obs_log(LOG_WARNING,
				"[ClipSave] trim skipped path=%s reason=keep_ge_file keep=%.3f span=%.3f epsilon=%.3f (file left full-length)",
				input_utf8.c_str(), keepSeconds, file_duration_sec, kTailEpsilon);
			return;
		}

		const size_t dot = inputPath.find_last_of(L'.');
		std::wstring tempPath = (dot == std::wstring::npos)
						? inputPath + L".clipsave.tmp"
						: inputPath.substr(0, dot) + L".clipsave.tmp" + inputPath.substr(dot);

		obs_log(LOG_INFO, "[ClipSave] remux begin path=%s temp=%s keep=%.3f trim_start=%.3f",
			input_utf8.c_str(), overlay::util::WideToUtf8(tempPath).c_str(), keepSeconds,
			trim_start_sec);
		const uint64_t remux_start_ns = os_gettime_ns();

		std::wstring err;
		// Use startSeconds remux directly — keep_last would probe the whole file again.
		const bool ok = overlay_replay_lossless_trim(inputPath, tempPath, trim_start_sec, err);
		const double remux_elapsed_sec =
			static_cast<double>(os_gettime_ns() - remux_start_ns) / 1000000000.0;
		if (!ok) {
			DeleteFileW(tempPath.c_str());
			obs_log(LOG_ERROR, "[ClipSave] trim failed path=%s remux_time=%.3f s error=%s",
				input_utf8.c_str(), remux_elapsed_sec,
				overlay::util::WideToUtf8(err.empty() ? L"unknown" : err).c_str());
			return;
		}

		obs_log(LOG_INFO, "[ClipSave] remux finished path=%s remux_time=%.3f s temp_size=%llu bytes",
			input_utf8.c_str(), remux_elapsed_sec,
			static_cast<unsigned long long>(get_file_size_bytes(tempPath)));

		if (get_file_size_bytes(tempPath) == 0) {
			obs_log(LOG_ERROR,
				"[ClipSave] trim reported success but temp output missing/empty path=%s temp=%s (file left full-length)",
				input_utf8.c_str(), overlay::util::WideToUtf8(tempPath).c_str());
			DeleteFileW(tempPath.c_str());
			return;
		}

		log_clip_probe("temp-trim", tempPath);

		if (!replace_file_with_trimmed(inputPath, tempPath)) {
			DeleteFileW(tempPath.c_str());
			obs_log(LOG_ERROR,
				"[ClipSave] trim replace failed path=%s (original may be incomplete; check prior errors)",
				input_utf8.c_str());
			return;
		}

		log_clip_probe("post-trim", inputPath);

		double actual_span_sec = 0.0;
		{
			double post_start_sec = 0.0;
			double post_end_sec = 0.0;
			std::wstring post_err;
			if (overlay_replay_probe_playback_range(inputPath, post_start_sec, post_end_sec, post_err)) {
				actual_span_sec = std::max(0.0, post_end_sec - post_start_sec);
			}
		}

		const double worker_elapsed_sec =
			static_cast<double>(os_gettime_ns() - trim_start_ns) / 1000000000.0;
		obs_log(LOG_INFO,
			"[ClipSave] trim done path=%s keep_target=%.3f actual=%.3f delta=%+.3f removed_planned=%.3f worker_time=%.3f s",
			input_utf8.c_str(), keepSeconds, actual_span_sec, actual_span_sec - keepSeconds,
			trim_remove_sec, worker_elapsed_sec);
	}).detach();
}
#endif // _WIN32

bool overlay_clip_save_on_replay_saved(const char *replay_path)
{
	const int pending = g_pending_clip_seconds.exchange(0);
	obs_log(LOG_INFO,
		"[ClipSave] on_replay_saved pending=%d s path=%s exists=%s keep_last_mode=%s",
		pending, replay_path ? replay_path : "(null)",
		(replay_path && replay_path[0] != '\0' && os_file_exists(replay_path)) ? "yes" : "no",
		overlay_smart_replay_clip_uses_keep_last() ? "yes" : "no");

	if (pending <= 0) {
		obs_log(LOG_INFO,
			"[ClipSave] on_replay_saved: no pending keep-last duration (timed clip trim will not run)");
		return false;
	}

	if (!replay_path || replay_path[0] == '\0' || !os_file_exists(replay_path)) {
		obs_log(LOG_WARNING, "[ClipSave] pending %d s but saved path missing/invalid", pending);
		return true;
	}

#ifdef _WIN32
	obs_log(LOG_INFO, "[ClipSave] scheduling keep-last trim path=%s keep=%d s size=%llu bytes",
		replay_path, pending,
		static_cast<unsigned long long>(get_file_size_bytes(overlay::util::Utf8ToWide(replay_path))));
	trim_clip_keep_last_async(overlay::util::Utf8ToWide(replay_path), static_cast<double>(pending));
#else
	UNUSED_PARAMETER(replay_path);
	obs_log(LOG_WARNING, "[ClipSave] timed trim is only supported on Windows");
#endif
	return true;
}

#else // !ENABLE_QT

bool overlay_clip_save_request(int seconds)
{
	UNUSED_PARAMETER(seconds);
	return false;
}

bool overlay_clip_save_on_replay_saved(const char *replay_path)
{
	UNUSED_PARAMETER(replay_path);
	return false;
}

int overlay_clip_save_get_buffer_length(void)
{
	return -1;
}

bool overlay_clip_save_set_buffer_length(int seconds)
{
	UNUSED_PARAMETER(seconds);
	return false;
}

#endif // ENABLE_QT

#else // !ENABLE_FRONTEND_API

bool overlay_clip_save_request(int seconds)
{
	UNUSED_PARAMETER(seconds);
	return false;
}

bool overlay_clip_save_on_replay_saved(const char *replay_path)
{
	UNUSED_PARAMETER(replay_path);
	return false;
}

int overlay_clip_save_get_buffer_length(void)
{
	return -1;
}

bool overlay_clip_save_set_buffer_length(int seconds)
{
	UNUSED_PARAMETER(seconds);
	return false;
}

#endif // ENABLE_FRONTEND_API
