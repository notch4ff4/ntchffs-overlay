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
// Returns true when the smart-replay module handled the save request.
bool overlay_smart_replay_request_save(void);
void overlay_smart_replay_on_save_completed(const char *replay_path);
void overlay_smart_replay_on_replay_buffer_stopped(void);

#ifdef __cplusplus
}
#endif
