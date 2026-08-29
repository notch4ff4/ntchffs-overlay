/*
ntchff's overlay for OBS
Copyright (C) 2026 ntchff

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>
#include <string>
#include <cstring>
#include "overlay-settings-storage.h"

#ifdef ENABLE_FRONTEND_API
#include <obs-frontend-api.h>
#include <obs-data.h>
#endif

#ifdef ENABLE_QT
#include "overlay-window-c.h"
#include "overlay-indicators-c.h"
#include "overlay-toasts-c.h"
#include "overlay-runtime-apply.h"
#include "overlay-smart-replay.h"
#include "overlay-clip-save.h"
#include "overlay-state.h"
#include <QAction>
#include <QInputDialog>
#include <QMainWindow>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <util/dstr.h>
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

#ifdef ENABLE_FRONTEND_API

// --- Hotkey IDs ---
static obs_hotkey_id toggle_overlay_hotkey = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id save_replay_hotkey = OBS_INVALID_HOTKEY_ID;

// Staged during preload so bindings can be applied after registration completes.
static obs_data_t *saved_hotkey_data = NULL;
static bool hotkeys_loaded = false;

#ifdef ENABLE_QT
static void *overlay_window = NULL;
static void *overlay_indicators = NULL;
static void *overlay_toasts = NULL;

static std::string FormatToggleOverlayHotkeyLabel()
{
	if (toggle_overlay_hotkey == OBS_INVALID_HOTKEY_ID)
		return "?";

	obs_data_array_t *bindings = obs_hotkey_save(toggle_overlay_hotkey);
	if (!bindings || obs_data_array_count(bindings) == 0) {
		if (bindings)
			obs_data_array_release(bindings);
		return "?";
	}

	obs_data_t *binding = obs_data_array_item(bindings, 0);
	const char *keyName = binding ? obs_data_get_string(binding, "key") : nullptr;
	const bool shift = binding && obs_data_get_bool(binding, "shift");
	const bool control = binding && obs_data_get_bool(binding, "control");
	const bool alt = binding && obs_data_get_bool(binding, "alt");

	std::string label;
	if (control)
		label += "Ctrl+";
	if (alt)
		label += "Alt+";
	if (shift)
		label += "Shift+";

	if (keyName && keyName[0]) {
		obs_key_t key = obs_key_from_name(keyName);
		struct dstr keyStr = {0};
		obs_key_to_str(key, &keyStr);
		if (keyStr.array && keyStr.array[0]) {
			label += keyStr.array;
		} else {
			const char *p = keyName;
			if (strncmp(p, "OBS_KEY_", 8) == 0)
				p += 8;
			label += p;
		}
		dstr_free(&keyStr);
	} else {
		label += "?";
	}

	if (binding)
		obs_data_release(binding);
	obs_data_array_release(bindings);
	return label;
}

static void MaybeShowOpenOverlayHint()
{
	if (!overlay_toasts)
		return;

	// Defer so OBS hotkey bindings are fully applied and the toast window is ready.
	QTimer::singleShot(1500, []() {
		if (!overlay_toasts)
			return;

		const bool enabled =
			!saved_settings_data || !obs_data_has_user_value(saved_settings_data, "toast_open_hint") ||
			obs_data_get_bool(saved_settings_data, "toast_open_hint");
		const bool masterEnabled =
			!saved_settings_data || !obs_data_has_user_value(saved_settings_data, "toasts_enabled") ||
			obs_data_get_bool(saved_settings_data, "toasts_enabled");
		if (!enabled || !masterEnabled)
			return;

		const std::string label = FormatToggleOverlayHotkeyLabel();
		overlay_toasts_show_open_hint(overlay_toasts, label.c_str());
		obs_log(LOG_INFO, "Overlay open-hint toast shown (hotkey: %s)", label.c_str());
	});
}

static void handle_replay_saved_event(void)
{
	OverlayStateManager::SetReplaySaving(false);

	const char *replay_path = obs_frontend_get_last_replay();
	if (replay_path && replay_path[0] != '\0' && os_file_exists(replay_path)) {
		obs_log(LOG_INFO, "Replay saved: %s", replay_path);
		if (overlay_indicators)
			overlay_indicators_notify_replay_saved(overlay_indicators);
		if (overlay_toasts)
			overlay_toasts_show_replay_saved(overlay_toasts, replay_path);
	} else {
		obs_log(LOG_WARNING,
			"Replay buffer saved event without a valid file path");
	}

	if (overlay_clip_save_on_replay_saved(replay_path))
		overlay_smart_replay_acknowledge_save(replay_path);
	else
		overlay_smart_replay_on_save_completed(replay_path);
}

static void handle_recording_stopped_event(void)
{
	const char *recording_path = obs_frontend_get_last_recording();
	if (recording_path && recording_path[0] != '\0' && os_file_exists(recording_path)) {
		obs_log(LOG_INFO, "Recording saved: %s", recording_path);
		if (overlay_toasts)
			overlay_toasts_show_replay_saved(overlay_toasts, recording_path);
	} else {
		obs_log(LOG_WARNING, "Recording stopped without a valid file path");
	}
}

static void frontend_event_callback(enum obs_frontend_event event, void *priv_data);

#endif

// --- Hotkey callbacks ---
static void toggle_overlay_callback(void *data, obs_hotkey_id id,
				    obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed)
		return;

#ifdef ENABLE_QT
	if (overlay_window) {
		overlay_window_toggle_visible(overlay_window);
		obs_log(LOG_INFO, "Overlay toggled");
	}
#endif
}

static void save_replay_hotkey_ui_task(void *param)
{
	UNUSED_PARAMETER(param);

	if (!obs_frontend_replay_buffer_active()) {
		obs_log(LOG_WARNING, "Save replay hotkey: replay buffer not active");
		return;
	}
	if (OverlayStateManager::GetReplaySaving()) {
		obs_log(LOG_INFO, "Save replay hotkey: save already in progress");
		return;
	}

	obs_log(LOG_INFO, "Save replay hotkey: saving replay buffer");
	if (!overlay_smart_replay_request_save()) {
		obs_frontend_replay_buffer_save();
	}
	OverlayStateManager::SetReplaySaving(true);
}

static void save_replay_callback(void *data, obs_hotkey_id id,
				 obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed)
		return;

	obs_queue_task(OBS_TASK_UI, save_replay_hotkey_ui_task, nullptr, false);
}

// --- Hotkey registration ---
static void register_hotkeys(void)
{
	// Plugin-prefixed IDs keep bindings stable across OBS restarts.
	toggle_overlay_hotkey = obs_hotkey_register_frontend(
		"obs-plugin-overlay.toggle_overlay", obs_module_text("ToggleOverlay"),
		toggle_overlay_callback, NULL);

	save_replay_hotkey = obs_hotkey_register_frontend(
		"obs-plugin-overlay.save_replay", obs_module_text("SaveReplay"),
		save_replay_callback, NULL);

	obs_log(LOG_INFO, "Hotkey registered");
}


// --- Hotkey unregistration ---
static void unregister_hotkeys(void)
{
	if (toggle_overlay_hotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(toggle_overlay_hotkey);
	if (save_replay_hotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(save_replay_hotkey);

	obs_log(LOG_INFO, "Hotkey unregistered");
}

// --- Hotkey persistence ---
static void save_hotkeys(obs_data_t *save_data, bool saving, void *private_data)
{
	UNUSED_PARAMETER(saving);
	UNUSED_PARAMETER(private_data);

	if (!save_data)
		return;

	obs_data_t *hotkeys = obs_data_create();

	if (toggle_overlay_hotkey != OBS_INVALID_HOTKEY_ID) {
		obs_data_array_t *array = obs_hotkey_save(toggle_overlay_hotkey);
		if (array) {
			obs_data_set_array(hotkeys, "toggle_overlay", array);
			obs_data_array_release(array);
		}
	}
	if (save_replay_hotkey != OBS_INVALID_HOTKEY_ID) {
		obs_data_array_t *array = obs_hotkey_save(save_replay_hotkey);
		if (array) {
			obs_data_set_array(hotkeys, "save_replay", array);
			obs_data_array_release(array);
		}
	}

	// Plugin-scoped key avoids collisions with other plugins' hotkey storage.
	char key[256];
	snprintf(key, sizeof(key), "%s.hotkeys", PLUGIN_NAME);
	obs_data_set_obj(save_data, key, hotkeys);
	obs_data_release(hotkeys);

	obs_log(LOG_INFO, "Hotkeys saved to configuration with key: %s", key);
}

static void preload_hotkeys(obs_data_t *save_data, bool saving, void *private_data)
{
	UNUSED_PARAMETER(saving);
	UNUSED_PARAMETER(private_data);

	if (!save_data)
		return;

	// Clear stale preload data when OBS reloads the profile.
	if (saved_hotkey_data) {
		obs_data_release(saved_hotkey_data);
		saved_hotkey_data = NULL;
	}

	// Copy hotkey arrays from profile data for load_hotkeys_from_saved_data().
	char key[256];
	snprintf(key, sizeof(key), "%s.hotkeys", PLUGIN_NAME);
	obs_data_t *hotkeys = obs_data_get_obj(save_data, key);
	if (hotkeys) {
		saved_hotkey_data = obs_data_create();
		obs_data_apply(saved_hotkey_data, hotkeys);
		obs_data_release(hotkeys);
		obs_log(LOG_INFO, "Hotkey data preloaded from configuration with key: %s", key);
	} else {
		obs_log(LOG_INFO, "No saved hotkey data found with key: %s", key);
	}
}

static void load_hotkeys_from_saved_data(void)
{
	if (!saved_hotkey_data || hotkeys_loaded)
		return;

	obs_data_t *hotkeys = saved_hotkey_data;

	obs_data_array_t *array = obs_data_get_array(hotkeys, "toggle_overlay");
	if (array && toggle_overlay_hotkey != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_load(toggle_overlay_hotkey, array);
		obs_data_array_release(array);
	}
	array = obs_data_get_array(hotkeys, "save_replay");
	if (array && save_replay_hotkey != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_load(save_replay_hotkey, array);
		obs_data_array_release(array);
	}

	hotkeys_loaded = true;
	obs_log(LOG_INFO, "Hotkeys loaded from saved configuration");
}

#endif

#ifdef ENABLE_QT
// --- Runtime apply bridge (used by the in-overlay settings panel) ---
extern "C" void overlay_runtime_set_indicators(bool enabled, int position, bool oledProtection, int scalePercent)
{
	if (!overlay_indicators)
		return;
	overlay_indicators_set_enabled(overlay_indicators, enabled);
	overlay_indicators_set_position(overlay_indicators, position);
	overlay_indicators_set_oled_protection(overlay_indicators, oledProtection);
	overlay_indicators_set_scale(overlay_indicators, scalePercent);
}

extern "C" void overlay_runtime_set_indicator_custom(int slot, const char *pathUtf8, bool tintEnabled, int colorRgb)
{
	if (!overlay_indicators)
		return;
	overlay_indicators_set_custom(overlay_indicators, slot, pathUtf8, tintEnabled, colorRgb);
}

extern "C" void overlay_runtime_set_indicator_sound(int eventIndex, const char *pathUtf8)
{
	if (!overlay_indicators)
		return;
	overlay_indicators_set_sound(overlay_indicators, eventIndex, pathUtf8);
}

extern "C" void overlay_runtime_set_smart_replay(bool enabled)
{
	overlay_smart_replay_set_enabled(enabled);
}

extern "C" void overlay_runtime_set_smart_replay_mode(int mode)
{
	overlay_smart_replay_set_mode(mode);
}

extern "C" void overlay_runtime_set_smart_replay_clip_ignore_trim(bool ignore)
{
	overlay_smart_replay_set_clip_ignore_trim(ignore);
}

extern "C" void overlay_runtime_set_toast_action_hotkeys(int viewVk, int viewMod, int deleteVk, int deleteMod)
{
	if (!overlay_toasts)
		return;
	overlay_toasts_set_action_hotkeys(overlay_toasts, viewVk, viewMod, deleteVk, deleteMod);
}

extern "C" void overlay_runtime_set_toast_hotkeys_always_active(bool always)
{
	if (!overlay_toasts)
		return;
	overlay_toasts_set_hotkeys_always_active(overlay_toasts, always);
}

extern "C" void overlay_runtime_set_toast_accent_color(int colorRgb)
{
	if (!overlay_toasts)
		return;
	overlay_toasts_set_accent_color(overlay_toasts, colorRgb);
}

extern "C" void overlay_runtime_set_toast_options(bool enabled, bool videoSaved, bool openHint, bool lowDisk,
						  int lowDiskThresholdGb, int durationSec)
{
	if (!overlay_toasts)
		return;
	overlay_toasts_set_options(overlay_toasts, enabled, videoSaved, openHint, lowDisk, lowDiskThresholdGb,
				   durationSec);
}

static void ToastViewInOverlayHandler(const char *pathUtf8, void *userData)
{
	UNUSED_PARAMETER(userData);
	if (!pathUtf8 || !pathUtf8[0] || !overlay_window)
		return;
	overlay_window_open_gallery_file(overlay_window, pathUtf8);
}

static void ToastReleaseFileHandler(const char *pathUtf8, void *userData)
{
	UNUSED_PARAMETER(userData);
	if (!pathUtf8 || !pathUtf8[0] || !overlay_window)
		return;
	overlay_window_release_gallery_file(overlay_window, pathUtf8);
}

static void ToastFileDeletedHandler(const char *pathUtf8, void *userData)
{
	UNUSED_PARAMETER(userData);
	if (!pathUtf8 || !pathUtf8[0] || !overlay_window)
		return;
	overlay_window_notify_gallery_file_deleted(overlay_window, pathUtf8);
}

static void ApplyToastHotkeysFromSettings()
{
	if (!overlay_toasts)
		return;

	int viewVk = 0x76; // VK_F7
	int viewMod = 2;
	int deleteVk = 0x77; // VK_F8
	int deleteMod = 2;
	if (saved_settings_data) {
		if (obs_data_has_user_value(saved_settings_data, "hotkey_toast_view_vk"))
			viewVk = (int)obs_data_get_int(saved_settings_data, "hotkey_toast_view_vk");
		if (obs_data_has_user_value(saved_settings_data, "hotkey_toast_view_mod"))
			viewMod = (int)obs_data_get_int(saved_settings_data, "hotkey_toast_view_mod");
		if (obs_data_has_user_value(saved_settings_data, "hotkey_toast_delete_vk"))
			deleteVk = (int)obs_data_get_int(saved_settings_data, "hotkey_toast_delete_vk");
		if (obs_data_has_user_value(saved_settings_data, "hotkey_toast_delete_mod"))
			deleteMod = (int)obs_data_get_int(saved_settings_data, "hotkey_toast_delete_mod");
	}
	if (viewVk <= 0)
		viewVk = 0x76;
	if (deleteVk <= 0)
		deleteVk = 0x77;
	overlay_toasts_set_action_hotkeys(overlay_toasts, viewVk, viewMod, deleteVk, deleteMod);
}

static void ApplyToastAccentFromSettings()
{
	if (!overlay_toasts)
		return;
	int accent = 0xA882FF;
	if (saved_settings_data && obs_data_has_user_value(saved_settings_data, "overlay_accent_color"))
		accent = (int)obs_data_get_int(saved_settings_data, "overlay_accent_color") & 0xFFFFFF;
	overlay_toasts_set_accent_color(overlay_toasts, accent);
}

static bool g_obs_exiting = false;
static bool g_remember_replay_snapshotted = false;

static bool settings_get_bool(const char *key, bool def)
{
	if (!saved_settings_data || !obs_data_has_user_value(saved_settings_data, key))
		return def;
	return obs_data_get_bool(saved_settings_data, key);
}

static void remember_replay_buffer_set_was_active(bool active, bool persist)
{
	if (!settings_get_bool("remember_replay_buffer", false))
		return;
	overlay_settings_ensure();
	if (!saved_settings_data)
		return;
	obs_data_set_bool(saved_settings_data, "replay_buffer_was_active", active);
	if (persist)
		overlay_settings_save();
}

static void remember_replay_buffer_snapshot_for_exit(void)
{
	if (g_remember_replay_snapshotted)
		return;
	g_obs_exiting = true;
	g_remember_replay_snapshotted = true;
	if (!settings_get_bool("remember_replay_buffer", false))
		return;
	const bool active = obs_frontend_replay_buffer_active();
	obs_log(LOG_INFO, "Remember Replay Buffer: exit snapshot active=%s", active ? "yes" : "no");
	remember_replay_buffer_set_was_active(active, true);
}

static void maybe_restore_replay_buffer_state(void)
{
	if (!settings_get_bool("remember_replay_buffer", false))
		return;
	if (!settings_get_bool("replay_buffer_was_active", false)) {
		obs_log(LOG_INFO, "Remember Replay Buffer: last state was inactive, skip start");
		return;
	}
	if (obs_frontend_replay_buffer_active()) {
		obs_log(LOG_INFO, "Remember Replay Buffer: already active, skip start");
		return;
	}
	obs_log(LOG_INFO, "Remember Replay Buffer: starting once to restore last state");
	obs_frontend_replay_buffer_start();
}

extern "C" void overlay_runtime_set_remember_replay_buffer(bool enabled)
{
	overlay_settings_ensure();
	if (!saved_settings_data)
		return;
	obs_data_set_bool(saved_settings_data, "remember_replay_buffer", enabled);
	if (enabled) {
		const bool active = obs_frontend_replay_buffer_active();
		obs_data_set_bool(saved_settings_data, "replay_buffer_was_active", active);
		obs_log(LOG_INFO, "Remember Replay Buffer: enabled, sync was_active=%s",
			active ? "yes" : "no");
	}
	overlay_settings_save();
}

static void ApplyToastOptionsFromSettings()
{
	if (!overlay_toasts)
		return;

	auto getBool = [](const char *key, bool def) { return settings_get_bool(key, def); };
	int thresholdGb = 5;
	if (saved_settings_data && obs_data_has_user_value(saved_settings_data, "toast_low_disk_gb")) {
		thresholdGb = (int)obs_data_get_int(saved_settings_data, "toast_low_disk_gb");
		if (thresholdGb < 1)
			thresholdGb = 1;
		if (thresholdGb > 100)
			thresholdGb = 100;
	}
	int durationSec = 8;
	if (saved_settings_data && obs_data_has_user_value(saved_settings_data, "toast_duration_sec")) {
		durationSec = (int)obs_data_get_int(saved_settings_data, "toast_duration_sec");
		if (durationSec < 2)
			durationSec = 2;
		if (durationSec > 60)
			durationSec = 60;
	}

	overlay_toasts_set_options(overlay_toasts, getBool("toasts_enabled", true),
				   getBool("toast_video_saved", true), getBool("toast_open_hint", true),
				   getBool("toast_low_disk", true), thresholdGb, durationSec);

	bool hotkeysAlways = false;
	if (saved_settings_data && obs_data_has_user_value(saved_settings_data, "toast_hotkeys_scope"))
		hotkeysAlways = obs_data_get_int(saved_settings_data, "toast_hotkeys_scope") == 1;
	else
		hotkeysAlways = getBool("toast_hotkeys_always", false);
	overlay_toasts_set_hotkeys_always_active(overlay_toasts, hotkeysAlways);
}

static void frontend_event_callback(enum obs_frontend_event event, void *priv_data)
{
	UNUSED_PARAMETER(priv_data);

	obs_log(LOG_INFO, "Frontend event received: %d", (int)event);

	// Overlay and hotkeys must wait until OBS frontend init is complete.
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		obs_log(LOG_INFO, "OBS finished loading, creating overlay window...");

		load_hotkeys_from_saved_data();

		if (!overlay_window) {
			obs_log(LOG_INFO, "Attempting to create overlay window...");
			overlay_window = overlay_window_create();
			if (overlay_window) {
				obs_log(LOG_INFO, "Overlay window created successfully");
				// Visibility stays user-controlled via hotkey rather than auto-showing at load.
				obs_log(LOG_INFO, "Overlay window created but not shown. Use hotkey to toggle visibility.");
			} else {
				obs_log(LOG_ERROR,
					"Failed to create overlay window - check Qt initialization");
			}
		} else {
			obs_log(LOG_INFO, "Overlay window already exists");
		}

		if (!overlay_indicators) {
			obs_log(LOG_INFO, "Attempting to create overlay indicators...");
			overlay_indicators = overlay_indicators_create();
			if (overlay_indicators) {
				obs_log(LOG_INFO, "Overlay indicators created successfully");
				if (saved_settings_data) {
					bool indicators_enabled =
						obs_data_has_user_value(saved_settings_data, "indicators_enabled")
							? obs_data_get_bool(saved_settings_data, "indicators_enabled")
							: true;
					int indicators_position = obs_data_get_int(saved_settings_data, "indicators_position");
					if (indicators_position < 0 || indicators_position > 8)
						indicators_position = 5;
					bool indicators_oled_protection =
						obs_data_has_user_value(saved_settings_data,
									"indicators_oled_protection")
							? obs_data_get_bool(saved_settings_data,
									    "indicators_oled_protection")
							: false;
					int indicators_scale =
						obs_data_has_user_value(saved_settings_data, "indicators_scale")
							? (int)obs_data_get_int(saved_settings_data, "indicators_scale")
							: 100;
					if (indicators_scale < 50 || indicators_scale > 300)
						indicators_scale = 100;
					overlay_indicators_set_enabled(overlay_indicators, indicators_enabled);
					overlay_indicators_set_position(overlay_indicators, indicators_position);
					overlay_indicators_set_oled_protection(overlay_indicators,
									       indicators_oled_protection);
					overlay_indicators_set_scale(overlay_indicators, indicators_scale);
					static const char *kSlots[] = {"recording", "replay", "saving", "saved"};
					for (int i = 0; i < 4; i++) {
						std::string iconKey = std::string("indicators_") + kSlots[i] + "_icon";
						std::string tintKey = std::string("indicators_") + kSlots[i] + "_tint";
						std::string colorKey =
							std::string("indicators_") + kSlots[i] + "_color";
						const char *path = obs_data_has_user_value(saved_settings_data,
											  iconKey.c_str())
									   ? obs_data_get_string(saved_settings_data,
												 iconKey.c_str())
									   : "";
						bool tint =
							obs_data_has_user_value(saved_settings_data, tintKey.c_str())
								? obs_data_get_bool(saved_settings_data,
										    tintKey.c_str())
								: false;
						int color =
							obs_data_has_user_value(saved_settings_data, colorKey.c_str())
								? (int)obs_data_get_int(saved_settings_data,
											colorKey.c_str())
								: 0xFFFFFF;
						overlay_indicators_set_custom(overlay_indicators, i, path, tint, color);
					}
					static const char *kSoundKeys[] = {
						"recording_on", "recording_off", "replay_on", "replay_off",
						"replay_saved"};
					for (int i = 0; i < 5; i++) {
						std::string key =
							std::string("indicators_sound_") + kSoundKeys[i];
						const char *spath =
							obs_data_has_user_value(saved_settings_data, key.c_str())
								? obs_data_get_string(saved_settings_data, key.c_str())
								: "";
						overlay_indicators_set_sound(overlay_indicators, i, spath);
					}
				} else {
					overlay_indicators_set_enabled(overlay_indicators, true);
					overlay_indicators_set_position(overlay_indicators, 5);
				}
			} else {
				obs_log(LOG_ERROR, "Failed to create overlay indicators");
			}
		}

		if (!overlay_toasts) {
			obs_log(LOG_INFO, "Attempting to create overlay toasts...");
			overlay_toasts = overlay_toasts_create();
			if (overlay_toasts) {
				obs_log(LOG_INFO, "Overlay toasts created successfully");
				ApplyToastHotkeysFromSettings();
				ApplyToastAccentFromSettings();
				ApplyToastOptionsFromSettings();
				overlay_toasts_set_view_handler(overlay_toasts, ToastViewInOverlayHandler, nullptr);
				overlay_toasts_set_delete_handlers(overlay_toasts, ToastReleaseFileHandler,
								   ToastFileDeletedHandler, nullptr);
				MaybeShowOpenOverlayHint();
				overlay_toasts_tick_disk_space(overlay_toasts);
			} else {
				obs_log(LOG_ERROR, "Failed to create overlay toasts");
			}
		}

		maybe_restore_replay_buffer_state();
	} else if (event == OBS_FRONTEND_EVENT_EXIT ||
		   event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN) {
		// Snapshot before outputs are torn down; after EXIT frontend API must not be used.
		remember_replay_buffer_snapshot_for_exit();
	} else if (event == OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED) {
		handle_replay_saved_event();
	} else if (event == OBS_FRONTEND_EVENT_RECORDING_STOPPED) {
		handle_recording_stopped_event();
	} else if (event == OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED) {
		if (!g_obs_exiting)
			remember_replay_buffer_set_was_active(true, true);
	} else if (event == OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPED) {
		// Shutdown stops the buffer after our EXIT snapshot — do not overwrite it.
		if (!g_obs_exiting && !overlay_smart_replay_is_restart_pending())
			remember_replay_buffer_set_was_active(false, true);
		overlay_smart_replay_on_replay_buffer_stopped();
	}
}
#endif

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)",
		PLUGIN_VERSION);

#ifdef ENABLE_FRONTEND_API
	overlay_settings_load();

#ifdef ENABLE_QT
	const bool hasSmartReplay =
		saved_settings_data &&
		obs_data_has_user_value(saved_settings_data, "smart_replay");
	overlay_smart_replay_set_enabled(!saved_settings_data || !hasSmartReplay
		? true
		: obs_data_get_bool(saved_settings_data, "smart_replay"));

	int smartReplayMode = OVERLAY_SMART_REPLAY_TIMESTAMP_TRIM;
	if (saved_settings_data && obs_data_has_user_value(saved_settings_data, "smart_replay_mode")) {
		smartReplayMode = static_cast<int>(obs_data_get_int(saved_settings_data, "smart_replay_mode"));
	}
	overlay_smart_replay_set_mode(smartReplayMode);

	const bool clipIgnoreTrim =
		!saved_settings_data ||
			!obs_data_has_user_value(saved_settings_data, "smart_replay_clip_ignore_trim")
			? true
			: obs_data_get_bool(saved_settings_data, "smart_replay_clip_ignore_trim");
	overlay_smart_replay_set_clip_ignore_trim(clipIgnoreTrim);
#endif

	// Frontend callbacks persist hotkey bindings in the OBS profile.
	obs_frontend_add_preload_callback(preload_hotkeys, NULL);
	obs_frontend_add_save_callback(save_hotkeys, NULL);
	obs_log(LOG_INFO, "Registered hotkey save/preload callbacks");

	register_hotkeys();

	// Binding load is deferred until FINISHED_LOADING because hotkey APIs need a live frontend.

#ifdef ENABLE_QT
	// Window creation is deferred to FINISHED_LOADING because early init can crash OBS during startup.
	obs_frontend_add_event_callback(frontend_event_callback, NULL);
	obs_log(LOG_INFO, "Registered frontend event callback for overlay creation");
#endif
#endif

	return true;
}

void obs_module_unload(void)
{
#ifdef ENABLE_FRONTEND_API
	obs_frontend_remove_save_callback(save_hotkeys, NULL);
	obs_frontend_remove_preload_callback(preload_hotkeys, NULL);

#ifdef ENABLE_QT
	obs_frontend_remove_event_callback(frontend_event_callback, NULL);
#endif

	// Leave hotkeys registered so OBS can persist them on shutdown; unregistering breaks save.
	// unregister_hotkeys();

	if (saved_hotkey_data) {
		obs_data_release(saved_hotkey_data);
		saved_hotkey_data = NULL;
	}

	hotkeys_loaded = false;

	// Do not query replay buffer here: after EXIT the frontend API is invalid, and
	// shutdown STOPPED would otherwise force was_active=false. Snapshot is done on EXIT.

	overlay_settings_save();

	if (saved_settings_data) {
		obs_data_release(saved_settings_data);
		saved_settings_data = NULL;
	}
#endif

#ifdef ENABLE_QT
	// Null globals first so deferred QTimer callbacks skip instead of touching freed objects.
	void *ow = overlay_window;
	void *oi = overlay_indicators;
	void *ot = overlay_toasts;
	overlay_window = NULL;
	overlay_indicators = NULL;
	overlay_toasts = NULL;

	if (ow) {
		overlay_window_destroy(ow);
		obs_log(LOG_INFO, "Overlay window destroyed");
	}
	if (oi) {
		overlay_indicators_destroy(oi);
		obs_log(LOG_INFO, "Overlay indicators destroyed");
	}
	if (ot) {
		overlay_toasts_destroy(ot);
		obs_log(LOG_INFO, "Overlay toasts destroyed");
	}
#endif

	obs_log(LOG_INFO, "plugin unloaded");
}
