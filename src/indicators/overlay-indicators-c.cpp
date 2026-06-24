#include "overlay-indicators-c.h"

#ifdef _WIN32
#include "overlay-indicators.h"
#include <obs-module.h>
#include <windows.h>
#include <util/base.h>
#include <exception>

extern "C" {
#include <plugin-support.h>
}

static OverlayIndicators *g_activeIndicators = nullptr;
static bool g_overlayVisible = false;

extern "C" {

void *overlay_indicators_create(void)
{
	OverlayIndicators *indicators = nullptr;
	try {
		indicators = new OverlayIndicators();
		HINSTANCE hInstance = GetModuleHandle(NULL);
		if (!indicators->Initialize(hInstance)) {
			delete indicators;
			return nullptr;
		}
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "overlay_indicators_create: Exception: %s", e.what());
		delete indicators;
		return nullptr;
	} catch (...) {
		obs_log(LOG_ERROR, "overlay_indicators_create: Unknown exception");
		delete indicators;
		return nullptr;
	}
	obs_log(LOG_INFO, "overlay_indicators_create: indicators initialized");
	g_activeIndicators = indicators;
	return indicators;
}

void overlay_indicators_destroy(void *indicators)
{
	if (!indicators) {
		return;
	}
	OverlayIndicators *renderer = static_cast<OverlayIndicators *>(indicators);
	if (g_activeIndicators == renderer) {
		g_activeIndicators = nullptr;
	}
	delete renderer;
	obs_log(LOG_INFO, "overlay_indicators_destroy: indicators destroyed");
}

void overlay_indicators_set_enabled(void *indicators, bool enabled)
{
	if (!indicators) {
		return;
	}
	OverlayIndicators *renderer = static_cast<OverlayIndicators *>(indicators);
	renderer->SetEnabled(enabled);
}

void overlay_indicators_set_position(void *indicators, int position)
{
	if (!indicators) {
		return;
	}
	OverlayIndicators *renderer = static_cast<OverlayIndicators *>(indicators);
	if (position < static_cast<int>(OverlayPosition::Top) ||
	    position > static_cast<int>(OverlayPosition::Center)) {
		position = static_cast<int>(OverlayPosition::TopRight);
	}
	renderer->SetPosition(static_cast<OverlayPosition>(position));
}

void overlay_indicators_set_oled_protection(void *indicators, bool enabled)
{
	if (!indicators) {
		return;
	}
	OverlayIndicators *renderer = static_cast<OverlayIndicators *>(indicators);
	renderer->SetOledProtection(enabled);
}

void overlay_indicators_notify_replay_saved(void *indicators)
{
	if (!indicators) {
		return;
	}
	OverlayIndicators *renderer = static_cast<OverlayIndicators *>(indicators);
	renderer->NotifyReplaySaved();
}

void overlay_indicators_raise_topmost(void *indicators)
{
	if (!indicators) {
		return;
	}
	OverlayIndicators *renderer = static_cast<OverlayIndicators *>(indicators);
	renderer->RaiseTopmost();
}

void overlay_indicators_raise_active_topmost(void)
{
	if (g_activeIndicators) {
		g_activeIndicators->RaiseTopmost();
	}
}

void overlay_indicators_set_overlay_visible(bool visible)
{
	g_overlayVisible = visible;
}

bool overlay_indicators_get_overlay_visible(void)
{
	return g_overlayVisible;
}

} // extern "C"

#endif // _WIN32
