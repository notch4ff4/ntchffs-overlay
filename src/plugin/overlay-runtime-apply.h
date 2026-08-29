#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bridge used by the in-overlay settings panel to apply settings that live in
// plugin-main globals (indicators + smart replay) which the renderer cannot
// reach directly.
void overlay_runtime_set_indicators(bool enabled, int position, bool oledProtection, int scalePercent);
void overlay_runtime_set_indicator_custom(int slot, const char *pathUtf8, bool tintEnabled, int colorRgb);
void overlay_runtime_set_indicator_sound(int eventIndex, const char *pathUtf8);
void overlay_runtime_set_smart_replay(bool enabled);
void overlay_runtime_set_smart_replay_mode(int mode);
void overlay_runtime_set_smart_replay_clip_ignore_trim(bool ignore);
void overlay_runtime_set_remember_replay_buffer(bool enabled);
void overlay_runtime_set_toast_action_hotkeys(int viewVk, int viewMod, int deleteVk, int deleteMod);
void overlay_runtime_set_toast_hotkeys_always_active(bool always);
void overlay_runtime_set_toast_accent_color(int colorRgb);
void overlay_runtime_set_toast_options(bool enabled, bool videoSaved, bool openHint, bool lowDisk,
				       int lowDiskThresholdGb, int durationSec);

#ifdef __cplusplus
}
#endif
