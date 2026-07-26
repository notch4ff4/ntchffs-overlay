#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(ENABLE_QT)
void *overlay_toasts_create(void);
void overlay_toasts_destroy(void *toasts);
void overlay_toasts_show(void *toasts, const char *textUtf8, unsigned int durationMs);
void overlay_toasts_show_replay_saved(void *toasts, const char *pathUtf8);
void overlay_toasts_show_open_hint(void *toasts, const char *hotkeyLabelUtf8);
void overlay_toasts_tick_disk_space(void *toasts);
void overlay_toasts_raise_topmost(void *toasts);
void overlay_toasts_raise_active_topmost(void);
void overlay_toasts_set_action_hotkeys(void *toasts, int viewVk, int viewMod, int deleteVk, int deleteMod);
void overlay_toasts_set_accent_color(void *toasts, int colorRgb);
void overlay_toasts_set_options(void *toasts, bool enabled, bool videoSaved, bool openHint, bool lowDisk,
				int lowDiskThresholdGb, int durationSec);
void overlay_toasts_set_view_handler(void *toasts, void (*handler)(const char *pathUtf8, void *userData),
				     void *userData);
#endif

#ifdef __cplusplus
}
#endif
