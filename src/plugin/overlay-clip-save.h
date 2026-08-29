/*
Clip-duration save flow adapted from Replay Buffer Pro by @JoshuaPotter
(https://github.com/JoshuaPotter/replay-buffer-pro), GPL-2.0-or-later.

Permission to adapt the code/logic was granted by the author in
https://github.com/JoshuaPotter/replay-buffer-pro/issues/30
("Hey, feel free to use what you'd like" — @JoshuaPotter, 2026-08-07).
*/
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
	OVERLAY_CLIP_SAVE_SLOT_COUNT = 3,
	OVERLAY_CLIP_SAVE_MIN_SECONDS = 1,
	OVERLAY_CLIP_SAVE_MAX_SECONDS = 21600,
};

// Request a timed clip save (seconds > 0).
// In timestamp-trim mode, keep-last vs since-last-timestamp is controlled by
// overlay_smart_replay_set_clip_ignore_trim. Timestamp is always noted for the timeline.
// Returns false if the request was rejected (inactive buffer, invalid duration, busy).
bool overlay_clip_save_request(int seconds);

// Called on OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED.
// Returns true when a pending timed clip was consumed (caller should acknowledge
// Smart Replay without overlap trim, instead of running the full save-completed path).
bool overlay_clip_save_on_replay_saved(const char *replay_path);

int overlay_clip_save_get_buffer_length(void);
bool overlay_clip_save_set_buffer_length(int seconds);

#ifdef __cplusplus
}
#endif
