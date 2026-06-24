#ifdef ENABLE_FRONTEND_API

#include "overlay-settings-storage.h"
#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

obs_data_t *saved_settings_data = NULL;

static const char *SETTINGS_FILE_NAME = "settings.json";

static char *settings_file_path(void)
{
	return obs_module_config_path(SETTINGS_FILE_NAME);
}

static bool ensure_settings_config_dir(void)
{
	char *config_dir = obs_module_config_path(NULL);
	if (!config_dir)
		return false;

	const int result = os_mkdirs(config_dir);
	bfree(config_dir);

	if (result == MKDIR_ERROR) {
		obs_log(LOG_ERROR, "Failed to create plugin config directory");
		return false;
	}

	return true;
}

void overlay_settings_ensure(void)
{
	if (!saved_settings_data)
		saved_settings_data = obs_data_create();
}

void overlay_settings_load(void)
{
	if (saved_settings_data) {
		obs_data_release(saved_settings_data);
		saved_settings_data = NULL;
	}

	ensure_settings_config_dir();

	char *path = settings_file_path();
	if (!path)
		return;

	obs_data_t *data = obs_data_create_from_json_file(path);
	if (data) {
		saved_settings_data = data;
		obs_log(LOG_INFO, "Settings loaded from %s", path);
	} else {
		saved_settings_data = obs_data_create();
		obs_log(LOG_INFO, "No settings file at %s, using defaults", path);
	}

	bfree(path);
}

bool overlay_settings_save(void)
{
	if (!saved_settings_data)
		return false;

	if (!ensure_settings_config_dir())
		return false;

	char *path = settings_file_path();
	if (!path)
		return false;

	const bool ok = obs_data_save_json_safe(saved_settings_data, path, ".tmp", ".bak");
	if (ok)
		obs_log(LOG_INFO, "Settings saved to %s", path);
	else
		obs_log(LOG_ERROR, "Failed to save settings to %s", path);

	bfree(path);
	return ok;
}

#endif
