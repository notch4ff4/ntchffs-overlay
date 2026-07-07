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
#include "overlay-settings-storage.h"

#ifdef ENABLE_FRONTEND_API
#include <obs-frontend-api.h>
#include <obs-data.h>
#endif

#ifdef ENABLE_QT
#include "overlay-window-c.h"
#include "overlay-indicators-c.h"
#include "overlay-runtime-apply.h"
#include "overlay-state.h"
#include <QAction>
#include <QInputDialog>
#include <QMainWindow>
#include <QObject>
#include <QPointer>
#include <QTimer>
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
static bool replay_restart_pending = false;
static int replay_restart_attempts = 0;
static bool smart_replay_enabled = true;
static constexpr int kSmartReplayStartDelayMs = 500;
static constexpr int kSmartReplayVerifyDelayMs = 700;
static constexpr int kSmartReplayMaxRestartAttempts = 4;

static void set_smart_replay_enabled(bool enabled)
{
	smart_replay_enabled = enabled;
	if (!enabled) {
		replay_restart_pending = false;
		replay_restart_attempts = 0;
	}
	obs_log(LOG_INFO, "Smart replay %s", enabled ? "enabled" : "disabled");
}

static void schedule_smart_replay_buffer_start(void)
{
	QTimer::singleShot(kSmartReplayStartDelayMs, []() {
		if (!smart_replay_enabled) {
			replay_restart_pending = false;
			replay_restart_attempts = 0;
			obs_log(LOG_INFO, "Smart replay: start cancelled (disabled)");
			return;
		}

		if (obs_frontend_replay_buffer_active()) {
			obs_log(LOG_INFO, "Smart replay: replay buffer already active");
			replay_restart_pending = false;
			replay_restart_attempts = 0;
			return;
		}

		++replay_restart_attempts;
		obs_log(LOG_INFO, "Smart replay: starting replay buffer (attempt %d/%d)",
			replay_restart_attempts, kSmartReplayMaxRestartAttempts);
		obs_frontend_replay_buffer_start();

		QTimer::singleShot(kSmartReplayVerifyDelayMs, []() {
			if (obs_frontend_replay_buffer_active()) {
				obs_log(LOG_INFO, "Smart replay: replay buffer restart confirmed");
				replay_restart_pending = false;
				replay_restart_attempts = 0;
				return;
			}

			if (!replay_restart_pending)
				return;

			if (replay_restart_attempts >= kSmartReplayMaxRestartAttempts) {
				obs_log(LOG_WARNING,
					"Smart replay: failed to restart replay buffer after %d attempts",
					replay_restart_attempts);
				replay_restart_pending = false;
				replay_restart_attempts = 0;
				return;
			}

			obs_log(LOG_WARNING, "Smart replay: replay buffer did not start, retrying");
			schedule_smart_replay_buffer_start();
		});
	});
}

static void restart_smart_replay_buffer_after_save(void)
{
	if (!smart_replay_enabled)
		return;

	replay_restart_pending = true;
	replay_restart_attempts = 0;

	if (obs_frontend_replay_buffer_active()) {
		obs_log(LOG_INFO, "Smart replay: stopping replay buffer for reset after save");
		obs_frontend_replay_buffer_stop();
		return;
	}

	obs_log(LOG_INFO, "Smart replay: replay buffer inactive after save completed");
	schedule_smart_replay_buffer_start();
}

static void handle_replay_saved_event(void)
{
	OverlayStateManager::SetReplaySaving(false);

	const char *replay_path = obs_frontend_get_last_replay();
	if (replay_path && replay_path[0] != '\0' && os_file_exists(replay_path)) {
		obs_log(LOG_INFO, "Replay saved: %s", replay_path);
		if (overlay_indicators)
			overlay_indicators_notify_replay_saved(overlay_indicators);
	} else {
		obs_log(LOG_WARNING,
			"Replay buffer saved event without a valid file path");
	}

	restart_smart_replay_buffer_after_save();
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

static void save_replay_callback(void *data, obs_hotkey_id id,
				 obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed)
		return;

	if (!obs_frontend_replay_buffer_active()) {
		obs_log(LOG_WARNING, "Save replay hotkey: replay buffer not active");
		return;
	}
	if (OverlayStateManager::GetReplaySaving()) {
		obs_log(LOG_INFO, "Save replay hotkey: save already in progress");
		return;
	}

	obs_log(LOG_INFO, "Save replay hotkey: saving replay buffer");
	OverlayStateManager::SetReplaySaving(true);
	obs_frontend_replay_buffer_save();
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
extern "C" void overlay_runtime_set_indicators(bool enabled, int position, bool oledProtection)
{
	if (!overlay_indicators)
		return;
	overlay_indicators_set_enabled(overlay_indicators, enabled);
	overlay_indicators_set_position(overlay_indicators, position);
	overlay_indicators_set_oled_protection(overlay_indicators, oledProtection);
}

extern "C" void overlay_runtime_set_smart_replay(bool enabled)
{
	set_smart_replay_enabled(enabled);
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
					overlay_indicators_set_enabled(overlay_indicators, indicators_enabled);
					overlay_indicators_set_position(overlay_indicators, indicators_position);
					overlay_indicators_set_oled_protection(overlay_indicators,
									       indicators_oled_protection);
				} else {
					overlay_indicators_set_enabled(overlay_indicators, true);
					overlay_indicators_set_position(overlay_indicators, 5);
				}
			} else {
				obs_log(LOG_ERROR, "Failed to create overlay indicators");
			}
		}
	} else if (event == OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED) {
		handle_replay_saved_event();
	} else if (event == OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPED) {
		if (replay_restart_pending) {
			obs_log(LOG_INFO,
				"Smart replay: replay buffer stopped, restarting after save completed");
			schedule_smart_replay_buffer_start();
		}
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
	set_smart_replay_enabled(!saved_settings_data || !hasSmartReplay
		? true
		: obs_data_get_bool(saved_settings_data, "smart_replay"));
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
	overlay_window = NULL;
	overlay_indicators = NULL;

	if (ow) {
		overlay_window_destroy(ow);
		obs_log(LOG_INFO, "Overlay window destroyed");
	}
	if (oi) {
		overlay_indicators_destroy(oi);
		obs_log(LOG_INFO, "Overlay indicators destroyed");
	}
#endif

	obs_log(LOG_INFO, "plugin unloaded");
}
