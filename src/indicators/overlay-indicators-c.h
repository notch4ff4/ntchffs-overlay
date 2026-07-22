#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(ENABLE_QT)
void *overlay_indicators_create(void);
void overlay_indicators_destroy(void *indicators);
void overlay_indicators_set_enabled(void *indicators, bool enabled);
void overlay_indicators_set_position(void *indicators, int position);
void overlay_indicators_set_oled_protection(void *indicators, bool enabled);
void overlay_indicators_set_scale(void *indicators, int scalePercent);
void overlay_indicators_set_custom(void *indicators, int slot, const char *pathUtf8, bool tintEnabled,
				   int colorRgb);
void overlay_indicators_set_sound(void *indicators, int eventIndex, const char *pathUtf8);
void overlay_indicators_notify_replay_saved(void *indicators);
void overlay_indicators_raise_topmost(void *indicators);
void overlay_indicators_raise_active_topmost(void);
void overlay_indicators_set_overlay_visible(bool visible);
#endif

#ifdef __cplusplus
}
#endif
