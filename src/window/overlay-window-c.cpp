#include "overlay-window-c.h"

#ifdef _WIN32
#include "overlay-renderer.h"
#include <windows.h>

extern "C" {
#include <obs-module.h>
#include <plugin-support.h>

void *overlay_window_create(void)
{
	OverlayRenderer *renderer = nullptr;

	try {
		renderer = new OverlayRenderer();
		if (renderer) {
			HINSTANCE hInstance = GetModuleHandle(NULL);
			if (renderer->Initialize(hInstance)) {
				obs_log(LOG_INFO, "overlay_window_create: Windows API renderer created successfully");
			} else {
				obs_log(LOG_ERROR, "overlay_window_create: Failed to initialize Windows API renderer");
				delete renderer;
				renderer = nullptr;
			}
		} else {
			obs_log(LOG_ERROR, "overlay_window_create: Failed to create renderer (nullptr)");
		}
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "overlay_window_create: Exception: %s", e.what());
		if (renderer) {
			delete renderer;
			renderer = nullptr;
		}
	} catch (...) {
		obs_log(LOG_ERROR, "overlay_window_create: Unknown exception");
		if (renderer) {
			delete renderer;
			renderer = nullptr;
		}
	}

	return renderer;
}

void overlay_window_destroy(void *window)
{
	if (window) {
		OverlayRenderer *renderer = static_cast<OverlayRenderer *>(window);

		// Hide first so teardown does not leave a visible ghost window.
		renderer->Hide();

		delete renderer;
		obs_log(LOG_INFO, "overlay_window_destroy: Renderer destroyed");
	}
}

void overlay_window_set_visible(void *window, bool visible)
{
	if (!window) {
		return;
	}

	OverlayRenderer *renderer = static_cast<OverlayRenderer *>(window);

	if (visible) {
		renderer->Show();
	} else {
		renderer->Hide();
	}
}

struct DelayedVisibilityData {
	OverlayRenderer *renderer;
	bool visible;
};

static OverlayPosition ClampOverlayPosition(int rawPosition)
{
	if (rawPosition < static_cast<int>(OverlayPosition::Top) ||
	    rawPosition > static_cast<int>(OverlayPosition::Center)) {
		return OverlayPosition::TopRight;
	}
	return static_cast<OverlayPosition>(rawPosition);
}

static OverlayOrientation ClampOverlayOrientation(int rawOrientation)
{
	if (rawOrientation != static_cast<int>(OverlayOrientation::Vertical) &&
	    rawOrientation != static_cast<int>(OverlayOrientation::Horizontal)) {
		return OverlayOrientation::Vertical;
	}
	return static_cast<OverlayOrientation>(rawOrientation);
}

static VOID CALLBACK DelayedVisibilityTimerProc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time)
{
	UNUSED_PARAMETER(hwnd);
	UNUSED_PARAMETER(msg);
	UNUSED_PARAMETER(time);

	DelayedVisibilityData *data = (DelayedVisibilityData *)id;
	if (data && data->renderer) {
		if (data->visible) {
			data->renderer->Show();
		} else {
			data->renderer->Hide();
		}
		delete data;
		KillTimer(NULL, id);
	}
}

void overlay_window_set_visible_delayed(void *window, bool visible, int delay_ms)
{
	if (window && delay_ms > 0) {
		OverlayRenderer *renderer = static_cast<OverlayRenderer *>(window);
		DelayedVisibilityData *data = new DelayedVisibilityData();
		data->renderer = renderer;
		data->visible = visible;

		if (!SetTimer(NULL, (UINT_PTR)data, delay_ms, DelayedVisibilityTimerProc)) {
			obs_log(LOG_WARNING,
				"overlay_window_set_visible_delayed: SetTimer failed, applying visibility immediately");
			overlay_window_set_visible(window, visible);
			delete data;
		}
	} else if (window) {
		overlay_window_set_visible(window, visible);
	}
}

bool overlay_window_is_visible(void *window)
{
	if (window) {
		OverlayRenderer *renderer = static_cast<OverlayRenderer *>(window);
		return renderer->IsVisible();
	}
	return false;
}

void overlay_window_toggle_visible(void *window)
{
	if (!window) {
		return;
	}

	OverlayRenderer *renderer = static_cast<OverlayRenderer *>(window);

	bool currentVisible = renderer->IsVisible();
	if (currentVisible) {
		renderer->Hide();
	} else {
		renderer->Show();
	}
}

void overlay_window_set_position(void *window, int position, int margin)
{
	if (!window) {
		return;
	}

	OverlayRenderer *renderer = static_cast<OverlayRenderer *>(window);
	renderer->SetPosition(ClampOverlayPosition(position), margin);
}

void overlay_window_apply_position(void *window)
{
	if (!window) {
		return;
	}

	OverlayRenderer *renderer = static_cast<OverlayRenderer *>(window);
	renderer->ApplyPosition();
}

void overlay_window_set_orientation(void *window, int orientation)
{
	if (!window) {
		return;
	}

	OverlayRenderer *renderer = static_cast<OverlayRenderer *>(window);
	renderer->SetOrientation(ClampOverlayOrientation(orientation));
}

void overlay_window_set_auto_hide(void *window, bool enabled, int seconds)
{
	if (!window) {
		return;
	}

	OverlayRenderer *renderer = static_cast<OverlayRenderer *>(window);
	renderer->SetAutoHide(enabled, seconds);
}

void overlay_window_set_gallery_enabled(void *window, bool enabled)
{
	if (!window) {
		return;
	}

	OverlayRenderer *renderer = static_cast<OverlayRenderer *>(window);
	renderer->SetGalleryEnabled(enabled);
}

void overlay_window_set_capture_focus(void *window, bool capture)
{
	if (!window) {
		return;
	}

	OverlayRenderer *renderer = static_cast<OverlayRenderer *>(window);
	renderer->SetCaptureFocus(capture);
}

void overlay_window_set_background_alpha(void *window, float alpha)
{
	if (!window) {
		return;
	}
	OverlayRenderer *renderer = static_cast<OverlayRenderer *>(window);
	renderer->SetOverlayBackgroundAlpha(alpha);
}

void overlay_window_open_settings(void *window)
{
	if (!window) {
		return;
	}
	OverlayRenderer *renderer = static_cast<OverlayRenderer *>(window);
	renderer->OpenSettings();
}

int overlay_window_get_position(void *window)
{
	if (!window) {
		return 0;
	}

	OverlayRenderer *renderer = static_cast<OverlayRenderer *>(window);
	return static_cast<int>(renderer->GetPosition());
}

int overlay_window_get_margin(void *window)
{
	if (!window) {
		return 0;
	}

	OverlayRenderer *renderer = static_cast<OverlayRenderer *>(window);
	return renderer->GetMargin();
}

int overlay_window_get_orientation(void *window)
{
	if (!window) {
		return 0;
	}

	OverlayRenderer *renderer = static_cast<OverlayRenderer *>(window);
	return static_cast<int>(renderer->GetOrientation());
}

} // extern "C"

#else
#error "Windows API implementation is required. This plugin only supports Windows."
#endif // _WIN32
