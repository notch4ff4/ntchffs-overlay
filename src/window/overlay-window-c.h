#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Exposed when Direct2D (Windows) or Qt overlay builds are enabled.
#if defined(_WIN32) || defined(ENABLE_QT)
void *overlay_window_create(void);
void overlay_window_destroy(void *window);
void overlay_window_set_visible(void *window, bool visible);
void overlay_window_set_visible_delayed(void *window, bool visible, int delay_ms);
bool overlay_window_is_visible(void *window);
void overlay_window_toggle_visible(void *window);
void overlay_window_set_position(void *window, int position, int margin);
void overlay_window_apply_position(void *window);
void overlay_window_set_orientation(void *window, int orientation);
void overlay_window_set_auto_hide(void *window, bool enabled, int seconds);
void overlay_window_set_gallery_enabled(void *window, bool enabled);
void overlay_window_set_capture_focus(void *window, bool capture);
void overlay_window_set_background_alpha(void *window, float alpha);
void overlay_window_open_settings(void *window);
int overlay_window_get_position(void *window);
int overlay_window_get_margin(void *window);
int overlay_window_get_orientation(void *window);
#endif

#ifdef __cplusplus
}
#endif

