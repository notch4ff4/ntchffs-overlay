#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bridge used by the in-overlay settings panel to apply settings that live in
// plugin-main globals (indicators + smart replay) which the renderer cannot
// reach directly.
void overlay_runtime_set_indicators(bool enabled, int position, bool oledProtection);
void overlay_runtime_set_smart_replay(bool enabled);

#ifdef __cplusplus
}
#endif
