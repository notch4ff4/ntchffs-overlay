#ifdef _WIN32

#include "overlay-state.h"
#include "overlay-smart-replay.h"
#include "overlay-ui-task.h"
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs-data.h>
#include <util/config-file.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <shlwapi.h>
#include <comdef.h>
#include <atomic>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

extern "C" {
#include <plugin-support.h>
}

OverlayStateManager::OverlayStateManager() {
}

static std::string TrimRecordingPath(const std::string &path)
{
	const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
	auto begin = std::find_if_not(path.begin(), path.end(), isSpace);
	if (begin == path.end()) {
		return "";
	}
	auto end = std::find_if_not(path.rbegin(), path.rend(), isSpace).base();
	return std::string(begin, end);
}

static bool IsRecordingPathNonEmpty(const char *path)
{
	return path && TrimRecordingPath(path).empty() == false;
}

static std::atomic<bool> s_replaySaving(false);

void OverlayStateManager::SetReplaySaving(bool saving) {
	s_replaySaving.store(saving);
}

bool OverlayStateManager::GetReplaySaving() {
	return s_replaySaving.load();
}

void OverlayStateManager::UpdateRecordingStatus(OverlayState &state) {
	try {
		state.recordingActive = obs_frontend_recording_active();
	} catch (...) {
		// Frontend APIs may be unavailable during early OBS startup.
	}
}

void OverlayStateManager::UpdateReplayStatus(OverlayState &state) {
	try {
		state.replayActive = obs_frontend_replay_buffer_active();
	} catch (...) {
		// Frontend APIs may be unavailable during early OBS startup.
	}
	state.replaySaving = GetReplaySaving();
}

void OverlayStateManager::UpdateStats(OverlayState &state) {
	if (!state.statsVisible) {
		return;
	}

	try {
		state.fps = obs_get_active_fps();

		obs_output_t *output = nullptr;
		if (obs_frontend_streaming_active()) {
			output = obs_frontend_get_streaming_output();
		} else if (obs_frontend_recording_active()) {
			output = obs_frontend_get_recording_output();
		}

		if (output) {
			state.droppedFrames = obs_output_get_frames_dropped(output);
			obs_output_release(output);
		} else {
			state.droppedFrames = 0;
		}

		std::string configuredPath = GetConfiguredRecordingPath();
		std::string recordingPath = configuredPath.empty() ? GetRecordingPath() : configuredPath;
		if (!recordingPath.empty()) {
			state.freeSpaceText = GetFreeSpaceText(recordingPath);
		} else {
			state.freeSpaceText = obs_module_text("Stats.FreeSpaceUnknown");
		}
	} catch (...) {
		state.fps = 0.0;
		state.droppedFrames = 0;
		state.freeSpaceText = obs_module_text("Stats.FreeSpaceUnknown");
	}
}

void OverlayStateManager::UpdateStatus(OverlayState &state) {
	UpdateRecordingStatus(state);
	UpdateReplayStatus(state);
	UpdateStats(state);
}

static void toggle_recording_ui_task(void *param)
{
	UNUSED_PARAMETER(param);

	try {
		const bool recording_active = obs_frontend_recording_active();
		if (recording_active) {
			obs_frontend_recording_stop();
		} else {
			obs_frontend_recording_start();
		}
	} catch (...) {
		obs_log(LOG_ERROR, "Failed to toggle recording");
	}
}

static void toggle_replay_buffer_ui_task(void *param)
{
	UNUSED_PARAMETER(param);

	try {
		const bool replay_active = obs_frontend_replay_buffer_active();
		if (replay_active) {
			obs_frontend_replay_buffer_stop();
		} else {
			obs_frontend_replay_buffer_start();
		}
	} catch (...) {
		obs_log(LOG_ERROR, "Failed to toggle replay buffer");
	}
}

