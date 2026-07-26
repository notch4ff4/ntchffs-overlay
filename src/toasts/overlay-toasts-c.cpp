#include "overlay-toasts-c.h"

#ifdef _WIN32
#include "overlay-toasts.h"
#include "overlay-string-utils.h"
#include <obs-module.h>
#include <windows.h>
#include <util/base.h>
#include <exception>
#include <string>

extern "C" {
#include <plugin-support.h>
}

static OverlayToasts *g_activeToasts = nullptr;

extern "C" {

void *overlay_toasts_create(void)
{
	OverlayToasts *toasts = nullptr;
	try {
		toasts = new OverlayToasts();
		HINSTANCE hInstance = GetModuleHandle(NULL);
		if (!toasts->Initialize(hInstance)) {
			delete toasts;
			return nullptr;
		}
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "overlay_toasts_create: Exception: %s", e.what());
		delete toasts;
		return nullptr;
	} catch (...) {
		obs_log(LOG_ERROR, "overlay_toasts_create: Unknown exception");
		delete toasts;
		return nullptr;
	}
	obs_log(LOG_INFO, "overlay_toasts_create: toasts initialized");
	g_activeToasts = toasts;
	return toasts;
}

void overlay_toasts_destroy(void *toasts)
{
	if (!toasts)
		return;
	OverlayToasts *renderer = static_cast<OverlayToasts *>(toasts);
	if (g_activeToasts == renderer)
		g_activeToasts = nullptr;
	delete renderer;
	obs_log(LOG_INFO, "overlay_toasts_destroy: toasts destroyed");
}

void overlay_toasts_show(void *toasts, const char *textUtf8, unsigned int durationMs)
{
	if (!toasts || !textUtf8)
		return;
	OverlayToasts *renderer = static_cast<OverlayToasts *>(toasts);
	std::wstring text = overlay::util::Utf8ToWide(textUtf8);
	renderer->Show(text.c_str(), durationMs);
}

void overlay_toasts_show_replay_saved(void *toasts, const char *pathUtf8)
{
	if (!toasts || !pathUtf8)
		return;
	OverlayToasts *renderer = static_cast<OverlayToasts *>(toasts);
	std::wstring path = overlay::util::Utf8ToWide(pathUtf8);
	renderer->ShowReplaySaved(path.c_str());
}

void overlay_toasts_show_open_hint(void *toasts, const char *hotkeyLabelUtf8)
{
	if (!toasts)
		return;
	OverlayToasts *renderer = static_cast<OverlayToasts *>(toasts);
	std::wstring label = hotkeyLabelUtf8 ? overlay::util::Utf8ToWide(hotkeyLabelUtf8) : std::wstring();
	renderer->ShowOpenHint(label.c_str());
}

void overlay_toasts_tick_disk_space(void *toasts)
{
	if (!toasts)
		return;
	OverlayToasts *renderer = static_cast<OverlayToasts *>(toasts);
	renderer->TickDiskSpace();
}

void overlay_toasts_raise_topmost(void *toasts)
{
	if (!toasts)
		return;
	OverlayToasts *renderer = static_cast<OverlayToasts *>(toasts);
	renderer->RaiseTopmost();
}

void overlay_toasts_raise_active_topmost(void)
{
	if (g_activeToasts)
		g_activeToasts->RaiseTopmost();
}

void overlay_toasts_set_action_hotkeys(void *toasts, int viewVk, int viewMod, int deleteVk, int deleteMod)
{
	if (!toasts)
		return;
	OverlayToasts *renderer = static_cast<OverlayToasts *>(toasts);
	renderer->SetActionHotkeys(viewVk, viewMod, deleteVk, deleteMod);
}

void overlay_toasts_set_accent_color(void *toasts, int colorRgb)
{
	if (!toasts)
		return;
	OverlayToasts *renderer = static_cast<OverlayToasts *>(toasts);
	renderer->SetAccentColor(colorRgb);
}

void overlay_toasts_set_options(void *toasts, bool enabled, bool videoSaved, bool openHint, bool lowDisk,
				int lowDiskThresholdGb, int durationSec)
{
	if (!toasts)
		return;
	OverlayToasts *renderer = static_cast<OverlayToasts *>(toasts);
	renderer->SetEnabled(enabled);
	renderer->SetVideoSavedEnabled(videoSaved);
	renderer->SetOpenHintEnabled(openHint);
	renderer->SetLowDiskEnabled(lowDisk);
	renderer->SetLowDiskThresholdGb(lowDiskThresholdGb);
	if (durationSec < 2)
		durationSec = 2;
	if (durationSec > 60)
		durationSec = 60;
	renderer->SetDurationMs(static_cast<ULONGLONG>(durationSec) * 1000ull);
}

void overlay_toasts_set_view_handler(void *toasts, void (*handler)(const char *pathUtf8, void *userData),
				     void *userData)
{
	if (!toasts)
		return;
	OverlayToasts *renderer = static_cast<OverlayToasts *>(toasts);

	struct Bridge {
		void (*handler)(const char *, void *);
		void *userData;
	};
	static Bridge g_bridge = {};
	g_bridge.handler = handler;
	g_bridge.userData = userData;

	renderer->SetViewFileHandler(
		[](const wchar_t *path, void *ud) {
			Bridge *b = static_cast<Bridge *>(ud);
			if (!b || !b->handler || !path)
				return;
			std::string utf8 = overlay::util::WideToUtf8(path);
			b->handler(utf8.c_str(), b->userData);
		},
		&g_bridge);
}

} // extern "C"

#endif // _WIN32
