#pragma once

#ifdef ENABLE_FRONTEND_API

#include <obs-data.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

extern obs_data_t *saved_settings_data;

void overlay_settings_ensure(void);
void overlay_settings_load(void);
bool overlay_settings_save(void);

#ifdef __cplusplus
}
#endif

#endif
