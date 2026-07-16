/*
ntchff's overlay for OBS
Copyright (C) 2026 ntchff

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "overlay-smart-replay.h"

#ifdef ENABLE_FRONTEND_API

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>
#include <util/config-file.h>

#ifdef ENABLE_QT
#include <obs-frontend-api.h>
#include <QTimer>
#include <cmath>
#include <cstring>
#include <deque>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "overlay-trimmer.h"
#include "overlay-string-utils.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <cstdio>
#endif

#ifdef ENABLE_QT

static bool g_smart_replay_enabled = true;
static int g_smart_replay_mode = OVERLAY_SMART_REPLAY_LEGACY;
static bool g_replay_restart_pending = false;
static int g_replay_restart_attempts = 0;
static uint64_t g_last_save_request_ns = 0;

struct SaveRequestRecord {
	uint64_t sequence;
	uint64_t request_ns;
	uint64_t prev_request_ns;
};

static std::deque<SaveRequestRecord> g_save_requests;
static uint64_t g_next_save_sequence = 1;

static double ns_to_seconds(uint64_t ns)
{
	return static_cast<double>(ns) / 1'000'000'000.0;
}

static int get_replay_buffer_max_secs(void)
{
	config_t *config = obs_frontend_get_profile_config();
	if (!config)
		return -1;

	const char *output_mode = config_get_string(config, "Output", "Mode");
	int rb_time = -1;
	if (output_mode && strcmp(output_mode, "Advanced") == 0) {
		rb_time = static_cast<int>(config_get_int(config, "AdvOut", "RecRBTime"));
	} else {
		rb_time = static_cast<int>(config_get_int(config, "SimpleOutput", "RecRBTime"));
	}
	return rb_time;
}

#ifdef _WIN32
static uint64_t get_file_size_bytes(const std::wstring &path)
{
	WIN32_FILE_ATTRIBUTE_DATA attrs = {};
	if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attrs)) {
		return 0;
	}
	ULARGE_INTEGER size;
	size.LowPart = attrs.nFileSizeLow;
	size.HighPart = attrs.nFileSizeHigh;
	return size.QuadPart;
}

static void log_playback_range(const char *label, const std::wstring &path)
{
	double start_sec = 0.0;
	double end_sec = 0.0;
	int ref_stream = -1;
	int video_packets = 0;
	double container_duration_sec = -1.0;
	std::wstring err;
	if (!overlay_replay_probe_playback_range(path, start_sec, end_sec, err, &ref_stream, &video_packets,
					       &container_duration_sec)) {
		obs_log(LOG_INFO,
			"[SmartReplay] %s probe failed path=%s error=%s",
			label, overlay::util::WideToUtf8(path).c_str(),
			overlay::util::WideToUtf8(err.empty() ? L"unknown" : err).c_str());
		return;
	}

	const double span_sec = std::max(0.0, end_sec - start_sec);
	obs_log(LOG_INFO,
		"[SmartReplay] %s path=%s size=%llu bytes container_duration=%.3f playback=[%.3f..%.3f] span=%.3f s ref_stream=%d video_packets=%d",
		label, overlay::util::WideToUtf8(path).c_str(),
		static_cast<unsigned long long>(get_file_size_bytes(path)), container_duration_sec, start_sec,
		end_sec, span_sec, ref_stream, video_packets);
}
#endif

static constexpr int kSmartReplayStartDelayMs = 500;
static constexpr int kSmartReplayVerifyDelayMs = 700;
static constexpr int kSmartReplayMaxRestartAttempts = 4;
static constexpr double kMinTrimSeconds = 0.25;

static void reset_save_timestamps(void)
{
	g_last_save_request_ns = 0;
	g_save_requests.clear();
	g_next_save_sequence = 1;
	obs_log(LOG_INFO, "[SmartReplay] timestamp state reset");
}

static void schedule_smart_replay_buffer_start(void);

static void set_smart_replay_enabled(bool enabled)
{
	g_smart_replay_enabled = enabled;
	if (!enabled) {
		g_replay_restart_pending = false;
		g_replay_restart_attempts = 0;
		reset_save_timestamps();
	}
	obs_log(LOG_INFO, "Smart replay %s", enabled ? "enabled" : "disabled");
}

static void set_smart_replay_mode(int mode)
{
	if (mode != OVERLAY_SMART_REPLAY_LEGACY && mode != OVERLAY_SMART_REPLAY_TIMESTAMP_TRIM)
		mode = OVERLAY_SMART_REPLAY_LEGACY;

	if (g_smart_replay_mode != mode) {
		reset_save_timestamps();
		g_replay_restart_pending = false;
		g_replay_restart_attempts = 0;
	}

	g_smart_replay_mode = mode;
	obs_log(LOG_INFO, "Smart replay mode: %s",
		mode == OVERLAY_SMART_REPLAY_TIMESTAMP_TRIM ? "timestamp trim" : "legacy");
}

static void schedule_smart_replay_buffer_start(void)
{
	QTimer::singleShot(kSmartReplayStartDelayMs, []() {
		if (!g_smart_replay_enabled || g_smart_replay_mode != OVERLAY_SMART_REPLAY_LEGACY) {
			g_replay_restart_pending = false;
			g_replay_restart_attempts = 0;
			obs_log(LOG_INFO, "Smart replay: start cancelled (disabled or non-legacy mode)");
			return;
		}

		if (obs_frontend_replay_buffer_active()) {
			obs_log(LOG_INFO, "Smart replay: replay buffer already active");
			g_replay_restart_pending = false;
			g_replay_restart_attempts = 0;
			return;
		}

		++g_replay_restart_attempts;
		obs_log(LOG_INFO, "Smart replay: starting replay buffer (attempt %d/%d)",
			g_replay_restart_attempts, kSmartReplayMaxRestartAttempts);
		obs_frontend_replay_buffer_start();

		QTimer::singleShot(kSmartReplayVerifyDelayMs, []() {
			if (obs_frontend_replay_buffer_active()) {
				obs_log(LOG_INFO, "Smart replay: replay buffer restart confirmed");
				g_replay_restart_pending = false;
				g_replay_restart_attempts = 0;
				return;
			}

			if (!g_replay_restart_pending)
				return;

			if (g_replay_restart_attempts >= kSmartReplayMaxRestartAttempts) {
				obs_log(LOG_WARNING,
					"Smart replay: failed to restart replay buffer after %d attempts",
					g_replay_restart_attempts);
				g_replay_restart_pending = false;
				g_replay_restart_attempts = 0;
				return;
			}

			obs_log(LOG_WARNING, "Smart replay: replay buffer did not start, retrying");
			schedule_smart_replay_buffer_start();
		});
	});
}

static void restart_smart_replay_buffer_after_save(void)
{
	if (!g_smart_replay_enabled || g_smart_replay_mode != OVERLAY_SMART_REPLAY_LEGACY)
		return;

	g_replay_restart_pending = true;
	g_replay_restart_attempts = 0;
	reset_save_timestamps();

	if (obs_frontend_replay_buffer_active()) {
		obs_log(LOG_INFO, "Smart replay: stopping replay buffer for reset after save");
		obs_frontend_replay_buffer_stop();
		return;
	}

	obs_log(LOG_INFO, "Smart replay: replay buffer inactive after save completed");
	schedule_smart_replay_buffer_start();
}

#ifdef _WIN32

static std::mutex g_trim_mutex;

static bool replace_file_with_trimmed(const std::wstring &originalPath, const std::wstring &trimmedPath)
{
	if (!DeleteFileW(originalPath.c_str())) {
		obs_log(LOG_ERROR, "Smart replay trim: failed to delete original file");
		return false;
	}
	if (!MoveFileW(trimmedPath.c_str(), originalPath.c_str())) {
		obs_log(LOG_ERROR, "Smart replay trim: failed to move trimmed file into place");
		return false;
	}
	return true;
}

static void trim_replay_overlap_async(const std::wstring &inputPath, double keepSeconds, uint64_t sequence)
{
	std::thread([inputPath, keepSeconds, sequence]() {
		std::lock_guard<std::mutex> lock(g_trim_mutex);

		const std::string input_utf8 = overlay::util::WideToUtf8(inputPath);
		const uint64_t trim_start_ns = os_gettime_ns();
		obs_log(LOG_INFO,
			"[SmartReplay] trim worker start seq=%llu path=%s keep_target=%.3f s",
			static_cast<unsigned long long>(sequence), input_utf8.c_str(), keepSeconds);

		log_playback_range("pre-trim", inputPath);

		double playback_start_sec = 0.0;
		double playback_end_sec = 0.0;
		std::wstring probe_err;
		if (!overlay_replay_probe_playback_range(inputPath, playback_start_sec, playback_end_sec, probe_err)) {
			obs_log(LOG_WARNING,
				"[SmartReplay] trim worker probe failed seq=%llu path=%s error=%s",
				static_cast<unsigned long long>(sequence), input_utf8.c_str(),
				overlay::util::WideToUtf8(probe_err.empty() ? L"unknown error" : probe_err).c_str());
			return;
		}

		const double file_duration_sec = std::max(0.0, playback_end_sec - playback_start_sec);
		const double trim_start_sec = std::max(playback_start_sec, playback_end_sec - keepSeconds);
		const double trim_remove_sec = std::max(0.0, file_duration_sec - keepSeconds);

		constexpr double kTailEpsilon = 0.05;
		if (keepSeconds + kTailEpsilon >= file_duration_sec) {
			obs_log(LOG_INFO,
				"[SmartReplay] trim worker skip seq=%llu path=%s reason=keep_ge_file keep=%.3f file_span=%.3f playback=[%.3f..%.3f]",
				static_cast<unsigned long long>(sequence), input_utf8.c_str(), keepSeconds,
				file_duration_sec, playback_start_sec, playback_end_sec);
			return;
		}

		const size_t dot = inputPath.find_last_of(L'.');
		std::wstring tempPath = (dot == std::wstring::npos)
						? inputPath + L".smartreplay.tmp"
						: inputPath.substr(0, dot) + L".smartreplay.tmp" + inputPath.substr(dot);

		obs_log(LOG_INFO,
			"[SmartReplay] trim worker plan seq=%llu path=%s playback=[%.3f..%.3f] span=%.3f keep=%.3f trim_start=%.3f remove=%.3f temp=%s",
			static_cast<unsigned long long>(sequence), input_utf8.c_str(), playback_start_sec,
			playback_end_sec, file_duration_sec, keepSeconds, trim_start_sec, trim_remove_sec,
			overlay::util::WideToUtf8(tempPath).c_str());

		std::wstring err;
		const bool ok = overlay_replay_lossless_trim_keep_last(inputPath, tempPath, keepSeconds, err);
		if (!ok) {
			DeleteFileW(tempPath.c_str());
			obs_log(LOG_ERROR,
				"[SmartReplay] trim worker failed seq=%llu path=%s error=%s",
				static_cast<unsigned long long>(sequence), input_utf8.c_str(),
				overlay::util::WideToUtf8(err.empty() ? L"unknown error" : err).c_str());
			return;
		}

		if (!replace_file_with_trimmed(inputPath, tempPath)) {
			DeleteFileW(tempPath.c_str());
			return;
		}

		log_playback_range("post-trim", inputPath);

		double actual_span_sec = 0.0;
		{
			double post_start_sec = 0.0;
			double post_end_sec = 0.0;
			std::wstring post_err;
			if (overlay_replay_probe_playback_range(inputPath, post_start_sec, post_end_sec, post_err)) {
				actual_span_sec = std::max(0.0, post_end_sec - post_start_sec);
			}
		}

		const double worker_elapsed_sec = ns_to_seconds(os_gettime_ns() - trim_start_ns);
		obs_log(LOG_INFO,
			"[SmartReplay] trim worker done seq=%llu path=%s keep_target=%.3f actual=%.3f delta=%+.3f removed_planned=%.3f worker_time=%.3f s",
			static_cast<unsigned long long>(sequence), input_utf8.c_str(), keepSeconds,
			actual_span_sec, actual_span_sec - keepSeconds, trim_remove_sec, worker_elapsed_sec);
	}).detach();
}

#endif // _WIN32

static void handle_timestamp_trim_save_completed(const char *replay_path)
{
	if (!replay_path || replay_path[0] == '\0' || !os_file_exists(replay_path)) {
		obs_log(LOG_WARNING, "[SmartReplay] save completed without a valid file path");
		return;
	}

	const uint64_t complete_ns = os_gettime_ns();
	const int configured_buffer_sec = get_replay_buffer_max_secs();

	if (g_save_requests.empty()) {
		obs_log(LOG_WARNING,
			"[SmartReplay] save completed without request timestamp path=%s complete_ns=%llu configured_buffer_sec=%d queue_depth=0",
			replay_path, static_cast<unsigned long long>(complete_ns), configured_buffer_sec);
		return;
	}

	const SaveRequestRecord record = g_save_requests.front();
	g_save_requests.pop_front();

	const double save_delay_sec =
		record.request_ns > 0 ? ns_to_seconds(complete_ns - record.request_ns) : 0.0;
	const double elapsed_since_prev_sec =
		(record.prev_request_ns > 0 && record.request_ns > record.prev_request_ns)
			? ns_to_seconds(record.request_ns - record.prev_request_ns)
			: 0.0;

	obs_log(LOG_INFO,
		"[SmartReplay] save completed seq=%llu path=%s request_ns=%llu prev_request_ns=%llu complete_ns=%llu save_delay=%.3f s elapsed_since_prev=%.3f s configured_buffer_sec=%d queue_depth=%zu replay_active=%s",
		static_cast<unsigned long long>(record.sequence), replay_path,
		static_cast<unsigned long long>(record.request_ns),
		static_cast<unsigned long long>(record.prev_request_ns),
		static_cast<unsigned long long>(complete_ns), save_delay_sec, elapsed_since_prev_sec,
		configured_buffer_sec, g_save_requests.size(),
		obs_frontend_replay_buffer_active() ? "yes" : "no");

#ifdef _WIN32
	log_playback_range("save-complete", overlay::util::Utf8ToWide(replay_path));
#endif

	if (record.prev_request_ns == 0 || record.request_ns <= record.prev_request_ns) {
		obs_log(LOG_INFO,
			"[SmartReplay] trim skipped seq=%llu reason=first_save path=%s",
			static_cast<unsigned long long>(record.sequence), replay_path);
		return;
	}

	const double elapsed_sec = elapsed_since_prev_sec;

	if (elapsed_sec < kMinTrimSeconds) {
		obs_log(LOG_INFO,
			"[SmartReplay] trim skipped seq=%llu reason=saves_too_close elapsed=%.3f s path=%s",
			static_cast<unsigned long long>(record.sequence), elapsed_sec, replay_path);
		return;
	}

	obs_log(LOG_INFO,
		"[SmartReplay] trim scheduled seq=%llu path=%s keep_target=%.3f s (configured_buffer_sec=%d save_delay=%.3f s)",
		static_cast<unsigned long long>(record.sequence), replay_path, elapsed_sec,
		configured_buffer_sec, save_delay_sec);

#ifdef _WIN32
	trim_replay_overlap_async(overlay::util::Utf8ToWide(replay_path), elapsed_sec, record.sequence);
#else
	UNUSED_PARAMETER(elapsed_sec);
	obs_log(LOG_WARNING, "[SmartReplay] timestamp trim is only supported on Windows");
#endif
}

#endif // ENABLE_QT

extern "C" void overlay_smart_replay_set_enabled(bool enabled)
{
#ifdef ENABLE_QT
	set_smart_replay_enabled(enabled);
#else
	UNUSED_PARAMETER(enabled);
#endif
}

extern "C" void overlay_smart_replay_set_mode(int mode)
{
#ifdef ENABLE_QT
	set_smart_replay_mode(mode);
#else
	UNUSED_PARAMETER(mode);
#endif
}

extern "C" bool overlay_smart_replay_request_save(void)
{
#ifdef ENABLE_QT
	if (!g_smart_replay_enabled || g_smart_replay_mode != OVERLAY_SMART_REPLAY_TIMESTAMP_TRIM)
		return false;

	if (!obs_frontend_replay_buffer_active())
		return false;

	const uint64_t now = os_gettime_ns();
	const uint64_t sequence = g_next_save_sequence++;
	const uint64_t prev_ns = g_last_save_request_ns;
	const double elapsed_since_prev_sec =
		(prev_ns > 0 && now > prev_ns) ? ns_to_seconds(now - prev_ns) : 0.0;

	SaveRequestRecord record = {sequence, now, prev_ns};
	g_save_requests.push_back(record);
	g_last_save_request_ns = now;

	obs_log(LOG_INFO,
		"[SmartReplay] save requested seq=%llu request_ns=%llu prev_request_ns=%llu elapsed_since_prev=%.3f s queue_depth=%zu configured_buffer_sec=%d replay_active=%s trim_target=%.3f s",
		static_cast<unsigned long long>(sequence),
		static_cast<unsigned long long>(now),
		static_cast<unsigned long long>(prev_ns), elapsed_since_prev_sec,
		g_save_requests.size(), get_replay_buffer_max_secs(),
		obs_frontend_replay_buffer_active() ? "yes" : "no", elapsed_since_prev_sec);

	obs_frontend_replay_buffer_save();

	return true;
#else
	return false;
#endif
}

extern "C" void overlay_smart_replay_on_save_completed(const char *replay_path)
{
#ifdef ENABLE_QT
	if (!g_smart_replay_enabled)
		return;

	if (g_smart_replay_mode == OVERLAY_SMART_REPLAY_TIMESTAMP_TRIM) {
		handle_timestamp_trim_save_completed(replay_path);
		return;
	}

	restart_smart_replay_buffer_after_save();
#else
	UNUSED_PARAMETER(replay_path);
#endif
}

extern "C" void overlay_smart_replay_on_replay_buffer_stopped(void)
{
#ifdef ENABLE_QT
	if (g_replay_restart_pending && g_smart_replay_mode == OVERLAY_SMART_REPLAY_LEGACY) {
		obs_log(LOG_INFO,
			"Smart replay: replay buffer stopped, restarting after save completed");
		schedule_smart_replay_buffer_start();
	}
#endif
}

#endif // ENABLE_FRONTEND_API