static void save_replay_ui_task(void *param)
{
	UNUSED_PARAMETER(param);

	try {
		if (!obs_frontend_replay_buffer_active()) {
			return;
		}

		if (!overlay_smart_replay_request_save()) {
			obs_frontend_replay_buffer_save();
		}
		OverlayStateManager::SetReplaySaving(true);
	} catch (...) {
		obs_log(LOG_ERROR, "Failed to save replay buffer");
	}
}

void OverlayStateManager::ToggleRecording() {
	overlay_run_on_ui_thread(toggle_recording_ui_task, nullptr);
}

void OverlayStateManager::ToggleReplayBuffer() {
	overlay_run_on_ui_thread(toggle_replay_buffer_ui_task, nullptr);
}

void OverlayStateManager::SaveReplay() {
	overlay_run_on_ui_thread(save_replay_ui_task, nullptr);
}

std::string OverlayStateManager::GetConfiguredRecordingPath() {
	std::string recordingPath;

	try {
		config_t *config = obs_frontend_get_profile_config();
		if (config) {
			const char *outputMode = config_get_string(config, "Output", "Mode");

			if (outputMode && strcmp(outputMode, "Advanced") == 0) {
				const char *recType = config_get_string(config, "AdvOut", "RecType");
				const char *path = nullptr;
				if (recType && strcmp(recType, "FFmpeg") == 0) {
					path = config_get_string(config, "AdvOut", "FFFilePath");
				} else {
					path = config_get_string(config, "AdvOut", "RecFilePath");
				}
				if (IsRecordingPathNonEmpty(path)) {
					recordingPath = TrimRecordingPath(path);
				}
			} else {
				const char *simplePath = config_get_string(config, "SimpleOutput", "FilePath");
				if (IsRecordingPathNonEmpty(simplePath)) {
					recordingPath = TrimRecordingPath(simplePath);
				}
			}
		}
	} catch (...) {
	}

	return recordingPath;
}

bool OverlayStateManager::HasRecordingPath()
{
	return !GetConfiguredRecordingPath().empty();
}

std::string OverlayStateManager::GetRecordingPath() {
	std::string recordingPath = GetConfiguredRecordingPath();

	try {
		// Fall back to Videos or profile folder so free-space stats still have a drive to query.
		if (recordingPath.empty()) {
			PWSTR videosPath = nullptr;
			if (SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_DEFAULT, NULL, &videosPath) == S_OK) {
				char pathA[MAX_PATH];
				WideCharToMultiByte(CP_UTF8, 0, videosPath, -1, pathA, MAX_PATH, NULL, NULL);
				recordingPath = pathA;
				CoTaskMemFree(videosPath);
			} else {
				PWSTR homePath = nullptr;
				if (SHGetKnownFolderPath(FOLDERID_Profile, KF_FLAG_DEFAULT, NULL, &homePath) == S_OK) {
					char pathA[MAX_PATH];
					WideCharToMultiByte(CP_UTF8, 0, homePath, -1, pathA, MAX_PATH, NULL, NULL);
					recordingPath = pathA;
					CoTaskMemFree(homePath);
				}
			}
		}
	} catch (...) {
	}

	return recordingPath;
}

std::string OverlayStateManager::GetFreeSpaceText(const std::string &path) {
	if (path.empty()) {
		return obs_module_text("Stats.FreeSpaceUnknown");
	}

	try {
		ULARGE_INTEGER freeBytes;
		ULARGE_INTEGER totalBytes;

		WCHAR widePath[MAX_PATH];
		MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, widePath, MAX_PATH);

		if (GetDiskFreeSpaceExW(widePath, &freeBytes, &totalBytes, NULL)) {
			double freeGB = freeBytes.QuadPart / (1024.0 * 1024.0 * 1024.0);

			std::ostringstream oss;
			oss << std::fixed << std::setprecision(1) << freeGB;
			return std::string(obs_module_text("Stats.FreeSpacePrefix")) + " " + oss.str() + " " +
			       obs_module_text("Stats.GB");
		}
	} catch (...) {
	}

	return obs_module_text("Stats.FreeSpaceUnknown");
}

#endif // _WIN32
