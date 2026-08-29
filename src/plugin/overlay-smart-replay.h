#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum overlay_smart_replay_mode {
	OVERLAY_SMART_REPLAY_LEGACY = 0,
	OVERLAY_SMART_REPLAY_TIMESTAMP_TRIM = 1,
};

void overlay_smart_replay_set_enabled(bool enabled);
void overlay_smart_replay_set_mode(int mode);
// When true (default), "Record last N minutes" keeps the last N seconds even in
// timestamp-trim mode. When false, those actions save everything since the last timestamp.
void overlay_smart_replay_set_clip_ignore_trim(bool ignore);
bool overlay_smart_replay_get_clip_ignore_trim(void);
// True when timed clip saves should keep-last-N instead of timestamp overlap trim.
bool overlay_smart_replay_clip_uses_keep_last(void);
// Records a save-request timestamp in timestamp-trim mode without starting a save.
// Used by timed clip saves so they advance the overlap timeline.
bool overlay_smart_replay_note_save_request(void);
// Returns true when the smart-replay module handled the save request.
bool overlay_smart_replay_request_save(void);
void overlay_smart_replay_on_save_completed(const char *replay_path);
// Consumes a noted save request without overlap trim (clip-save already trimmed the file).
void overlay_smart_replay_acknowledge_save(const char *replay_path);
void overlay_smart_replay_on_replay_buffer_stopped(void);
bool overlay_smart_replay_is_restart_pending(void);

#ifdef __cplusplus
}
#endif
