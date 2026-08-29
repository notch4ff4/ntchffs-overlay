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
	if (seconds <= 0) {
		obs_log(LOG_WARNING, "[ClipSave] request rejected: non-positive duration");
		return false;
	}

	if (!obs_frontend_replay_buffer_active()) {
		obs_log(LOG_WARNING, "[ClipSave] request rejected: replay buffer inactive");
		return false;
	}

	seconds = clamp_clip_seconds(seconds);
	const bool keep_last = overlay_smart_replay_clip_uses_keep_last();
	if (keep_last) {
		const int buffer_len = overlay_clip_save_get_buffer_length();
		if (buffer_len > 0 && seconds > buffer_len) {
			obs_log(LOG_WARNING, "[ClipSave] request rejected: %d s > buffer %d s", seconds, buffer_len);
			return false;
		}

		int expected = 0;
		if (!g_pending_clip_seconds.compare_exchange_strong(expected, seconds)) {
			obs_log(LOG_INFO, "[ClipSave] request rejected: pending clip already set (%d s)", expected);
			return false;
		}
		obs_log(LOG_INFO, "[ClipSave] saving clip keep-last %d s", seconds);
	} else {
		g_pending_clip_seconds.store(0);
		obs_log(LOG_INFO,
			"[ClipSave] saving via timestamp trim (requested %d s label ignored for length)",
			seconds);
	}

	// Advance Smart Replay timestamp-trim timeline so later full saves trim correctly.
	overlay_smart_replay_note_save_request();
	obs_frontend_replay_buffer_save();
	return true;
}

#ifdef _WIN32
static bool replace_file_with_trimmed(const std::wstring &originalPath, const std::wstring &trimmedPath)
{
	if (!DeleteFileW(originalPath.c_str())) {
		obs_log(LOG_ERROR, "[ClipSave] failed to delete original file");
		return false;
	}
	if (!MoveFileW(trimmedPath.c_str(), originalPath.c_str())) {
		obs_log(LOG_ERROR, "[ClipSave] failed to move trimmed file into place");
		return false;
	}
	return true;
}

static void trim_clip_keep_last_async(const std::wstring &inputPath, double keepSeconds)
{
	std::thread([inputPath, keepSeconds]() {
		std::lock_guard<std::mutex> lock(g_clip_trim_mutex);

		const std::string input_utf8 = overlay::util::WideToUtf8(inputPath);
		obs_log(LOG_INFO, "[ClipSave] trim worker start path=%s keep=%.3f s", input_utf8.c_str(),
			keepSeconds);

		double playback_start_sec = 0.0;
		double playback_end_sec = 0.0;
		std::wstring probe_err;
		if (!overlay_replay_probe_playback_range(inputPath, playback_start_sec, playback_end_sec, probe_err)) {
			obs_log(LOG_WARNING, "[ClipSave] trim probe failed path=%s error=%s", input_utf8.c_str(),
				overlay::util::WideToUtf8(probe_err.empty() ? L"unknown" : probe_err).c_str());
			return;
		}

		const double file_duration_sec = std::max(0.0, playback_end_sec - playback_start_sec);
		constexpr double kTailEpsilon = 0.05;
		if (keepSeconds + kTailEpsilon >= file_duration_sec) {
			obs_log(LOG_INFO,
				"[ClipSave] trim skipped path=%s reason=keep_ge_file keep=%.3f span=%.3f",
				input_utf8.c_str(), keepSeconds, file_duration_sec);
			return;
		}

		const size_t dot = inputPath.find_last_of(L'.');
		std::wstring tempPath = (dot == std::wstring::npos)
						? inputPath + L".clipsave.tmp"
						: inputPath.substr(0, dot) + L".clipsave.tmp" + inputPath.substr(dot);

		std::wstring err;
		const bool ok = overlay_replay_lossless_trim_keep_last(inputPath, tempPath, keepSeconds, err);
		if (!ok) {
			DeleteFileW(tempPath.c_str());
			obs_log(LOG_ERROR, "[ClipSave] trim failed path=%s error=%s", input_utf8.c_str(),
				overlay::util::WideToUtf8(err.empty() ? L"unknown" : err).c_str());
			return;
		}

		if (!replace_file_with_trimmed(inputPath, tempPath)) {
			DeleteFileW(tempPath.c_str());
			return;
		}

		obs_log(LOG_INFO, "[ClipSave] trim done path=%s keep=%.3f s", input_utf8.c_str(), keepSeconds);
	}).detach();
}
#endif // _WIN32

bool overlay_clip_save_on_replay_saved(const char *replay_path)
{
	const int pending = g_pending_clip_seconds.exchange(0);
	if (pending <= 0)
		return false;

	if (!replay_path || replay_path[0] == '\0' || !os_file_exists(replay_path)) {
		obs_log(LOG_WARNING, "[ClipSave] pending %d s but saved path missing", pending);
		return true;
	}

#ifdef _WIN32
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
