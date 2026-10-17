// Should be refactored. Plan to separate into smaller files. (will be removed after checking)
#ifdef _WIN32

#include "overlay-renderer.h"
#include "overlay-icons-resource.h"
#include "overlay-string-utils.h"
#include "overlay-indicators-c.h"
#include "overlay-toasts-c.h"
#include "overlay-svg-rasterize.h"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <sstream>
#include <iomanip>
#include <shellapi.h>
#include <objbase.h>
#include <shlobj.h>
#include <comdef.h>
#include <commdlg.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwctype>

#ifdef ENABLE_QT
#include <QMetaObject>
#include <QWidget>
#endif

extern "C" {
#include <plugin-support.h>
#include "overlay-settings-storage.h"
#include "overlay-runtime-apply.h"
#include "overlay-clip-save.h"
}

OverlayRenderer *OverlayRenderer::s_timerInstance = nullptr;

namespace {
constexpr UINT_PTR kOpenWithTimerId = 42;
constexpr UINT_PTR kClipHotkeyTimerId = 43;
constexpr UINT_PTR kExportQueueTimerId = 44;
constexpr UINT kOpenWithDelayMs = 200;
constexpr UINT kClipHotkeyPollMs = 33;
// Keep harvesting export workers while the overlay is hidden so completion toasts fire.
constexpr UINT kExportQueuePollMs = 250;
} // namespace

// Anchor symbol so GetModuleHandleEx can resolve this DLL for embedded icons.
static int g_overlayIconsModuleAnchor = 0;

namespace {

constexpr int kCompressQualityStrongCompression = 0;
constexpr int kCompressQualityBalanced = 1;
constexpr int kCompressQualityQuality = 2;

constexpr int kClipHotkeyBaseIndexEarly = 9;

std::wstring FormatClipDurationLabel(int seconds);

constexpr const wchar_t *kAboutUrls[] = {
	L"https://github.com/notch4ff4/ntchffs-overlay",
	L"https://t.me/ntchff",
	L"https://www.twitch.tv/ntchff",
	L"https://www.donationalerts.com/r/notch4ff4",
};

int ClampInt(int v, int lo, int hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

int CrfForCompressQuality(int preset)
{
	// CRF trades quality for file size; presets map to fixed values for export UI.
	switch (preset) {
	case kCompressQualityStrongCompression:
		return 32;
	case kCompressQualityQuality:
		return 22;
	case kCompressQualityBalanced:
	default:
		// Balanced preset matches the legacy default CRF of 28.
		return 28;
	}
}

int CompressQualityFromCrf(int crf)
{
	// Older configs stored CRF only; map to the nearest preset on load.
	if (crf <= 24)
		return kCompressQualityQuality;
	if (crf <= 29)
		return kCompressQualityBalanced;
	return kCompressQualityStrongCompression;
}

const std::wstring CompressQualityLabel(int preset)
{
	switch (preset) {
	case kCompressQualityStrongCompression:
		return overlay::util::ModuleTextW("Compress.Strong");
	case kCompressQualityQuality:
		return overlay::util::ModuleTextW("Compress.Quality");
	case kCompressQualityBalanced:
	default:
		return overlay::util::ModuleTextW("Compress.Balanced");
	}
}

} // namespace

OverlayRenderer::OverlayRenderer()
	: m_d2dFactory(nullptr),
	  m_renderTarget(nullptr),
	  m_writeFactory(nullptr),
	  m_wicFactory(nullptr),
	  m_wicBitmap(nullptr),
	  m_bitmapRenderTarget(nullptr),
	  m_backgroundBrush(nullptr),
	  m_textBrush(nullptr),
	  m_activeBrush(nullptr),
	  m_borderBrush(nullptr),
	  m_statsTitleBrush(nullptr),
	  m_statusTextFormat(nullptr),
	  m_buttonTextFormat(nullptr),
	  m_replayActiveTextFormat(nullptr),
	  m_statsTitleFormat(nullptr),
	  m_statsValueFormat(nullptr),
	  m_iconPlayGeometry(nullptr),
	  m_position(OverlayPosition::Center),
	  m_margin(20),
	  m_orientation(OverlayOrientation::Horizontal),
	  m_monitorMode(OverlayMonitorMode::Primary),
	  m_monitorIndex(0),
	  m_autoHideEnabled(false),
	  m_autoHideSeconds(5),
	  m_lastInteractionTick(0),
	  m_hoverTarget(HoverTarget::None),
	  m_galleryEnabled(true),
	  m_returnHomeOnHide(false),
	  m_galleryOpen(false),
	  m_galleryPage(0),
	  m_gallerySelectedIndex(-1),
	  m_galleryTrimIn(0.0),
	  m_galleryTrimOut(0.0),
	  m_galleryTrimUserSet(false),
	  m_galleryShareCompress(false),
	  m_galleryShareCompressQuality(1),
	  m_galleryShareCrf(28),
	  m_galleryFullscreen(false),
	  m_galleryMuted(false),
	  m_galleryVolume(100),
	  m_galleryExportSubmenuOpen(false),
	  m_galleryAudioTrackSubmenuOpen(false),
	  m_galleryContextMenuOpen(false),
	  m_galleryContextMenuIndex(-1),
	  m_galleryContextMenuHoverIndex(-1),
	  m_replayClipPickerOpen(false),
	  m_dragCandidate(false),
	  m_dragActive(false),
	  m_timelineDragging(false),
	  m_suppressNextGalleryClick(false),
	  m_overlayBackgroundAlpha(0.88f),
	  m_overlayScalePercent(100),
	  m_textScalePercent(100),
	  m_accentColorRgb(0xA882FF),
	  m_captureFocus(true),
	  m_dimContentOffset{0, 0},
	  m_galleryContentWidth(0),
	  m_galleryContentHeight(0),
	  m_updateTimer(0),
	  m_updateIntervalMs(500),
	  m_clipHotkeyTimer(0),
	  m_exportQueueTimer(0)
{
	ZeroMemory(&m_dragStart, sizeof(m_dragStart));
	ZeroMemory(&m_galleryListRect, sizeof(m_galleryListRect));
	ZeroMemory(&m_galleryPreviewRect, sizeof(m_galleryPreviewRect));
	ZeroMemory(&m_galleryControlsRect, sizeof(m_galleryControlsRect));
	ZeroMemory(&m_galleryTimelineRect, sizeof(m_galleryTimelineRect));
	ZeroMemory(&m_galleryBackRect, sizeof(m_galleryBackRect));
	ZeroMemory(&m_galleryFolderBackRect, sizeof(m_galleryFolderBackRect));
	ZeroMemory(&m_galleryFullscreenRect, sizeof(m_galleryFullscreenRect));
	ZeroMemory(&m_galleryPrevPageRect, sizeof(m_galleryPrevPageRect));
	ZeroMemory(&m_galleryNextPageRect, sizeof(m_galleryNextPageRect));
	ZeroMemory(&m_galleryPlayRect, sizeof(m_galleryPlayRect));
	ZeroMemory(&m_gallerySetInRect, sizeof(m_gallerySetInRect));
	ZeroMemory(&m_gallerySetOutRect, sizeof(m_gallerySetOutRect));
	ZeroMemory(&m_galleryTrimRect, sizeof(m_galleryTrimRect));
	ZeroMemory(&m_galleryDeleteRect, sizeof(m_galleryDeleteRect));
	ZeroMemory(&m_galleryShareRect, sizeof(m_galleryShareRect));
	ZeroMemory(&m_galleryCompressRect, sizeof(m_galleryCompressRect));
	ZeroMemory(&m_galleryExportPanelRect, sizeof(m_galleryExportPanelRect));
	ZeroMemory(&m_galleryExportConfirmRect, sizeof(m_galleryExportConfirmRect));
	ZeroMemory(&m_galleryQualityPrevRect, sizeof(m_galleryQualityPrevRect));
	ZeroMemory(&m_galleryQualityNextRect, sizeof(m_galleryQualityNextRect));
	ZeroMemory(&m_galleryMuteRect, sizeof(m_galleryMuteRect));
	ZeroMemory(&m_galleryVolumeRect, sizeof(m_galleryVolumeRect));
	ZeroMemory(&m_galleryAudioTrackPrevRect, sizeof(m_galleryAudioTrackPrevRect));
	ZeroMemory(&m_galleryAudioTrackNextRect, sizeof(m_galleryAudioTrackNextRect));
	ZeroMemory(&m_galleryAudioTrackButtonRect, sizeof(m_galleryAudioTrackButtonRect));
	ZeroMemory(&m_galleryAudioTrackMenuRect, sizeof(m_galleryAudioTrackMenuRect));
	ZeroMemory(&m_galleryExportCancelRect, sizeof(m_galleryExportCancelRect));
	ZeroMemory(&m_galleryContextMenuRect, sizeof(m_galleryContextMenuRect));
	ZeroMemory(&m_galleryContextMenuAnchor, sizeof(m_galleryContextMenuAnchor));
	ZeroMemory(&m_replaySaveRect, sizeof(m_replaySaveRect));
	ZeroMemory(&m_replayClipChevronRect, sizeof(m_replayClipChevronRect));
	for (int i = 0; i < ICON_CACHE_SIZE; i++) {
		m_iconBitmaps[i] = nullptr;
		m_iconMasks[i] = IconMask{};
	}

	m_settingsOpen = false;
	m_settingsPage = SettingsPage::Categories;
	m_settingsContentWidth = 0;
	m_settingsContentHeight = 0;
	m_settingsScrollOffset = 0;
	m_settingsContentTotalHeight = 0;
	m_settingsViewportTop = 0;
	m_settingsViewportBottom = 0;
	m_settingsCapturingHotkeyIndex = -1;
	m_settingsEditingIntField = -1;
	m_settingsEditText.clear();
	m_settingsHoverIndex = -1;
	m_setPosition = 8;
	m_setMargin = 20;
	m_setOrientation = 0;
	m_setMonitorMode = 0;
	m_setMonitorIndex = 0;
	m_setOpacityPct = 88;
	m_setOverlayScale = 100;
	m_setTextScale = 100;
	m_setAccentColor = 0xA882FF;
	m_setAutoHideEnabled = false;
	m_setAutoHideSeconds = 5;
	m_setIndicatorsEnabled = true;
	m_setIndicatorsPosition = 5;
	m_setIndicatorsOled = false;
	m_setIndicatorsScale = 100;
	for (int i = 0; i < INDICATOR_SOUND_EVENT_COUNT; i++)
		m_setIndicatorSoundPath[i].clear();
	for (int i = 0; i < INDICATOR_CUSTOM_SLOT_COUNT; i++) {
		m_setIndicatorIconPath[i].clear();
		m_setIndicatorTint[i] = false;
		m_setIndicatorColor[i] = 0xFFFFFF;
	}
	m_setRememberReplayBuffer = false;
	m_setSmartReplay = true;
	m_setSmartReplayMode = 1;
	m_setSmartReplayClipIgnoreTrim = true;
	m_setBufferLengthSec = 120;
	static const int kDefaultClipSeconds[CLIP_SAVE_SLOT_COUNT] = {15, 30, 60};
	for (int i = 0; i < CLIP_SAVE_SLOT_COUNT; i++) {
		m_setClipSeconds[i] = kDefaultClipSeconds[i];
		m_setClipShow[i] = true;
	}
	m_setGalleryInOverlay = true;
	m_setReturnHomeOnHide = false;
	m_setCaptureFocus = true;
	m_setToastsEnabled = true;
	m_setToastVideoSaved = true;
	m_setToastExport = true;
	m_setToastOpenHint = true;
	m_setToastLowDisk = true;
	m_setToastLowDiskGb = 5;
	m_setToastDurationSec = 8;
	m_setToastHotkeysScope = 0;
	for (int i = 0; i < SETTINGS_HOTKEY_COUNT; i++) {
		m_setHotkeyVk[i] = 0;
		m_setHotkeyMod[i] = 0;
	}
	for (int i = 0; i < CLIP_SAVE_SLOT_COUNT; i++)
		m_clipHotkeyWasDown[i] = false;
}

OverlayRenderer::~OverlayRenderer()
{
	Shutdown();
}

void CALLBACK OverlayRenderer::TimerProc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time)
{
	UNUSED_PARAMETER(msg);
	UNUSED_PARAMETER(time);

	if (id == kOpenWithTimerId) {
		KillTimer(hwnd, kOpenWithTimerId);
		if (s_timerInstance) {
			s_timerInstance->RunPendingOpenWith();
		}
		return;
	}

	if (id == kClipHotkeyTimerId) {
		if (s_timerInstance)
			s_timerInstance->PollClipSaveHotkeys();
		return;
	}

	if (id == kExportQueueTimerId) {
		if (s_timerInstance)
			s_timerInstance->PollExportQueue();
		return;
	}

	UNUSED_PARAMETER(hwnd);
	if (s_timerInstance) {
		s_timerInstance->UpdateStatus();
		s_timerInstance->Render();
	}
}

bool OverlayRenderer::Initialize(HINSTANCE hInstance)
{
	// WIC requires COM on the UI thread.
	HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
		obs_log(LOG_ERROR, "Failed to initialize COM: 0x%08x", hr);
		return false;
	}
	OleInitialize(NULL);

	if (!m_windowManager.Initialize(hInstance)) {
		return false;
	}

	// Overlay and gallery must receive clicks instead of passing them through.
	m_windowManager.SetClickThrough(false);
	// When enabled, the overlay can take focus over fullscreen games.
	bool captureFocus = true;
	if (saved_settings_data && obs_data_has_user_value(saved_settings_data, "capture_focus")) {
		captureFocus = obs_data_get_bool(saved_settings_data, "capture_focus");
	}
	m_captureFocus = captureFocus;
	m_windowManager.SetAllowActivate(captureFocus);

	if (!InitializeDirect2D()) {
		return false;
	}

	if (saved_settings_data) {
		if (obs_data_has_user_value(saved_settings_data, "position")) {
			int64_t position = obs_data_get_int(saved_settings_data, "position");
			if (position >= 0 && position <= 8) {
				m_position = static_cast<OverlayPosition>(static_cast<int>(position));
			}
		}

		int64_t margin = obs_data_get_int(saved_settings_data, "margin");
		if (margin < 0 || margin > 200) {
			m_margin = 20;
		} else {
			m_margin = static_cast<int>(margin);
		}

		int64_t orientation = obs_data_get_int(saved_settings_data, "orientation");
		if (orientation == 0 || orientation == 1) {
			m_orientation = static_cast<OverlayOrientation>(static_cast<int>(orientation));
		}

		int64_t monitorMode = obs_data_get_int(saved_settings_data, "overlay_monitor_mode");
		if (monitorMode >= 0 && monitorMode <= 2) {
			m_monitorMode = static_cast<OverlayMonitorMode>(static_cast<int>(monitorMode));
		}
		int64_t monitorIndex = obs_data_get_int(saved_settings_data, "overlay_monitor_index");
		if (monitorIndex < 0)
			monitorIndex = 0;
		{
			const int monitorCount = std::max(1, m_layoutManager.GetMonitorCount());
			if (monitorIndex >= monitorCount)
				monitorIndex = monitorCount - 1;
			m_monitorIndex = static_cast<int>(monitorIndex);
		}

		m_autoHideEnabled = obs_data_get_bool(saved_settings_data, "auto_hide_enabled");
		int64_t autoHideSeconds = obs_data_get_int(saved_settings_data, "auto_hide_seconds");
		if (autoHideSeconds < 1 || autoHideSeconds > 3600) {
			m_autoHideSeconds = 5;
		} else {
			m_autoHideSeconds = static_cast<int>(autoHideSeconds);
		}

		if (obs_data_has_user_value(saved_settings_data, "gallery_in_overlay")) {
			m_galleryEnabled = obs_data_get_bool(saved_settings_data, "gallery_in_overlay");
		}
		if (obs_data_has_user_value(saved_settings_data, "return_home_on_hide")) {
			m_returnHomeOnHide = obs_data_get_bool(saved_settings_data, "return_home_on_hide");
		}
		if (obs_data_has_user_value(saved_settings_data, "gallery_share_compress")) {
			m_galleryShareCompress = obs_data_get_bool(saved_settings_data, "gallery_share_compress");
		}
		// Preset is stored in settings; CRF is derived for ffmpeg export.
		if (obs_data_has_user_value(saved_settings_data, "gallery_share_compress_quality")) {
			int q = static_cast<int>(
				obs_data_get_int(saved_settings_data, "gallery_share_compress_quality"));
			q = ClampInt(q, kCompressQualityStrongCompression, kCompressQualityQuality);
			m_galleryShareCompressQuality = q;
			m_galleryShareCrf = CrfForCompressQuality(q);
			// Mirror CRF so older plugin builds can still read the value.
			if (!obs_data_has_user_value(saved_settings_data, "gallery_share_crf")) {
				obs_data_set_int(saved_settings_data, "gallery_share_crf", m_galleryShareCrf);
			}
		} else if (obs_data_has_user_value(saved_settings_data, "gallery_share_crf")) {
			// Migrate legacy CRF-only settings into the preset field.
			int crf = static_cast<int>(obs_data_get_int(saved_settings_data, "gallery_share_crf"));
			if (crf < 18)
				crf = 18;
			if (crf > 32)
				crf = 32;
			m_galleryShareCrf = crf;
			m_galleryShareCompressQuality = CompressQualityFromCrf(crf);
			obs_data_set_int(saved_settings_data, "gallery_share_compress_quality",
					 m_galleryShareCompressQuality);
		} else {
			m_galleryShareCompressQuality = kCompressQualityBalanced;
			m_galleryShareCrf = CrfForCompressQuality(m_galleryShareCompressQuality);
		}
		if (obs_data_has_user_value(saved_settings_data, "gallery_muted")) {
			m_galleryMuted = obs_data_get_bool(saved_settings_data, "gallery_muted");
		}
		if (obs_data_has_user_value(saved_settings_data, "gallery_volume")) {
			int volume = static_cast<int>(obs_data_get_int(saved_settings_data, "gallery_volume"));
			if (volume < 0)
				volume = 0;
			if (volume > 100)
				volume = 100;
			m_galleryVolume = volume;
		}
		if (obs_data_has_user_value(saved_settings_data, "overlay_background_alpha")) {
			double a = obs_data_get_double(saved_settings_data, "overlay_background_alpha");
			if (a >= 0.5 && a <= 1.0)
				m_overlayBackgroundAlpha = static_cast<float>(a);
		}
		if (obs_data_has_user_value(saved_settings_data, "overlay_scale")) {
			int scale = static_cast<int>(obs_data_get_int(saved_settings_data, "overlay_scale"));
			m_overlayScalePercent = ClampInt(scale, 50, 300);
		}
		if (obs_data_has_user_value(saved_settings_data, "overlay_text_scale")) {
			int textScale = static_cast<int>(obs_data_get_int(saved_settings_data, "overlay_text_scale"));
			m_textScalePercent = ClampInt(textScale, 50, 300);
		}
		if (obs_data_has_user_value(saved_settings_data, "overlay_accent_color")) {
			m_accentColorRgb = static_cast<int>(obs_data_get_int(saved_settings_data, "overlay_accent_color")) &
					   0xFFFFFF;
		}
		ApplyAccentBrushes();
	}

	// Clip-save hotkeys are polled (not RegisterHotKey) so bare keys stay usable in other apps.
	if (saved_settings_data) {
		for (int i = 0; i < CLIP_SAVE_SLOT_COUNT; i++) {
			const std::string secKey = "clip_slot_" + std::to_string(i) + "_seconds";
			const std::string showKey = "clip_slot_" + std::to_string(i) + "_show";
			if (obs_data_has_user_value(saved_settings_data, secKey.c_str())) {
				m_setClipSeconds[i] = ClampInt(
					static_cast<int>(obs_data_get_int(saved_settings_data, secKey.c_str())),
					OVERLAY_CLIP_SAVE_MIN_SECONDS, OVERLAY_CLIP_SAVE_MAX_SECONDS);
			}
			if (obs_data_has_user_value(saved_settings_data, showKey.c_str())) {
				m_setClipShow[i] = obs_data_get_bool(saved_settings_data, showKey.c_str());
			}
			const std::string vkKey = std::string("hotkey_clip_") + std::to_string(i) + "_vk";
			const std::string modKey = std::string("hotkey_clip_") + std::to_string(i) + "_mod";
			if (obs_data_has_user_value(saved_settings_data, vkKey.c_str())) {
				m_setHotkeyVk[9 + i] =
					static_cast<int>(obs_data_get_int(saved_settings_data, vkKey.c_str()));
			}
			if (obs_data_has_user_value(saved_settings_data, modKey.c_str())) {
				m_setHotkeyMod[9 + i] =
					static_cast<int>(obs_data_get_int(saved_settings_data, modKey.c_str()));
			}
		}
	}
	ApplyClipSaveHotkeys();
	EnsureExportInfrastructure();

	m_windowManager.SetClickHandler(
		[](int x, int y, void *userData) {
			OverlayRenderer *renderer = static_cast<OverlayRenderer *>(userData);
			if (renderer) {
				renderer->HandleClick(x, y);
			}
		},
		this);
	m_windowManager.SetRightClickHandler(
		[](int x, int y, void *userData) {
			OverlayRenderer *renderer = static_cast<OverlayRenderer *>(userData);
			if (renderer) {
				renderer->HandleRightClick(x, y);
			}
		},
		this);
	m_windowManager.SetDeferredHandler(
		[](void *userData) {
			OverlayRenderer *renderer = static_cast<OverlayRenderer *>(userData);
			if (renderer) {
				renderer->RunPendingOpenWith();
			}
		},
		this);
	m_windowManager.SetMouseDownHandler(
		[](int x, int y, void *userData) {
			OverlayRenderer *renderer = static_cast<OverlayRenderer *>(userData);
			if (renderer) {
				renderer->HandleMouseDown(x, y);
			}
		},
		this);
	m_windowManager.SetMouseUpHandler(
		[](int x, int y, void *userData) {
			OverlayRenderer *renderer = static_cast<OverlayRenderer *>(userData);
			if (renderer) {
				renderer->HandleMouseUp(x, y);
			}
		},
		this);

	// Mouse activity resets the auto-hide idle timer.
	m_windowManager.SetMouseMoveHandler(
		[](int x, int y, void *userData) {
			OverlayRenderer *renderer = static_cast<OverlayRenderer *>(userData);
			if (renderer) {
				renderer->HandleMouseMove(x, y);
			}
		},
		this);
	m_windowManager.SetMouseLeaveHandler(
		[](void *userData) {
			OverlayRenderer *renderer = static_cast<OverlayRenderer *>(userData);
			if (renderer) {
				renderer->HandleMouseLeave();
			}
		},
		this);

	obs_log(LOG_INFO, "OverlayRenderer initialized successfully");
	return true;
}

void OverlayRenderer::Shutdown()
{
	HWND hwnd = m_windowManager.GetHWND();
	if (hwnd && m_clipHotkeyTimer) {
		KillTimer(hwnd, m_clipHotkeyTimer);
		m_clipHotkeyTimer = 0;
	}
	if (hwnd && m_exportQueueTimer) {
		KillTimer(hwnd, m_exportQueueTimer);
		m_exportQueueTimer = 0;
	}

	if (m_updateTimer) {
		KillTimer(m_windowManager.GetHWND(), m_updateTimer);
		m_updateTimer = 0;
	}

	s_timerInstance = nullptr;

	ShutdownDirect2D();
	m_windowManager.Shutdown();

	m_videoPlayer.Shutdown();

	OleUninitialize();
	CoUninitialize();
}

bool OverlayRenderer::InitializeDirect2D()
{
	HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_wicFactory));
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "Failed to create WIC factory: 0x%08x", hr);
		return false;
	}

	hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_d2dFactory);
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "Failed to create D2D1 factory: 0x%08x", hr);
		return false;
	}

	hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
				 reinterpret_cast<IUnknown **>(&m_writeFactory));
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "Failed to create DirectWrite factory: 0x%08x", hr);
		return false;
	}

	// Device resources are created lazily when the window is first shown.
	return true;
}

void OverlayRenderer::ShutdownDirect2D()
{
	DiscardDeviceResources();

	if (m_wicBitmap) {
		m_wicBitmap->Release();
		m_wicBitmap = nullptr;
	}

	if (m_wicFactory) {
		m_wicFactory->Release();
		m_wicFactory = nullptr;
	}

	if (m_writeFactory) {
		m_writeFactory->Release();
		m_writeFactory = nullptr;
	}

	if (m_d2dFactory) {
		m_d2dFactory->Release();
		m_d2dFactory = nullptr;
	}
}

void OverlayRenderer::CreateDeviceResources()
{
	if (m_bitmapRenderTarget) {
		return;
	}

	HWND hwnd = m_windowManager.GetHWND();
	if (!hwnd) {
		return;
	}

	RECT clientRect;
	GetClientRect(hwnd, &clientRect);

	int width = clientRect.right - clientRect.left;
	int height = clientRect.bottom - clientRect.top;

	if (width <= 0 || height <= 0) {
		return;
	}

	D2D1_SIZE_U size = D2D1::SizeU(width, height);

	HRESULT hr = m_wicFactory->CreateBitmap(size.width, size.height, GUID_WICPixelFormat32bppPBGRA,
						WICBitmapCacheOnLoad, &m_wicBitmap);
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "Failed to create WIC bitmap: 0x%08x", hr);
		return;
	}

	D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
		D2D1_RENDER_TARGET_TYPE_DEFAULT,
		D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

	hr = m_d2dFactory->CreateWicBitmapRenderTarget(m_wicBitmap, rtProps, &m_bitmapRenderTarget);
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "Failed to create bitmap render target: 0x%08x", hr);
		return;
	}

	// Hwnd target is kept for API compatibility; layered output uses the WIC bitmap target.
	hr = m_d2dFactory->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
						  D2D1::HwndRenderTargetProperties(hwnd, size), &m_renderTarget);

	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "Failed to create Hwnd render target: 0x%08x", hr);
	}

	ID2D1RenderTarget *target = m_bitmapRenderTarget ? m_bitmapRenderTarget : m_renderTarget;
	if (!target) {
		return;
	}

	// Theme: bg #141419, accent (user-selectable), border #3a3a45
	D2D1_COLOR_F bgColor = D2D1::ColorF(20.0f / 255.0f, 20.0f / 255.0f, 25.0f / 255.0f, 1.0f);
	target->CreateSolidColorBrush(bgColor, &m_backgroundBrush);

	D2D1_COLOR_F textColor = D2D1::ColorF(1.0f, 1.0f, 1.0f);
	target->CreateSolidColorBrush(textColor, &m_textBrush);

	D2D1_COLOR_F activeColor = AccentColor();
	target->CreateSolidColorBrush(activeColor, &m_activeBrush);

	D2D1_COLOR_F borderColor = D2D1::ColorF(58.0f / 255.0f, 58.0f / 255.0f, 69.0f / 255.0f, 1.0f);
	target->CreateSolidColorBrush(borderColor, &m_borderBrush);

	target->CreateSolidColorBrush(activeColor, &m_statsTitleBrush);

	m_writeFactory->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
					 DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"", &m_statusTextFormat);

	m_writeFactory->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
					 DWRITE_FONT_STRETCH_NORMAL, 10.0f, L"", &m_buttonTextFormat);

	m_writeFactory->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
					 DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"", &m_replayActiveTextFormat);

	m_writeFactory->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
					 DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"", &m_statsTitleFormat);

	m_writeFactory->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
					 DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"", &m_statsValueFormat);

	if (m_statusTextFormat) {
		m_statusTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
		m_statusTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
	}

	if (m_buttonTextFormat) {
		m_buttonTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
		m_buttonTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
	}

	if (m_replayActiveTextFormat) {
		m_replayActiveTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
		m_replayActiveTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
	}

	if (m_statsTitleFormat) {
		m_statsTitleFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
		m_statsTitleFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
	}

	if (m_statsValueFormat) {
		m_statsValueFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
		m_statsValueFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
	}

	// Fallback play icon when PNG assets are unavailable.
	if (m_d2dFactory && !m_iconPlayGeometry) {
		ID2D1PathGeometry *path = nullptr;
		if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(&path)) && path) {
			ID2D1GeometrySink *sink = nullptr;
			if (SUCCEEDED(path->Open(&sink)) && sink) {
				D2D1_POINT_2F p0 = D2D1::Point2F(0.15f, 0.2f);
				D2D1_POINT_2F p1 = D2D1::Point2F(0.15f, 0.8f);
				D2D1_POINT_2F p2 = D2D1::Point2F(0.85f, 0.5f);
				sink->BeginFigure(p0, D2D1_FIGURE_BEGIN_FILLED);
				sink->AddLine(p1);
				sink->AddLine(p2);
				sink->EndFigure(D2D1_FIGURE_END_CLOSED);
				sink->Close();
				sink->Release();
			}
			m_iconPlayGeometry = path;
		}
	}
}

void OverlayRenderer::DiscardDeviceResources()
{
	if (m_statsValueFormat) {
		m_statsValueFormat->Release();
		m_statsValueFormat = nullptr;
	}
	if (m_statsTitleFormat) {
		m_statsTitleFormat->Release();
		m_statsTitleFormat = nullptr;
	}
	if (m_replayActiveTextFormat) {
		m_replayActiveTextFormat->Release();
		m_replayActiveTextFormat = nullptr;
	}
	if (m_buttonTextFormat) {
		m_buttonTextFormat->Release();
		m_buttonTextFormat = nullptr;
	}
	if (m_statusTextFormat) {
		m_statusTextFormat->Release();
		m_statusTextFormat = nullptr;
	}

	if (m_statsTitleBrush) {
		m_statsTitleBrush->Release();
		m_statsTitleBrush = nullptr;
	}
	if (m_borderBrush) {
		m_borderBrush->Release();
		m_borderBrush = nullptr;
	}
	if (m_activeBrush) {
		m_activeBrush->Release();
		m_activeBrush = nullptr;
	}
	if (m_textBrush) {
		m_textBrush->Release();
		m_textBrush = nullptr;
	}
	if (m_backgroundBrush) {
		m_backgroundBrush->Release();
		m_backgroundBrush = nullptr;
	}

	if (m_galleryVideoBitmap) {
		m_galleryVideoBitmap->Release();
		m_galleryVideoBitmap = nullptr;
	}
	m_galleryVideoBitmapW = 0;
	m_galleryVideoBitmapH = 0;

	if (m_iconPlayGeometry) {
		m_iconPlayGeometry->Release();
		m_iconPlayGeometry = nullptr;
	}
	ReleaseIconBitmaps();

	if (m_bitmapRenderTarget) {
		m_bitmapRenderTarget->Release();
		m_bitmapRenderTarget = nullptr;
	}

	if (m_renderTarget) {
		m_renderTarget->Release();
		m_renderTarget = nullptr;
	}

	if (m_wicBitmap) {
		m_wicBitmap->Release();
		m_wicBitmap = nullptr;
	}
}

void OverlayRenderer::Show()
{
	m_state.recordingPathConfigured = m_stateManager.HasRecordingPath();

	if (m_galleryOpen) {
		ApplyGalleryWindowSize();
		SetUpdateInterval(16);
	} else if (m_settingsOpen) {
		ApplySettingsWindowSize();
		SetUpdateInterval(16);
	} else {
		ApplyWindowGeometry();
		SetUpdateInterval(500);
	}

	CreateDeviceResources();

	m_windowManager.Show();
	overlay_indicators_set_overlay_visible(true);

	if (!m_updateTimer) {
		s_timerInstance = this;
		m_updateTimer = SetTimer(m_windowManager.GetHWND(), 1, m_updateIntervalMs, TimerProc);
	}
	m_lastInteractionTick = GetTickCount64();

	UpdateStatus();
	Render();
}

void OverlayRenderer::Hide()
{
	if (m_returnHomeOnHide) {
		if (m_settingsOpen)
			CloseSettings();
		else if (m_galleryOpen)
			CloseGallery();
		m_replayClipPickerOpen = false;
	}

	if (m_updateTimer) {
		KillTimer(m_windowManager.GetHWND(), m_updateTimer);
		m_updateTimer = 0;
	}

	// Overlay update timer stops on hide; keep a dedicated poll so export toasts still fire.
	SyncExportQueueTimer();

	m_windowManager.Hide();
	overlay_indicators_set_overlay_visible(false);
}

bool OverlayRenderer::IsVisible() const
{
	return m_windowManager.IsVisible();
}

void OverlayRenderer::SetPosition(OverlayPosition position, int margin)
{
	m_position = position;
	m_margin = margin;

	if (IsVisible()) {
		ApplyPosition();
	}
}

void OverlayRenderer::SetOrientation(OverlayOrientation orientation)
{
	m_orientation = orientation;

	if (IsVisible()) {
		ApplyPosition();
	}
}

void OverlayRenderer::ApplyPosition()
{
	ApplyWindowGeometry();

	Render();
}

void OverlayRenderer::ApplyWindowGeometry()
{
	m_layoutManager.CalculateLayout(m_position, m_margin, m_orientation, m_state.statsVisible,
					m_overlayScalePercent, m_layout);

	const RECT workArea = GetTargetWorkArea();
	if (IsDimmingActive()) {
		POINT normalPos = m_layoutManager.CalculateWindowPosition(m_position, m_margin, m_layout, workArea);
		ApplyDimmingWindowGeometry(normalPos, m_layout.windowWidth, m_layout.windowHeight);
	} else {
		m_dimContentOffset = {0, 0};
		POINT windowPos = m_layoutManager.CalculateWindowPosition(m_position, m_margin, m_layout, workArea);
		m_windowManager.SetPosition(windowPos, m_layout.windowWidth, m_layout.windowHeight);
	}
}

RECT OverlayRenderer::GetTargetWorkArea() const
{
	return GetTargetWorkArea(m_monitorMode, m_monitorIndex);
}

RECT OverlayRenderer::GetTargetWorkArea(OverlayMonitorMode mode, int monitorIndex) const
{
	return m_layoutManager.ResolveWorkArea(mode, monitorIndex);
}

std::wstring OverlayRenderer::FormatMonitorLabel(int monitorIndex) const
{
	RECT workArea = {};
	RECT monitorBounds = {};
	if (!m_layoutManager.GetMonitorInfoByIndex(monitorIndex, workArea, monitorBounds)) {
		return overlay::util::ModuleTextW("Monitor.Unknown");
	}

	const int width = monitorBounds.right - monitorBounds.left;
	const int height = monitorBounds.bottom - monitorBounds.top;
	return overlay::util::ModuleTextW("Monitor.N") + L" " + std::to_wstring(monitorIndex + 1) + L" (" +
	       std::to_wstring(width) + L"x" + std::to_wstring(height) + L")";
}

void OverlayRenderer::ApplyDimmingWindowGeometry(POINT contentScreenTopLeft, int contentWidth,
					       int contentHeight)
{
	const POINT contentCenter = {contentScreenTopLeft.x + contentWidth / 2,
				       contentScreenTopLeft.y + contentHeight / 2};
	const RECT monitorBounds = m_layoutManager.GetMonitorBoundsForPoint(contentCenter);
	m_dimContentOffset.x = contentScreenTopLeft.x - monitorBounds.left;
	m_dimContentOffset.y = contentScreenTopLeft.y - monitorBounds.top;
	m_windowManager.SetPosition(POINT{monitorBounds.left, monitorBounds.top},
				    monitorBounds.right - monitorBounds.left,
				    monitorBounds.bottom - monitorBounds.top);
}

bool OverlayRenderer::IsDimmingActive() const
{
	return m_captureFocus;
}

POINT OverlayRenderer::ClientToLayoutPoint(int x, int y) const
{
	if (IsDimmingActive()) {
		return {x - m_dimContentOffset.x, y - m_dimContentOffset.y};
	}
	return {x, y};
}

void OverlayRenderer::SyncExportQueueTimer()
{
	HWND hwnd = m_windowManager.GetHWND();
	if (!hwnd)
		return;

	const bool need = m_exportInfraReady && m_exportQueue.IsBusy();
	if (need) {
		s_timerInstance = this;
		if (!m_exportQueueTimer)
			m_exportQueueTimer = SetTimer(hwnd, kExportQueueTimerId, kExportQueuePollMs, TimerProc);
	} else if (m_exportQueueTimer) {
		KillTimer(hwnd, m_exportQueueTimer);
		m_exportQueueTimer = 0;
	}
}

void OverlayRenderer::PollExportQueue()
{
	if (m_exportInfraReady)
		m_exportQueue.Tick();
	SyncExportQueueTimer();
}

void OverlayRenderer::UpdateStatus()
{
	if (m_exportInfraReady)
		m_exportQueue.Tick();
	SyncExportQueueTimer();

	m_stateManager.UpdateStatus(m_state);
	UpdateAutoHide();
	if (m_galleryOpen) {
		m_videoPlayer.Update();
		if (!m_galleryTrimUserSet) {
			double duration = m_videoPlayer.GetDuration();
			if (duration > 0.0) {
				m_galleryTrimOut = duration;
			}
		}
	}
}

void OverlayRenderer::Render()
{
	if (!m_bitmapRenderTarget) {
		CreateDeviceResources();
		if (!m_bitmapRenderTarget) {
			return;
		}
	}

	HWND hwnd = m_windowManager.GetHWND();
	if (!hwnd) {
		return;
	}

	// Re-assert topmost so borderless games do not cover the overlay.
	if (m_windowManager.IsVisible()) {
		m_windowManager.EnsureTopmost();
		overlay_indicators_raise_active_topmost();
		overlay_toasts_raise_active_topmost();
	}

	RECT clientRect;
	GetClientRect(hwnd, &clientRect);
	int width = clientRect.right - clientRect.left;
	int height = clientRect.bottom - clientRect.top;

	if (width <= 0 || height <= 0) {
		return;
	}

	if (m_wicBitmap) {
		UINT bitmapWidth, bitmapHeight;
		m_wicBitmap->GetSize(&bitmapWidth, &bitmapHeight);
		if (bitmapWidth != (UINT)width || bitmapHeight != (UINT)height) {
			DiscardDeviceResources();
			CreateDeviceResources();
			if (!m_bitmapRenderTarget) {
				return;
			}
		}
	}

	if (m_settingsOpen && m_settingsPage == SettingsPage::Indicators) {
		for (int i = 0; i < INDICATOR_CUSTOM_SLOT_COUNT; i++)
			EnsureIndicatorPreview(i);
	}

	m_bitmapRenderTarget->BeginDraw();
	m_bitmapRenderTarget->SetTransform(D2D1::Matrix3x2F::Identity());

	m_bitmapRenderTarget->Clear(D2D1::ColorF(0, 0, 0, 0));

	if (IsDimmingActive()) {
		RenderDimBackground(width, height);
		m_bitmapRenderTarget->SetTransform(D2D1::Matrix3x2F::Translation(
			static_cast<float>(m_dimContentOffset.x), static_cast<float>(m_dimContentOffset.y)));
	}

	if (m_settingsOpen) {
		RenderSettingsPanel();
	} else if (m_galleryOpen) {
		RenderGallery();
	} else {
		RenderMainOverlay();
		RenderSettingsContainer();
		if (m_state.statsVisible) {
			RenderStatsContainer();
		}
		RenderPanels();
		RenderButtons();
	}

	if (IsDimmingActive()) {
		m_bitmapRenderTarget->SetTransform(D2D1::Matrix3x2F::Identity());
	}

	HRESULT hr = m_bitmapRenderTarget->EndDraw();

	if (hr == D2DERR_RECREATE_TARGET) {
		DiscardDeviceResources();
		CreateDeviceResources();
		Render();
		return;
	}

	if (SUCCEEDED(hr) && m_wicBitmap) {
		IWICBitmapLock *pLock = nullptr;
		WICRect rect = {0, 0, width, height};
		hr = m_wicBitmap->Lock(&rect, WICBitmapLockRead, &pLock);

		if (SUCCEEDED(hr)) {
			UINT bufferSize = 0;
			BYTE *pData = nullptr;
			UINT stride = 0;

			hr = pLock->GetDataPointer(&bufferSize, &pData);
			if (SUCCEEDED(hr)) {
				pLock->GetStride(&stride);

				POINT ptSrc = {0, 0};
				POINT ptDst;
				RECT windowRect;
				GetWindowRect(hwnd, &windowRect);
				ptDst.x = windowRect.left;
				ptDst.y = windowRect.top;
				SIZE sizeWnd = {width, height};

				HDC hdcScreen = GetDC(NULL);
				BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};

				HDC hdcMem = CreateCompatibleDC(hdcScreen);
				BITMAPINFO bmi = {0};
				bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
				bmi.bmiHeader.biWidth = width;
				bmi.bmiHeader.biHeight = -height;
				bmi.bmiHeader.biPlanes = 1;
				bmi.bmiHeader.biBitCount = 32;
				bmi.bmiHeader.biCompression = BI_RGB;

				BYTE *pBits = nullptr;
				int dibStride = width * 4;
				HBITMAP hBitmap =
					CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, (void **)&pBits, NULL, 0);
				if (hBitmap) {
					HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);

					// WIC and DIB strides may differ, so copy row by row.
					BYTE *pSrc = pData;
					BYTE *pDst = pBits;
					for (int y = 0; y < height; y++) {
						memcpy(pDst, pSrc, width * 4);
						pSrc += stride;
						pDst += dibStride;
					}

					UpdateLayeredWindow(hwnd, hdcScreen, &ptDst, &sizeWnd, hdcMem, &ptSrc, 0,
							    &blend, ULW_ALPHA);

					SelectObject(hdcMem, hOldBitmap);
					DeleteObject(hBitmap);
				}

				DeleteDC(hdcMem);
				ReleaseDC(NULL, hdcScreen);
			}

			pLock->Release();
		}
	}
}

void OverlayRenderer::RenderDimBackground(int width, int height)
{
	ID2D1RenderTarget *target = m_bitmapRenderTarget ? m_bitmapRenderTarget : m_renderTarget;
	if (!target || !m_backgroundBrush) {
		return;
	}

	D2D1_RECT_F dimRect = D2D1::RectF(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
	m_backgroundBrush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.55f));
	target->FillRectangle(dimRect, m_backgroundBrush);
}

void OverlayRenderer::RenderMainOverlay()
{
	if (!m_bitmapRenderTarget)
		return;
	const float overlayAlpha = m_overlayBackgroundAlpha;
	DrawRoundedRectangle(m_layout.mainOverlayRect, 16.0f,
			     D2D1::ColorF(20.0f / 255.0f, 20.0f / 255.0f, 25.0f / 255.0f, overlayAlpha));
	// Border improves readability when the panel is semi-transparent.
	if (overlayAlpha < 1.0f) {
		DrawRoundedRectangleBorder(m_layout.mainOverlayRect, 16.0f, 1.0f,
					   D2D1::ColorF(58.0f / 255.0f, 58.0f / 255.0f, 69.0f / 255.0f, 0.6f));
	}
}

void OverlayRenderer::RenderSettingsContainer()
{
	if (m_layout.settingsRect.right <= 0) {
		return;
	}
	const D2D1_COLOR_F hoverColor = AccentHover();
	const D2D1_COLOR_F hoverBgColor = AccentHover(0.35f);

	DrawRoundedRectangle(m_layout.settingsRect, 10.0f,
			     D2D1::ColorF(20.0f / 255.0f, 20.0f / 255.0f, 25.0f / 255.0f, 1.0f));

	DrawRoundedRectangleBorder(m_layout.settingsRect, 10.0f, 1.0f,
				   D2D1::ColorF(58.0f / 255.0f, 58.0f / 255.0f, 69.0f / 255.0f, 1.0f));

	RECT settingsIconRect = m_layout.settingsButtonRect;
	const bool hoverSettings = (m_hoverTarget == HoverTarget::SettingsButton);
	if (hoverSettings) {
		DrawRoundedRectangle(settingsIconRect, 6.0f, hoverBgColor);
	}
	{
		D2D1_COLOR_F iconColor = hoverSettings ? hoverColor : D2D1::ColorF(0.78f, 0.78f, 0.78f);
		DrawGalleryIcon(settingsIconRect, 10, &iconColor);
	}

	RECT statsIconRect = m_layout.statsButtonRect;
	const bool hoverStats = (m_hoverTarget == HoverTarget::StatsButton);
	if (hoverStats && !m_state.statsVisible) {
		DrawRoundedRectangle(statsIconRect, 6.0f, hoverBgColor);
	}
	{
		D2D1_COLOR_F iconColor = m_state.statsVisible
						 ? hoverColor
						 : (hoverStats ? hoverColor : D2D1::ColorF(0.78f, 0.78f, 0.78f));
		DrawGalleryIcon(statsIconRect, 11, &iconColor);
	}
}

void OverlayRenderer::RenderStatsContainer()
{
	if (m_layout.statsRect.right <= 0) {
		return;
	}

	DrawRoundedRectangle(m_layout.statsRect, 12.0f,
			     D2D1::ColorF(20.0f / 255.0f, 20.0f / 255.0f, 25.0f / 255.0f, 1.0f));

	DrawRoundedRectangleBorder(m_layout.statsRect, 12.0f, 1.0f,
				   D2D1::ColorF(58.0f / 255.0f, 58.0f / 255.0f, 69.0f / 255.0f, 1.0f));

	int padding = UiPx(10);
	int lineHeight = UiPx(16);
	int startY = m_layout.statsRect.top + UiPx(8);

	RECT titleRect = {m_layout.statsRect.left + padding, startY, m_layout.statsRect.right - padding,
			  startY + lineHeight};
	RenderText(overlay::util::ModuleTextW("Stats"), titleRect, 12.0f * FontScale(), DWRITE_FONT_WEIGHT_BOLD,
		   AccentColor(), DWRITE_TEXT_ALIGNMENT_LEADING);

	std::wostringstream fpsText;
	fpsText << overlay::util::ModuleTextW("Stats.Fps") << L" " << std::fixed << std::setprecision(1) << m_state.fps;
	RECT fpsRect = {m_layout.statsRect.left + padding, startY + lineHeight + UiPx(4),
			m_layout.statsRect.right - padding, startY + lineHeight * 2 + UiPx(4)};
	RenderText(fpsText.str(), fpsRect, 11.0f * FontScale(), DWRITE_FONT_WEIGHT_NORMAL, D2D1::ColorF(1.0f, 1.0f, 1.0f),
		   DWRITE_TEXT_ALIGNMENT_LEADING);

	std::wostringstream droppedText;
	droppedText << overlay::util::ModuleTextW("Stats.Dropped") << L" " << m_state.droppedFrames;
	RECT droppedRect = {m_layout.statsRect.left + padding, startY + lineHeight * 2 + UiPx(8),
			    m_layout.statsRect.right - padding, startY + lineHeight * 3 + UiPx(8)};
	RenderText(droppedText.str(), droppedRect, 11.0f * FontScale(), DWRITE_FONT_WEIGHT_NORMAL,
		   D2D1::ColorF(1.0f, 1.0f, 1.0f), DWRITE_TEXT_ALIGNMENT_LEADING);

	std::wstring freeSpaceW = overlay::util::Utf8ToWide(m_state.freeSpaceText);
	RECT freeRect = {m_layout.statsRect.left + padding, startY + lineHeight * 3 + UiPx(12),
			 m_layout.statsRect.right - padding, startY + lineHeight * 4 + UiPx(12)};
	RenderText(freeSpaceW, freeRect, 11.0f * FontScale(), DWRITE_FONT_WEIGHT_NORMAL, D2D1::ColorF(1.0f, 1.0f, 1.0f),
		   DWRITE_TEXT_ALIGNMENT_LEADING);
}

void OverlayRenderer::RenderPanels()
{
	RECT recRect = m_layout.recordingPanelRect;
	const bool hoverRecording = (m_hoverTarget == HoverTarget::RecordingPanel);
	const D2D1_COLOR_F hoverColor = AccentHover();

	float centerX = (recRect.left + recRect.right) / 2.0f;
	float centerY = recRect.top + 30.0f * UiScale();
	float radius = 18.0f * UiScale();

	D2D1_ELLIPSE ellipse = D2D1::Ellipse(D2D1::Point2F(centerX, centerY), radius, radius);

	ID2D1RenderTarget *target = m_bitmapRenderTarget ? m_bitmapRenderTarget : m_renderTarget;
	if (m_state.recordingActive || hoverRecording) {
		target->FillEllipse(ellipse, m_activeBrush);
	} else {
		// RenderText may leave m_textBrush tinted; reset before drawing the idle dot.
		m_textBrush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
		target->FillEllipse(ellipse, m_textBrush);
	}

	RECT statusRect = {recRect.left, (int)(centerY + radius + 10.0f * UiScale()), recRect.right, recRect.bottom};
	std::wstring statusText = m_state.recordingActive ? overlay::util::ModuleTextW("Recording")
							  : overlay::util::ModuleTextW("NotRecording");
	D2D1_COLOR_F statusColor = (m_state.recordingActive || hoverRecording)
					   ? (m_state.recordingActive ? AccentColor() : hoverColor)
					   : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
	RenderText(statusText, statusRect, 13.0f * FontScale(), DWRITE_FONT_WEIGHT_NORMAL, statusColor);

	if (m_state.recordingActive || hoverRecording) {
		const int borderPad = UiPx(3);
		RECT borderRect = {recRect.left - borderPad, recRect.top - borderPad, recRect.right + borderPad,
				   recRect.bottom + borderPad};
		DrawRoundedRectangleBorder(borderRect, 10.0f * UiScale(), 3.0f * UiScale(), AccentColor());
	}

	RECT repRect = m_layout.replayPanelRect;
	const bool hoverReplay = (m_hoverTarget == HoverTarget::ReplayPanel);
	const D2D1_COLOR_F hoverReplayColor = AccentHover();

	if (m_state.replayActive) {
		const int borderPad = UiPx(3);
		RECT borderRect = {repRect.left - borderPad, repRect.top - borderPad, repRect.right + borderPad,
				   repRect.bottom + borderPad};
		DrawRoundedRectangleBorder(borderRect, 10.0f * UiScale(), 3.0f * UiScale(), AccentColor());

		const int inset = UiPx(5);
		ID2D1RenderTarget *target = m_bitmapRenderTarget ? m_bitmapRenderTarget : m_renderTarget;

		m_replayClipOptionRects.clear();
		m_replayClipOptionSeconds.clear();
		m_replayClipOptionEnabled.clear();
		m_replaySaveRect = RECT{0, 0, 0, 0};
		m_replayClipChevronRect = RECT{0, 0, 0, 0};

		std::vector<int> visibleSeconds;
		for (int i = 0; i < CLIP_SAVE_SLOT_COUNT; i++) {
			if (m_setClipShow[i])
				visibleSeconds.push_back(m_setClipSeconds[i]);
		}
		const int presetCount = static_cast<int>(visibleSeconds.size());
		if (presetCount <= 0)
			m_replayClipPickerOpen = false;

		if (m_state.replaySaving) {
			m_replayClipPickerOpen = false;
			int halfHeight = (repRect.bottom - repRect.top) / 2;
			RECT upperRect = {repRect.left + inset, repRect.top + inset, repRect.right - inset,
					  repRect.top + halfHeight - inset};
			RECT lowerRect = {repRect.left + inset, repRect.top + halfHeight + inset, repRect.right - inset,
					  repRect.bottom - inset};
			RenderText(overlay::util::ModuleTextW("Replay.TurnOff"), upperRect, 11.0f * FontScale(),
				   DWRITE_FONT_WEIGHT_NORMAL, AccentColor());
			target->DrawLine(D2D1::Point2F((float)(repRect.left + inset), (float)(repRect.top + halfHeight)),
					 D2D1::Point2F((float)(repRect.right - inset), (float)(repRect.top + halfHeight)),
					 m_activeBrush, 1.0f * UiScale());
			m_replaySaveRect = lowerRect;
			RenderText(overlay::util::ModuleTextW("Saving"), lowerRect, 11.0f * FontScale(),
				   DWRITE_FONT_WEIGHT_NORMAL, D2D1::ColorF(0.6f, 0.6f, 0.6f, 1.0f));
		} else if (m_replayClipPickerOpen && presetCount > 0) {
			// Vertical swap: whole panel becomes preset list + compact back strip.
			const int backH = std::max(UiPx(22), static_cast<int>(repRect.bottom - repRect.top) / 5);
			RECT backRect = {repRect.left + inset, repRect.top + inset, repRect.right - inset,
					 repRect.top + inset + backH};
			m_replayClipChevronRect = backRect;
			const bool hoverBack = (m_hoverTarget == HoverTarget::ReplayClipChevron);
			RenderText(L"▴", backRect, 14.0f * FontScale(), DWRITE_FONT_WEIGHT_BOLD,
				   hoverBack ? AccentHover() : AccentColor());

			target->DrawLine(D2D1::Point2F((float)(repRect.left + inset), (float)backRect.bottom),
					 D2D1::Point2F((float)(repRect.right - inset), (float)backRect.bottom),
					 m_activeBrush, 1.0f * UiScale());

			const int listTop = backRect.bottom;
			const int listBottom = repRect.bottom - inset;
			const int listH = std::max(1, listBottom - listTop);
			const int rowH = listH / presetCount;
			const int bufferLen = overlay_clip_save_get_buffer_length();

			for (int i = 0; i < presetCount; i++) {
				const int top = listTop + i * rowH;
				const int bottom = (i == presetCount - 1) ? listBottom : (top + rowH);
				RECT optRect = {repRect.left + inset, top, repRect.right - inset, bottom};
				const int seconds = visibleSeconds[i];
				const bool enabled = bufferLen <= 0 || seconds <= bufferLen;
				m_replayClipOptionRects.push_back(optRect);
				m_replayClipOptionSeconds.push_back(seconds);
				m_replayClipOptionEnabled.push_back(enabled);

				if (i > 0) {
					target->DrawLine(D2D1::Point2F((float)(repRect.left + inset), (float)top),
							 D2D1::Point2F((float)(repRect.right - inset), (float)top),
							 m_activeBrush, 1.0f * UiScale());
				}

				RenderText(FormatClipDurationLabel(seconds), optRect, 13.0f * FontScale(),
					   DWRITE_FONT_WEIGHT_NORMAL,
					   enabled ? AccentColor() : D2D1::ColorF(0.45f, 0.45f, 0.48f, 1.0f));
			}
		} else {
			int halfHeight = (repRect.bottom - repRect.top) / 2;
			RECT upperRect = {repRect.left + inset, repRect.top + inset, repRect.right - inset,
					  repRect.top + halfHeight - inset};
			RECT lowerRect = {repRect.left + inset, repRect.top + halfHeight + inset, repRect.right - inset,
					  repRect.bottom - inset};

			RenderText(overlay::util::ModuleTextW("Replay.TurnOff"), upperRect, 11.0f * FontScale(),
				   DWRITE_FONT_WEIGHT_NORMAL, AccentColor());
			target->DrawLine(D2D1::Point2F((float)(repRect.left + inset), (float)(repRect.top + halfHeight)),
					 D2D1::Point2F((float)(repRect.right - inset), (float)(repRect.top + halfHeight)),
					 m_activeBrush, 1.0f * UiScale());

			const bool hoverSave = (m_hoverTarget == HoverTarget::ReplaySaveButton);
			const bool hoverChevron = (m_hoverTarget == HoverTarget::ReplayClipChevron);

			if (presetCount <= 0) {
				m_replaySaveRect = lowerRect;
				RenderText(overlay::util::ModuleTextW("Save"), m_replaySaveRect, 11.0f * FontScale(),
					   DWRITE_FONT_WEIGHT_NORMAL, hoverSave ? AccentHover() : AccentColor());
			} else {
				const int chevronW = UiPx(34);
				m_replaySaveRect = {lowerRect.left, lowerRect.top, lowerRect.right - chevronW,
						    lowerRect.bottom};
				m_replayClipChevronRect = {lowerRect.right - chevronW, lowerRect.top, lowerRect.right,
							   lowerRect.bottom};
				RenderText(overlay::util::ModuleTextW("Save"), m_replaySaveRect, 11.0f * FontScale(),
					   DWRITE_FONT_WEIGHT_NORMAL, hoverSave ? AccentHover() : AccentColor());
				RenderText(L"▾", m_replayClipChevronRect, 16.0f * FontScale(), DWRITE_FONT_WEIGHT_BOLD,
					   hoverChevron ? AccentHover() : AccentColor());
				target->DrawLine(
					D2D1::Point2F((float)m_replayClipChevronRect.left, (float)(lowerRect.top + UiPx(6))),
					D2D1::Point2F((float)m_replayClipChevronRect.left,
						      (float)(lowerRect.bottom - UiPx(6))),
					m_activeBrush, 1.0f * UiScale());
			}
		}
	} else {
		m_replayClipPickerOpen = false;
		m_replaySaveRect = RECT{0, 0, 0, 0};
		m_replayClipChevronRect = RECT{0, 0, 0, 0};
		m_replayClipOptionRects.clear();
		m_replayClipOptionSeconds.clear();
		m_replayClipOptionEnabled.clear();
		float centerX = (repRect.left + repRect.right) / 2.0f;
		float centerY = repRect.top + 30.0f * UiScale();
		std::wstring replayIcon = L"⟲";
		const int iconHalf = UiPx(20);
		RECT iconRect = {(int)(centerX - iconHalf), (int)(centerY - iconHalf), (int)(centerX + iconHalf),
				 (int)(centerY + iconHalf)};
		RenderText(replayIcon, iconRect, 36.0f * FontScale(), DWRITE_FONT_WEIGHT_NORMAL,
			   hoverReplay ? hoverReplayColor : D2D1::ColorF(1.0f, 1.0f, 1.0f));

		RECT statusRect = {repRect.left, (int)(centerY + 20.0f * UiScale()), repRect.right, repRect.bottom};
		RenderText(overlay::util::ModuleTextW("Off"), statusRect, 13.0f * FontScale(), DWRITE_FONT_WEIGHT_NORMAL,
			   hoverReplay ? hoverReplayColor : D2D1::ColorF(1.0f, 1.0f, 1.0f));
	}
}

void OverlayRenderer::RenderButtons()
{
	RECT folderRect = m_layout.recordFolderButtonRect;
	const bool galleryAvailable = m_state.recordingPathConfigured;
	const bool hoverFolder = galleryAvailable && (m_hoverTarget == HoverTarget::RecordFolderButton);
	const D2D1_COLOR_F hoverColor = AccentHover();
	const D2D1_COLOR_F disabledColor = D2D1::ColorF(0.45f, 0.45f, 0.45f, 1.0f);
	const D2D1_COLOR_F folderColor =
		!galleryAvailable ? disabledColor : (hoverFolder ? hoverColor : D2D1::ColorF(1.0f, 1.0f, 1.0f));
	RenderText(overlay::util::ModuleTextW("Gallery"), folderRect, 10.0f * FontScale(), DWRITE_FONT_WEIGHT_NORMAL,
		   folderColor);

	RECT obsRect = m_layout.showOBSButtonRect;
	const bool hoverShowObs = (m_hoverTarget == HoverTarget::ShowOBSButton);
	bool obsVisible = false;
#ifdef ENABLE_QT
	if (QWidget *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window()))
		obsVisible = mainWindow->isVisible() && !mainWindow->isMinimized();
#else
	if (HWND obsHwnd = (HWND)obs_frontend_get_main_window())
		obsVisible = IsWindow(obsHwnd) && IsWindowVisible(obsHwnd) && !IsIconic(obsHwnd);
#endif
	RenderText(overlay::util::ModuleTextW(obsVisible ? "HideObs" : "ShowObs"), obsRect, 10.0f * FontScale(),
		   DWRITE_FONT_WEIGHT_NORMAL, hoverShowObs ? hoverColor : D2D1::ColorF(1.0f, 1.0f, 1.0f));
}

void OverlayRenderer::RenderText(const std::wstring &text, const RECT &rect, float fontSize, DWRITE_FONT_WEIGHT weight,
				 D2D1_COLOR_F color, DWRITE_TEXT_ALIGNMENT alignment)
{
	ID2D1RenderTarget *target = m_bitmapRenderTarget ? m_bitmapRenderTarget : m_renderTarget;
	if (!target || !m_writeFactory) {
		return;
	}

	IDWriteTextFormat *textFormat = nullptr;

	if (fontSize == 13.0f) {
		textFormat = m_statusTextFormat;
		if (textFormat) {
			textFormat->SetTextAlignment(alignment);
		}
	} else if (fontSize == 10.0f) {
		textFormat = m_buttonTextFormat;
		if (textFormat) {
			textFormat->SetTextAlignment(alignment);
		}
	} else if (fontSize == 11.0f && weight == DWRITE_FONT_WEIGHT_NORMAL &&
		   alignment == DWRITE_TEXT_ALIGNMENT_CENTER) {
		textFormat = m_replayActiveTextFormat;
	} else if (fontSize == 12.0f && weight == DWRITE_FONT_WEIGHT_BOLD) {
		textFormat = m_statsTitleFormat;
	} else if (fontSize == 11.0f && weight == DWRITE_FONT_WEIGHT_NORMAL &&
		   alignment == DWRITE_TEXT_ALIGNMENT_LEADING) {
		textFormat = m_statsValueFormat;
	} else {
		m_writeFactory->CreateTextFormat(L"Segoe UI", NULL, weight, DWRITE_FONT_STYLE_NORMAL,
						 DWRITE_FONT_STRETCH_NORMAL, fontSize, L"", &textFormat);
		if (textFormat) {
			textFormat->SetTextAlignment(alignment);
			textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
		}
	}

	if (!textFormat) {
		return;
	}

	ID2D1SolidColorBrush *brush = nullptr;
	const D2D1_COLOR_F accent = AccentColor();
	if (std::fabs(color.r - accent.r) < 0.02f && std::fabs(color.g - accent.g) < 0.02f &&
	    std::fabs(color.b - accent.b) < 0.02f && color.a > 0.9f) {
		brush = m_activeBrush;
	} else {
		brush = m_textBrush;
		brush->SetColor(color);
	}

	D2D1_RECT_F layoutRect = D2D1::RectF((float)rect.left, (float)rect.top, (float)rect.right, (float)rect.bottom);

	target->DrawText(text.c_str(), (UINT32)text.length(), textFormat, layoutRect, brush);

	if (textFormat != m_statusTextFormat && textFormat != m_buttonTextFormat &&
	    textFormat != m_replayActiveTextFormat && textFormat != m_statsTitleFormat &&
	    textFormat != m_statsValueFormat) {
		textFormat->Release();
	}
}

void OverlayRenderer::DrawRoundedRectangle(const RECT &rect, float radius, D2D1_COLOR_F fillColor)
{
	ID2D1RenderTarget *target = m_bitmapRenderTarget ? m_bitmapRenderTarget : m_renderTarget;
	if (!target) {
		return;
	}

	D2D1_ROUNDED_RECT roundedRect = D2D1::RoundedRect(
		D2D1::RectF((float)rect.left, (float)rect.top, (float)rect.right, (float)rect.bottom), radius, radius);

	ID2D1SolidColorBrush *brush = m_backgroundBrush;
	brush->SetColor(fillColor);
	target->FillRoundedRectangle(roundedRect, brush);
}

void OverlayRenderer::DrawRoundedRectangleBorder(const RECT &rect, float radius, float borderWidth,
						 D2D1_COLOR_F borderColor)
{
	ID2D1RenderTarget *target = m_bitmapRenderTarget ? m_bitmapRenderTarget : m_renderTarget;
	if (!target) {
		return;
	}

	D2D1_ROUNDED_RECT roundedRect = D2D1::RoundedRect(
		D2D1::RectF((float)rect.left, (float)rect.top, (float)rect.right, (float)rect.bottom), radius, radius);

	ID2D1SolidColorBrush *brush = m_borderBrush;
	brush->SetColor(borderColor);
	target->DrawRoundedRectangle(roundedRect, brush, borderWidth);
}

void OverlayRenderer::ReleaseIconBitmaps()
{
	for (auto &kv : m_iconTintCache) {
		if (kv.second) {
			kv.second->Release();
		}
	}
	m_iconTintCache.clear();
	for (int i = 0; i < ICON_CACHE_SIZE; i++) {
		if (m_iconBitmaps[i]) {
			m_iconBitmaps[i]->Release();
			m_iconBitmaps[i] = nullptr;
		}
		m_iconMasks[i] = IconMask{};
	}
	if (m_aboutLogoBitmap) {
		m_aboutLogoBitmap->Release();
		m_aboutLogoBitmap = nullptr;
	}
	m_aboutLogoLoadAttempted = false;
	for (int i = 0; i < ABOUT_SOCIAL_COUNT; i++) {
		if (m_aboutSocialBitmaps[i]) {
			m_aboutSocialBitmaps[i]->Release();
			m_aboutSocialBitmaps[i] = nullptr;
		}
	}
	m_aboutSocialLoadAttempted = false;
	ReleaseIndicatorPreviews();
}

void OverlayRenderer::ReleaseIndicatorPreviews()
{
	for (int i = 0; i < INDICATOR_CUSTOM_SLOT_COUNT; i++) {
		InvalidateIndicatorPreview(i);
	}
}

void OverlayRenderer::InvalidateIndicatorPreview(int slot)
{
	if (slot < 0 || slot >= INDICATOR_CUSTOM_SLOT_COUNT)
		return;
	overlay::icon::InvalidateSlot(m_indicatorPreview[slot]);
	m_indicatorPreview[slot].path.clear();
	m_indicatorPreview[slot].tintEnabled = false;
	m_indicatorPreview[slot].colorRgb = 0xFFFFFF;
}

bool OverlayRenderer::EnsureIndicatorPreview(int slot)
{
	if (slot < 0 || slot >= INDICATOR_CUSTOM_SLOT_COUNT)
		return false;

	ID2D1RenderTarget *target = m_bitmapRenderTarget ? m_bitmapRenderTarget : m_renderTarget;
	return overlay::icon::EnsureLoaded(m_wicFactory, m_d2dFactory, target, m_indicatorPreview[slot],
					   m_setIndicatorIconPath[slot], m_setIndicatorTint[slot],
					   m_setIndicatorColor[slot], overlay::icon::kDefaultSvgPx);
}

void OverlayRenderer::DrawIndicatorPreview(int slot, const RECT &rect)
{
	ID2D1RenderTarget *target = m_bitmapRenderTarget ? m_bitmapRenderTarget : m_renderTarget;
	if (!target || !m_textBrush || slot < 0 || slot >= INDICATOR_CUSTOM_SLOT_COUNT)
		return;

	DrawRoundedRectangle(rect, 6.0f, D2D1::ColorF(0.12f, 0.12f, 0.15f, 1.0f));
	DrawRoundedRectangleBorder(rect, 6.0f, 1.0f, D2D1::ColorF(58.0f / 255.0f, 58.0f / 255.0f, 69.0f / 255.0f, 1.0f));

	const int pad = 4;
	RECT iconRect = {rect.left + pad, rect.top + pad, rect.right - pad, rect.bottom - pad};
	const int rgb = m_setIndicatorColor[slot] & 0xFFFFFF;
	D2D1_COLOR_F color =
		m_setIndicatorTint[slot]
			? D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f,
					(rgb & 0xFF) / 255.0f, 1.0f)
			: D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);

	if (EnsureIndicatorPreview(slot)) {
		overlay::icon::Slot &p = m_indicatorPreview[slot];
		overlay::icon::AdvanceAnimation(p, GetTickCount64());
		ID2D1Bitmap *bmp = overlay::icon::CurrentBitmap(p);
		if (bmp) {
			D2D1_RECT_F dest = D2D1::RectF((float)iconRect.left, (float)iconRect.top, (float)iconRect.right,
						       (float)iconRect.bottom);
			target->DrawBitmap(bmp, dest, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
			return;
		}
	}

	m_textBrush->SetColor(color);
	overlay::icon::DrawBuiltin(target, m_d2dFactory, m_textBrush, slot, iconRect, 2.0f);
}

uint64_t OverlayRenderer::MakeTintKey(int iconType, const D2D1_COLOR_F &tint) const
{
	auto clamp8 = [](float v) -> uint32_t {
		if (v < 0.0f)
			v = 0.0f;
		if (v > 1.0f)
			v = 1.0f;
		return static_cast<uint32_t>(v * 255.0f + 0.5f);
	};
	const uint32_t a = clamp8(tint.a);
	const uint32_t r = clamp8(tint.r);
	const uint32_t g = clamp8(tint.g);
	const uint32_t b = clamp8(tint.b);
	const uint32_t argb = (a << 24) | (r << 16) | (g << 8) | b;
	return (static_cast<uint64_t>(static_cast<uint32_t>(iconType)) << 32) | argb;
}

ID2D1Bitmap *OverlayRenderer::GetTintedIconBitmap(int iconType, const D2D1_COLOR_F &tint)
{
	if (iconType < 0 || iconType >= ICON_CACHE_SIZE)
		return nullptr;
	if (!m_iconMasks[iconType].ready())
		return nullptr;
	const uint64_t key = MakeTintKey(iconType, tint);
	auto it = m_iconTintCache.find(key);
	if (it != m_iconTintCache.end())
		return it->second;

	ID2D1RenderTarget *target = m_bitmapRenderTarget ? m_bitmapRenderTarget : m_renderTarget;
	if (!target)
		return nullptr;

	const UINT w = m_iconMasks[iconType].w;
	const UINT h = m_iconMasks[iconType].h;
	const uint8_t *a = m_iconMasks[iconType].alpha.data();
	const uint32_t stride = w * 4;
	std::vector<uint8_t> bgra(static_cast<size_t>(stride) * static_cast<size_t>(h));

	auto clamp01 = [](float v) -> float {
		if (v < 0.0f)
			return 0.0f;
		if (v > 1.0f)
			return 1.0f;
		return v;
	};
	const float ca = clamp01(tint.a);
	const float cr = clamp01(tint.r);
	const float cg = clamp01(tint.g);
	const float cb = clamp01(tint.b);

	for (UINT y = 0; y < h; y++) {
		for (UINT x = 0; x < w; x++) {
			const size_t idx = static_cast<size_t>(y) * w + x;
			const float af = (a[idx] / 255.0f) * ca;
			const uint8_t A = static_cast<uint8_t>(af * 255.0f + 0.5f);
			const uint8_t B = static_cast<uint8_t>(cb * 255.0f * af + 0.5f);
			const uint8_t G = static_cast<uint8_t>(cg * 255.0f * af + 0.5f);
			const uint8_t R = static_cast<uint8_t>(cr * 255.0f * af + 0.5f);
			const size_t o = static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4;
			bgra[o + 0] = B;
			bgra[o + 1] = G;
			bgra[o + 2] = R;
			bgra[o + 3] = A;
		}
	}

	D2D1_BITMAP_PROPERTIES props =
		D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
	ID2D1Bitmap *bmp = nullptr;
	if (FAILED(target->CreateBitmap(D2D1::SizeU(w, h), bgra.data(), stride, &props, &bmp)) || !bmp)
		return nullptr;

	m_iconTintCache.emplace(key, bmp);
	return bmp;
}

bool OverlayRenderer::LoadIconFromResource(int iconType)
{
	if (iconType < 0 || iconType >= ICON_CACHE_SIZE || m_iconBitmaps[iconType]) {
		return m_iconBitmaps[iconType] != nullptr;
	}
	static const int resourceIds[] = {IDR_ICON_PLAY,        IDR_ICON_PAUSE,         IDR_ICON_CARET_LEFT,
					  IDR_ICON_CARET_RIGHT, IDR_ICON_DOWNLOAD,      IDR_ICON_TRASH,
					  IDR_ICON_SPEAKER,     IDR_ICON_ARROW_LEFT,    IDR_ICON_ARROWS_OUT,
					  IDR_ICON_ARROWS_IN,   IDR_ICON_GEAR,          IDR_ICON_CHART,
					  IDR_ICON_SPEAKER_SLASH};
	HMODULE hMod = NULL;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				(LPCWSTR)&g_overlayIconsModuleAnchor, &hMod) ||
	    !hMod) {
		return false;
	}
	HRSRC hRes = FindResource(hMod, MAKEINTRESOURCE(resourceIds[iconType]), RT_RCDATA);
	if (!hRes) {
		return false;
	}
	HGLOBAL hLoaded = LoadResource(hMod, hRes);
	if (!hLoaded) {
		return false;
	}
	void *pData = LockResource(hLoaded);
	DWORD size = SizeofResource(hMod, hRes);
	if (!pData || size == 0) {
		return false;
	}
	IStream *stream = NULL;
	if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &stream)) || !stream) {
		return false;
	}
	ULONG written = 0;
	HRESULT hr = stream->Write(pData, size, &written);
	if (FAILED(hr) || written != size) {
		stream->Release();
		return false;
	}
	LARGE_INTEGER zero = {0};
	stream->Seek(zero, STREAM_SEEK_SET, NULL);
	IWICBitmapDecoder *decoder = NULL;
	hr = m_wicFactory->CreateDecoderFromStream(stream, NULL, WICDecodeMetadataCacheOnLoad, &decoder);
	stream->Release();
	if (FAILED(hr) || !decoder) {
		return false;
	}
	IWICBitmapFrameDecode *frame = NULL;
	hr = decoder->GetFrame(0, &frame);
	decoder->Release();
	if (FAILED(hr) || !frame) {
		return false;
	}
	IWICFormatConverter *converter = NULL;
	hr = m_wicFactory->CreateFormatConverter(&converter);
	if (FAILED(hr) || !converter) {
		frame->Release();
		return false;
	}
	hr = converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0.0f,
				   WICBitmapPaletteTypeCustom);
	frame->Release();
	if (FAILED(hr)) {
		converter->Release();
		return false;
	}
	ID2D1RenderTarget *target = m_bitmapRenderTarget ? m_bitmapRenderTarget : m_renderTarget;
	if (!target) {
		converter->Release();
		return false;
	}

	// Alpha mask enables per-state icon tinting without reloading PNGs.
	UINT w = 0, h = 0;
	if (SUCCEEDED(converter->GetSize(&w, &h)) && w > 0 && h > 0) {
		std::vector<uint8_t> pixels(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
		const UINT stride = w * 4;
		if (SUCCEEDED(converter->CopyPixels(nullptr, stride, (UINT)pixels.size(), pixels.data()))) {
			m_iconMasks[iconType].w = w;
			m_iconMasks[iconType].h = h;
			m_iconMasks[iconType].alpha.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
			for (size_t i = 0; i < m_iconMasks[iconType].alpha.size(); i++) {
				m_iconMasks[iconType].alpha[i] = pixels[i * 4 + 3];
			}
		}
	}

	ID2D1Bitmap *bitmap = NULL;
	hr = target->CreateBitmapFromWicBitmap(converter, &bitmap);
	converter->Release();
	if (FAILED(hr) || !bitmap) {
		return false;
	}
	m_iconBitmaps[iconType] = bitmap;
	return true;
}

bool OverlayRenderer::LoadIconBitmap(int iconType)
{
	if (iconType < 0 || iconType >= ICON_CACHE_SIZE || m_iconBitmaps[iconType]) {
		return m_iconBitmaps[iconType] != nullptr;
	}
	if (LoadIconFromResource(iconType)) {
		return true;
	}
	static const char *const iconFiles[] = {"icons/play.png",
						"icons/pause.png",
						"icons/caret-line-left.png",
						"icons/caret-line-right.png",
						"icons/plus.png",
						"icons/trash-simple.png",
						"icons/speaker-high.png",
						"icons/arrow-left.png",
						"icons/arrows-out.png",
						"icons/arrows-in.png",
						"icons/gear.png",
						"icons/chart-line.png",
						"icons/speaker-slash.png"};
	const char *fileName = iconFiles[iconType];
	char *path = obs_module_file(fileName);
	if (!path) {
		return false;
	}
	int wideLen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
	if (wideLen <= 0) {
		bfree(path);
		return false;
	}
	std::vector<wchar_t> widePath(static_cast<size_t>(wideLen));
	MultiByteToWideChar(CP_UTF8, 0, path, -1, widePath.data(), wideLen);
	bfree(path);

	IWICBitmapDecoder *decoder = nullptr;
	HRESULT hr = m_wicFactory->CreateDecoderFromFilename(widePath.data(), NULL, GENERIC_READ,
							     WICDecodeMetadataCacheOnLoad, &decoder);
	if (FAILED(hr) || !decoder) {
		return false;
	}
	IWICBitmapFrameDecode *frame = nullptr;
	hr = decoder->GetFrame(0, &frame);
	decoder->Release();
	if (FAILED(hr) || !frame) {
		return false;
	}
	IWICFormatConverter *converter = nullptr;
	hr = m_wicFactory->CreateFormatConverter(&converter);
	if (FAILED(hr) || !converter) {
		frame->Release();
		return false;
	}
	hr = converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0.0f,
				   WICBitmapPaletteTypeCustom);
	frame->Release();
	if (FAILED(hr)) {
		converter->Release();
		return false;
	}
	ID2D1RenderTarget *target = m_bitmapRenderTarget ? m_bitmapRenderTarget : m_renderTarget;
	if (!target) {
		converter->Release();
		return false;
	}

	// Alpha mask enables per-state icon tinting without reloading PNGs.
	UINT w = 0, h = 0;
	if (SUCCEEDED(converter->GetSize(&w, &h)) && w > 0 && h > 0) {
		std::vector<uint8_t> pixels(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
		const UINT stride = w * 4;
		if (SUCCEEDED(converter->CopyPixels(nullptr, stride, (UINT)pixels.size(), pixels.data()))) {
			m_iconMasks[iconType].w = w;
			m_iconMasks[iconType].h = h;
			m_iconMasks[iconType].alpha.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
			for (size_t i = 0; i < m_iconMasks[iconType].alpha.size(); i++) {
				m_iconMasks[iconType].alpha[i] = pixels[i * 4 + 3];
			}
		}
	}
	ID2D1Bitmap *bitmap = nullptr;
	hr = target->CreateBitmapFromWicBitmap(converter, &bitmap);
	converter->Release();
	if (FAILED(hr) || !bitmap) {
		return false;
	}
	m_iconBitmaps[iconType] = bitmap;
	return true;
}

bool OverlayRenderer::LoadAboutBitmapFromModuleFile(const char *relativePath, UINT svgSizePx, ID2D1Bitmap **outBitmap)
{
	if (!relativePath || !outBitmap || !m_wicFactory)
		return false;
	*outBitmap = nullptr;

	char *path = obs_module_file(relativePath);
	if (!path)
		return false;
	int wideLen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
	if (wideLen <= 0) {
		bfree(path);
		return false;
	}
	std::vector<wchar_t> widePath(static_cast<size_t>(wideLen));
	MultiByteToWideChar(CP_UTF8, 0, path, -1, widePath.data(), wideLen);
	bfree(path);

	const std::wstring wpath(widePath.data());
	const bool isSvg = overlay::svg::IsSvgPath(wpath);

	ID2D1RenderTarget *target = m_bitmapRenderTarget ? m_bitmapRenderTarget : m_renderTarget;
	if (!target)
		return false;

	if (isSvg) {
		if (!m_d2dFactory)
			return false;
		std::vector<uint8_t> bgra;
		UINT w = 0, h = 0, stride = 0;
		if (!overlay::svg::RasterizeFile(m_wicFactory, m_d2dFactory, wpath, svgSizePx, bgra, w, h, stride))
			return false;
		D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(D2D1::PixelFormat(
			DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
		ID2D1Bitmap *bitmap = nullptr;
		if (FAILED(target->CreateBitmap(D2D1::SizeU(w, h), bgra.data(), stride, &props, &bitmap)) || !bitmap)
			return false;
		*outBitmap = bitmap;
		return true;
	}

	IWICBitmapDecoder *decoder = nullptr;
	HRESULT hr = m_wicFactory->CreateDecoderFromFilename(wpath.c_str(), NULL, GENERIC_READ,
							     WICDecodeMetadataCacheOnLoad, &decoder);
	if (FAILED(hr) || !decoder)
		return false;
	IWICBitmapFrameDecode *frame = nullptr;
	hr = decoder->GetFrame(0, &frame);
	decoder->Release();
	if (FAILED(hr) || !frame)
		return false;
	IWICFormatConverter *converter = nullptr;
	hr = m_wicFactory->CreateFormatConverter(&converter);
	if (FAILED(hr) || !converter) {
		frame->Release();
		return false;
	}
	hr = converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0.0f,
				   WICBitmapPaletteTypeCustom);
	frame->Release();
	if (FAILED(hr)) {
		converter->Release();
		return false;
	}
	ID2D1Bitmap *bitmap = nullptr;
	hr = target->CreateBitmapFromWicBitmap(converter, &bitmap);
	converter->Release();
	if (FAILED(hr) || !bitmap)
		return false;
	*outBitmap = bitmap;
	return true;
}

bool OverlayRenderer::EnsureAboutLogoLoaded()
{
	if (m_aboutLogoBitmap)
		return true;
	if (m_aboutLogoLoadAttempted)
		return false;
	m_aboutLogoLoadAttempted = true;
	return LoadAboutBitmapFromModuleFile("logo/logo.png", 0, &m_aboutLogoBitmap);
}

bool OverlayRenderer::EnsureAboutSocialIconsLoaded()
{
	if (m_aboutSocialLoadAttempted) {
		for (int i = 0; i < ABOUT_SOCIAL_COUNT; i++) {
			if (m_aboutSocialBitmaps[i])
				return true;
		}
		return false;
	}
	m_aboutSocialLoadAttempted = true;
	static const char *const kFiles[ABOUT_SOCIAL_COUNT] = {
		"logo/github.svg",
		"logo/telegram.svg",
		"logo/twitch.svg",
		"logo/donationalerts.svg",
	};
	bool any = false;
	for (int i = 0; i < ABOUT_SOCIAL_COUNT; i++) {
		if (LoadAboutBitmapFromModuleFile(kFiles[i], 96, &m_aboutSocialBitmaps[i]))
			any = true;
	}
	return any;
}

void OverlayRenderer::DrawGalleryIcon(const RECT &rect, int iconType, const D2D1_COLOR_F *tintColor)
{
	ID2D1RenderTarget *target = m_bitmapRenderTarget ? m_bitmapRenderTarget : m_renderTarget;
	if (!target) {
		return;
	}
	float l = (float)rect.left;
	float t = (float)rect.top;
	float r = (float)rect.right;
	float b = (float)rect.bottom;
	float w = r - l;
	float h = b - t;
	float cx = l + w * 0.5f;
	float cy = t + h * 0.5f;
	float margin = (w < h ? w : h) * 0.2f;

	if (iconType >= 0 && iconType < ICON_CACHE_SIZE && LoadIconBitmap(iconType) && m_iconBitmaps[iconType]) {
		ID2D1Bitmap *iconBmp = m_iconBitmaps[iconType];
		D2D1_SIZE_F bmpSize = iconBmp->GetSize();
		float srcAspect = bmpSize.width / bmpSize.height;
		float destAspect = w / h;
		float drawW = w;
		float drawH = h;
		if (srcAspect > destAspect) {
			drawH = w / srcAspect;
		} else {
			drawW = h * srcAspect;
		}
		float x = l + (w - drawW) * 0.5f;
		float y = t + (h - drawH) * 0.5f;
		D2D1_RECT_F destRect = D2D1::RectF(x, y, x + drawW, y + drawH);
		ID2D1Bitmap *drawBmp = iconBmp;
		if (tintColor) {
			ID2D1Bitmap *tinted = GetTintedIconBitmap(iconType, *tintColor);
			if (tinted) {
				drawBmp = tinted;
				bmpSize = tinted->GetSize();
			}
		}
		float opacity = 1.0f;
		if (drawBmp == iconBmp && tintColor && tintColor->a > 0.0f) {
			opacity = tintColor->a;
		}
		target->DrawBitmap(drawBmp, destRect, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
		return;
	}

	ID2D1SolidColorBrush *brush = m_textBrush;
	brush->SetColor(tintColor ? *tintColor : D2D1::ColorF(1.0f, 1.0f, 1.0f));

	if (iconType == 0) {
		// Play
		if (m_iconPlayGeometry) {
			D2D1_MATRIX_3X2_F transform =
				D2D1::Matrix3x2F::Scale(w, h) * D2D1::Matrix3x2F::Translation(l, t);
			target->SetTransform(transform);
			target->FillGeometry(m_iconPlayGeometry, brush);
			target->SetTransform(D2D1::Matrix3x2F::Identity());
		}
	} else if (iconType == 1) {
		// Pause
		float barW = w * 0.25f;
		float barH = h * 0.6f;
		float gap = w * 0.15f;
		float leftX = cx - gap - barW;
		float rightX = cx + gap;
		D2D1_RECT_F bar1 = D2D1::RectF(leftX, cy - barH * 0.5f, leftX + barW, cy + barH * 0.5f);
		D2D1_RECT_F bar2 = D2D1::RectF(rightX, cy - barH * 0.5f, rightX + barW, cy + barH * 0.5f);
		target->FillRectangle(bar1, brush);
		target->FillRectangle(bar2, brush);
	} else if (iconType == 2) {
		// Set In
		float thick = (w < h ? w : h) * 0.15f;
		D2D1_RECT_F bar = D2D1::RectF(l + margin, t + margin, l + margin + thick, b - margin);
		target->FillRectangle(bar, brush);
		D2D1_RECT_F top = D2D1::RectF(l + margin, t + margin, l + margin + thick * 2.5f, t + margin + thick);
		D2D1_RECT_F bot = D2D1::RectF(l + margin, b - margin - thick, l + margin + thick * 2.5f, b - margin);
		target->FillRectangle(top, brush);
		target->FillRectangle(bot, brush);
	} else if (iconType == 3) {
		// Set Out
		float thick = (w < h ? w : h) * 0.15f;
		D2D1_RECT_F bar = D2D1::RectF(r - margin - thick, t + margin, r - margin, b - margin);
		target->FillRectangle(bar, brush);
		D2D1_RECT_F top =
			D2D1::RectF(r - margin - thick * 2.5f, t + margin, r - margin - thick, t + margin + thick);
		D2D1_RECT_F bot =
			D2D1::RectF(r - margin - thick * 2.5f, b - margin - thick, r - margin - thick, b - margin);
		target->FillRectangle(top, brush);
		target->FillRectangle(bot, brush);
	} else if (iconType == 4) {
		// Export
		float arrowW = w * 0.5f;
		float arrowH = h * 0.4f;
		D2D1_POINT_2F tip = D2D1::Point2F(cx, b - margin);
		D2D1_POINT_2F left = D2D1::Point2F(cx - arrowW * 0.5f, b - margin - arrowH);
		D2D1_POINT_2F right = D2D1::Point2F(cx + arrowW * 0.5f, b - margin - arrowH);
		target->DrawLine(tip, left, brush, 2.0f);
		target->DrawLine(left, right, brush, 2.0f);
		target->DrawLine(right, tip, brush, 2.0f);
		float lineY = t + margin + h * 0.2f;
		target->DrawLine(D2D1::Point2F(l + margin, lineY), D2D1::Point2F(r - margin, lineY), brush, 1.5f);
	} else if (iconType == 5) {
		// Delete
		float canW = w * 0.5f;
		float canH = h * 0.5f;
		float canL = cx - canW * 0.5f;
		float canR = cx + canW * 0.5f;
		float canT = cy - canH * 0.3f;
		float canB = cy + canH * 0.5f;
		target->DrawLine(D2D1::Point2F(canL, canT), D2D1::Point2F(canR, canT), brush, 1.5f);
		target->DrawLine(D2D1::Point2F(canL, canT), D2D1::Point2F(canL + 4, canB), brush, 1.5f);
		target->DrawLine(D2D1::Point2F(canR, canT), D2D1::Point2F(canR - 4, canB), brush, 1.5f);
		target->DrawLine(D2D1::Point2F(canL + 4, canB), D2D1::Point2F(canR - 4, canB), brush, 1.5f);
	} else if (iconType == 6) {
		// Mute
		float boxL = l + margin;
		float boxR = l + margin + w * 0.25f;
		float boxT = cy - h * 0.25f;
		float boxB = cy + h * 0.25f;
		D2D1_RECT_F speaker = D2D1::RectF(boxL, boxT, boxR, boxB);
		target->FillRectangle(speaker, brush);
		target->DrawLine(D2D1::Point2F(boxR, cy), D2D1::Point2F(cx + 2, cy), brush, 1.5f);
		target->DrawLine(D2D1::Point2F(cx + 4, cy - 6), D2D1::Point2F(cx + 4, cy + 6), brush, 1.5f);
		target->DrawLine(D2D1::Point2F(cx + 8, cy - 10), D2D1::Point2F(cx + 8, cy + 10), brush, 1.5f);
	} else if (iconType == 7) {
		// Back
		float arrowW = w * 0.4f;
		float arrowH = h * 0.35f;
		float th = (w < h ? w : h) * 0.12f;
		D2D1_POINT_2F tip = D2D1::Point2F(l + margin, cy);
		D2D1_POINT_2F top = D2D1::Point2F(l + margin + arrowW, cy - arrowH);
		D2D1_POINT_2F bot = D2D1::Point2F(l + margin + arrowW, cy + arrowH);
		target->DrawLine(tip, top, brush, th);
		target->DrawLine(tip, bot, brush, th);
		target->DrawLine(top, bot, brush, th);
	} else if (iconType == 8) {
		// Fullscreen
		float d = (w < h ? w : h) * 0.28f;
		float th = (w < h ? w : h) * 0.1f;
		// Top-left
		target->DrawLine(D2D1::Point2F(l + margin, cy - d), D2D1::Point2F(l + margin, cy), brush, th);
		target->DrawLine(D2D1::Point2F(l + margin, cy - d), D2D1::Point2F(l + margin + d, cy - d), brush, th);
		// Top-right
		target->DrawLine(D2D1::Point2F(r - margin - d, cy - d), D2D1::Point2F(r - margin, cy - d), brush, th);
		target->DrawLine(D2D1::Point2F(r - margin, cy - d), D2D1::Point2F(r - margin, cy), brush, th);
		// Bottom-right
		target->DrawLine(D2D1::Point2F(r - margin, cy + d), D2D1::Point2F(r - margin, cy), brush, th);
		target->DrawLine(D2D1::Point2F(r - margin - d, cy + d), D2D1::Point2F(r - margin, cy + d), brush, th);
		// Bottom-left
		target->DrawLine(D2D1::Point2F(l + margin, cy + d), D2D1::Point2F(l + margin + d, cy + d), brush, th);
		target->DrawLine(D2D1::Point2F(l + margin, cy + d), D2D1::Point2F(l + margin, cy), brush, th);
	} else if (iconType == 9) {
		// Window
		float d = (w < h ? w : h) * 0.28f;
		float th = (w < h ? w : h) * 0.1f;
		// Top-left
		target->DrawLine(D2D1::Point2F(l + margin, cy - d), D2D1::Point2F(l + margin + d, cy - d), brush, th);
		target->DrawLine(D2D1::Point2F(l + margin + d, cy - d), D2D1::Point2F(l + margin + d, cy), brush, th);
		// Top-right
		target->DrawLine(D2D1::Point2F(r - margin - d, cy - d), D2D1::Point2F(r - margin, cy - d), brush, th);
		target->DrawLine(D2D1::Point2F(r - margin - d, cy - d), D2D1::Point2F(r - margin - d, cy), brush, th);
		// Bottom-right
		target->DrawLine(D2D1::Point2F(r - margin - d, cy + d), D2D1::Point2F(r - margin, cy + d), brush, th);
		target->DrawLine(D2D1::Point2F(r - margin - d, cy), D2D1::Point2F(r - margin - d, cy + d), brush, th);
		// Bottom-left
		target->DrawLine(D2D1::Point2F(l + margin, cy + d), D2D1::Point2F(l + margin + d, cy + d), brush, th);
		target->DrawLine(D2D1::Point2F(l + margin + d, cy), D2D1::Point2F(l + margin + d, cy + d), brush, th);
	} else if (iconType == 10) {
		// Settings
		float outerR = (w < h ? w : h) * 0.38f;
		float innerR = outerR * 0.52f;
		int teeth = 8;
		float th = (w < h ? w : h) * 0.08f;
		for (int i = 0; i < teeth; i++) {
			float a = (float)i * (2.0f * 3.14159265f / (float)teeth);
			float xo = cx + outerR * cosf(a);
			float yo = cy + outerR * sinf(a);
			float xi = cx + innerR * cosf(a);
			float yi = cy + innerR * sinf(a);
			target->DrawLine(D2D1::Point2F(xo, yo), D2D1::Point2F(xi, yi), brush, th);
		}
		D2D1_ELLIPSE hole = D2D1::Ellipse(D2D1::Point2F(cx, cy), innerR * 0.45f, innerR * 0.45f);
		target->DrawEllipse(hole, brush, (w < h ? w : h) * 0.06f);
	} else if (iconType == 11) {
		// Stats
		float barW = w * 0.2f;
		float gap = w * 0.12f;
		float baseY = b - margin;
		int heights[] = {6, 10, 14};
		float scale = h * 0.35f / 14.0f;
		for (int i = 0; i < 3; i++) {
			float barH = heights[i] * scale;
			float lx = cx - (gap + barW) * 1.5f + i * (gap + barW);
			D2D1_RECT_F barRect = D2D1::RectF(lx, baseY - barH, lx + barW, baseY);
			target->FillRectangle(barRect, brush);
		}
	}
}

void OverlayRenderer::SetAutoHide(bool enabled, int seconds)
{
	m_autoHideEnabled = enabled;
	if (seconds < 1)
		seconds = 1;
	if (seconds > 3600)
		seconds = 3600;
	m_autoHideSeconds = seconds;
	m_lastInteractionTick = GetTickCount64();
}

void OverlayRenderer::SetGalleryEnabled(bool enabled)
{
	m_galleryEnabled = enabled;
	if (!m_galleryEnabled && m_galleryOpen) {
		CloseGallery();
	}
}

void OverlayRenderer::SetCaptureFocus(bool capture)
{
	m_captureFocus = capture;
	m_windowManager.SetAllowActivate(capture);
	if (!IsVisible())
		return;
	if (m_galleryOpen)
		ApplyGalleryWindowSize();
	else
		ApplyWindowGeometry();
	Render();
}

void OverlayRenderer::SetOverlayBackgroundAlpha(float alpha)
{
	if (alpha >= 0.5f && alpha <= 1.0f) {
		m_overlayBackgroundAlpha = alpha;
	}
}

void OverlayRenderer::SetOverlayScalePercent(int scalePercent)
{
	m_overlayScalePercent = ClampInt(scalePercent, 50, 300);
	if (IsVisible())
		ApplyPosition();
}

void OverlayRenderer::SetAccentColorRgb(int rgb)
{
	m_accentColorRgb = rgb & 0xFFFFFF;
	ApplyAccentBrushes();
}

float OverlayRenderer::UiScale() const
{
	return m_overlayScalePercent / 100.0f;
}

float OverlayRenderer::FontScale() const
{
	return UiScale() * (m_textScalePercent / 100.0f);
}

int OverlayRenderer::UiPx(int value) const
{
	return static_cast<int>(value * UiScale() + (value >= 0 ? 0.5f : -0.5f));
}

D2D1_COLOR_F OverlayRenderer::ColorFromRgb(int rgb, float alpha)
{
	return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha);
}

namespace {

void RgbToHsl(float r, float g, float b, float &h, float &s, float &l)
{
	const float maxc = std::max(r, std::max(g, b));
	const float minc = std::min(r, std::min(g, b));
	l = (maxc + minc) * 0.5f;
	if (maxc == minc) {
		h = 0.0f;
		s = 0.0f;
		return;
	}
	const float d = maxc - minc;
	s = l > 0.5f ? d / (2.0f - maxc - minc) : d / (maxc + minc);
	if (maxc == r)
		h = (g - b) / d + (g < b ? 6.0f : 0.0f);
	else if (maxc == g)
		h = (b - r) / d + 2.0f;
	else
		h = (r - g) / d + 4.0f;
	h /= 6.0f;
}

float HueToRgb(float p, float q, float t)
{
	if (t < 0.0f)
		t += 1.0f;
	if (t > 1.0f)
		t -= 1.0f;
	if (t < 1.0f / 6.0f)
		return p + (q - p) * 6.0f * t;
	if (t < 1.0f / 2.0f)
		return q;
	if (t < 2.0f / 3.0f)
		return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
	return p;
}

void HslToRgb(float h, float s, float l, float &r, float &g, float &b)
{
	if (s <= 0.0001f) {
		r = g = b = l;
		return;
	}
	const float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
	const float p = 2.0f * l - q;
	r = HueToRgb(p, q, h + 1.0f / 3.0f);
	g = HueToRgb(p, q, h);
	b = HueToRgb(p, q, h - 1.0f / 3.0f);
}

D2D1_COLOR_F DeriveAccentShade(int rgb, float satMul, float lightMul, float alpha)
{
	float r = ((rgb >> 16) & 0xFF) / 255.0f;
	float g = ((rgb >> 8) & 0xFF) / 255.0f;
	float b = (rgb & 0xFF) / 255.0f;
	float h, s, l;
	RgbToHsl(r, g, b, h, s, l);
	s = std::max(0.0f, std::min(1.0f, s * satMul));
	l = std::max(0.0f, std::min(1.0f, l * lightMul));
	HslToRgb(h, s, l, r, g, b);
	return D2D1::ColorF(r, g, b, alpha);
}

} // namespace

D2D1_COLOR_F OverlayRenderer::AccentColor(float alpha) const
{
	return ColorFromRgb(m_accentColorRgb, alpha);
}

D2D1_COLOR_F OverlayRenderer::AccentSoft() const
{
	return AccentColor(0.55f);
}

D2D1_COLOR_F OverlayRenderer::AccentMuted() const
{
	// Matches the previous hardcoded muted purple (~130,110,200) relative to #a882ff.
	return DeriveAccentShade(m_accentColorRgb, 0.85f, 0.78f, 1.0f);
}

D2D1_COLOR_F OverlayRenderer::AccentHover(float alpha) const
{
	return DeriveAccentShade(m_accentColorRgb, 0.55f, 0.72f, alpha);
}

D2D1_COLOR_F OverlayRenderer::AccentSelected(float alpha) const
{
	// Dark accent fill for selected rows / primary buttons (~70,55–60,110–120).
	return DeriveAccentShade(m_accentColorRgb, 0.70f, 0.33f, alpha);
}

D2D1_COLOR_F OverlayRenderer::AccentPanel(float alpha) const
{
	// Subtle accent-tinted panel background (~35,32,50).
	return DeriveAccentShade(m_accentColorRgb, 0.45f, 0.15f, alpha);
}

void OverlayRenderer::ApplyAccentBrushes()
{
	const D2D1_COLOR_F accent = AccentColor();
	if (m_activeBrush)
		m_activeBrush->SetColor(accent);
	if (m_statsTitleBrush)
		m_statsTitleBrush->SetColor(accent);
}

void OverlayRenderer::UpdateAutoHide()
{
	if (!m_autoHideEnabled)
		return;
	if (!IsVisible())
		return;
	// Gallery playback and export must stay visible while active.
	if (m_galleryOpen)
		return;
	// Keep the overlay visible while the settings panel is open.
	if (m_settingsOpen)
		return;
	// Long exports should not trigger auto-hide mid-job.
	if (m_exportInfraReady && m_exportQueue.IsBusy())
		return;

	ULONGLONG now = GetTickCount64();
	ULONGLONG elapsed = now - m_lastInteractionTick;
	if (elapsed >= static_cast<ULONGLONG>(m_autoHideSeconds) * 1000ULL) {
		Hide();
	}
}

void OverlayRenderer::SetUpdateInterval(int intervalMs)
{
	if (intervalMs < 16)
		intervalMs = 16;
	if (intervalMs > 2000)
		intervalMs = 2000;
	if (m_updateIntervalMs == intervalMs)
		return;
	m_updateIntervalMs = intervalMs;
	HWND hwnd = m_windowManager.GetHWND();
	if (!hwnd || !m_updateTimer)
		return;
	KillTimer(hwnd, m_updateTimer);
	m_updateTimer = SetTimer(hwnd, 1, m_updateIntervalMs, TimerProc);
}

namespace {

std::wstring WideFromUtf8(const std::string &text)
{
	if (text.empty()) {
		return std::wstring();
	}
	int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
	if (needed <= 0) {
		return std::wstring();
	}
	std::wstring wide(static_cast<size_t>(needed), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &wide[0], needed);
	wide.resize(wcslen(wide.c_str()));
	return wide;
}

std::wstring FormatTime(double seconds)
{
	if (seconds < 0.0) {
		seconds = 0.0;
	}
	int total = static_cast<int>(seconds + 0.5);
	int mins = total / 60;
	int secs = total % 60;
	wchar_t buffer[32] = {};
	swprintf(buffer, 32, L"%02d:%02d", mins, secs);
	return buffer;
}

std::wstring FormatFileSize(uint64_t sizeBytes)
{
	double value = static_cast<double>(sizeBytes);
	const wchar_t *unit = L"B";
	if (value >= 1024.0) {
		value /= 1024.0;
		unit = L"KB";
	}
	if (value >= 1024.0) {
		value /= 1024.0;
		unit = L"MB";
	}
	if (value >= 1024.0) {
		value /= 1024.0;
		unit = L"GB";
	}
	wchar_t buffer[32] = {};
	if (wcscmp(unit, L"B") == 0) {
		swprintf(buffer, 32, L"%.0f %s", value, unit);
	} else {
		swprintf(buffer, 32, L"%.1f %s", value, unit);
	}
	return buffer;
}

std::wstring GetFileName(const std::wstring &path)
{
	size_t pos = path.find_last_of(L"\\/");
	if (pos == std::wstring::npos) {
		return path;
	}
	return path.substr(pos + 1);
}

std::wstring GetFileExtension(const std::wstring &path)
{
	size_t pos = path.find_last_of(L'.');
	if (pos == std::wstring::npos) {
		return L"";
	}
	return path.substr(pos);
}

std::wstring BuildSharedFolder(const std::string &recordingPath)
{
	std::wstring base = WideFromUtf8(recordingPath);
	if (base.empty()) {
		return std::wstring();
	}
	if (base.back() != L'\\' && base.back() != L'/') {
		base.push_back(L'\\');
	}
	return base + L"Shared";
}

std::wstring BuildGalleryExportFolder(const std::string &recordingPath)
{
	if (saved_settings_data && obs_data_has_user_value(saved_settings_data, "gallery_export_path")) {
		const char *customPath = obs_data_get_string(saved_settings_data, "gallery_export_path");
		if (customPath && customPath[0] != '\0') {
			return WideFromUtf8(std::string(customPath));
		}
	}
	return BuildSharedFolder(recordingPath);
}

std::wstring BuildSharedOutputPath(const std::wstring &sharedFolder, const std::wstring &sourcePath,
				   const std::wstring &suffix)
{
	SYSTEMTIME st = {};
	GetLocalTime(&st);
	std::wstring baseName = GetFileName(sourcePath);
	std::wstring ext = GetFileExtension(baseName);
	if (!ext.empty()) {
		baseName = baseName.substr(0, baseName.size() - ext.size());
	}
	wchar_t timeBuf[32] = {};
	swprintf(timeBuf, 32, L"%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
		 st.wSecond);
	std::wstring fileName = baseName + suffix + L"_" + timeBuf + ext;
	return sharedFolder + L"\\" + fileName;
}

std::wstring BuildSharedOutputPathExt(const std::wstring &sharedFolder, const std::wstring &sourcePath,
				      const std::wstring &suffix, const std::wstring &forcedExt)
{
	SYSTEMTIME st = {};
	GetLocalTime(&st);
	std::wstring baseName = GetFileName(sourcePath);
	std::wstring ext = GetFileExtension(baseName);
	if (!ext.empty()) {
		baseName = baseName.substr(0, baseName.size() - ext.size());
	}
	wchar_t timeBuf[32] = {};
	swprintf(timeBuf, 32, L"%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
		 st.wSecond);
	std::wstring outExt = forcedExt;
	if (!outExt.empty() && outExt[0] != L'.') {
		outExt = L"." + outExt;
	}
	if (outExt.empty()) {
		outExt = ext;
	}
	std::wstring fileName = baseName + suffix + L"_" + timeBuf + outExt;
	return sharedFolder + L"\\" + fileName;
}

} // namespace

void OverlayRenderer::EnsureExportInfrastructure()
{
	if (m_exportInfraReady)
		return;
	overlay_settings_ensure();
	if (saved_settings_data) {
		m_rebuildPresets.EnsureSeeded(saved_settings_data);
		m_rebuildPresets.Load(saved_settings_data);
		overlay_settings_save();
	} else {
		m_rebuildPresets.presets = RebuildPresetStore::DefaultSeedPresets();
		m_rebuildPresets.selectedId = "seed_balanced";
	}
	m_rebuildDraft = m_rebuildPresets.GetSelectedOrDefault();
	m_setExportMaxParallel = m_rebuildPresets.maxParallel;
	m_exportQueue.SetMaxParallel(m_setExportMaxParallel);
	m_exportQueue.SetBatchPreset(m_rebuildDraft);
	m_exportQueue.SetToastHandler([this](const std::string &utf8) {
		if (m_setToastExport)
			overlay_runtime_show_toast(utf8.c_str(), 5000);
	});
	m_exportQueue.Load();
	m_exportInfraReady = true;
}

void OverlayRenderer::SyncExportQueueFolder()
{
	std::string recordingPath = m_stateManager.GetRecordingPath();
	if (recordingPath.empty())
		recordingPath = m_stateManager.GetConfiguredRecordingPath();
	m_exportQueue.SetDefaultExportFolder(BuildGalleryExportFolder(recordingPath));
}

void OverlayRenderer::PersistRebuildPresets()
{
	overlay_settings_ensure();
	if (!saved_settings_data)
		return;
	m_rebuildPresets.selectedId = m_rebuildDraft.id;
	m_rebuildPresets.maxParallel = m_setExportMaxParallel;
	m_rebuildPresets.Save(saved_settings_data);
	obs_data_set_bool(saved_settings_data, "gallery_share_compress", m_rebuildDraft.mode != RebuildMode::Copy);
	obs_data_set_int(saved_settings_data, "gallery_share_crf", m_rebuildDraft.Crf());
	overlay_settings_save();
	m_exportQueue.SetMaxParallel(m_setExportMaxParallel);
}

void OverlayRenderer::PopulateExportTrackChoices(const std::vector<int> *keepStreamIndices)
{
	m_exportTrackStreamIndices.clear();
	m_exportTrackKeep.clear();
	m_exportTrackLabels.clear();

	auto kept = [&](int streamIndex) -> bool {
		if (!keepStreamIndices || keepStreamIndices->empty())
			return true;
		for (int idx : *keepStreamIndices) {
			if (idx == streamIndex)
				return true;
		}
		return false;
	};

	int trackNum = 0;
	const int videoIdx = m_videoPlayer.GetVideoStreamIndex();
	if (videoIdx >= 0) {
		trackNum++;
		m_exportTrackStreamIndices.push_back(videoIdx);
		m_exportTrackKeep.push_back(kept(videoIdx));
		std::wstring videoName = WideFromUtf8(m_videoPlayer.GetVideoStreamName());
		m_exportTrackLabels.push_back(std::to_wstring(trackNum) + L": " +
					      (videoName.empty() ? overlay::util::ModuleTextW("Export.Video") : videoName));
	}
	const int audioCount = m_videoPlayer.GetAudioTrackCount();
	for (int i = 0; i < audioCount; i++) {
		OverlayVideoPlayer::AudioTrackInfo info;
		m_videoPlayer.GetAudioTrackInfo(i, info);
		trackNum++;
		m_exportTrackStreamIndices.push_back(info.streamIndex);
		m_exportTrackKeep.push_back(kept(info.streamIndex));
		std::wstring name = WideFromUtf8(info.name);
		m_exportTrackLabels.push_back(
			std::to_wstring(trackNum) + L": " +
			(name.empty() ? overlay::util::ModuleTextW("Export.Audio") + L" " + std::to_wstring(i + 1)
				      : name));
	}
}

void OverlayRenderer::CaptureExportTracksToDraft()
{
	if (m_exportTrackStreamIndices.empty())
		return;

	m_rebuildDraft.tracksConfigured = true;
	m_rebuildDraft.keepVideoTrack = true;
	m_rebuildDraft.keepAudioOrdinals.clear();

	const int videoIdx = m_videoPlayer.GetVideoStreamIndex();
	int audioOrd = 0;
	for (size_t i = 0; i < m_exportTrackStreamIndices.size(); i++) {
		const bool on = (i < m_exportTrackKeep.size()) && m_exportTrackKeep[i];
		const int streamIdx = m_exportTrackStreamIndices[i];
		if (streamIdx == videoIdx) {
			m_rebuildDraft.keepVideoTrack = on;
		} else {
			if (on)
				m_rebuildDraft.keepAudioOrdinals.push_back(audioOrd);
			audioOrd++;
		}
	}
	m_rebuildDraft.keepAudio = !m_rebuildDraft.keepAudioOrdinals.empty();
}

void OverlayRenderer::ApplyDraftTracksToExportUi()
{
	PopulateExportTrackChoices(nullptr);
	if (!m_rebuildDraft.tracksConfigured)
		return;

	const int videoIdx = m_videoPlayer.GetVideoStreamIndex();
	int audioOrd = 0;
	for (size_t i = 0; i < m_exportTrackStreamIndices.size(); i++) {
		const int streamIdx = m_exportTrackStreamIndices[i];
		if (streamIdx == videoIdx) {
			if (i < m_exportTrackKeep.size())
				m_exportTrackKeep[i] = m_rebuildDraft.keepVideoTrack;
		} else {
			if (i < m_exportTrackKeep.size())
				m_exportTrackKeep[i] = m_rebuildDraft.ShouldKeepAudioOrdinal(audioOrd);
			audioOrd++;
		}
	}
}

bool OverlayRenderer::AddSelectedClipToExportQueue()
{
	const GalleryItem *item = m_gallery.GetItem(m_gallerySelectedIndex);
	if (!item || item->type != GalleryItem::Type::Video)
		return false;
	EnsureExportInfrastructure();
	SyncExportQueueFolder();

	ExportQueueJob job;
	job.inputPath = item->path;
	job.displayName = item->name;
	job.startSeconds = m_galleryTrimIn;
	job.endSeconds = m_galleryTrimOut;
	if (job.endSeconds <= job.startSeconds)
		job.endSeconds = job.startSeconds + 1.0;
	const double mediaDur = m_videoPlayer.GetDuration();
	if (job.startSeconds <= 0.001 && mediaDur > 0.0 && job.endSeconds >= mediaDur - 0.15)
		job.endSeconds = 0.0;
	job.preset = m_exportQueue.GetBatchPreset();
	job.streamIndicesToKeep.clear();
	m_exportQueue.Enqueue(job);
	if (m_setToastExport)
		overlay_runtime_show_toast(
			(std::string("Added to export queue: ") + overlay::util::WideToUtf8(job.displayName)).c_str(),
			3500);
	return true;
}

namespace {

HGLOBAL CreateHDropData(const std::wstring &path)
{
	size_t bytes = (path.size() + 2) * sizeof(wchar_t);
	size_t total = sizeof(DROPFILES) + bytes;
	HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, total);
	if (!hMem) {
		return nullptr;
	}
	DROPFILES *df = static_cast<DROPFILES *>(GlobalLock(hMem));
	if (!df) {
		GlobalFree(hMem);
		return nullptr;
	}
	df->pFiles = sizeof(DROPFILES);
	df->fWide = TRUE;
	wchar_t *dest = reinterpret_cast<wchar_t *>(reinterpret_cast<BYTE *>(df) + sizeof(DROPFILES));
	wcscpy_s(dest, path.size() + 2, path.c_str());
	dest[path.size() + 1] = L'\0';
	GlobalUnlock(hMem);
	return hMem;
}

bool CopyFileToClipboard(HWND hwnd, const std::wstring &path, bool cut)
{
	HGLOBAL hDrop = CreateHDropData(path);
	if (!hDrop) {
		return false;
	}

	UINT dropEffectFormat = RegisterClipboardFormat(CFSTR_PREFERREDDROPEFFECT);
	HGLOBAL hEffect = nullptr;
	if (dropEffectFormat != 0) {
		hEffect = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
		if (!hEffect) {
			GlobalFree(hDrop);
			return false;
		}
		DWORD *effect = static_cast<DWORD *>(GlobalLock(hEffect));
		if (!effect) {
			GlobalFree(hDrop);
			GlobalFree(hEffect);
			return false;
		}
		*effect = cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
		GlobalUnlock(hEffect);
	}

	if (!OpenClipboard(hwnd)) {
		GlobalFree(hDrop);
		if (hEffect) {
			GlobalFree(hEffect);
		}
		return false;
	}
	EmptyClipboard();
	SetClipboardData(CF_HDROP, hDrop);
	if (dropEffectFormat != 0 && hEffect) {
		SetClipboardData(dropEffectFormat, hEffect);
	} else if (hEffect) {
		GlobalFree(hEffect);
	}
	CloseClipboard();
	return true;
}

bool CopyPathToClipboard(HWND hwnd, const std::wstring &path)
{
	const size_t bytes = (path.size() + 1) * sizeof(wchar_t);
	HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
	if (!hMem) {
		return false;
	}
	wchar_t *dest = static_cast<wchar_t *>(GlobalLock(hMem));
	if (!dest) {
		GlobalFree(hMem);
		return false;
	}
	wcscpy_s(dest, path.size() + 1, path.c_str());
	GlobalUnlock(hMem);

	if (!OpenClipboard(hwnd)) {
		GlobalFree(hMem);
		return false;
	}
	EmptyClipboard();
	SetClipboardData(CF_UNICODETEXT, hMem);
	CloseClipboard();
	return true;
}

void ShowFileInExplorer(const std::wstring &path)
{
	PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
	if (!pidl) {
		return;
	}
	SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
	ILFree(pidl);
}

void OpenWithDialog(HWND /*unused*/, const std::wstring &path)
{
	// SHOpenWithDialog can open minimized when the last active window is a hidden
	// topmost overlay. Use a short-lived normal owner instead.
	HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP, 0, 0, 1, 1, nullptr,
				     nullptr, GetModuleHandleW(nullptr), nullptr);
	if (owner) {
		ShowWindow(owner, SW_SHOWNORMAL);
		BringWindowToTop(owner);
	}

	OPENASINFO info = {};
	info.pcszFile = path.c_str();
	info.pcszClass = nullptr;
	info.oaifInFlags = OAIF_EXEC;
	SHOpenWithDialog(owner, &info);

	if (owner) {
		DestroyWindow(owner);
	}
}

struct RenamePromptState {
	std::wstring text;
	bool accepted = false;
	bool closed = false;
	bool selectStemBeforeDot = true;
};

constexpr int kRenameEditId = 1001;
constexpr wchar_t kRenamePromptClass[] = L"NtchffsOverlayRenamePrompt";

LRESULT CALLBACK RenamePromptWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	RenamePromptState *state = reinterpret_cast<RenamePromptState *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	switch (msg) {
	case WM_CREATE: {
		auto *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
		state = static_cast<RenamePromptState *>(cs->lpCreateParams);
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

		HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", state->text.c_str(),
					    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 12, 14, 360, 26,
					    hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRenameEditId)),
					    cs->hInstance, nullptr);
		CreateWindowW(L"BUTTON", overlay::util::ModuleTextW("OK").c_str(),
			      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 200, 52, 84, 28, hwnd,
			      reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), cs->hInstance, nullptr);
		CreateWindowW(L"BUTTON", overlay::util::ModuleTextW("Cancel").c_str(),
			      WS_CHILD | WS_VISIBLE | WS_TABSTOP, 292, 52, 84, 28, hwnd,
			      reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), cs->hInstance, nullptr);

		if (edit) {
			SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),
				     TRUE);
			size_t selEnd = state->text.size();
			if (state->selectStemBeforeDot) {
				const size_t dot = state->text.find_last_of(L'.');
				if (dot != std::wstring::npos)
					selEnd = dot;
			}
			SendMessageW(edit, EM_SETSEL, 0, static_cast<LPARAM>(selEnd));
			SetFocus(edit);
		}
		HWND okBtn = GetDlgItem(hwnd, IDOK);
		HWND cancelBtn = GetDlgItem(hwnd, IDCANCEL);
		if (okBtn) {
			SendMessageW(okBtn, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),
				     TRUE);
		}
		if (cancelBtn) {
			SendMessageW(cancelBtn, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),
				     TRUE);
		}
		return 0;
	}
	case WM_COMMAND: {
		const int id = LOWORD(wParam);
		if (id == IDOK && state) {
			wchar_t buf[MAX_PATH] = {};
			GetWindowTextW(GetDlgItem(hwnd, kRenameEditId), buf, MAX_PATH);
			state->text = buf;
			state->accepted = true;
			DestroyWindow(hwnd);
			return 0;
		}
		if (id == IDCANCEL) {
			DestroyWindow(hwnd);
			return 0;
		}
		break;
	}
	case WM_CLOSE:
		DestroyWindow(hwnd);
		return 0;
	case WM_DESTROY:
		if (state) {
			state->closed = true;
		}
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool EnsureRenamePromptClass(HINSTANCE instance)
{
	static bool registered = false;
	if (registered) {
		return true;
	}
	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = RenamePromptWndProc;
	wc.hInstance = instance;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
	wc.lpszClassName = kRenamePromptClass;
	if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
		return false;
	}
	registered = true;
	return true;
}

bool PromptTextInput(HWND owner, const std::wstring &title, std::wstring &textInOut, bool selectStemBeforeDot)
{
	HMODULE instance = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   reinterpret_cast<LPCWSTR>(&RenamePromptWndProc), &instance);
	if (!instance) {
		instance = GetModuleHandleW(nullptr);
	}
	if (!EnsureRenamePromptClass(instance)) {
		return false;
	}

	RenamePromptState state;
	state.text = textInOut;
	state.selectStemBeforeDot = selectStemBeforeDot;

	const int width = 400;
	const int height = 120;
	RECT ownerRect = {};
	if (owner) {
		GetWindowRect(owner, &ownerRect);
	} else {
		SystemParametersInfoW(SPI_GETWORKAREA, 0, &ownerRect, 0);
	}
	const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
	const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;

	HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, kRenamePromptClass, title.c_str(),
				    WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height, owner, nullptr,
				    instance, &state);
	if (!hwnd) {
		return false;
	}

	if (owner) {
		EnableWindow(owner, FALSE);
	}
	ShowWindow(hwnd, SW_SHOW);
	UpdateWindow(hwnd);

	MSG msg = {};
	while (!state.closed && GetMessageW(&msg, nullptr, 0, 0) > 0) {
		if (!IsDialogMessageW(hwnd, &msg)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	if (owner) {
		EnableWindow(owner, TRUE);
		SetForegroundWindow(owner);
	}

	if (!state.accepted) {
		return false;
	}
	textInOut = state.text;
	return true;
}

bool PromptRenameFileName(HWND owner, std::wstring &nameInOut)
{
	return PromptTextInput(owner, overlay::util::ModuleTextW("Gallery.Ctx.Rename"), nameInOut, true);
}

static std::wstring TrimPresetName(std::wstring s)
{
	while (!s.empty() && (s.front() == L' ' || s.front() == L'\t'))
		s.erase(s.begin());
	while (!s.empty() && (s.back() == L' ' || s.back() == L'\t'))
		s.pop_back();
	return s;
}

static bool PromptPresetName(HWND owner, std::wstring &nameInOut)
{
	std::wstring draft = nameInOut;
	if (!PromptTextInput(owner, overlay::util::ModuleTextW("Rebuild.PresetName"), draft, false))
		return false;
	draft = TrimPresetName(draft);
	if (draft.empty())
		return false;
	nameInOut = draft;
	return true;
}

bool IsValidFileName(const std::wstring &name)
{
	if (name.empty() || name == L"." || name == L"..") {
		return false;
	}
	if (name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
		return false;
	}
	if (name.back() == L'.' || name.back() == L' ') {
		return false;
	}
	return true;
}

enum class GalleryContextAction {
	OpenFolder = 0,
	OpenWith,
	Copy,
	CopyPath,
	Rename,
	Delete,
	Count
};

const char *GalleryContextActionLocaleKey(GalleryContextAction action)
{
	switch (action) {
	case GalleryContextAction::OpenFolder:
		return "Gallery.Ctx.OpenFolder";
	case GalleryContextAction::OpenWith:
		return "Gallery.Ctx.OpenWith";
	case GalleryContextAction::Copy:
		return "Gallery.Ctx.Copy";
	case GalleryContextAction::CopyPath:
		return "Gallery.Ctx.CopyPath";
	case GalleryContextAction::Rename:
		return "Gallery.Ctx.Rename";
	case GalleryContextAction::Delete:
		return "Gallery.Ctx.Delete";
	default:
		return "Gallery.Ctx.Copy";
	}
}

class SimpleDataObject : public IDataObject {
public:
	explicit SimpleDataObject(const std::wstring &path) : m_ref(1), m_path(path) {}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
	{
		if (!ppv) {
			return E_POINTER;
		}
		if (riid == IID_IUnknown || riid == IID_IDataObject) {
			*ppv = static_cast<IDataObject *>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}

	ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }

	ULONG STDMETHODCALLTYPE Release() override
	{
		ULONG res = InterlockedDecrement(&m_ref);
		if (res == 0) {
			delete this;
		}
		return res;
	}

	HRESULT STDMETHODCALLTYPE GetData(FORMATETC *pformatetc, STGMEDIUM *pmedium) override
	{
		if (!pformatetc || !pmedium) {
			return E_INVALIDARG;
		}
		if (pformatetc->cfFormat != CF_HDROP || !(pformatetc->tymed & TYMED_HGLOBAL)) {
			return DV_E_FORMATETC;
		}
		HGLOBAL hDrop = CreateHDropData(m_path);
		if (!hDrop) {
			return E_OUTOFMEMORY;
		}
		pmedium->tymed = TYMED_HGLOBAL;
		pmedium->hGlobal = hDrop;
		pmedium->pUnkForRelease = nullptr;
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *pformatetc) override
	{
		if (!pformatetc) {
			return E_INVALIDARG;
		}
		if (pformatetc->cfFormat == CF_HDROP && (pformatetc->tymed & TYMED_HGLOBAL)) {
			return S_OK;
		}
		return DV_E_FORMATETC;
	}

	HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *, STGMEDIUM *) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC *, FORMATETC *) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE SetData(FORMATETC *, STGMEDIUM *, BOOL) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC **) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override
	{
		return OLE_E_ADVISENOTSUPPORTED;
	}
	HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
	HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
	LONG m_ref;
	std::wstring m_path;
};

class SimpleDropSource : public IDropSource {
public:
	SimpleDropSource() : m_ref(1) {}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
	{
		if (!ppv) {
			return E_POINTER;
		}
		if (riid == IID_IUnknown || riid == IID_IDropSource) {
			*ppv = static_cast<IDropSource *>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}

	ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }

	ULONG STDMETHODCALLTYPE Release() override
	{
		ULONG res = InterlockedDecrement(&m_ref);
		if (res == 0) {
			delete this;
		}
		return res;
	}

	HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL fEscapePressed, DWORD grfKeyState) override
	{
		if (fEscapePressed) {
			return DRAGDROP_S_CANCEL;
		}
		if (!(grfKeyState & MK_LBUTTON)) {
			return DRAGDROP_S_DROP;
		}
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

private:
	LONG m_ref;
};

} // namespace

void OverlayRenderer::OpenGallery()
{
	if (m_galleryOpen || !m_state.recordingPathConfigured) {
		return;
	}
	m_galleryOpen = true;
	m_galleryPage = 0;
	m_gallerySelectedIndex = -1;
	m_galleryTrimIn = 0.0;
	m_galleryTrimOut = 0.0;
	m_galleryTrimUserSet = false;
	m_galleryExportSubmenuOpen = false;
	m_galleryAudioTrackSubmenuOpen = false;
	m_exportSettingsPendingStart = false;
	m_editingQueueJobId.clear();
	CloseGalleryContextMenu();
	EnsureExportInfrastructure();
	m_trimmer.Reset();
	SetUpdateInterval(16);
	m_videoPlayer.SetMuted(m_galleryMuted);
	m_videoPlayer.SetVolume(static_cast<double>(m_galleryVolume) / 100.0);
	m_windowManager.SetKeyHandler(
		[](int vkCode, int mods, void *userData) {
			OverlayRenderer *r = static_cast<OverlayRenderer *>(userData);
			if (r)
				r->HandleGalleryKey(vkCode, mods);
		},
		this);
	if (saved_settings_data && obs_data_has_user_value(saved_settings_data, "gallery_fullscreen")) {
		m_galleryFullscreen = obs_data_get_bool(saved_settings_data, "gallery_fullscreen");
	}
	ApplyGalleryWindowSize();

	std::string path = m_stateManager.GetConfiguredRecordingPath();
	m_gallery.OpenRoot(path);
	if (!m_gallery.GetItems().empty()) {
		const std::vector<GalleryItem> &items = m_gallery.GetItems();
		for (size_t i = 0; i < items.size(); ++i) {
			if (items[i].type == GalleryItem::Type::Video) {
				m_gallerySelectedIndex = static_cast<int>(i);
				const GalleryItem *item = m_gallery.GetItem(i);
				if (item) {
					m_videoPlayer.Open(item->path);
					m_galleryTrimIn = 0.0;
					m_galleryTrimOut = m_videoPlayer.GetDuration();
					m_galleryTrimUserSet = false;
					ApplyGalleryWindowSize();
				}
				break;
			}
		}
	}
	Render();
}

void OverlayRenderer::OpenGalleryFile(const std::wstring &filePath)
{
	if (filePath.empty())
		return;
	const DWORD attrs = GetFileAttributesW(filePath.c_str());
	if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY))
		return;

	const size_t slash = filePath.find_last_of(L"\\/");
	if (slash == std::wstring::npos)
		return;
	const std::wstring dir = filePath.substr(0, slash);
	const std::wstring fileName = filePath.substr(slash + 1);

	Show();
	if (m_settingsOpen)
		CloseSettings();

	const bool alreadyOpen = m_galleryOpen;
	m_galleryOpen = true;
	m_galleryPage = 0;
	m_gallerySelectedIndex = -1;
	m_galleryTrimIn = 0.0;
	m_galleryTrimOut = 0.0;
	m_galleryTrimUserSet = false;
	m_galleryExportSubmenuOpen = false;
	m_galleryAudioTrackSubmenuOpen = false;
	CloseGalleryContextMenu();
	m_trimmer.Reset();
	SetUpdateInterval(16);
	m_videoPlayer.SetMuted(m_galleryMuted);
	m_videoPlayer.SetVolume(static_cast<double>(m_galleryVolume) / 100.0);
	if (!alreadyOpen) {
		m_windowManager.SetKeyHandler(
			[](int vkCode, int mods, void *userData) {
				OverlayRenderer *r = static_cast<OverlayRenderer *>(userData);
				if (r)
					r->HandleGalleryKey(vkCode, mods);
			},
			this);
		if (saved_settings_data && obs_data_has_user_value(saved_settings_data, "gallery_fullscreen")) {
			m_galleryFullscreen = obs_data_get_bool(saved_settings_data, "gallery_fullscreen");
		}
	}
	ApplyGalleryWindowSize();

	m_gallery.Load(overlay::util::WideToUtf8(dir));

	const std::vector<GalleryItem> &items = m_gallery.GetItems();
	int found = -1;
	for (size_t i = 0; i < items.size(); ++i) {
		if (items[i].type != GalleryItem::Type::Video)
			continue;
		if (_wcsicmp(items[i].path.c_str(), filePath.c_str()) == 0) {
			found = static_cast<int>(i);
			break;
		}
	}
	if (found < 0) {
		for (size_t i = 0; i < items.size(); ++i) {
			if (items[i].type != GalleryItem::Type::Video)
				continue;
			if (_wcsicmp(items[i].name.c_str(), fileName.c_str()) == 0) {
				found = static_cast<int>(i);
				break;
			}
		}
	}

	if (found >= 0) {
		m_gallerySelectedIndex = found;
		const GalleryItem *item = m_gallery.GetItem(static_cast<size_t>(found));
		if (item) {
			m_videoPlayer.Open(item->path);
			m_galleryTrimIn = 0.0;
			m_galleryTrimOut = m_videoPlayer.GetDuration();
			m_galleryTrimUserSet = false;
			ApplyGalleryWindowSize();
		}
	} else {
		m_videoPlayer.Close();
	}

	Render();
}

void OverlayRenderer::CloseGallery()
{
	m_galleryOpen = false;
	m_galleryFullscreen = false;
	m_galleryExportSubmenuOpen = false;
	m_galleryAudioTrackSubmenuOpen = false;
	m_exportSettingsPendingStart = false;
	m_editingQueueJobId.clear();
	CloseGalleryContextMenu();
	m_sharedFolderToOpenOnSuccess.clear();
	m_gallerySelectedIndex = -1;
	m_galleryPage = 0;
	m_windowManager.SetKeyHandler(nullptr, nullptr);
	m_videoPlayer.Close();
	m_trimmer.Reset();
	SetUpdateInterval(500);
	ApplyPosition();
	Render();
}

void OverlayRenderer::ReleaseGalleryFileIfOpen(const std::wstring &filePath)
{
	if (filePath.empty())
		return;

	auto fileNameOf = [](const std::wstring &p) -> std::wstring {
		const size_t slash = p.find_last_of(L"\\/");
		return (slash == std::wstring::npos) ? p : p.substr(slash + 1);
	};
	auto sameFile = [&](const std::wstring &a, const std::wstring &b) -> bool {
		if (a.empty() || b.empty())
			return false;
		if (_wcsicmp(a.c_str(), b.c_str()) == 0)
			return true;
		wchar_t fullA[MAX_PATH];
		wchar_t fullB[MAX_PATH];
		const DWORD na = GetFullPathNameW(a.c_str(), MAX_PATH, fullA, nullptr);
		const DWORD nb = GetFullPathNameW(b.c_str(), MAX_PATH, fullB, nullptr);
		if (na > 0 && na < MAX_PATH && nb > 0 && nb < MAX_PATH && _wcsicmp(fullA, fullB) == 0)
			return true;
		return _wcsicmp(fileNameOf(a).c_str(), fileNameOf(b).c_str()) == 0;
	};

	const std::wstring &openPath = m_videoPlayer.GetOpenPath();
	if (!openPath.empty() && sameFile(openPath, filePath)) {
		obs_log(LOG_INFO, "Gallery: releasing open player file before external delete");
		m_videoPlayer.Close();
		return;
	}

	const GalleryItem *item = m_gallery.GetItem(m_gallerySelectedIndex);
	if (item && item->type == GalleryItem::Type::Video && sameFile(item->path, filePath)) {
		obs_log(LOG_INFO, "Gallery: releasing selected player file before external delete");
		m_videoPlayer.Close();
	}
}

void OverlayRenderer::ReleaseGalleryFileIfOpenSync(const std::wstring &filePath)
{
	struct Ctx {
		OverlayRenderer *self;
		const std::wstring *path;
	} ctx{this, &filePath};
	m_windowManager.RunOnWindowThreadSync(
		[](void *param) {
			Ctx *c = static_cast<Ctx *>(param);
			c->self->ReleaseGalleryFileIfOpen(*c->path);
		},
		&ctx);
}

void OverlayRenderer::NotifyGalleryFileDeleted(const std::wstring &filePath)
{
	if (filePath.empty() || !m_galleryOpen)
		return;

	auto fileNameOf = [](const std::wstring &p) -> std::wstring {
		const size_t slash = p.find_last_of(L"\\/");
		return (slash == std::wstring::npos) ? p : p.substr(slash + 1);
	};
	auto sameFile = [&](const std::wstring &a, const std::wstring &b) -> bool {
		if (a.empty() || b.empty())
			return false;
		if (_wcsicmp(a.c_str(), b.c_str()) == 0)
			return true;
		return _wcsicmp(fileNameOf(a).c_str(), fileNameOf(b).c_str()) == 0;
	};

	const GalleryItem *selected = m_gallery.GetItem(m_gallerySelectedIndex);
	const bool wasSelected = selected && selected->type == GalleryItem::Type::Video &&
				 sameFile(selected->path, filePath);

	// Ensure decoder is not still holding the deleted path.
	if (!m_videoPlayer.GetOpenPath().empty() && sameFile(m_videoPlayer.GetOpenPath(), filePath))
		m_videoPlayer.Close();

	m_gallery.ReloadCurrent();
	const int count = static_cast<int>(m_gallery.GetItems().size());
	if (count == 0) {
		m_gallerySelectedIndex = -1;
		m_videoPlayer.Close();
	} else if (wasSelected) {
		m_gallerySelectedIndex =
			(m_gallerySelectedIndex >= count) ? count - 1 : m_gallerySelectedIndex;
		const GalleryItem *nextItem = m_gallery.GetItem(m_gallerySelectedIndex);
		if (nextItem && nextItem->type == GalleryItem::Type::Video) {
			m_videoPlayer.Open(nextItem->path);
			m_galleryTrimIn = 0.0;
			m_galleryTrimOut = m_videoPlayer.GetDuration();
			m_galleryTrimUserSet = false;
		} else {
			m_gallerySelectedIndex = -1;
			m_videoPlayer.Close();
		}
	} else if (m_gallerySelectedIndex >= count) {
		m_gallerySelectedIndex = count - 1;
	}

	if (IsVisible())
		Render();
}

void OverlayRenderer::NotifyGalleryFileDeletedSync(const std::wstring &filePath)
{
	struct Ctx {
		OverlayRenderer *self;
		const std::wstring *path;
	} ctx{this, &filePath};
	m_windowManager.RunOnWindowThreadSync(
		[](void *param) {
			Ctx *c = static_cast<Ctx *>(param);
			c->self->NotifyGalleryFileDeleted(*c->path);
		},
		&ctx);
}

void OverlayRenderer::ApplyGalleryWindowSize()
{
	if (!m_galleryOpen)
		return;
	RECT workArea = GetTargetWorkArea();
	int workWidth = workArea.right - workArea.left;
	int workHeight = workArea.bottom - workArea.top;

	int targetWidth;
	int targetHeight;
	POINT contentPos;

	if (m_galleryFullscreen) {
		targetWidth = workWidth;
		targetHeight = workHeight;
		contentPos = {workArea.left, workArea.top};
		m_galleryLayoutSrcW = m_videoPlayer.GetSourceWidth();
		m_galleryLayoutSrcH = m_videoPlayer.GetSourceHeight();
	} else {
		constexpr int kOuterPad = 3;
		constexpr int kContentInsetH = 14;
		constexpr int kHeaderH = 30;
		constexpr int kHeaderTop = 12;
		constexpr int kHeaderGap = 12;
		constexpr int kSectionGap = 16;
		constexpr int kPreviewTimelineGap = 28;
		constexpr int kControlsH = 42;
		constexpr int kTimelineH = 30;
		constexpr int kSidebarW = 260;
		constexpr int kQueueW = 280;
		constexpr int kMinPreviewW = 480;
		constexpr int kMinWindowH = 520;

		targetHeight = std::min(820, workHeight - 40);
		if (targetHeight < kMinWindowH)
			targetHeight = std::max(kMinWindowH, workHeight - 20);

		const int contentTop = kOuterPad + kHeaderTop + kHeaderH + kHeaderGap;
		const int contentBottom = targetHeight - kOuterPad - kContentInsetH;
		const int contentH = std::max(1, contentBottom - contentTop);
		int previewH = contentH - kControlsH - kSectionGap - kTimelineH - kPreviewTimelineGap;
		if (previewH < 200)
			previewH = 200;

		int srcW = m_videoPlayer.GetSourceWidth();
		int srcH = m_videoPlayer.GetSourceHeight();
		if (srcW <= 0 || srcH <= 0) {
			srcW = 16;
			srcH = 9;
		}
		m_galleryLayoutSrcW = srcW;
		m_galleryLayoutSrcH = srcH;

		const double aspect = static_cast<double>(srcW) / static_cast<double>(srcH);
		int previewW = static_cast<int>(std::lround(static_cast<double>(previewH) * aspect));
		if (previewW < kMinPreviewW)
			previewW = kMinPreviewW;

		const int chromeW = kOuterPad * 2 + kContentInsetH * 2 + kSidebarW + kSectionGap + kSectionGap + kQueueW;
		targetWidth = chromeW + previewW;

		const int maxW = std::max(800, workWidth - 40);
		if (targetWidth > maxW) {
			// Monitor too narrow: shrink preview width, then height so video still fills preview.
			previewW = std::max(kMinPreviewW, maxW - chromeW);
			previewH = static_cast<int>(std::lround(static_cast<double>(previewW) / aspect));
			if (previewH < 200)
				previewH = 200;
			const int neededContentH = previewH + kControlsH + kSectionGap + kTimelineH + kPreviewTimelineGap;
			targetHeight = kOuterPad + kHeaderTop + kHeaderH + kHeaderGap + neededContentH + kContentInsetH +
				       kOuterPad;
			if (targetHeight > workHeight - 40)
				targetHeight = workHeight - 40;
			if (targetHeight < kMinWindowH)
				targetHeight = std::min(kMinWindowH, workHeight - 20);
			targetWidth = chromeW + previewW;
			if (targetWidth > maxW)
				targetWidth = maxW;
		}

		contentPos = {workArea.left + (workWidth - targetWidth) / 2,
			     workArea.top + (workHeight - targetHeight) / 2};
	}

	const bool sizeChanged =
		(targetWidth != m_galleryContentWidth || targetHeight != m_galleryContentHeight);
	m_galleryContentWidth = targetWidth;
	m_galleryContentHeight = targetHeight;

	if (IsDimmingActive()) {
		ApplyDimmingWindowGeometry(contentPos, targetWidth, targetHeight);
	} else if (sizeChanged) {
		m_dimContentOffset = {0, 0};
		m_windowManager.SetPosition(contentPos, targetWidth, targetHeight);
	}
}

void OverlayRenderer::RenderGallery()
{
	int width;
	int height;
	if (IsDimmingActive()) {
		width = m_galleryContentWidth;
		height = m_galleryContentHeight;
	} else {
		HWND hwnd = m_windowManager.GetHWND();
		if (!hwnd) {
			return;
		}
		RECT clientRect;
		GetClientRect(hwnd, &clientRect);
		width = clientRect.right - clientRect.left;
		height = clientRect.bottom - clientRect.top;
	}

	if (width <= 0 || height <= 0) {
		return;
	}

	const RECT clientRect = {0, 0, width, height};

	EnsureExportInfrastructure();
	SyncExportQueueFolder();
	m_exportQueue.Tick();
	SyncExportQueueTimer();

	const int srcW = m_videoPlayer.GetSourceWidth();
	const int srcH = m_videoPlayer.GetSourceHeight();
	if (!m_galleryFullscreen && srcW > 0 && srcH > 0 &&
	    (srcW != m_galleryLayoutSrcW || srcH != m_galleryLayoutSrcH)) {
		ApplyGalleryWindowSize();
		if (IsDimmingActive()) {
			width = m_galleryContentWidth;
			height = m_galleryContentHeight;
		} else {
			HWND hwnd = m_windowManager.GetHWND();
			if (hwnd) {
				RECT clientRect;
				GetClientRect(hwnd, &clientRect);
				width = clientRect.right - clientRect.left;
				height = clientRect.bottom - clientRect.top;
			}
		}
	}

	int outerPadding = 3;
	int sectionGap = 16;
	int contentInsetH = 14;
	int headerHeight = 30;
	int sidebarWidth = 260;
	int queueWidth = 280;
	int controlsHeight = 42;
	int timelineHeight = 30;

	const RECT shellRect = {outerPadding, outerPadding, width - outerPadding, height - outerPadding};
	const RECT headerRect = {shellRect.left + contentInsetH, shellRect.top + 12, shellRect.right - contentInsetH,
				 shellRect.top + 12 + headerHeight};
	const int contentTop = headerRect.bottom + 12;
	const int contentBottom = shellRect.bottom - contentInsetH;

	m_galleryListRect = {shellRect.left + contentInsetH, contentTop, shellRect.left + contentInsetH + sidebarWidth,
			     contentBottom};
	m_galleryQueueRect = {shellRect.right - contentInsetH - queueWidth, contentTop, shellRect.right - contentInsetH,
			      contentBottom};
	const int previewColumnLeft = m_galleryListRect.right + sectionGap;
	const int previewColumnRight = m_galleryQueueRect.left - sectionGap;

	m_galleryControlsRect = {previewColumnLeft, contentBottom - controlsHeight, previewColumnRight, contentBottom};
	m_galleryTimelineRect = {previewColumnLeft, m_galleryControlsRect.top - sectionGap - timelineHeight,
				 previewColumnRight, m_galleryControlsRect.top - sectionGap};
	const int previewTimelineGap = 28;
	m_galleryPreviewRect = {previewColumnLeft, contentTop, previewColumnRight,
				m_galleryTimelineRect.top - previewTimelineGap};

	const int headerIconSize = 30;
	const int headerIconGap = 8;
	m_galleryBackRect = {headerRect.right - headerIconSize, headerRect.top, headerRect.right, headerRect.bottom};
	m_galleryFullscreenRect = {m_galleryBackRect.left - headerIconGap - headerIconSize, headerRect.top,
				   m_galleryBackRect.left - headerIconGap, headerRect.bottom};
	const int folderBackWidth = 96;
	m_galleryFolderBackRect = {headerRect.left + 110, headerRect.top, headerRect.left + 110 + folderBackWidth,
				   headerRect.bottom};
	m_galleryPrevPageRect = {m_galleryListRect.left + 12, m_galleryListRect.bottom - 34, m_galleryListRect.left + 78,
				 m_galleryListRect.bottom - 10};
	m_galleryNextPageRect = {m_galleryListRect.right - 78, m_galleryListRect.bottom - 34, m_galleryListRect.right - 12,
				 m_galleryListRect.bottom - 10};

	DrawRoundedRectangle(clientRect, 14.0f,
			     D2D1::ColorF(20.0f / 255.0f, 20.0f / 255.0f, 25.0f / 255.0f, m_overlayBackgroundAlpha));
	DrawRoundedRectangle(shellRect, 14.0f, D2D1::ColorF(20.0f / 255.0f, 20.0f / 255.0f, 25.0f / 255.0f, 0.95f));
	DrawRoundedRectangle(m_galleryListRect, 12.0f, D2D1::ColorF(26.0f / 255.0f, 26.0f / 255.0f, 34.0f / 255.0f, 1.0f));
	RenderText(overlay::util::ModuleTextW("Media"), {m_galleryListRect.left + 12, headerRect.top, m_galleryListRect.right - 12, headerRect.bottom},
		   13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, D2D1::ColorF(0.93f, 0.95f, 1.0f), DWRITE_TEXT_ALIGNMENT_LEADING);
	// Header controls
	const D2D1_COLOR_F headerIconColor = D2D1::ColorF(0.85f, 0.85f, 0.9f);
	const D2D1_COLOR_F hoverColor = AccentHover();
	const D2D1_COLOR_F hoverBgColor = AccentHover(0.25f);

	const bool hoverFs = (m_hoverTarget == HoverTarget::GalleryFullscreen);
	const bool hoverBack = (m_hoverTarget == HoverTarget::GalleryBack);
	const bool hoverFolderBack = (m_hoverTarget == HoverTarget::GalleryFolderBack);
	if (hoverFs) {
		DrawRoundedRectangle(m_galleryFullscreenRect, 8.0f, hoverBgColor);
	}
	if (hoverBack) {
		DrawRoundedRectangle(m_galleryBackRect, 8.0f, hoverBgColor);
	}
	const bool canGoUp = m_gallery.CanGoUp();
	const D2D1_COLOR_F folderBg =
		canGoUp ? D2D1::ColorF(40.0f / 255.0f, 40.0f / 255.0f, 50.0f / 255.0f, 1.0f)
			: D2D1::ColorF(34.0f / 255.0f, 34.0f / 255.0f, 42.0f / 255.0f, 1.0f);
	DrawRoundedRectangle(m_galleryFolderBackRect, 8.0f, folderBg);
	if (hoverFolderBack && canGoUp) {
		DrawRoundedRectangle(m_galleryFolderBackRect, 8.0f, hoverBgColor);
	}
	const D2D1_COLOR_F fsTint = hoverFs ? hoverColor : headerIconColor;
	const D2D1_COLOR_F backTint = hoverBack ? hoverColor : headerIconColor;
	const D2D1_COLOR_F folderBackTint =
		canGoUp ? (hoverFolderBack ? hoverColor : D2D1::ColorF(0.86f, 0.9f, 0.97f))
			: D2D1::ColorF(0.52f, 0.56f, 0.64f);
	DrawGalleryIcon(m_galleryFullscreenRect, m_galleryFullscreen ? 9 : 8, &fsTint);
	DrawGalleryIcon(m_galleryBackRect, 7, &backTint);
	RenderText(overlay::util::ModuleTextW("Back"), m_galleryFolderBackRect, 10.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, folderBackTint);

	RenderGalleryList(m_galleryListRect, 52, 0);

	DrawRoundedRectangle(m_galleryPreviewRect, 0.0f,
			     D2D1::ColorF(30.0f / 255.0f, 30.0f / 255.0f, 38.0f / 255.0f, 1.0f));
	// Match decode resolution to preview size to save CPU.
	m_videoPlayer.SetTargetSize(m_galleryPreviewRect.right - m_galleryPreviewRect.left,
				    m_galleryPreviewRect.bottom - m_galleryPreviewRect.top);

	OverlayVideoPlayer::FrameView frame;
	if (m_videoPlayer.AcquireFrame(frame)) {
		const int fW = frame.width;
		const int fH = frame.height;
		if (fW > 0 && fH > 0 && frame.data) {
			if (!m_galleryVideoBitmap || m_galleryVideoBitmapW != fW || m_galleryVideoBitmapH != fH) {
				if (m_galleryVideoBitmap) {
					m_galleryVideoBitmap->Release();
					m_galleryVideoBitmap = nullptr;
				}
				m_galleryVideoBitmapW = 0;
				m_galleryVideoBitmapH = 0;
				D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
					D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
				if (SUCCEEDED(m_bitmapRenderTarget->CreateBitmap(D2D1::SizeU(fW, fH), nullptr, 0,
										 &props, &m_galleryVideoBitmap))) {
					m_galleryVideoBitmapW = fW;
					m_galleryVideoBitmapH = fH;
				}
			}
			if (m_galleryVideoBitmap) {
				m_galleryVideoBitmap->CopyFromMemory(nullptr, frame.data, fW * 4);
				float srcAspect = static_cast<float>(fW) / static_cast<float>(fH);
				float destW =
					static_cast<float>(m_galleryPreviewRect.right - m_galleryPreviewRect.left);
				float destH =
					static_cast<float>(m_galleryPreviewRect.bottom - m_galleryPreviewRect.top);
				float destAspect = destW / std::max(1.0f, destH);
				D2D1_RECT_F destRect = D2D1::RectF(static_cast<float>(m_galleryPreviewRect.left),
								   static_cast<float>(m_galleryPreviewRect.top),
								   static_cast<float>(m_galleryPreviewRect.right),
								   static_cast<float>(m_galleryPreviewRect.bottom));
				constexpr float kAspectEps = 0.02f;
				if (std::fabs(srcAspect - destAspect) > kAspectEps) {
					if (srcAspect > destAspect) {
						float newH = destW / srcAspect;
						float top = static_cast<float>(m_galleryPreviewRect.top) +
							    (destH - newH) * 0.5f;
						destRect.top = top;
						destRect.bottom = top + newH;
					} else {
						float newW = destH * srcAspect;
						float left = static_cast<float>(m_galleryPreviewRect.left) +
							     (destW - newW) * 0.5f;
						destRect.left = left;
						destRect.right = left + newW;
					}
				}
				if (m_galleryFullscreen) {
					DrawRoundedRectangle(m_galleryPreviewRect, 0.0f,
							     D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));
				}
				m_bitmapRenderTarget->DrawBitmap(m_galleryVideoBitmap, destRect);
			}
		}
	} else {
		RenderText(overlay::util::ModuleTextW("PreviewUnavailable"), m_galleryPreviewRect, 12.0f, DWRITE_FONT_WEIGHT_NORMAL,
			   D2D1::ColorF(0.72f, 0.76f, 0.84f));
	}

	m_galleryExportCancelRect = RECT{0, 0, 0, 0};

	// Timeline
	DrawRoundedRectangle(m_galleryTimelineRect, 10.0f, D2D1::ColorF(40.0f / 255.0f, 40.0f / 255.0f, 50.0f / 255.0f, 1.0f));
	DrawRoundedRectangleBorder(m_galleryTimelineRect, 10.0f, 1.0f,
				   D2D1::ColorF(58.0f / 255.0f, 58.0f / 255.0f, 69.0f / 255.0f, 1.0f));
	double duration = m_videoPlayer.GetDuration();
	double current = m_videoPlayer.GetCurrentTime();
	double trimIn = m_galleryTrimIn;
	double trimOut = m_galleryTrimOut;
	double displayDuration = duration;
	if (displayDuration <= 0.0) {
		displayDuration = std::max(trimOut, current);
	}
	if (displayDuration > 0.0) {
		int left = m_galleryTimelineRect.left + 6;
		int right = m_galleryTimelineRect.right - 6;
		int top = m_galleryTimelineRect.top + 10;
		int bottom = m_galleryTimelineRect.bottom - 10;
		int trackWidth = right - left;
		const std::wstring &openPath = m_videoPlayer.GetOpenPath();
		auto sameMediaPath = [](const std::wstring &a, const std::wstring &b) -> bool {
			if (a.empty() || b.empty())
				return false;
			if (_wcsicmp(a.c_str(), b.c_str()) == 0)
				return true;
			wchar_t fullA[MAX_PATH];
			wchar_t fullB[MAX_PATH];
			const DWORD na = GetFullPathNameW(a.c_str(), MAX_PATH, fullA, nullptr);
			const DWORD nb = GetFullPathNameW(b.c_str(), MAX_PATH, fullB, nullptr);
			return na > 0 && na < MAX_PATH && nb > 0 && nb < MAX_PATH && _wcsicmp(fullA, fullB) == 0;
		};

		double startRatio = std::max(0.0, std::min(1.0, trimIn / displayDuration));
		double endRatio = std::max(0.0, std::min(1.0, trimOut / displayDuration));
		double curRatio = std::max(0.0, std::min(1.0, current / displayDuration));
		int trimLeft = left + static_cast<int>(trackWidth * startRatio);
		int trimRight = left + static_cast<int>(trackWidth * endRatio);
		RECT trimRect = {trimLeft, top, std::max(trimLeft + 4, trimRight), bottom};
		D2D1_COLOR_F trimColor = AccentMuted();
		if (!m_editingQueueJobId.empty()) {
			for (const auto &job : m_exportQueue.GetJobs()) {
				if (job.id == m_editingQueueJobId && job.markerColorRgb != 0) {
					trimColor = ColorFromRgb(job.markerColorRgb, 0.80f);
					break;
				}
			}
		}
		DrawRoundedRectangle(trimRect, 4.0f, trimColor);

		if (m_exportInfraReady && !openPath.empty() && trackWidth > 0) {
			for (const auto &job : m_exportQueue.GetJobs()) {
				if (job.inputPath.empty() || !sameMediaPath(job.inputPath, openPath))
					continue;
				if (!m_editingQueueJobId.empty() && job.id == m_editingQueueJobId)
					continue; // active trim already uses this job's color
				double jIn = std::max(0.0, job.startSeconds);
				double jOut = job.endSeconds;
				if (!(jOut > jIn))
					jOut = displayDuration;
				jOut = std::min(jOut, displayDuration);
				if (jOut <= jIn)
					continue;
				const double sR = std::max(0.0, std::min(1.0, jIn / displayDuration));
				const double eR = std::max(0.0, std::min(1.0, jOut / displayDuration));
				const int jL = left + static_cast<int>(trackWidth * sR);
				const int jR = left + static_cast<int>(trackWidth * eR);
				RECT mark = {jL, top + 1, std::max(jL + 3, jR), bottom - 1};
				const int rgb = job.markerColorRgb != 0 ? job.markerColorRgb : 0xE67E22;
				DrawRoundedRectangle(mark, 3.0f, ColorFromRgb(rgb, 0.55f));
				DrawRoundedRectangleBorder(mark, 3.0f, 1.5f, ColorFromRgb(rgb, 1.0f));
			}
		}

		int playX = left + static_cast<int>(trackWidth * curRatio);
		RECT playRect = {playX - 2, m_galleryTimelineRect.top, playX + 2, m_galleryTimelineRect.bottom};
		DrawRoundedRectangle(playRect, 2.0f,
				     D2D1::ColorF(220.0f / 255.0f, 220.0f / 255.0f, 230.0f / 255.0f, 1.0f));
	}
	std::wstring timelineProgress = FormatTime(current) + L" / " + FormatTime(displayDuration);
	RECT progressRect = {m_galleryTimelineRect.left + 10, m_galleryTimelineRect.top - 20,
			     m_galleryTimelineRect.right - 10, m_galleryTimelineRect.top - 2};
	RenderText(timelineProgress, progressRect, 10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
		   D2D1::ColorF(0.86f, 0.86f, 0.9f), DWRITE_TEXT_ALIGNMENT_TRAILING);

	RenderGalleryControls(m_galleryControlsRect);
	RenderGalleryExportQueue(m_galleryQueueRect);
	RenderGalleryContextMenu(width, height);
}

void OverlayRenderer::RenderGalleryExportQueue(const RECT &queueRect)
{
	m_queueItemRects.clear();
	m_queueCancelRects.clear();
	m_queueRemoveRects.clear();
	m_queueRetryRects.clear();
	m_queueEditRects.clear();
	m_queueRenameRects.clear();
	m_queueOpenRects.clear();
	m_queueUpRects.clear();
	m_queueDownRects.clear();
	m_queueItemIds.clear();
	m_galleryQueueStartRect = RECT{0, 0, 0, 0};
	m_galleryQueueClearRect = RECT{0, 0, 0, 0};
	m_galleryQueueOpenFolderRect = RECT{0, 0, 0, 0};

	DrawRoundedRectangle(queueRect, 12.0f, D2D1::ColorF(30.0f / 255.0f, 30.0f / 255.0f, 38.0f / 255.0f, 1.0f));
	DrawRoundedRectangleBorder(queueRect, 12.0f, 1.0f,
				   D2D1::ColorF(58.0f / 255.0f, 58.0f / 255.0f, 69.0f / 255.0f, 1.0f));

	const int clearBtnW = 78;
	const int clearBtnH = 26;
	m_galleryQueueClearRect = {queueRect.right - 12 - clearBtnW, queueRect.top + 10, queueRect.right - 12,
				   queueRect.top + 10 + clearBtnH};
	const bool mergeMode = m_exportQueue.GetMode() == ExportQueueMode::Merge;
	double mergeProgress = 0.0;
	const bool showMergeProgress = mergeMode && m_exportQueue.GetMergeProgress(mergeProgress);

	RenderText(overlay::util::ModuleTextW("Export.Queue"),
		   RECT{queueRect.left + 12, queueRect.top + 12, m_galleryQueueClearRect.left - 8, queueRect.top + 34},
		   11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, D2D1::ColorF(0.93f, 0.95f, 1.0f),
		   DWRITE_TEXT_ALIGNMENT_LEADING);
	if (showMergeProgress) {
		wchar_t mergePct[24];
		swprintf(mergePct, 24, L"%s %d%%", overlay::util::ModuleTextW("Export.Merge").c_str(),
			 static_cast<int>(mergeProgress * 100.0 + 0.5));
		RenderText(mergePct,
			   RECT{queueRect.left + 12, queueRect.top + 12, m_galleryQueueClearRect.left - 8,
				queueRect.top + 34},
			   11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, D2D1::ColorF(0.95f, 0.72f, 0.28f),
			   DWRITE_TEXT_ALIGNMENT_TRAILING);
	}
	const bool canClear = !m_exportQueue.IsEmpty();
	DrawRoundedRectangle(m_galleryQueueClearRect, 8.0f,
			     canClear ? D2D1::ColorF(0.42f, 0.18f, 0.18f, 1.0f)
				      : D2D1::ColorF(40.0f / 255.0f, 40.0f / 255.0f, 50.0f / 255.0f, 1.0f));
	RenderText(overlay::util::ModuleTextW("Export.ClearQueue"), m_galleryQueueClearRect, 9.5f,
		   DWRITE_FONT_WEIGHT_SEMI_BOLD, canClear ? D2D1::ColorF(1, 1, 1) : D2D1::ColorF(0.55f, 0.55f, 0.6f));

	const int footerH = 72;
	RECT listArea = {queueRect.left + 10, queueRect.top + 44, queueRect.right - 10, queueRect.bottom - footerH};
	const auto jobs = m_exportQueue.GetJobs();
	const int rowH = 68;
	const int rowGap = 6;
	int y = listArea.top - m_queueScroll;
	for (const auto &job : jobs) {
		if (y + rowH < listArea.top) {
			y += rowH + rowGap;
			continue;
		}
		if (y > listArea.bottom)
			break;
		const int rowBottom = (y + rowH < listArea.bottom) ? (y + rowH) : listArea.bottom;
		RECT row = {listArea.left, y, listArea.right, rowBottom};
		m_queueItemRects.push_back(row);
		m_queueItemIds.push_back(job.id);
		const bool editingRow = (!m_editingQueueJobId.empty() && job.id == m_editingQueueJobId);
		DrawRoundedRectangle(row, 8.0f,
				     editingRow ? D2D1::ColorF(34.0f / 255.0f, 34.0f / 255.0f, 46.0f / 255.0f, 1.0f)
						: D2D1::ColorF(26.0f / 255.0f, 26.0f / 255.0f, 34.0f / 255.0f, 1.0f));
		const int markerRgb = job.markerColorRgb != 0 ? job.markerColorRgb : 0xE67E22;
		RECT colorBar = {row.left + 3, row.top + 6, row.left + 7, row.bottom - 6};
		DrawRoundedRectangle(colorBar, 2.0f, ColorFromRgb(markerRgb, 1.0f));
		if (editingRow)
			DrawRoundedRectangleBorder(row, 8.0f, 1.0f, ColorFromRgb(markerRgb, 0.9f));

		const int remW = 22;
		const int remH = 22;
		RECT remRect = {row.right - 8 - remW, row.top + 6, row.right - 8, row.top + 6 + remH};
		m_queueRemoveRects.push_back(remRect);
		DrawRoundedRectangle(remRect, 6.0f, D2D1::ColorF(0.4f, 0.16f, 0.16f, 1.0f));
		if (m_bitmapRenderTarget && m_textBrush) {
			m_textBrush->SetColor(D2D1::ColorF(1.0f, 0.85f, 0.85f));
			const float inset = 6.5f;
			const float x0 = static_cast<float>(remRect.left) + inset;
			const float y0 = static_cast<float>(remRect.top) + inset;
			const float x1 = static_cast<float>(remRect.right) - inset;
			const float y1 = static_cast<float>(remRect.bottom) - inset;
			m_bitmapRenderTarget->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), m_textBrush, 1.8f);
			m_bitmapRenderTarget->DrawLine(D2D1::Point2F(x1, y0), D2D1::Point2F(x0, y1), m_textBrush, 1.8f);
		}

		std::wstring st;
		switch (job.status) {
		case ExportJobStatus::Queued:
			st = overlay::util::ModuleTextW("Export.Status.Queued");
			break;
		case ExportJobStatus::Running:
			st = overlay::util::ModuleTextW("Export.Status.Running");
			break;
		case ExportJobStatus::Success:
			st = overlay::util::ModuleTextW("Done");
			break;
		case ExportJobStatus::Failed:
			st = overlay::util::ModuleTextW("Failed");
			break;
		case ExportJobStatus::Cancelled:
			st = overlay::util::ModuleTextW("Export.Status.Cancelled");
			break;
		}
		RenderText(job.displayName, RECT{row.left + 12, row.top + 6, remRect.left - 6, row.top + 24}, 10.0f,
			   DWRITE_FONT_WEIGHT_SEMI_BOLD, D2D1::ColorF(0.95f, 0.96f, 1.0f),
			   DWRITE_TEXT_ALIGNMENT_LEADING);
		std::wstring meta = st;
		if (job.status == ExportJobStatus::Running && !mergeMode) {
			wchar_t pbuf[16];
			swprintf(pbuf, 16, L" %d%%", static_cast<int>(job.progress * 100.0 + 0.5));
			meta += pbuf;
		} else if (job.status == ExportJobStatus::Running && mergeMode) {
			meta += L" · ";
			meta += FormatTime(job.startSeconds);
			meta += L"–";
			if (job.endSeconds > job.startSeconds)
				meta += FormatTime(job.endSeconds);
			else
				meta += L"EOF";
		} else if (job.status == ExportJobStatus::Failed && !job.errorText.empty()) {
			meta += L": ";
			meta += job.errorText.size() > 32 ? job.errorText.substr(0, 32) + L"…" : job.errorText;
		} else {
			meta += L" · ";
			meta += FormatTime(job.startSeconds);
			meta += L"–";
			if (job.endSeconds > job.startSeconds)
				meta += FormatTime(job.endSeconds);
			else
				meta += L"EOF";
		}
		RenderText(meta, RECT{row.left + 12, row.top + 24, row.right - 10, row.top + 40}, 8.5f,
			   DWRITE_FONT_WEIGHT_NORMAL, D2D1::ColorF(0.66f, 0.71f, 0.82f),
			   DWRITE_TEXT_ALIGNMENT_LEADING);

		const int btnH = 22;
		int btnY = row.bottom - btnH - 6;
		int bx = row.left + 8;
		auto pushBtn = [&](std::vector<RECT> &store, const std::wstring &label, int w) {
			RECT r = {bx, btnY, bx + w, btnY + btnH};
			store.push_back(r);
			DrawRoundedRectangle(r, 6.0f, D2D1::ColorF(40.0f / 255.0f, 40.0f / 255.0f, 50.0f / 255.0f, 1.0f));
			RenderText(label, r, 9.0f, DWRITE_FONT_WEIGHT_NORMAL, D2D1::ColorF(0.9f, 0.9f, 0.9f));
			bx += w + 5;
		};
		pushBtn(m_queueUpRects, L"↑", 24);
		pushBtn(m_queueDownRects, L"↓", 24);
		pushBtn(m_queueEditRects, overlay::util::ModuleTextW("Export.Edit"), 52);
		if (job.status != ExportJobStatus::Running)
			pushBtn(m_queueRenameRects, overlay::util::ModuleTextW("Export.Rename"), 58);
		else
			m_queueRenameRects.push_back(RECT{0, 0, 0, 0});
		m_queueCancelRects.push_back(RECT{0, 0, 0, 0}); // cancel is the footer Export/Cancel button
		if (job.status == ExportJobStatus::Failed || job.status == ExportJobStatus::Cancelled)
			pushBtn(m_queueRetryRects, overlay::util::ModuleTextW("Export.Retry"), 58);
		else
			m_queueRetryRects.push_back(RECT{0, 0, 0, 0});
		if (job.status == ExportJobStatus::Success && !job.outputFolder.empty())
			pushBtn(m_queueOpenRects, overlay::util::ModuleTextW("Export.OpenDir"), 54);
		else
			m_queueOpenRects.push_back(RECT{0, 0, 0, 0});
		y += rowH + rowGap;
	}

	RECT footer = {queueRect.left + 10, queueRect.bottom - footerH + 6, queueRect.right - 10, queueRect.bottom - 10};

	m_galleryQueueStartRect = {footer.left, footer.top, footer.right, footer.top + 30};
	const bool exportBusy = m_exportQueue.IsBusy();
	DrawRoundedRectangle(m_galleryQueueStartRect, 8.0f,
			     exportBusy ? D2D1::ColorF(0.42f, 0.18f, 0.18f, 1.0f) : AccentSelected());
	RenderText(exportBusy ? overlay::util::ModuleTextW("Cancel") : overlay::util::ModuleTextW("Export.Start"),
		   m_galleryQueueStartRect, 11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, D2D1::ColorF(1, 1, 1));

	if (m_exportQueue.HasAnySuccess() && !m_exportQueue.IsBusy()) {
		std::wstring common;
		const bool oneFolder = m_exportQueue.AllOutputsSameFolder(common);
		if (oneFolder) {
			m_galleryQueueOpenFolderRect = {footer.left, footer.top + 36, footer.right, footer.bottom};
			DrawRoundedRectangle(m_galleryQueueOpenFolderRect, 8.0f,
					     D2D1::ColorF(40.0f / 255.0f, 40.0f / 255.0f, 50.0f / 255.0f, 1.0f));
			RenderText(overlay::util::ModuleTextW("Export.OpenFolder"), m_galleryQueueOpenFolderRect, 9.5f,
				   DWRITE_FONT_WEIGHT_NORMAL, D2D1::ColorF(0.9f, 0.9f, 0.9f));
		}
	}
}

void OverlayRenderer::RenderGalleryList(const RECT &listRect, int itemHeight, int itemsPerPageHint)
{
	UNUSED_PARAMETER(itemsPerPageHint);
	int itemsAreaBottom = listRect.bottom - 34;
	int itemsAreaHeight = itemsAreaBottom - listRect.top;
	int itemsPerPage = std::max(1, itemsAreaHeight / itemHeight);
	int startIndex = m_galleryPage * itemsPerPage;
	const std::vector<GalleryItem> &items = m_gallery.GetItems();

	DrawRoundedRectangle(listRect, 12.0f, D2D1::ColorF(26.0f / 255.0f, 26.0f / 255.0f, 34.0f / 255.0f, 1.0f));

	if (items.empty()) {
		RenderText(overlay::util::ModuleTextW("NoMediaFound"), listRect, 11.0f, DWRITE_FONT_WEIGHT_NORMAL,
			   D2D1::ColorF(0.6f, 0.6f, 0.6f));
		return;
	}

	for (int i = 0; i < itemsPerPage; ++i) {
		int itemIndex = startIndex + i;
		if (itemIndex >= static_cast<int>(items.size())) {
			break;
		}
		RECT itemRect = {listRect.left + 8, listRect.top + 8 + i * itemHeight, listRect.right - 8,
				 listRect.top + 8 + (i + 1) * itemHeight - 6};
		if (itemIndex == m_gallerySelectedIndex) {
			DrawRoundedRectangle(itemRect, 8.0f, AccentSelected());
		} else {
			DrawRoundedRectangle(itemRect, 8.0f, D2D1::ColorF(26.0f / 255.0f, 26.0f / 255.0f, 34.0f / 255.0f, 0.8f));
		}
		RECT titleRect = {itemRect.left + 10, itemRect.top + 4, itemRect.right - 10, itemRect.top + 22};
		RenderText(items[itemIndex].name, titleRect, 10.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
			   D2D1::ColorF(0.95f, 0.96f, 1.0f), DWRITE_TEXT_ALIGNMENT_LEADING);
		std::wstring meta;
		if (items[itemIndex].type == GalleryItem::Type::Folder) {
			meta = overlay::util::ModuleTextW("Folder");
		} else {
			std::wstring ext = GetFileExtension(items[itemIndex].name);
			if (!ext.empty() && ext[0] == L'.')
				ext = ext.substr(1);
			for (auto &ch : ext)
				ch = static_cast<wchar_t>(towupper(ch));
			meta = (ext.empty() ? overlay::util::ModuleTextW("VideoLabel") : ext) + L"  " + FormatFileSize(items[itemIndex].sizeBytes);
		}
		RECT metaRect = {itemRect.left + 10, itemRect.top + 22, itemRect.right - 10, itemRect.bottom - 3};
		RenderText(meta, metaRect, 8.5f, DWRITE_FONT_WEIGHT_NORMAL, D2D1::ColorF(0.66f, 0.71f, 0.82f),
			   DWRITE_TEXT_ALIGNMENT_LEADING);
	}

	DrawRoundedRectangle(m_galleryPrevPageRect, 8.0f, D2D1::ColorF(40.0f / 255.0f, 40.0f / 255.0f, 50.0f / 255.0f, 1.0f));
	DrawRoundedRectangle(m_galleryNextPageRect, 8.0f, D2D1::ColorF(40.0f / 255.0f, 40.0f / 255.0f, 50.0f / 255.0f, 1.0f));
	RenderText(overlay::util::ModuleTextW("Prev"), m_galleryPrevPageRect, 9.0f, DWRITE_FONT_WEIGHT_NORMAL, D2D1::ColorF(0.84f, 0.87f, 0.95f));
	RenderText(overlay::util::ModuleTextW("Next"), m_galleryNextPageRect, 9.0f, DWRITE_FONT_WEIGHT_NORMAL, D2D1::ColorF(0.84f, 0.87f, 0.95f));
}

void OverlayRenderer::RenderGalleryControls(const RECT &controlsRect)
{
	DrawRoundedRectangle(controlsRect, 12.0f, D2D1::ColorF(30.0f / 255.0f, 30.0f / 255.0f, 38.0f / 255.0f, 1.0f));
	DrawRoundedRectangleBorder(controlsRect, 12.0f, 1.0f,
				   D2D1::ColorF(58.0f / 255.0f, 58.0f / 255.0f, 69.0f / 255.0f, 1.0f));
	int spacing = 8;
	int sideInset = 10;
	int panelH = controlsRect.bottom - controlsRect.top;
	int contentH = 30;
	int contentTop = controlsRect.top + (panelH - contentH) / 2;
	int contentBottom = contentTop + contentH;

	int muteIconW = 24;
	int volBarW = 56;
	int volBarH = 8;
	int btnWidth = 30;
	int btnSpacing = 5;

	int centerY = contentTop + contentH / 2;
	int muteTop = centerY - muteIconW / 2;
	int muteBottom = centerY + muteIconW / 2;
	if (muteTop < contentTop)
		muteTop = contentTop;
	if (muteBottom > contentBottom)
		muteBottom = contentBottom;

	// Left zone
	m_galleryMuteRect = {controlsRect.left + sideInset, muteTop, controlsRect.left + sideInset + muteIconW, muteBottom};
	int volTop = centerY - volBarH / 2;
	int volBottom = centerY + volBarH / 2;
	m_galleryVolumeRect = {m_galleryMuteRect.right + spacing, volTop, m_galleryMuteRect.right + spacing + volBarW,
			       volBottom};

	int audioTrackCount = m_videoPlayer.GetAudioTrackCount();
	m_galleryAudioTrackPrevRect = RECT{0, 0, 0, 0};
	m_galleryAudioTrackNextRect = RECT{0, 0, 0, 0};
	m_galleryAudioTrackMenuRect = RECT{0, 0, 0, 0};
	m_galleryAudioTrackOptionRects.clear();
	if (audioTrackCount > 1) {
		int atLeft = m_galleryVolumeRect.right + spacing;
		int atBtnW = 90;
		int atBtnH = contentBottom - contentTop;
		UNUSED_PARAMETER(atBtnH);
		m_galleryAudioTrackButtonRect = {atLeft, contentTop, atLeft + atBtnW, contentBottom};
		std::wstring atLabel;
		{
			int cur = m_videoPlayer.GetCurrentAudioTrackIndex();
			OverlayVideoPlayer::AudioTrackInfo info;
			m_videoPlayer.GetAudioTrackInfo(cur, info);
			atLabel = WideFromUtf8(info.name);
			if (atLabel.empty())
				atLabel = overlay::util::ModuleTextW("Track") + L" " + std::to_wstring(cur + 1);
		}
		DrawRoundedRectangle(m_galleryAudioTrackButtonRect, atBtnH / 2.0f,
				     D2D1::ColorF(40.0f / 255.0f, 40.0f / 255.0f, 50.0f / 255.0f, 1.0f));
		RenderText(atLabel, m_galleryAudioTrackButtonRect, 9.5f, DWRITE_FONT_WEIGHT_NORMAL,
			   D2D1::ColorF(0.9f, 0.9f, 0.9f));
		if (m_galleryAudioTrackSubmenuOpen) {
			int optH = 22;
			int pad = 4;
			int subW = 160;
			int subLeft = m_galleryAudioTrackButtonRect.left;
			int subTop = m_galleryAudioTrackButtonRect.top - pad - audioTrackCount * (optH + 2);
			if (subTop < controlsRect.top - 120)
				subTop = m_galleryAudioTrackButtonRect.top - pad - audioTrackCount * (optH + 2);
			RECT subPanel = {subLeft, subTop, subLeft + subW,
					 subTop + pad * 2 + audioTrackCount * (optH + 2)};
			m_galleryAudioTrackMenuRect = subPanel;
			DrawRoundedRectangle(subPanel, 8.0f, AccentPanel());
			DrawRoundedRectangleBorder(subPanel, 8.0f, 1.0f,
						   D2D1::ColorF(58.0f / 255.0f, 58.0f / 255.0f, 69.0f / 255.0f, 1.0f));
			for (int i = 0; i < audioTrackCount; i++) {
				int rowTop = subTop + pad + i * (optH + 2);
				RECT optRect = {subLeft + pad, rowTop, subLeft + subW - pad, rowTop + optH};
				m_galleryAudioTrackOptionRects.push_back(optRect);
				float radius = optH / 2.0f;
				bool isCurrent = (i == m_videoPlayer.GetCurrentAudioTrackIndex());
				DrawRoundedRectangle(optRect, radius,
						     isCurrent ? AccentSelected() : AccentHover(0.40f));
				OverlayVideoPlayer::AudioTrackInfo info;
				m_videoPlayer.GetAudioTrackInfo(i, info);
				std::wstring rowLabel = std::to_wstring(i + 1) + L": " + WideFromUtf8(info.name);
				RECT textRect = {optRect.left + 10, optRect.top, optRect.right - 6, optRect.bottom};
				RenderText(rowLabel, textRect, 9.0f, DWRITE_FONT_WEIGHT_NORMAL,
					   D2D1::ColorF(0.95f, 0.95f, 0.95f), DWRITE_TEXT_ALIGNMENT_LEADING);
			}
		}
	}

	int btnTop = centerY - btnWidth / 2;
	int btnBottom = centerY + btnWidth / 2;
	if (btnTop < contentTop)
		btnTop = contentTop;
	if (btnBottom > contentBottom)
		btnBottom = contentBottom;

	// Center controls
	int centerX = (controlsRect.left + controlsRect.right) / 2;
	int centerGroupW = btnWidth * 3 + btnSpacing * 2;
	int centerLeft = centerX - centerGroupW / 2;
	m_galleryPlayRect = {centerLeft, btnTop, centerLeft + btnWidth, btnBottom};
	m_gallerySetInRect = {m_galleryPlayRect.right + btnSpacing, btnTop,
			      m_galleryPlayRect.right + btnSpacing + btnWidth, btnBottom};
	m_gallerySetOutRect = {m_gallerySetInRect.right + btnSpacing, btnTop,
			       m_gallerySetInRect.right + btnSpacing + btnWidth, btnBottom};

	// Right controls
	int rightGroupW = btnWidth * 2 + btnSpacing;
	int rightStart = controlsRect.right - sideInset - rightGroupW;
	m_galleryTrimRect = {rightStart, btnTop, rightStart + btnWidth, btnBottom};
	m_galleryDeleteRect = {m_galleryTrimRect.right + btnSpacing, btnTop, controlsRect.right - sideInset, btnBottom};

	const D2D1_COLOR_F hoverColor = AccentHover();
	const D2D1_COLOR_F normalIcon = D2D1::ColorF(0.85f, 0.85f, 0.9f);

	const bool hoverMute = (m_hoverTarget == HoverTarget::GalleryMute);
	const D2D1_COLOR_F muteRed = D2D1::ColorF(0.95f, 0.25f, 0.25f);
	const D2D1_COLOR_F muteRedHover = D2D1::ColorF(1.0f, 0.4f, 0.4f);
	const D2D1_COLOR_F muteTint = m_galleryMuted ? (hoverMute ? muteRedHover : muteRed)
						     : (hoverMute ? hoverColor : normalIcon);
	DrawGalleryIcon(m_galleryMuteRect, m_galleryMuted ? 12 : 6, &muteTint);
	DrawRoundedRectangle(m_galleryVolumeRect, 4.0f,
			     D2D1::ColorF(40.0f / 255.0f, 40.0f / 255.0f, 50.0f / 255.0f, 1.0f));
	int volWidth = m_galleryVolumeRect.right - m_galleryVolumeRect.left - 4;
	int volFill = static_cast<int>(volWidth * (m_galleryVolume / 100.0));
	RECT volRect = {m_galleryVolumeRect.left + 2, m_galleryVolumeRect.top + 2,
			m_galleryVolumeRect.left + 2 + volFill, m_galleryVolumeRect.bottom - 2};
	DrawRoundedRectangle(volRect, 3.0f, AccentMuted());

	const bool hoverPlay = (m_hoverTarget == HoverTarget::GalleryPlay);
	const bool hoverIn = (m_hoverTarget == HoverTarget::GallerySetIn);
	const bool hoverOut = (m_hoverTarget == HoverTarget::GallerySetOut);
	const bool hoverTrim = (m_hoverTarget == HoverTarget::GalleryTrim);
	const bool hoverDel = (m_hoverTarget == HoverTarget::GalleryDelete);

	const bool playing = m_videoPlayer.IsPlaying();
	const D2D1_COLOR_F playTint = (hoverPlay || playing) ? hoverColor : normalIcon;
	const D2D1_COLOR_F inTint = hoverIn ? hoverColor : normalIcon;
	const D2D1_COLOR_F outTint = hoverOut ? hoverColor : normalIcon;
	const D2D1_COLOR_F trimTint = hoverTrim ? hoverColor : normalIcon;
	const D2D1_COLOR_F delTint = hoverDel ? hoverColor : normalIcon;

	DrawGalleryIcon(m_galleryPlayRect, playing ? 1 : 0, &playTint);
	DrawGalleryIcon(m_gallerySetInRect, 2, &inTint);
	DrawGalleryIcon(m_gallerySetOutRect, 3, &outTint);
	DrawGalleryIcon(m_galleryTrimRect, 4, &trimTint);
	DrawGalleryIcon(m_galleryDeleteRect, 5, &delTint);

	if (m_galleryExportSubmenuOpen) {
		const int pad = 12;
		const int rowH = 22;
		const int rowGap = 4;
		const int row3H = 30;
		const int trackRowH = 18;
		const int presetBtnH = 22;
		const int presetBtnGap = 4;
		const int sectionGap = 6;
		const size_t trackCount = m_exportTrackStreamIndices.size();
		const int tracksHeight =
			trackCount > 0 ? (static_cast<int>(trackCount) * trackRowH + sectionGap) : 0;
		const bool isRe = m_rebuildDraft.mode == RebuildMode::Reencode;
		const bool isGif = isRe && m_rebuildDraft.format == RebuildOutputFormat::Gif;
		const int reRows = isRe ? (4 * (rowH + rowGap) + (isGif ? 18 : 0)) : 0;
		const int presetBtnsBlockH = presetBtnH + sectionGap;
		const int queueModeBlockH = rowH + sectionGap;
		const int boxH = pad + queueModeBlockH + (rowH + rowGap) + presetBtnsBlockH + (rowH + sectionGap) +
				 reRows + tracksHeight + row3H + pad;
		const int boxW = 320;
		const int gap = 8;
		const int panelLeft = (controlsRect.left + controlsRect.right) / 2 - boxW / 2;
		const int panelTop = controlsRect.top - gap - boxH;
		RECT exportBlockRect = {panelLeft, panelTop, panelLeft + boxW, panelTop + boxH};
		m_galleryExportPanelRect = exportBlockRect;
		DrawRoundedRectangle(exportBlockRect, 10.0f, AccentPanel());
		DrawRoundedRectangleBorder(exportBlockRect, 10.0f, 1.0f,
					   D2D1::ColorF(58.0f / 255.0f, 58.0f / 255.0f, 69.0f / 255.0f, 1.0f));

		int contentTop = exportBlockRect.top + pad;
		auto cycleRow = [&](RECT &prev, RECT &next, const std::wstring &label, const std::wstring &value) {
			const int btnW = 22;
			prev = {exportBlockRect.left + pad, contentTop, exportBlockRect.left + pad + btnW,
				contentTop + rowH};
			next = {exportBlockRect.right - pad - btnW, contentTop, exportBlockRect.right - pad,
				contentTop + rowH};
			RenderText(L"<", prev, 11.0f, DWRITE_FONT_WEIGHT_NORMAL, D2D1::ColorF(0.9f, 0.9f, 0.9f));
			RenderText(label + L": " + value,
				   RECT{prev.right + 4, contentTop, next.left - 4, contentTop + rowH}, 9.0f,
				   DWRITE_FONT_WEIGHT_NORMAL, D2D1::ColorF(0.9f, 0.9f, 0.9f),
				   DWRITE_TEXT_ALIGNMENT_LEADING);
			RenderText(L">", next, 11.0f, DWRITE_FONT_WEIGHT_NORMAL, D2D1::ColorF(0.9f, 0.9f, 0.9f));
			contentTop += rowH + rowGap;
		};

		// 0) Queue output mode — not part of rebuild preset
		{
			const bool merge = m_exportQueue.GetMode() == ExportQueueMode::Merge;
			const int modeW = (boxW - pad * 2 - 4) / 2;
			m_galleryQueueModeSeparateRect = {exportBlockRect.left + pad, contentTop,
							  exportBlockRect.left + pad + modeW, contentTop + rowH};
			m_galleryQueueModeMergeRect = {m_galleryQueueModeSeparateRect.right + 4, contentTop,
						       exportBlockRect.right - pad, contentTop + rowH};
			DrawRoundedRectangle(m_galleryQueueModeSeparateRect, 4.0f,
					     !merge ? AccentSelected()
						    : D2D1::ColorF(0.2f, 0.2f, 0.25f, 1.0f));
			DrawRoundedRectangle(m_galleryQueueModeMergeRect, 4.0f,
					     merge ? AccentSelected() : D2D1::ColorF(0.2f, 0.2f, 0.25f, 1.0f));
			RenderText(overlay::util::ModuleTextW("Export.Separate"), m_galleryQueueModeSeparateRect, 9.5f,
				   DWRITE_FONT_WEIGHT_NORMAL, D2D1::ColorF(1, 1, 1));
			RenderText(overlay::util::ModuleTextW("Export.Merge"), m_galleryQueueModeMergeRect, 9.5f,
				   DWRITE_FONT_WEIGHT_NORMAL, D2D1::ColorF(1, 1, 1));
			contentTop += queueModeBlockH;
		}

		// 1) Preset
		cycleRow(m_galleryPresetPrevRect, m_galleryPresetNextRect, overlay::util::ModuleTextW("Rebuild.Preset"),
			 m_rebuildDraft.name.empty() ? L"—" : m_rebuildDraft.name);
		const int presetBtnW = (boxW - pad * 2 - presetBtnGap * 2) / 3;
		const D2D1_COLOR_F presetBtnBg = D2D1::ColorF(40.0f / 255.0f, 40.0f / 255.0f, 50.0f / 255.0f, 1.0f);
		m_galleryPresetSaveRect = {exportBlockRect.left + pad, contentTop,
					   exportBlockRect.left + pad + presetBtnW, contentTop + presetBtnH};
		m_galleryPresetSaveAsRect = {m_galleryPresetSaveRect.right + presetBtnGap, contentTop,
					     m_galleryPresetSaveRect.right + presetBtnGap + presetBtnW,
					     contentTop + presetBtnH};
		m_galleryPresetDeleteRect = {m_galleryPresetSaveAsRect.right + presetBtnGap, contentTop,
					     exportBlockRect.right - pad, contentTop + presetBtnH};
		DrawRoundedRectangle(m_galleryPresetSaveRect, 6.0f, presetBtnBg);
		DrawRoundedRectangle(m_galleryPresetSaveAsRect, 6.0f, presetBtnBg);
		DrawRoundedRectangle(m_galleryPresetDeleteRect, 6.0f, D2D1::ColorF(0.32f, 0.2f, 0.22f, 1.0f));
		RenderText(overlay::util::ModuleTextW("Rebuild.SavePreset"), m_galleryPresetSaveRect, 8.5f,
			   DWRITE_FONT_WEIGHT_NORMAL, D2D1::ColorF(1, 1, 1));
		RenderText(overlay::util::ModuleTextW("Rebuild.SaveAsPreset"), m_galleryPresetSaveAsRect, 8.5f,
			   DWRITE_FONT_WEIGHT_NORMAL, D2D1::ColorF(1, 1, 1));
		RenderText(overlay::util::ModuleTextW("Rebuild.DeletePreset"), m_galleryPresetDeleteRect, 8.5f,
			   DWRITE_FONT_WEIGHT_NORMAL, D2D1::ColorF(1, 1, 1));
		contentTop += presetBtnsBlockH;

		// 2) Type Copy / Reencode
		int modeW = (boxW - pad * 2 - 4) / 2;
		m_galleryRebuildModeCopyRect = {exportBlockRect.left + pad, contentTop,
						exportBlockRect.left + pad + modeW, contentTop + rowH};
		m_galleryRebuildModeReencodeRect = {m_galleryRebuildModeCopyRect.right + 4, contentTop,
						    exportBlockRect.right - pad, contentTop + rowH};
		auto drawMode = [&](const RECT &r, RebuildMode mode, const wchar_t *label) {
			const bool on = m_rebuildDraft.mode == mode;
			DrawRoundedRectangle(r, 4.0f, on ? AccentSelected() : D2D1::ColorF(0.2f, 0.2f, 0.25f, 1.0f));
			RenderText(label, r, 9.5f, DWRITE_FONT_WEIGHT_NORMAL, D2D1::ColorF(1, 1, 1));
		};
		drawMode(m_galleryRebuildModeCopyRect, RebuildMode::Copy, L"Copy");
		drawMode(m_galleryRebuildModeReencodeRect, RebuildMode::Reencode, L"Reencode");
		contentTop += rowH + sectionGap;

		m_galleryFormatPrevRect = m_galleryFormatNextRect = RECT{0, 0, 0, 0};
		m_galleryResPrevRect = m_galleryResNextRect = RECT{0, 0, 0, 0};
		m_galleryBitratePrevRect = m_galleryBitrateNextRect = RECT{0, 0, 0, 0};
		m_galleryFpsPrevRect = m_galleryFpsNextRect = RECT{0, 0, 0, 0};
		m_galleryQualityPrevRect = m_galleryQualityNextRect = RECT{0, 0, 0, 0};

		if (isRe) {
			cycleRow(m_galleryFormatPrevRect, m_galleryFormatNextRect,
				 overlay::util::ModuleTextW("Rebuild.Format"), m_rebuildDraft.FormatLabel());
			cycleRow(m_galleryResPrevRect, m_galleryResNextRect,
				 overlay::util::ModuleTextW("Rebuild.Resolution"), m_rebuildDraft.ResolutionLabel());
			cycleRow(m_galleryBitratePrevRect, m_galleryBitrateNextRect,
				 overlay::util::ModuleTextW("Rebuild.Quality"), m_rebuildDraft.QualityLabel());
			cycleRow(m_galleryFpsPrevRect, m_galleryFpsNextRect, overlay::util::ModuleTextW("Rebuild.Fps"),
				 m_rebuildDraft.FpsLabel());
			if (isGif) {
				wchar_t gifExtra[64];
				swprintf(gifExtra, 64, L"%d colors · dither %d", m_rebuildDraft.gifColors,
					 m_rebuildDraft.gifDither);
				RenderText(gifExtra,
					   RECT{exportBlockRect.left + pad, contentTop, exportBlockRect.right - pad,
						contentTop + 16},
					   8.0f, DWRITE_FONT_WEIGHT_NORMAL, D2D1::ColorF(0.75f, 0.75f, 0.8f),
					   DWRITE_TEXT_ALIGNMENT_LEADING);
				contentTop += 18;
			}
		}

		m_exportTrackKeepRects.clear();
		if (trackCount > 0) {
			contentTop += sectionGap;
			for (size_t i = 0; i < trackCount; i++) {
				const int rowTop = contentTop + static_cast<int>(i) * trackRowH;
				const int cbSize = 14;
				RECT keepRect = {exportBlockRect.left + pad, rowTop,
						 exportBlockRect.left + pad + cbSize, rowTop + trackRowH};
				m_exportTrackKeepRects.push_back(keepRect);
				DrawRoundedRectangle(keepRect, 3.0f, D2D1::ColorF(0.2f, 0.2f, 0.25f, 1.0f));
				if (i < m_exportTrackKeep.size() && m_exportTrackKeep[i]) {
					RECT checkR = {keepRect.left + 2, keepRect.top + 3, keepRect.right - 2,
						       keepRect.bottom - 3};
					DrawRoundedRectangle(checkR, 2.0f, AccentMuted());
				}
				RECT labelRect = {exportBlockRect.left + pad + cbSize + 6, rowTop,
						  exportBlockRect.right - pad, rowTop + trackRowH};
				RenderText(i < m_exportTrackLabels.size() ? m_exportTrackLabels[i] : L"", labelRect,
					   9.0f, DWRITE_FONT_WEIGHT_NORMAL, D2D1::ColorF(0.9f, 0.9f, 0.9f),
					   DWRITE_TEXT_ALIGNMENT_LEADING);
			}
			contentTop += static_cast<int>(trackCount) * trackRowH;
		}

		m_galleryExportConfirmRect = {exportBlockRect.left + pad, exportBlockRect.bottom - pad - row3H,
					      exportBlockRect.right - pad, exportBlockRect.bottom - pad};
		DrawRoundedRectangle(m_galleryExportConfirmRect, 6.0f, AccentSelected());
		RenderText(m_exportSettingsPendingStart ? overlay::util::ModuleTextW("Export.Start")
							  : overlay::util::ModuleTextW("Export.AddToQueue"),
			   m_galleryExportConfirmRect, 11.0f, DWRITE_FONT_WEIGHT_NORMAL,
			   D2D1::ColorF(1.0f, 1.0f, 1.0f));
		m_galleryCompressRect = RECT{0, 0, 0, 0};
	} else {
		m_galleryCompressRect = RECT{0, 0, 0, 0};
		m_galleryExportPanelRect = RECT{0, 0, 0, 0};
		m_galleryQualityPrevRect = RECT{0, 0, 0, 0};
		m_galleryQualityNextRect = RECT{0, 0, 0, 0};
		m_galleryExportConfirmRect = RECT{0, 0, 0, 0};
		m_galleryRebuildModeCopyRect = RECT{0, 0, 0, 0};
		m_galleryRebuildModeReencodeRect = RECT{0, 0, 0, 0};
		m_galleryQueueModeSeparateRect = RECT{0, 0, 0, 0};
		m_galleryQueueModeMergeRect = RECT{0, 0, 0, 0};
		m_galleryPresetPrevRect = RECT{0, 0, 0, 0};
		m_galleryPresetNextRect = RECT{0, 0, 0, 0};
		m_galleryPresetSaveRect = RECT{0, 0, 0, 0};
		m_galleryPresetSaveAsRect = RECT{0, 0, 0, 0};
		m_galleryPresetDeleteRect = RECT{0, 0, 0, 0};
		m_galleryFormatPrevRect = m_galleryFormatNextRect = RECT{0, 0, 0, 0};
		m_galleryResPrevRect = m_galleryResNextRect = RECT{0, 0, 0, 0};
		m_galleryBitratePrevRect = m_galleryBitrateNextRect = RECT{0, 0, 0, 0};
		m_galleryFpsPrevRect = m_galleryFpsNextRect = RECT{0, 0, 0, 0};
	}

	std::wstring status = L"";
	if (m_exportInfraReady && m_exportQueue.IsBusy()) {
		status = overlay::util::ModuleTextW("Processing");
	}
	if (status.empty() && m_videoPlayer.IsDurationScanActive()) {
		status = overlay::util::ModuleTextW("ScanningDuration");
	}

	if (!status.empty()) {
		RECT statusRect = {controlsRect.left, controlsRect.top - 20, controlsRect.right, controlsRect.top};
		RenderText(status, statusRect, 10.0f, DWRITE_FONT_WEIGHT_NORMAL, D2D1::ColorF(0.7f, 0.7f, 0.7f));
	}

	double displayDuration = m_videoPlayer.GetDuration();
	if (displayDuration <= 0.0) {
		displayDuration = std::max(m_galleryTrimOut, m_videoPlayer.GetCurrentTime());
	}
	std::wstring timeText = FormatTime(m_videoPlayer.GetCurrentTime()) + L" / " + FormatTime(displayDuration);
	UNUSED_PARAMETER(timeText);
}

static void getHotkeyVkMod(obs_data_t *data, const char *name, int defaultVk, int defaultMod, int &outVk, int &outMod)
{
#ifdef ENABLE_FRONTEND_API
	if (data && obs_data_has_user_value(data, (std::string(name) + "_vk").c_str())) {
		outVk = static_cast<int>(obs_data_get_int(data, (std::string(name) + "_vk").c_str()));
		outMod = static_cast<int>(obs_data_get_int(data, (std::string(name) + "_mod").c_str()));
		return;
	}
#endif
	outVk = defaultVk;
	outMod = defaultMod;
}

void OverlayRenderer::HandleGalleryKey(int vkCode, int mods)
{
	if (!m_galleryOpen) {
		return;
	}
	if (vkCode == VK_ESCAPE && m_galleryContextMenuOpen) {
		CloseGalleryContextMenu();
		Render();
		return;
	}
	if (m_gallerySelectedIndex < 0) {
		return;
	}
	int vk, m;
	getHotkeyVkMod(saved_settings_data, "hotkey_play", 32, 0, vk, m); // VK_SPACE
	if (vkCode == vk && mods == m) {
		m_videoPlayer.TogglePlaying();
		Render();
		return;
	}
	getHotkeyVkMod(saved_settings_data, "hotkey_seek_forward_5", 39, 0, vk, m); // VK_RIGHT
	if (vkCode == vk && mods == m) {
		double t = m_videoPlayer.GetCurrentTime();
		m_videoPlayer.Seek(t + 5.0);
		Render();
		return;
	}
	getHotkeyVkMod(saved_settings_data, "hotkey_seek_back_5", 37, 0, vk, m); // VK_LEFT
	if (vkCode == vk && mods == m) {
		double t = m_videoPlayer.GetCurrentTime();
		m_videoPlayer.Seek((t > 5.0) ? (t - 5.0) : 0.0);
		Render();
		return;
	}
	getHotkeyVkMod(saved_settings_data, "hotkey_frame_forward", 190, 0, vk, m); // VK_OEM_PERIOD
	if (vkCode == vk && mods == m) {
		m_videoPlayer.SeekOneFrameForward();
		Render();
		return;
	}
	getHotkeyVkMod(saved_settings_data, "hotkey_frame_back", 188, 0, vk, m); // VK_OEM_COMMA
	if (vkCode == vk && mods == m) {
		m_videoPlayer.SeekOneFrameBackward();
		Render();
		return;
	}
	getHotkeyVkMod(saved_settings_data, "hotkey_go_in", 36, 0, vk, m); // VK_HOME
	if (vkCode == vk && mods == m) {
		m_videoPlayer.Seek(m_galleryTrimIn);
		Render();
		return;
	}
	getHotkeyVkMod(saved_settings_data, "hotkey_go_out", 35, 0, vk, m); // VK_END
	if (vkCode == vk && mods == m) {
		m_videoPlayer.Seek(m_galleryTrimOut);
		Render();
		return;
	}
}

namespace {

const char *const kSettingsHotkeyKeys[12] = {
	"hotkey_play",         "hotkey_seek_forward_5", "hotkey_seek_back_5", "hotkey_frame_forward",
	"hotkey_frame_back",   "hotkey_go_in",          "hotkey_go_out",      "hotkey_toast_view",
	"hotkey_toast_delete", "hotkey_clip_0",         "hotkey_clip_1",      "hotkey_clip_2"};
const int kSettingsHotkeyDefaultVk[12] = {32, 39, 37, 190, 188, 36, 35, VK_F7, VK_F8, 0, 0, 0};
const int kSettingsHotkeyDefaultMod[12] = {0, 0, 0, 0, 0, 0, 0, 2, 2, 0, 0, 0};
const char *const kSettingsHotkeyLabelKeys[12] = {
	"Hotkeys.PlayPause", "Hotkeys.SeekForward5", "Hotkeys.SeekBack5", "Hotkeys.FrameForward",
	"Hotkeys.FrameBack", "Hotkeys.GoIn",         "Hotkeys.GoOut",     "Hotkeys.ToastView",
	"Hotkeys.ToastDelete", "ClipSave.Hotkey1", "ClipSave.Hotkey2", "ClipSave.Hotkey3"};
static const int kGalleryHotkeyCount = 7;
static const int kToastViewHotkeyIndex = 7;
static const int kToastDeleteHotkeyIndex = 8;
static const int kClipHotkeyBaseIndex = 9;
static const int kClipHotkeyCount = 3;

const int kDefaultClipSeconds[3] = {15, 30, 60};

std::wstring FormatClipDurationLabel(int seconds)
{
	if (seconds <= 0)
		return L"0s";
	if (seconds < 60)
		return std::to_wstring(seconds) + L"s";
	if (seconds % 3600 == 0)
		return std::to_wstring(seconds / 3600) + L"h";
	if (seconds % 60 == 0)
		return std::to_wstring(seconds / 60) + L"m";
	const int m = seconds / 60;
	const int s = seconds % 60;
	return std::to_wstring(m) + L"m " + std::to_wstring(s) + L"s";
}

// True when the foreground app is typing into a text field. Used so bare clip
// hotkeys (e.g. 8/9/0) do not fire while the user is entering text.
bool ForegroundHasTextCaret()
{
	HWND fg = GetForegroundWindow();
	if (!fg)
		return false;
	DWORD tid = GetWindowThreadProcessId(fg, nullptr);
	if (!tid)
		return false;
	GUITHREADINFO gi = {};
	gi.cbSize = sizeof(gi);
	if (!GetGUIThreadInfo(tid, &gi))
		return false;
	return gi.hwndCaret != nullptr;
}

int CurrentModifierMask()
{
	int mods = 0;
	if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
		mods |= 1;
	if (GetAsyncKeyState(VK_CONTROL) & 0x8000)
		mods |= 2;
	if (GetAsyncKeyState(VK_MENU) & 0x8000)
		mods |= 4;
	return mods;
}

bool ClipHotkeyComboDown(int vk, int mod)
{
	if (vk <= 0)
		return false;
	if (!(GetAsyncKeyState(vk) & 0x8000))
		return false;
	return CurrentModifierMask() == mod;
}

const char *const kSettingsPositionKeys[9] = {
	"Position.Top",        "Position.Bottom",     "Position.Left",
	"Position.Right",      "Position.TopLeft",    "Position.TopRight",
	"Position.BottomLeft", "Position.BottomRight", "Position.Center"};
const char *const kSettingsMonitorModeKeys[3] = {
	"Monitor.Primary", "Monitor.Specific", "Monitor.Cursor"};
const char *const kSettingsOrientationKeys[2] = {"Orientation.Horizontal", "Orientation.Vertical"};
const char *const kSettingsSmartReplayModeKeys[2] = {"SmartReplay.Mode.Legacy", "SmartReplay.Mode.TimestampTrim"};
const char *const kSettingsToastHotkeysScopeKeys[2] = {"Hotkeys.ToastScope.WhileToast",
						       "Hotkeys.ToastScope.Always"};

const char *const kIndicatorSlotKeys[4] = {"recording", "replay", "saving", "saved"};
const char *const kIndicatorSlotLabelKeys[4] = {"Indicators.Recording", "Indicators.Replay", "Indicators.Saving",
						"Indicators.Saved"};
const char *const kIndicatorSoundKeys[5] = {"recording_on", "recording_off", "replay_on", "replay_off",
					    "replay_saved"};
const char *const kIndicatorSoundLabelKeys[5] = {
	"Indicators.Sound.RecordingOn", "Indicators.Sound.RecordingOff", "Indicators.Sound.ReplayOn",
	"Indicators.Sound.ReplayOff", "Indicators.Sound.ReplaySaved"};

bool BrowseForExportFolderNative(HWND owner, std::wstring &out)
{
	bool didInit = false;
	HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	if (SUCCEEDED(hrInit))
		didInit = true;

	bool result = false;
	IFileOpenDialog *dlg = nullptr;
	HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg));
	if (SUCCEEDED(hr) && dlg) {
		DWORD opts = 0;
		dlg->GetOptions(&opts);
		dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
		if (SUCCEEDED(dlg->Show(owner))) {
			IShellItem *item = nullptr;
			if (SUCCEEDED(dlg->GetResult(&item)) && item) {
				PWSTR path = nullptr;
				if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
					out = path;
					result = true;
					CoTaskMemFree(path);
				}
				item->Release();
			}
		}
		dlg->Release();
	}

	if (didInit)
		CoUninitialize();
	return result;
}

bool BrowseForIndicatorIconNative(HWND owner, std::wstring &out)
{
	bool didInit = false;
	HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	if (SUCCEEDED(hrInit))
		didInit = true;

	bool result = false;
	IFileOpenDialog *dlg = nullptr;
	HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg));
	if (SUCCEEDED(hr) && dlg) {
		COMDLG_FILTERSPEC filters[] = {
			{L"Images (PNG, SVG, JPEG, GIF)", L"*.png;*.svg;*.jpeg;*.jpg;*.gif"},
			{L"PNG", L"*.png"},
			{L"SVG", L"*.svg"},
			{L"JPEG", L"*.jpeg;*.jpg"},
			{L"GIF", L"*.gif"},
			{L"All files", L"*.*"},
		};
		dlg->SetFileTypes(ARRAYSIZE(filters), filters);
		dlg->SetFileTypeIndex(1);
		dlg->SetTitle(overlay::util::ModuleTextW("Indicators.SelectIcon").c_str());
		DWORD opts = 0;
		dlg->GetOptions(&opts);
		dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST);
		if (SUCCEEDED(dlg->Show(owner))) {
			IShellItem *item = nullptr;
			if (SUCCEEDED(dlg->GetResult(&item)) && item) {
				PWSTR path = nullptr;
				if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
					out = path;
					result = true;
					CoTaskMemFree(path);
				}
				item->Release();
			}
		}
		dlg->Release();
	}

	if (didInit)
		CoUninitialize();
	return result;
}

bool PickIndicatorColorNative(HWND owner, int &rgb)
{
	static COLORREF customColors[16] = {};
	CHOOSECOLOR cc = {};
	cc.lStructSize = sizeof(cc);
	cc.hwndOwner = owner;
	cc.lpCustColors = customColors;
	cc.rgbResult = RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
	cc.Flags = CC_FULLOPEN | CC_RGBINIT;
	if (!ChooseColor(&cc))
		return false;
	rgb = ((GetRValue(cc.rgbResult) & 0xFF) << 16) | ((GetGValue(cc.rgbResult) & 0xFF) << 8) |
	      (GetBValue(cc.rgbResult) & 0xFF);
	return true;
}

std::wstring IndicatorIconFileNameOnly(const std::wstring &path)
{
	if (path.empty())
		return path;
	size_t pos = path.find_last_of(L"\\/");
	if (pos == std::wstring::npos)
		return path;
	return path.substr(pos + 1);
}

std::wstring IndicatorIconExtension(const std::wstring &path)
{
	size_t dot = path.find_last_of(L'.');
	size_t slash = path.find_last_of(L"\\/");
	if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash))
		return L".png";
	std::wstring ext = path.substr(dot);
	for (wchar_t &c : ext)
		c = static_cast<wchar_t>(towlower(c));
	if (ext == L".png" || ext == L".svg" || ext == L".jpeg" || ext == L".jpg" || ext == L".gif")
		return ext;
	return L".png";
}

bool EnsureIndicatorIconsConfigDir(std::wstring &outDirWide)
{
	char *dirUtf8 = obs_module_config_path("indicators");
	if (!dirUtf8)
		return false;
	const int mk = os_mkdirs(dirUtf8);
	outDirWide = overlay::util::Utf8ToWide(dirUtf8);
	bfree(dirUtf8);
	if (mk == MKDIR_ERROR)
		return false;
	if (!outDirWide.empty() && outDirWide.back() != L'\\' && outDirWide.back() != L'/')
		outDirWide += L'\\';
	return true;
}

void DeleteStoredIndicatorIcons(int slot)
{
	if (slot < 0 || slot >= 4)
		return;
	std::wstring dir;
	if (!EnsureIndicatorIconsConfigDir(dir))
		return;
	static const wchar_t *const exts[] = {L".png", L".svg", L".jpeg", L".jpg", L".gif"};
	const std::wstring base = overlay::util::Utf8ToWide(kIndicatorSlotKeys[slot]);
	for (const wchar_t *ext : exts) {
		const std::wstring path = dir + base + ext;
		DeleteFileW(path.c_str());
	}
}

bool CopyIndicatorIconIntoConfig(int slot, const std::wstring &sourcePath, std::wstring &outStoredPath)
{
	if (slot < 0 || slot >= 4 || sourcePath.empty())
		return false;

	std::wstring dir;
	if (!EnsureIndicatorIconsConfigDir(dir))
		return false;

	DeleteStoredIndicatorIcons(slot);

	const std::wstring ext = IndicatorIconExtension(sourcePath);
	const std::wstring dest = dir + overlay::util::Utf8ToWide(kIndicatorSlotKeys[slot]) + ext;
	if (!CopyFileW(sourcePath.c_str(), dest.c_str(), FALSE)) {
		obs_log(LOG_WARNING, "Failed to copy indicator icon for slot %d (err=%lu)", slot, GetLastError());
		return false;
	}
	outStoredPath = dest;
	return true;
}

bool EnsureIndicatorSoundsConfigDir(std::wstring &outDirWide)
{
	char *dirUtf8 = obs_module_config_path("indicator-sounds");
	if (!dirUtf8)
		return false;
	const int mk = os_mkdirs(dirUtf8);
	outDirWide = overlay::util::Utf8ToWide(dirUtf8);
	bfree(dirUtf8);
	if (mk == MKDIR_ERROR)
		return false;
	if (!outDirWide.empty() && outDirWide.back() != L'\\' && outDirWide.back() != L'/')
		outDirWide += L'\\';
	return true;
}

void DeleteStoredIndicatorSound(int eventIndex)
{
	if (eventIndex < 0 || eventIndex >= 5)
		return;
	std::wstring dir;
	if (!EnsureIndicatorSoundsConfigDir(dir))
		return;
	const std::wstring path = dir + overlay::util::Utf8ToWide(kIndicatorSoundKeys[eventIndex]) + L".wav";
	DeleteFileW(path.c_str());
}

bool CopyIndicatorSoundIntoConfig(int eventIndex, const std::wstring &sourcePath, std::wstring &outStoredPath)
{
	if (eventIndex < 0 || eventIndex >= 5 || sourcePath.empty())
		return false;
	std::wstring dir;
	if (!EnsureIndicatorSoundsConfigDir(dir))
		return false;
	DeleteStoredIndicatorSound(eventIndex);
	const std::wstring dest = dir + overlay::util::Utf8ToWide(kIndicatorSoundKeys[eventIndex]) + L".wav";
	if (!CopyFileW(sourcePath.c_str(), dest.c_str(), FALSE)) {
		obs_log(LOG_WARNING, "Failed to copy indicator sound for event %d (err=%lu)", eventIndex,
			GetLastError());
		return false;
	}
	outStoredPath = dest;
	return true;
}

bool BrowseForIndicatorSoundNative(HWND owner, std::wstring &out)
{
	bool didInit = false;
	HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	if (SUCCEEDED(hrInit))
		didInit = true;

	bool result = false;
	IFileOpenDialog *dlg = nullptr;
	HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg));
	if (SUCCEEDED(hr) && dlg) {
		COMDLG_FILTERSPEC filters[] = {
			{L"WAV audio", L"*.wav"},
			{L"All files", L"*.*"},
		};
		dlg->SetFileTypes(ARRAYSIZE(filters), filters);
		dlg->SetFileTypeIndex(1);
		dlg->SetTitle(overlay::util::ModuleTextW("Indicators.SelectSound").c_str());
		DWORD opts = 0;
		dlg->GetOptions(&opts);
		dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST);
		if (SUCCEEDED(dlg->Show(owner))) {
			IShellItem *item = nullptr;
			if (SUCCEEDED(dlg->GetResult(&item)) && item) {
				PWSTR path = nullptr;
				if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
					out = path;
					result = true;
					CoTaskMemFree(path);
				}
				item->Release();
			}
		}
		dlg->Release();
	}

	if (didInit)
		CoUninitialize();
	return result;
}

} // namespace

std::wstring OverlayRenderer::SettingsHotkeyName(int vk, int mod) const
{
	std::wstring s;
	if (mod & 2)
		s += L"Ctrl+";
	if (mod & 4)
		s += L"Alt+";
	if (mod & 1)
		s += L"Shift+";

	static const struct {
		int vk;
		const wchar_t *name;
	} names[] = {{32, L"Space"}, {37, L"Left"}, {39, L"Right"}, {38, L"Up"},  {40, L"Down"},
		     {36, L"Home"},  {35, L"End"},  {188, L","},    {190, L"."}, {112, L"F1"},
		     {113, L"F2"},   {114, L"F3"},  {115, L"F4"},   {116, L"F5"}, {117, L"F6"},
		     {118, L"F7"},   {119, L"F8"},  {120, L"F9"},   {121, L"F10"}, {0, nullptr}};
	for (int i = 0; names[i].name != nullptr; i++) {
		if (names[i].vk == vk)
			return s + names[i].name;
	}
	if (vk >= 0x41 && vk <= 0x5A)
		return s + static_cast<wchar_t>(vk);
	if (vk >= 0x30 && vk <= 0x39)
		return s + static_cast<wchar_t>(vk);
	return s + std::to_wstring(vk);
}

void OverlayRenderer::LoadSettingsWorkingValues()
{
	obs_data_t *d = saved_settings_data;
	auto getIntDef = [&](const char *k, int def) {
		return (d && obs_data_has_user_value(d, k)) ? static_cast<int>(obs_data_get_int(d, k)) : def;
	};
	auto getBoolDef = [&](const char *k, bool def) {
		return (d && obs_data_has_user_value(d, k)) ? obs_data_get_bool(d, k) : def;
	};

	m_setPosition = ClampInt(getIntDef("position", 8), 0, 8);
	m_setMargin = ClampInt(getIntDef("margin", 20), 0, 200);
	int orient = getIntDef("orientation", 0);
	m_setOrientation = (orient == 1) ? 1 : 0;
	m_setMonitorMode = ClampInt(getIntDef("overlay_monitor_mode", 0), 0, 2);
	{
		const int monitorCount = std::max(1, m_layoutManager.GetMonitorCount());
		m_setMonitorIndex = ClampInt(getIntDef("overlay_monitor_index", 0), 0, monitorCount - 1);
	}
	double alpha = (d && obs_data_has_user_value(d, "overlay_background_alpha"))
			       ? obs_data_get_double(d, "overlay_background_alpha")
			       : 0.88;
	m_setOpacityPct = ClampInt(static_cast<int>(alpha * 100.0 + 0.5), 50, 100);
	m_setOverlayScale = ClampInt(getIntDef("overlay_scale", 100), 50, 300);
	m_setTextScale = ClampInt(getIntDef("overlay_text_scale", 100), 50, 300);
	m_setAccentColor = getIntDef("overlay_accent_color", 0xA882FF) & 0xFFFFFF;
	m_setAutoHideEnabled = getBoolDef("auto_hide_enabled", false);
	m_setAutoHideSeconds = ClampInt(getIntDef("auto_hide_seconds", 5), 1, 3600);
	m_setIndicatorsEnabled = getBoolDef("indicators_enabled", true);
	m_setIndicatorsPosition = ClampInt(getIntDef("indicators_position", 5), 0, 8);
	m_setIndicatorsOled = getBoolDef("indicators_oled_protection", false);
	m_setIndicatorsScale = ClampInt(getIntDef("indicators_scale", 100), 50, 300);
	for (int i = 0; i < INDICATOR_SOUND_EVENT_COUNT; i++) {
		std::string key = std::string("indicators_sound_") + kIndicatorSoundKeys[i];
		if (d && obs_data_has_user_value(d, key.c_str()))
			m_setIndicatorSoundPath[i] = overlay::util::Utf8ToWide(obs_data_get_string(d, key.c_str()));
		else
			m_setIndicatorSoundPath[i].clear();
	}
	for (int i = 0; i < INDICATOR_CUSTOM_SLOT_COUNT; i++) {
		std::string iconKey = std::string("indicators_") + kIndicatorSlotKeys[i] + "_icon";
		std::string tintKey = std::string("indicators_") + kIndicatorSlotKeys[i] + "_tint";
		std::string colorKey = std::string("indicators_") + kIndicatorSlotKeys[i] + "_color";
		if (d && obs_data_has_user_value(d, iconKey.c_str()))
			m_setIndicatorIconPath[i] = overlay::util::Utf8ToWide(obs_data_get_string(d, iconKey.c_str()));
		else
			m_setIndicatorIconPath[i].clear();
		m_setIndicatorTint[i] = getBoolDef(tintKey.c_str(), false);
		m_setIndicatorColor[i] = getIntDef(colorKey.c_str(), 0xFFFFFF) & 0xFFFFFF;
	}
	m_setRememberReplayBuffer = getBoolDef("remember_replay_buffer", false);
	m_setSmartReplay = getBoolDef("smart_replay", true);
	m_setSmartReplayMode = ClampInt(getIntDef("smart_replay_mode", 1), 0, 1);
	m_setSmartReplayClipIgnoreTrim = getBoolDef("smart_replay_clip_ignore_trim", true);
	{
		const int profileLen = overlay_clip_save_get_buffer_length();
		m_setBufferLengthSec = ClampInt(profileLen > 0 ? profileLen : getIntDef("clip_buffer_length", 120),
						OVERLAY_CLIP_SAVE_MIN_SECONDS, OVERLAY_CLIP_SAVE_MAX_SECONDS);
	}
	for (int i = 0; i < CLIP_SAVE_SLOT_COUNT; i++) {
		const std::string secKey = "clip_slot_" + std::to_string(i) + "_seconds";
		const std::string showKey = "clip_slot_" + std::to_string(i) + "_show";
		m_setClipSeconds[i] = ClampInt(getIntDef(secKey.c_str(), kDefaultClipSeconds[i]),
					       OVERLAY_CLIP_SAVE_MIN_SECONDS, OVERLAY_CLIP_SAVE_MAX_SECONDS);
		m_setClipShow[i] = getBoolDef(showKey.c_str(), true);
	}
	m_setGalleryInOverlay = getBoolDef("gallery_in_overlay", true);
	m_setReturnHomeOnHide = getBoolDef("return_home_on_hide", false);
	m_setExportMaxParallel = ClampInt(getIntDef("gallery_export_max_parallel", 1), 1, 4);
	m_setCaptureFocus = getBoolDef("capture_focus", true);
	m_setToastsEnabled = getBoolDef("toasts_enabled", true);
	m_setToastVideoSaved = getBoolDef("toast_video_saved", true);
	m_setToastExport = getBoolDef("toast_export", true);
	m_setToastOpenHint = getBoolDef("toast_open_hint", true);
	m_setToastLowDisk = getBoolDef("toast_low_disk", true);
	m_setToastLowDiskGb = ClampInt(getIntDef("toast_low_disk_gb", 5), 1, 100);
	m_setToastDurationSec = ClampInt(getIntDef("toast_duration_sec", 8), 2, 60);
	m_setToastHotkeysScope = ClampInt(getIntDef("toast_hotkeys_scope", 0), 0, 1);
	if (d && obs_data_has_user_value(d, "gallery_export_path"))
		m_setExportPath = overlay::util::Utf8ToWide(obs_data_get_string(d, "gallery_export_path"));
	else
		m_setExportPath.clear();

	for (int i = 0; i < SETTINGS_HOTKEY_COUNT; i++) {
		int hkVk, hkMod;
		getHotkeyVkMod(d, kSettingsHotkeyKeys[i], kSettingsHotkeyDefaultVk[i], kSettingsHotkeyDefaultMod[i],
			       hkVk, hkMod);
		m_setHotkeyVk[i] = hkVk;
		m_setHotkeyMod[i] = hkMod;
	}
}

void OverlayRenderer::PersistSettingsWorkingValues()
{
	overlay_settings_ensure();
	obs_data_t *d = saved_settings_data;
	if (!d)
		return;
	obs_data_set_int(d, "position", m_setPosition);
	obs_data_set_int(d, "margin", m_setMargin);
	obs_data_set_int(d, "orientation", m_setOrientation);
	obs_data_set_int(d, "overlay_monitor_mode", m_setMonitorMode);
	obs_data_set_int(d, "overlay_monitor_index", m_setMonitorIndex);
	obs_data_set_double(d, "overlay_background_alpha", m_setOpacityPct / 100.0);
	obs_data_set_int(d, "overlay_scale", m_setOverlayScale);
	obs_data_set_int(d, "overlay_text_scale", m_setTextScale);
	obs_data_set_int(d, "overlay_accent_color", m_setAccentColor & 0xFFFFFF);
	obs_data_set_bool(d, "auto_hide_enabled", m_setAutoHideEnabled);
	obs_data_set_int(d, "auto_hide_seconds", m_setAutoHideSeconds);
	obs_data_set_bool(d, "indicators_enabled", m_setIndicatorsEnabled);
	obs_data_set_int(d, "indicators_position", m_setIndicatorsPosition);
	obs_data_set_bool(d, "indicators_oled_protection", m_setIndicatorsOled);
	obs_data_set_int(d, "indicators_scale", m_setIndicatorsScale);
	for (int i = 0; i < INDICATOR_SOUND_EVENT_COUNT; i++) {
		std::string key = std::string("indicators_sound_") + kIndicatorSoundKeys[i];
		obs_data_set_string(d, key.c_str(), overlay::util::WideToUtf8(m_setIndicatorSoundPath[i]).c_str());
	}
	for (int i = 0; i < INDICATOR_CUSTOM_SLOT_COUNT; i++) {
		std::string iconKey = std::string("indicators_") + kIndicatorSlotKeys[i] + "_icon";
		std::string tintKey = std::string("indicators_") + kIndicatorSlotKeys[i] + "_tint";
		std::string colorKey = std::string("indicators_") + kIndicatorSlotKeys[i] + "_color";
		obs_data_set_string(d, iconKey.c_str(), overlay::util::WideToUtf8(m_setIndicatorIconPath[i]).c_str());
		obs_data_set_bool(d, tintKey.c_str(), m_setIndicatorTint[i]);
		obs_data_set_int(d, colorKey.c_str(), m_setIndicatorColor[i] & 0xFFFFFF);
	}
	obs_data_set_bool(d, "remember_replay_buffer", m_setRememberReplayBuffer);
	obs_data_set_bool(d, "smart_replay", m_setSmartReplay);
	obs_data_set_int(d, "smart_replay_mode", m_setSmartReplayMode);
	obs_data_set_bool(d, "smart_replay_clip_ignore_trim", m_setSmartReplayClipIgnoreTrim);
	obs_data_set_int(d, "clip_buffer_length", m_setBufferLengthSec);
	for (int i = 0; i < CLIP_SAVE_SLOT_COUNT; i++) {
		obs_data_set_int(d, ("clip_slot_" + std::to_string(i) + "_seconds").c_str(), m_setClipSeconds[i]);
		obs_data_set_bool(d, ("clip_slot_" + std::to_string(i) + "_show").c_str(), m_setClipShow[i]);
	}
	obs_data_set_bool(d, "gallery_in_overlay", m_setGalleryInOverlay);
	obs_data_set_bool(d, "return_home_on_hide", m_setReturnHomeOnHide);
	obs_data_set_int(d, "gallery_export_max_parallel", m_setExportMaxParallel);
	obs_data_set_bool(d, "capture_focus", m_setCaptureFocus);
	if (m_exportInfraReady) {
		m_rebuildPresets.maxParallel = m_setExportMaxParallel;
		m_exportQueue.SetMaxParallel(m_setExportMaxParallel);
		m_rebuildPresets.Save(d);
	}
	obs_data_set_bool(d, "toasts_enabled", m_setToastsEnabled);
	obs_data_set_bool(d, "toast_video_saved", m_setToastVideoSaved);
	obs_data_set_bool(d, "toast_export", m_setToastExport);
	obs_data_set_bool(d, "toast_open_hint", m_setToastOpenHint);
	obs_data_set_bool(d, "toast_low_disk", m_setToastLowDisk);
	obs_data_set_int(d, "toast_low_disk_gb", m_setToastLowDiskGb);
	obs_data_set_int(d, "toast_duration_sec", m_setToastDurationSec);
	obs_data_set_int(d, "toast_hotkeys_scope", m_setToastHotkeysScope);
	obs_data_set_bool(d, "toast_hotkeys_always", m_setToastHotkeysScope == 1);
	obs_data_set_string(d, "gallery_export_path", overlay::util::WideToUtf8(m_setExportPath).c_str());
	for (int i = 0; i < SETTINGS_HOTKEY_COUNT; i++) {
		obs_data_set_int(d, (std::string(kSettingsHotkeyKeys[i]) + "_vk").c_str(), m_setHotkeyVk[i]);
		obs_data_set_int(d, (std::string(kSettingsHotkeyKeys[i]) + "_mod").c_str(), m_setHotkeyMod[i]);
	}
	ApplyIndicatorCustoms();
	ApplyIndicatorSounds();
	ApplyToastActionHotkeys();
	ApplyClipSaveHotkeys();
	ApplyToastAccentColor();
	ApplyToastOptions();
#ifdef ENABLE_QT
	overlay_runtime_set_smart_replay(m_setSmartReplay);
	overlay_runtime_set_smart_replay_mode(m_setSmartReplayMode);
	overlay_runtime_set_smart_replay_clip_ignore_trim(m_setSmartReplayClipIgnoreTrim);
#endif
}

void OverlayRenderer::ApplyIndicatorCustoms()
{
#ifdef ENABLE_QT
	for (int i = 0; i < INDICATOR_CUSTOM_SLOT_COUNT; i++) {
		overlay_runtime_set_indicator_custom(i, overlay::util::WideToUtf8(m_setIndicatorIconPath[i]).c_str(),
						     m_setIndicatorTint[i], m_setIndicatorColor[i]);
	}
#endif
}

void OverlayRenderer::ApplyIndicatorSounds()
{
#ifdef ENABLE_QT
	for (int i = 0; i < INDICATOR_SOUND_EVENT_COUNT; i++) {
		overlay_runtime_set_indicator_sound(i, overlay::util::WideToUtf8(m_setIndicatorSoundPath[i]).c_str());
	}
#endif
}

void OverlayRenderer::ApplyToastActionHotkeys()
{
#ifdef ENABLE_QT
	overlay_runtime_set_toast_action_hotkeys(m_setHotkeyVk[kToastViewHotkeyIndex],
						 m_setHotkeyMod[kToastViewHotkeyIndex],
						 m_setHotkeyVk[kToastDeleteHotkeyIndex],
						 m_setHotkeyMod[kToastDeleteHotkeyIndex]);
#endif
}

void OverlayRenderer::ApplyClipSaveHotkeys()
{
	HWND hwnd = m_windowManager.GetHWND();
	if (!hwnd)
		return;

	// Do not use RegisterHotKey: it steals bare keys (8/9/0, etc.) system-wide
	// and blocks typing in browsers and other apps. Poll instead, like OBS hotkeys.
	bool anyBound = false;
	for (int i = 0; i < kClipHotkeyCount; i++) {
		const int vk = m_setHotkeyVk[kClipHotkeyBaseIndex + i];
		const int mod = m_setHotkeyMod[kClipHotkeyBaseIndex + i];
		m_clipHotkeyWasDown[i] = ClipHotkeyComboDown(vk, mod);
		if (vk > 0)
			anyBound = true;
	}

	if (anyBound) {
		s_timerInstance = this;
		if (!m_clipHotkeyTimer)
			m_clipHotkeyTimer = SetTimer(hwnd, kClipHotkeyTimerId, kClipHotkeyPollMs, TimerProc);
	} else if (m_clipHotkeyTimer) {
		KillTimer(hwnd, m_clipHotkeyTimer);
		m_clipHotkeyTimer = 0;
	}
}

void OverlayRenderer::PollClipSaveHotkeys()
{
	const bool suppress = m_settingsOpen || m_galleryOpen || m_settingsCapturingHotkeyIndex >= 0 ||
			      !obs_frontend_replay_buffer_active() || OverlayStateManager::GetReplaySaving() ||
			      ForegroundHasTextCaret();

	for (int i = 0; i < kClipHotkeyCount; i++) {
		const int vk = m_setHotkeyVk[kClipHotkeyBaseIndex + i];
		const int mod = m_setHotkeyMod[kClipHotkeyBaseIndex + i];
		const bool down = ClipHotkeyComboDown(vk, mod);
		if (!suppress && down && !m_clipHotkeyWasDown[i]) {
			m_stateManager.SaveReplayClip(m_setClipSeconds[i]);
			if (IsVisible()) {
				UpdateStatus();
				Render();
			}
		}
		m_clipHotkeyWasDown[i] = down;
	}
}

void OverlayRenderer::ApplyToastAccentColor()
{
#ifdef ENABLE_QT
	overlay_runtime_set_toast_accent_color(m_setAccentColor & 0xFFFFFF);
#endif
}

void OverlayRenderer::ApplyToastOptions()
{
#ifdef ENABLE_QT
	overlay_runtime_set_toast_options(m_setToastsEnabled, m_setToastVideoSaved, m_setToastOpenHint,
					  m_setToastLowDisk, m_setToastLowDiskGb, m_setToastDurationSec);
	overlay_runtime_set_toast_hotkeys_always_active(m_setToastHotkeysScope == 1);
#endif
}

void OverlayRenderer::OpenSettings()
{
	if (m_settingsOpen)
		return;
	if (m_galleryOpen)
		CloseGallery();

	m_replayClipPickerOpen = false;
	m_settingsOpen = true;
	m_settingsPage = SettingsPage::Categories;
	m_settingsScrollOffset = 0;
	m_settingsCapturingHotkeyIndex = -1;
	m_settingsEditingIntField = -1;
	m_settingsEditText.clear();
	m_settingsHoverIndex = -1;
	m_settingsHits.clear();
	LoadSettingsWorkingValues();

	m_windowManager.SetKeyHandler(
		[](int vkCode, int mods, void *userData) {
			OverlayRenderer *r = static_cast<OverlayRenderer *>(userData);
			if (r)
				r->HandleSettingsKey(vkCode, mods);
		},
		this);
	m_windowManager.SetWheelHandler(
		[](int delta, int x, int y, void *userData) {
			UNUSED_PARAMETER(x);
			UNUSED_PARAMETER(y);
			OverlayRenderer *r = static_cast<OverlayRenderer *>(userData);
			if (r)
				r->HandleSettingsWheel(delta);
		},
		this);

	m_lastInteractionTick = GetTickCount64();
	if (!IsVisible()) {
		Show();
	} else {
		ApplySettingsWindowSize();
		SetUpdateInterval(16);
	}
	Render();
}

void OverlayRenderer::CloseSettings()
{
	if (!m_settingsOpen)
		return;
	if (m_settingsEditingIntField >= 0)
		CommitSettingsIntEdit();
	m_settingsOpen = false;
	m_settingsPage = SettingsPage::Categories;
	m_settingsCapturingHotkeyIndex = -1;
	m_settingsEditingIntField = -1;
	m_settingsEditText.clear();
	m_settingsHoverIndex = -1;
	m_settingsHits.clear();
	m_windowManager.SetKeyboardCaptureActive(false);
	m_windowManager.SetKeyHandler(nullptr, nullptr);
	m_windowManager.SetWheelHandler(nullptr, nullptr);

	PersistSettingsWorkingValues();
	if (!overlay_settings_save())
		obs_log(LOG_ERROR, "Overlay settings not saved to disk");

	m_position = static_cast<OverlayPosition>(m_setPosition);
	m_margin = m_setMargin;
	m_orientation = static_cast<OverlayOrientation>(m_setOrientation);
	m_monitorMode = static_cast<OverlayMonitorMode>(ClampInt(m_setMonitorMode, 0, 2));
	{
		const int monitorCount = std::max(1, m_layoutManager.GetMonitorCount());
		m_monitorIndex = ClampInt(m_setMonitorIndex, 0, monitorCount - 1);
		m_setMonitorIndex = m_monitorIndex;
	}
	m_overlayBackgroundAlpha = static_cast<float>(m_setOpacityPct) / 100.0f;
	m_overlayScalePercent = ClampInt(m_setOverlayScale, 50, 300);
	m_textScalePercent = ClampInt(m_setTextScale, 50, 300);
	m_accentColorRgb = m_setAccentColor & 0xFFFFFF;
	ApplyAccentBrushes();
	m_autoHideEnabled = m_setAutoHideEnabled;
	m_autoHideSeconds = ClampInt(m_setAutoHideSeconds, 1, 3600);
	m_galleryEnabled = m_setGalleryInOverlay;
	m_returnHomeOnHide = m_setReturnHomeOnHide;
	m_captureFocus = m_setCaptureFocus;
	m_windowManager.SetAllowActivate(m_setCaptureFocus);

	m_lastInteractionTick = GetTickCount64();
	SetUpdateInterval(500);
	ApplyPosition();
	Render();
}

void OverlayRenderer::ApplySettingsWindowSize()
{
	if (!m_settingsOpen)
		return;
	const OverlayMonitorMode mode = static_cast<OverlayMonitorMode>(ClampInt(m_setMonitorMode, 0, 2));
	RECT workArea = GetTargetWorkArea(mode, m_setMonitorIndex);
	int workWidth = workArea.right - workArea.left;
	int workHeight = workArea.bottom - workArea.top;

	int targetWidth = std::min(640, workWidth - 40);
	int targetHeight = std::min(780, workHeight - 60);
	if (targetWidth < 360)
		targetWidth = std::max(360, workWidth - 20);
	if (targetHeight < 360)
		targetHeight = std::max(360, workHeight - 20);

	POINT contentPos = {workArea.left + (workWidth - targetWidth) / 2,
			    workArea.top + (workHeight - targetHeight) / 2};

	m_settingsContentWidth = targetWidth;
	m_settingsContentHeight = targetHeight;

	if (IsDimmingActive()) {
		ApplyDimmingWindowGeometry(contentPos, targetWidth, targetHeight);
	} else {
		m_dimContentOffset = {0, 0};
		m_windowManager.SetPosition(contentPos, targetWidth, targetHeight);
	}
}

void OverlayRenderer::RenderSettingsPanel()
{
	ID2D1RenderTarget *target = m_bitmapRenderTarget ? m_bitmapRenderTarget : m_renderTarget;
	if (!target)
		return;

	int width;
	int height;
	if (IsDimmingActive()) {
		width = m_settingsContentWidth;
		height = m_settingsContentHeight;
	} else {
		HWND hwnd = m_windowManager.GetHWND();
		if (!hwnd)
			return;
		RECT clientRect;
		GetClientRect(hwnd, &clientRect);
		width = clientRect.right - clientRect.left;
		height = clientRect.bottom - clientRect.top;
	}
	if (width <= 0 || height <= 0)
		return;

	m_settingsHits.clear();

	const D2D1_COLOR_F kWhite = D2D1::ColorF(1.0f, 1.0f, 1.0f);
	const D2D1_COLOR_F kDim = D2D1::ColorF(0.75f, 0.75f, 0.80f);
	const D2D1_COLOR_F kAccent = AccentColor();
	const D2D1_COLOR_F kRowBg = D2D1::ColorF(30.0f / 255.0f, 30.0f / 255.0f, 38.0f / 255.0f, 0.9f);
	const D2D1_COLOR_F kRowHover = AccentHover(0.30f);
	const D2D1_COLOR_F kCtrlBg = D2D1::ColorF(0.12f, 0.12f, 0.15f, 1.0f);
	const D2D1_COLOR_F kBorder = D2D1::ColorF(58.0f / 255.0f, 58.0f / 255.0f, 69.0f / 255.0f, 1.0f);

	const int pad = 3;
	RECT shell = {pad, pad, width - pad, height - pad};
	DrawRoundedRectangle(shell, 14.0f, D2D1::ColorF(20.0f / 255.0f, 20.0f / 255.0f, 25.0f / 255.0f, 0.98f));
	DrawRoundedRectangleBorder(shell, 14.0f, 1.0f, kBorder);

	const int insetH = 18;
	const int headerTop = shell.top + 12;
	const int headerH = 34;
	RECT headerRect = {shell.left + insetH, headerTop, shell.right - insetH, headerTop + headerH};

	const bool onRoot = (m_settingsPage == SettingsPage::Categories);
	std::wstring headerTitle = overlay::util::ModuleTextW("OverlaySettings");
	switch (m_settingsPage) {
	case SettingsPage::General:
		headerTitle = overlay::util::ModuleTextW("Tab.General");
		break;
	case SettingsPage::Appearance:
		headerTitle = overlay::util::ModuleTextW("Appearance");
		break;
	case SettingsPage::Indicators:
		headerTitle = overlay::util::ModuleTextW("Indicators");
		break;
	case SettingsPage::Audio:
		headerTitle = overlay::util::ModuleTextW("Tab.Audio");
		break;
	case SettingsPage::Replay:
		headerTitle = overlay::util::ModuleTextW("Replay");
		break;
	case SettingsPage::ClipSave:
		headerTitle = overlay::util::ModuleTextW("ClipSave");
		break;
	case SettingsPage::Notifications:
		headerTitle = overlay::util::ModuleTextW("Notifications");
		break;
	case SettingsPage::Hotkeys:
		headerTitle = overlay::util::ModuleTextW("Tab.Hotkeys");
		break;
	case SettingsPage::About:
		headerTitle = overlay::util::ModuleTextW("About");
		break;
	case SettingsPage::Categories:
	default:
		break;
	}
	RenderText(headerTitle, {headerRect.left, headerRect.top, headerRect.right - 40, headerRect.bottom}, 15.0f,
		   DWRITE_FONT_WEIGHT_BOLD, kWhite, DWRITE_TEXT_ALIGNMENT_LEADING);

	const int closeSize = 30;
	RECT closeRect = {headerRect.right - closeSize, headerRect.top + (headerH - closeSize) / 2,
			  headerRect.right, headerRect.top + (headerH - closeSize) / 2 + closeSize};
	{
		int idx = static_cast<int>(m_settingsHits.size());
		bool hover = (idx == m_settingsHoverIndex);
		if (hover)
			DrawRoundedRectangle(closeRect, 8.0f, kRowHover);
		D2D1_COLOR_F tint = hover ? kAccent : kDim;
		DrawGalleryIcon(closeRect, 7, &tint); // icon 7 = back/close
		m_settingsHits.push_back(
			{closeRect, onRoot ? SettingsAction::Close : SettingsAction::Back, 0});
	}

	const int viewTop = headerRect.bottom + 10;
	const int viewBottom = shell.bottom - 12;
	m_settingsViewportTop = viewTop;
	m_settingsViewportBottom = viewBottom;
	const int viewportHeight = viewBottom - viewTop;

	const int contentLeft = shell.left + insetH;
	const int contentRight = shell.right - insetH - 10;
	const int labelWidth = (contentRight - contentLeft) / 2;
	const int rowGap = 8;

	target->PushAxisAlignedClip(
		D2D1::RectF(static_cast<float>(shell.left), static_cast<float>(viewTop),
			    static_cast<float>(shell.right), static_cast<float>(viewBottom)),
		D2D1_ANTIALIAS_MODE_ALIASED);

	int y = viewTop - m_settingsScrollOffset;

	auto categoryRow = [&](const std::wstring &title, SettingsPage page) {
		const int h = 44;
		RECT r = {contentLeft, y, contentRight, y + h};
		int idx = static_cast<int>(m_settingsHits.size());
		bool hover = (idx == m_settingsHoverIndex);
		DrawRoundedRectangle(r, 8.0f, hover ? kRowHover : kRowBg);
		RECT lr = {r.left + 12, r.top, r.right - 36, r.bottom};
		RenderText(title, lr, 13.0f, DWRITE_FONT_WEIGHT_NORMAL, kWhite, DWRITE_TEXT_ALIGNMENT_LEADING);
		RECT chevron = {r.right - 28, r.top, r.right - 8, r.bottom};
		RenderText(L"\u25B6", chevron, 11.0f, DWRITE_FONT_WEIGHT_NORMAL, kAccent, DWRITE_TEXT_ALIGNMENT_CENTER);
		m_settingsHits.push_back({r, SettingsAction::OpenCategory, static_cast<int>(page)});
		y += h + rowGap;
	};

	auto checkboxRow = [&](const std::wstring &label, bool value, SettingsField field) {
		const int h = 40;
		RECT r = {contentLeft, y, contentRight, y + h};
		int idx = static_cast<int>(m_settingsHits.size());
		bool hover = (idx == m_settingsHoverIndex);
		DrawRoundedRectangle(r, 8.0f, hover ? kRowHover : kRowBg);
		RECT lr = {r.left + 12, r.top, r.right - 60, r.bottom};
		RenderText(label, lr, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, kWhite, DWRITE_TEXT_ALIGNMENT_LEADING);
		const int box = 22;
		RECT br = {r.right - 12 - box, r.top + (h - box) / 2, r.right - 12, r.top + (h - box) / 2 + box};
		DrawRoundedRectangle(br, 5.0f, value ? kAccent : kCtrlBg);
		DrawRoundedRectangleBorder(br, 5.0f, 1.0f, kBorder);
		if (value) {
			RECT ck = {br.left + 5, br.top + 5, br.right - 5, br.bottom - 5};
			DrawRoundedRectangle(ck, 3.0f, kWhite);
		}
		m_settingsHits.push_back({r, SettingsAction::ToggleBool, static_cast<int>(field)});
		y += h + rowGap;
	};

	auto stepperRow = [&](const std::wstring &label, const std::wstring &value, SettingsField field) {
		const int h = 40;
		RECT r = {contentLeft, y, contentRight, y + h};
		DrawRoundedRectangle(r, 8.0f, kRowBg);
		RECT lr = {r.left + 12, r.top, r.left + labelWidth, r.bottom};
		RenderText(label, lr, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, kWhite, DWRITE_TEXT_ALIGNMENT_LEADING);

		const int btn = 28;
		const int cy = r.top + (h - btn) / 2;
		RECT rightBtn = {r.right - 12 - btn, cy, r.right - 12, cy + btn};
		const int valW = 96;
		RECT valRect = {rightBtn.left - 4 - valW, r.top, rightBtn.left - 4, r.bottom};
		RECT leftBtn = {valRect.left - 4 - btn, cy, valRect.left - 4, cy + btn};

		int idxL = static_cast<int>(m_settingsHits.size());
		bool hovL = (idxL == m_settingsHoverIndex);
		DrawRoundedRectangle(leftBtn, 6.0f, hovL ? kRowHover : kCtrlBg);
		RenderText(L"\u25C0", leftBtn, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, kAccent, DWRITE_TEXT_ALIGNMENT_CENTER);
		m_settingsHits.push_back({leftBtn, SettingsAction::StepDown, static_cast<int>(field)});

		RenderText(value, valRect, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, kWhite, DWRITE_TEXT_ALIGNMENT_CENTER);

		int idxR = static_cast<int>(m_settingsHits.size());
		bool hovR = (idxR == m_settingsHoverIndex);
		DrawRoundedRectangle(rightBtn, 6.0f, hovR ? kRowHover : kCtrlBg);
		RenderText(L"\u25B6", rightBtn, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, kAccent, DWRITE_TEXT_ALIGNMENT_CENTER);
		m_settingsHits.push_back({rightBtn, SettingsAction::StepUp, static_cast<int>(field)});

		y += h + rowGap;
	};

	auto intTextRow = [&](const std::wstring &label, const std::wstring &value, SettingsField field,
			      bool enabled = true) {
		const int h = 40;
		RECT r = {contentLeft, y, contentRight, y + h};
		DrawRoundedRectangle(r, 8.0f, kRowBg);
		RECT lr = {r.left + 12, r.top, r.left + labelWidth, r.bottom};
		RenderText(label, lr, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, enabled ? kWhite : kDim,
			   DWRITE_TEXT_ALIGNMENT_LEADING);

		const int boxH = 28;
		const int boxW = 120;
		const int cy = r.top + (h - boxH) / 2;
		RECT box = {r.right - 12 - boxW, cy, r.right - 12, cy + boxH};
		const bool editing = enabled && (m_settingsEditingIntField == static_cast<int>(field));
		int idx = static_cast<int>(m_settingsHits.size());
		bool hov = enabled && !editing && (idx == m_settingsHoverIndex);
		DrawRoundedRectangle(box, 6.0f, editing ? kCtrlBg : (hov ? kRowHover : kCtrlBg));
		DrawRoundedRectangleBorder(box, 6.0f, editing ? 2.0f : 1.0f, editing ? kAccent : kBorder);
		const std::wstring shown =
			editing ? (m_settingsEditText + L"|") : value;
		RenderText(shown, box, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, enabled ? kWhite : kDim,
			   DWRITE_TEXT_ALIGNMENT_CENTER);
		if (enabled) {
			m_settingsHits.push_back({box, SettingsAction::EditInt, static_cast<int>(field)});
		}

		y += h + rowGap;
	};

	auto hotkeyRow = [&](const std::wstring &label, int index) {
		const int h = 40;
		RECT r = {contentLeft, y, contentRight, y + h};
		DrawRoundedRectangle(r, 8.0f, kRowBg);
		RECT lr = {r.left + 12, r.top, r.left + labelWidth, r.bottom};
		RenderText(label, lr, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, kWhite, DWRITE_TEXT_ALIGNMENT_LEADING);

		const int btnW = 150;
		const int btn = 28;
		const int cy = r.top + (h - btn) / 2;
		RECT b = {r.right - 12 - btnW, cy, r.right - 12, cy + btn};
		int idx = static_cast<int>(m_settingsHits.size());
		bool hover = (idx == m_settingsHoverIndex);
		bool capturing = (m_settingsCapturingHotkeyIndex == index);
		DrawRoundedRectangle(b, 6.0f, capturing ? kAccent : (hover ? kRowHover : kCtrlBg));
		DrawRoundedRectangleBorder(b, 6.0f, 1.0f, kBorder);
		std::wstring t = capturing ? overlay::util::ModuleTextW("Hotkeys.PressKey")
					   : SettingsHotkeyName(m_setHotkeyVk[index], m_setHotkeyMod[index]);
		RenderText(t, b, 11.0f, DWRITE_FONT_WEIGHT_NORMAL, capturing ? kWhite : kDim,
			   DWRITE_TEXT_ALIGNMENT_CENTER);
		m_settingsHits.push_back({b, SettingsAction::HotkeyCapture, index});

		y += h + rowGap;
	};

	auto pathRow = [&]() {
		const int h = 58;
		RECT r = {contentLeft, y, contentRight, y + h};
		DrawRoundedRectangle(r, 8.0f, kRowBg);
		RECT lr = {r.left + 12, r.top + 6, r.right - 12, r.top + 24};
		RenderText(overlay::util::ModuleTextW("Gallery.ExportFolder"), lr, 12.0f, DWRITE_FONT_WEIGHT_NORMAL,
			   kWhite, DWRITE_TEXT_ALIGNMENT_LEADING);

		const int btnW = 110;
		const int btnH = 26;
		const int by = r.bottom - 8 - btnH;
		RECT browse = {r.right - 12 - btnW, by, r.right - 12, by + btnH};
		RECT pathRect = {r.left + 12, by, browse.left - 8, by + btnH};
		DrawRoundedRectangle(pathRect, 6.0f, kCtrlBg);
		RECT pTextRect = {pathRect.left + 8, pathRect.top, pathRect.right - 8, pathRect.bottom};
		std::wstring p = m_setExportPath.empty() ? overlay::util::ModuleTextW("Gallery.ExportPlaceholder")
							 : m_setExportPath;
		RenderText(p, pTextRect, 11.0f, DWRITE_FONT_WEIGHT_NORMAL, m_setExportPath.empty() ? kDim : kWhite,
			   DWRITE_TEXT_ALIGNMENT_LEADING);

		int idx = static_cast<int>(m_settingsHits.size());
		bool hover = (idx == m_settingsHoverIndex);
		DrawRoundedRectangle(browse, 6.0f, hover ? kRowHover : kCtrlBg);
		DrawRoundedRectangleBorder(browse, 6.0f, 1.0f, kBorder);
		RenderText(overlay::util::ModuleTextW("Gallery.Browse"), browse, 11.0f, DWRITE_FONT_WEIGHT_NORMAL,
			   kAccent, DWRITE_TEXT_ALIGNMENT_CENTER);
		m_settingsHits.push_back({browse, SettingsAction::Browse, 0});

		y += h + rowGap;
	};

	switch (m_settingsPage) {
	case SettingsPage::Categories:
		categoryRow(overlay::util::ModuleTextW("Tab.General"), SettingsPage::General);
		categoryRow(overlay::util::ModuleTextW("Appearance"), SettingsPage::Appearance);
		categoryRow(overlay::util::ModuleTextW("Indicators"), SettingsPage::Indicators);
		categoryRow(overlay::util::ModuleTextW("Tab.Audio"), SettingsPage::Audio);
		categoryRow(overlay::util::ModuleTextW("Replay"), SettingsPage::Replay);
		categoryRow(overlay::util::ModuleTextW("ClipSave"), SettingsPage::ClipSave);
		categoryRow(overlay::util::ModuleTextW("Notifications"), SettingsPage::Notifications);
		categoryRow(overlay::util::ModuleTextW("Tab.Hotkeys"), SettingsPage::Hotkeys);
		categoryRow(overlay::util::ModuleTextW("About"), SettingsPage::About);
		break;
	case SettingsPage::General:
		checkboxRow(overlay::util::ModuleTextW("AutoHide.Enable"), m_setAutoHideEnabled,
			    SettingsField::AutoHideEnabled);
		stepperRow(overlay::util::ModuleTextW("AutoHide.Delay"), std::to_wstring(m_setAutoHideSeconds) + L" s",
			   SettingsField::AutoHideSeconds);
		checkboxRow(overlay::util::ModuleTextW("Gallery.InOverlay"), m_setGalleryInOverlay,
			    SettingsField::GalleryInOverlay);
		checkboxRow(overlay::util::ModuleTextW("ReturnHomeOnHide"), m_setReturnHomeOnHide,
			    SettingsField::ReturnHomeOnHide);
		pathRow();
		stepperRow(overlay::util::ModuleTextW("Export.MaxParallel"), std::to_wstring(m_setExportMaxParallel),
			   SettingsField::ExportMaxParallel);
		checkboxRow(overlay::util::ModuleTextW("CaptureFocus"), m_setCaptureFocus, SettingsField::CaptureFocus);
		break;
	case SettingsPage::Appearance:
		stepperRow(overlay::util::ModuleTextW("Monitor.Label"),
			   overlay::util::ModuleTextW(kSettingsMonitorModeKeys[ClampInt(m_setMonitorMode, 0, 2)]),
			   SettingsField::MonitorMode);
		if (m_setMonitorMode == static_cast<int>(OverlayMonitorMode::Specific)) {
			stepperRow(overlay::util::ModuleTextW("Monitor.Index"), FormatMonitorLabel(m_setMonitorIndex),
				   SettingsField::MonitorIndex);
		}
		stepperRow(overlay::util::ModuleTextW("Position.Label"),
			   overlay::util::ModuleTextW(kSettingsPositionKeys[m_setPosition]), SettingsField::Position);
		stepperRow(overlay::util::ModuleTextW("Margin"), std::to_wstring(m_setMargin) + L" px",
			   SettingsField::Margin);
		stepperRow(overlay::util::ModuleTextW("Layout"),
			   overlay::util::ModuleTextW(kSettingsOrientationKeys[m_setOrientation]),
			   SettingsField::Orientation);
		stepperRow(overlay::util::ModuleTextW("OverlayOpacity"), std::to_wstring(m_setOpacityPct) + L"%",
			   SettingsField::Opacity);
		stepperRow(overlay::util::ModuleTextW("OverlayScale"), std::to_wstring(m_setOverlayScale) + L"%",
			   SettingsField::OverlayScale);
		stepperRow(overlay::util::ModuleTextW("TextSize"), std::to_wstring(m_setTextScale) + L"%",
			   SettingsField::TextScale);
		{
			const int h = 44;
			RECT r = {contentLeft, y, contentRight, y + h};
			DrawRoundedRectangle(r, 8.0f, kRowBg);
			RECT lr = {r.left + 12, r.top, r.left + labelWidth, r.bottom};
			RenderText(overlay::util::ModuleTextW("AccentColor"), lr, 12.0f, DWRITE_FONT_WEIGHT_NORMAL,
				   kWhite, DWRITE_TEXT_ALIGNMENT_LEADING);

			const int sw = 28;
			RECT swatch = {r.right - 12 - sw, r.top + (h - sw) / 2, r.right - 12, r.top + (h - sw) / 2 + sw};
			DrawRoundedRectangle(swatch, 6.0f, ColorFromRgb(m_setAccentColor));
			int swatchIdx = static_cast<int>(m_settingsHits.size());
			bool swatchHover = (swatchIdx == m_settingsHoverIndex);
			DrawRoundedRectangleBorder(swatch, 6.0f, 1.0f, swatchHover ? kAccent : kBorder);
			m_settingsHits.push_back({swatch, SettingsAction::PickAccentColor, 0});
			y += h + rowGap;
		}
		break;
	case SettingsPage::Indicators:
		checkboxRow(overlay::util::ModuleTextW("Indicators.Show"), m_setIndicatorsEnabled,
			    SettingsField::IndicatorsEnabled);
		stepperRow(overlay::util::ModuleTextW("Position.Label"),
			   overlay::util::ModuleTextW(kSettingsPositionKeys[m_setIndicatorsPosition]),
			   SettingsField::IndicatorsPosition);
		stepperRow(overlay::util::ModuleTextW("Indicators.Scale"),
			   std::to_wstring(m_setIndicatorsScale) + L"%", SettingsField::IndicatorsScale);
		checkboxRow(overlay::util::ModuleTextW("Indicators.OledProtection"), m_setIndicatorsOled,
			    SettingsField::IndicatorsOled);

		{
			const int previewSize = 32;
			const int previewGap = 8;
			const int h = 48;
			RECT r = {contentLeft, y, contentRight, y + h};
			DrawRoundedRectangle(r, 8.0f, kRowBg);
			DrawRoundedRectangleBorder(r, 8.0f, 1.0f, kBorder);

			const int iconsW = INDICATOR_CUSTOM_SLOT_COUNT * previewSize +
					   (INDICATOR_CUSTOM_SLOT_COUNT - 1) * previewGap;
			RECT lr = {r.left + 12, r.top, r.right - 12 - iconsW - 12, r.bottom};
			RenderText(overlay::util::ModuleTextW("Indicators.Preview"), lr, 12.0f,
				   DWRITE_FONT_WEIGHT_NORMAL, kWhite, DWRITE_TEXT_ALIGNMENT_LEADING);

			int px = r.right - 12 - iconsW;
			const int py = r.top + (h - previewSize) / 2;
			for (int slot = 0; slot < INDICATOR_CUSTOM_SLOT_COUNT; slot++) {
				RECT preview = {px, py, px + previewSize, py + previewSize};
				DrawIndicatorPreview(slot, preview);
				px += previewSize + previewGap;
			}
			y += h + 14;
		}

		for (int slot = 0; slot < INDICATOR_CUSTOM_SLOT_COUNT; slot++) {
			const int cardPad = 12;
			const int previewSize = 40;
			const int btnW = 90;
			const int btnH = 26;
			const int box = 22;
			const int sw = 28;
			const int innerGap = 10;
			const int headerH = 44;
			const int pathH = 52;
			const int bottomH = 40;
			const int cardH = cardPad + headerH + innerGap + pathH + innerGap + bottomH + cardPad;

			RECT card = {contentLeft, y, contentRight, y + cardH};
			DrawRoundedRectangle(card, 8.0f, kRowBg);
			DrawRoundedRectangleBorder(card, 8.0f, 1.0f, kBorder);

			const int innerLeft = card.left + cardPad;
			const int innerRight = card.right - cardPad;
			int cy = card.top + cardPad;

			{
				RECT header = {innerLeft, cy, innerRight, cy + headerH};
				RECT preview = {header.right - previewSize, header.top + (headerH - previewSize) / 2,
						header.right, header.top + (headerH - previewSize) / 2 + previewSize};
				RECT title = {header.left, header.top, preview.left - 12, header.bottom};
				RenderText(overlay::util::ModuleTextW(kIndicatorSlotLabelKeys[slot]), title, 13.0f,
					   DWRITE_FONT_WEIGHT_SEMI_BOLD, kWhite, DWRITE_TEXT_ALIGNMENT_LEADING);
				DrawIndicatorPreview(slot, preview);
				cy += headerH + innerGap;
			}

			{
				RECT block = {innerLeft, cy, innerRight, cy + pathH};
				RECT label = {block.left, block.top, block.right, block.top + 20};
				RenderText(overlay::util::ModuleTextW("Indicators.Icon"), label, 11.0f,
					   DWRITE_FONT_WEIGHT_NORMAL, kDim, DWRITE_TEXT_ALIGNMENT_LEADING);

				const int by = block.bottom - btnH;
				RECT clearBtn = {block.right - btnW, by, block.right, by + btnH};
				RECT browse = {clearBtn.left - 8 - btnW, by, clearBtn.left - 8, by + btnH};
				RECT pathRect = {block.left, by, browse.left - 8, by + btnH};
				DrawRoundedRectangle(pathRect, 6.0f, kCtrlBg);
				RECT pTextRect = {pathRect.left + 8, pathRect.top, pathRect.right - 8, pathRect.bottom};
				const bool empty = m_setIndicatorIconPath[slot].empty();
				std::wstring p =
					empty ? overlay::util::ModuleTextW("Indicators.IconPlaceholder")
					      : IndicatorIconFileNameOnly(m_setIndicatorIconPath[slot]);
				RenderText(p, pTextRect, 11.0f, DWRITE_FONT_WEIGHT_NORMAL, empty ? kDim : kWhite,
					   DWRITE_TEXT_ALIGNMENT_LEADING);

				int browseIdx = static_cast<int>(m_settingsHits.size());
				bool browseHover = (browseIdx == m_settingsHoverIndex);
				DrawRoundedRectangle(browse, 6.0f, browseHover ? kRowHover : kCtrlBg);
				DrawRoundedRectangleBorder(browse, 6.0f, 1.0f, kBorder);
				RenderText(overlay::util::ModuleTextW("Indicators.Browse"), browse, 11.0f,
					   DWRITE_FONT_WEIGHT_NORMAL, kAccent, DWRITE_TEXT_ALIGNMENT_CENTER);
				m_settingsHits.push_back({browse, SettingsAction::BrowseIndicator, slot});

				int clearIdx = static_cast<int>(m_settingsHits.size());
				bool clearHover = (clearIdx == m_settingsHoverIndex);
				DrawRoundedRectangle(clearBtn, 6.0f, clearHover ? kRowHover : kCtrlBg);
				DrawRoundedRectangleBorder(clearBtn, 6.0f, 1.0f, kBorder);
				RenderText(overlay::util::ModuleTextW("Indicators.Clear"), clearBtn, 11.0f,
					   DWRITE_FONT_WEIGHT_NORMAL, kAccent, DWRITE_TEXT_ALIGNMENT_CENTER);
				m_settingsHits.push_back({clearBtn, SettingsAction::ClearIndicator, slot});

				cy += pathH + innerGap;
			}

			{
				RECT row = {innerLeft, cy, innerRight, cy + bottomH};
				const SettingsField tintField = static_cast<SettingsField>(
					static_cast<int>(SettingsField::IndicatorsTint0) + slot);

				RECT checkHit = {row.left, row.top, row.left + (row.right - row.left) / 2, row.bottom};
				int checkIdx = static_cast<int>(m_settingsHits.size());
				bool checkHover = (checkIdx == m_settingsHoverIndex);
				if (checkHover)
					DrawRoundedRectangle(checkHit, 6.0f, kRowHover);

				RECT br = {checkHit.left, row.top + (bottomH - box) / 2, checkHit.left + box,
					   row.top + (bottomH - box) / 2 + box};
				DrawRoundedRectangle(br, 5.0f, m_setIndicatorTint[slot] ? kAccent : kCtrlBg);
				DrawRoundedRectangleBorder(br, 5.0f, 1.0f, kBorder);
				if (m_setIndicatorTint[slot]) {
					RECT ck = {br.left + 5, br.top + 5, br.right - 5, br.bottom - 5};
					DrawRoundedRectangle(ck, 3.0f, kWhite);
				}
				RECT tintLabel = {br.right + 10, row.top, checkHit.right - 8, row.bottom};
				RenderText(overlay::util::ModuleTextW("Indicators.Recolor"), tintLabel, 12.0f,
					   DWRITE_FONT_WEIGHT_NORMAL, kWhite, DWRITE_TEXT_ALIGNMENT_LEADING);
				m_settingsHits.push_back(
					{checkHit, SettingsAction::ToggleBool, static_cast<int>(tintField)});

				RECT swatch = {row.right - sw, row.top + (bottomH - sw) / 2, row.right,
					       row.top + (bottomH - sw) / 2 + sw};
				RECT colorLabel = {checkHit.right + 8, row.top, swatch.left - 10, row.bottom};
				RenderText(overlay::util::ModuleTextW("Indicators.Color"), colorLabel, 12.0f,
					   DWRITE_FONT_WEIGHT_NORMAL, kWhite, DWRITE_TEXT_ALIGNMENT_TRAILING);

				const int rgb = m_setIndicatorColor[slot];
				D2D1_COLOR_F fill = D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f,
								 ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f,
								 m_setIndicatorTint[slot] ? 1.0f : 0.45f);
				int swatchIdx = static_cast<int>(m_settingsHits.size());
				bool swatchHover = (swatchIdx == m_settingsHoverIndex);
				DrawRoundedRectangle(swatch, 6.0f, fill);
				DrawRoundedRectangleBorder(swatch, 6.0f, 1.0f, swatchHover ? kAccent : kBorder);
				m_settingsHits.push_back({swatch, SettingsAction::PickIndicatorColor, slot});
			}

			y += cardH + 14;
		}
		break;
	case SettingsPage::Audio:
		for (int snd = 0; snd < INDICATOR_SOUND_EVENT_COUNT; snd++) {
			const int h = 58;
			RECT r = {contentLeft, y, contentRight, y + h};
			DrawRoundedRectangle(r, 8.0f, kRowBg);
			DrawRoundedRectangleBorder(r, 8.0f, 1.0f, kBorder);
			RECT lr = {r.left + 12, r.top + 6, r.right - 12, r.top + 24};
			RenderText(overlay::util::ModuleTextW(kIndicatorSoundLabelKeys[snd]), lr, 12.0f,
				   DWRITE_FONT_WEIGHT_NORMAL, kWhite, DWRITE_TEXT_ALIGNMENT_LEADING);

			const int btnW = 90;
			const int btnH = 26;
			const int by = r.bottom - 8 - btnH;
			RECT clearBtn = {r.right - 12 - btnW, by, r.right - 12, by + btnH};
			RECT browse = {clearBtn.left - 8 - btnW, by, clearBtn.left - 8, by + btnH};
			RECT pathRect = {r.left + 12, by, browse.left - 8, by + btnH};
			DrawRoundedRectangle(pathRect, 6.0f, kCtrlBg);
			RECT pTextRect = {pathRect.left + 8, pathRect.top, pathRect.right - 8, pathRect.bottom};
			const bool empty = m_setIndicatorSoundPath[snd].empty();
			std::wstring p = empty ? overlay::util::ModuleTextW("Indicators.SoundPlaceholder")
					       : IndicatorIconFileNameOnly(m_setIndicatorSoundPath[snd]);
			RenderText(p, pTextRect, 11.0f, DWRITE_FONT_WEIGHT_NORMAL, empty ? kDim : kWhite,
				   DWRITE_TEXT_ALIGNMENT_LEADING);

			int browseIdx = static_cast<int>(m_settingsHits.size());
			bool browseHover = (browseIdx == m_settingsHoverIndex);
			DrawRoundedRectangle(browse, 6.0f, browseHover ? kRowHover : kCtrlBg);
			DrawRoundedRectangleBorder(browse, 6.0f, 1.0f, kBorder);
			RenderText(overlay::util::ModuleTextW("Indicators.Browse"), browse, 11.0f,
				   DWRITE_FONT_WEIGHT_NORMAL, kAccent, DWRITE_TEXT_ALIGNMENT_CENTER);
			m_settingsHits.push_back({browse, SettingsAction::BrowseSound, snd});

			int clearIdx = static_cast<int>(m_settingsHits.size());
			bool clearHover = (clearIdx == m_settingsHoverIndex);
			DrawRoundedRectangle(clearBtn, 6.0f, clearHover ? kRowHover : kCtrlBg);
			DrawRoundedRectangleBorder(clearBtn, 6.0f, 1.0f, kBorder);
			RenderText(overlay::util::ModuleTextW("Indicators.Clear"), clearBtn, 11.0f,
				   DWRITE_FONT_WEIGHT_NORMAL, kAccent, DWRITE_TEXT_ALIGNMENT_CENTER);
			m_settingsHits.push_back({clearBtn, SettingsAction::ClearSound, snd});

			y += h + rowGap;
		}
		break;
	case SettingsPage::Replay:
		checkboxRow(overlay::util::ModuleTextW("RememberReplayBuffer"), m_setRememberReplayBuffer,
			    SettingsField::RememberReplayBuffer);
		checkboxRow(overlay::util::ModuleTextW("SmartReplay"), m_setSmartReplay, SettingsField::SmartReplay);
		if (m_setSmartReplay) {
			stepperRow(overlay::util::ModuleTextW("SmartReplay.Mode"),
				   overlay::util::ModuleTextW(kSettingsSmartReplayModeKeys[m_setSmartReplayMode]),
				   SettingsField::SmartReplayMode);
			if (m_setSmartReplayMode == 1) {
				checkboxRow(overlay::util::ModuleTextW("SmartReplay.ClipIgnoreTrim"),
					    m_setSmartReplayClipIgnoreTrim,
					    SettingsField::SmartReplayClipIgnoreTrim);
			}
		}
		break;
	case SettingsPage::ClipSave: {
		const bool bufferLocked = m_state.replayActive;
		intTextRow(overlay::util::ModuleTextW("ClipSave.BufferLength"),
			   std::to_wstring(m_setBufferLengthSec) + L" s" +
				   (bufferLocked ? overlay::util::ModuleTextW("ClipSave.BufferLocked") : L""),
			   SettingsField::ClipBufferLength, !bufferLocked);
		for (int i = 0; i < CLIP_SAVE_SLOT_COUNT; i++) {
			const std::wstring slotTitle =
				overlay::util::ModuleTextW("ClipSave.Slot") + L" " + std::to_wstring(i + 1);
			{
				const int h = 28;
				RECT r = {contentLeft, y, contentRight, y + h};
				RenderText(slotTitle, r, 12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, kDim,
					   DWRITE_TEXT_ALIGNMENT_LEADING);
				y += h + 4;
			}
			intTextRow(overlay::util::ModuleTextW("ClipSave.Duration"),
				   std::to_wstring(m_setClipSeconds[i]) + L" s",
				   static_cast<SettingsField>(static_cast<int>(SettingsField::ClipDuration0) + i));
			hotkeyRow(overlay::util::ModuleTextW("ClipSave.Hotkey"), kClipHotkeyBaseIndex + i);
			checkboxRow(overlay::util::ModuleTextW("ClipSave.ShowInOverlay"), m_setClipShow[i],
				    static_cast<SettingsField>(static_cast<int>(SettingsField::ClipShow0) + i));
		}
		break;
	}
	case SettingsPage::Notifications:
		checkboxRow(overlay::util::ModuleTextW("Notifications.Enable"), m_setToastsEnabled,
			    SettingsField::ToastsEnabled);
		checkboxRow(overlay::util::ModuleTextW("Notifications.VideoSaved"), m_setToastVideoSaved,
			    SettingsField::ToastVideoSaved);
		checkboxRow(overlay::util::ModuleTextW("Notifications.Export"), m_setToastExport,
			    SettingsField::ToastExport);
		checkboxRow(overlay::util::ModuleTextW("Notifications.OpenHint"), m_setToastOpenHint,
			    SettingsField::ToastOpenHint);
		checkboxRow(overlay::util::ModuleTextW("Notifications.LowDisk"), m_setToastLowDisk,
			    SettingsField::ToastLowDisk);
		stepperRow(overlay::util::ModuleTextW("Notifications.Duration"),
			   std::to_wstring(m_setToastDurationSec) + overlay::util::ModuleTextW("Notifications.Seconds"),
			   SettingsField::ToastDurationSec);
		stepperRow(overlay::util::ModuleTextW("Notifications.LowDiskThreshold"),
			   std::to_wstring(m_setToastLowDiskGb) + overlay::util::ModuleTextW("Notifications.GB"),
			   SettingsField::ToastLowDiskGb);
		break;
	case SettingsPage::Hotkeys: {
		auto sectionLabel = [&](const std::wstring &text) {
			const int h = 28;
			RECT r = {contentLeft, y, contentRight, y + h};
			RenderText(text, r, 12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, kDim, DWRITE_TEXT_ALIGNMENT_LEADING);
			y += h + 4;
		};
		sectionLabel(overlay::util::ModuleTextW("Hotkeys.Gallery"));
		for (int i = 0; i < kGalleryHotkeyCount; i++) {
			hotkeyRow(overlay::util::ModuleTextW(kSettingsHotkeyLabelKeys[i]), i);
		}
		sectionLabel(overlay::util::ModuleTextW("Hotkeys.Toasts"));
		stepperRow(overlay::util::ModuleTextW("Hotkeys.ToastScope"),
			   overlay::util::ModuleTextW(kSettingsToastHotkeysScopeKeys[m_setToastHotkeysScope]),
			   SettingsField::ToastHotkeysScope);
		hotkeyRow(overlay::util::ModuleTextW(kSettingsHotkeyLabelKeys[kToastViewHotkeyIndex]),
			  kToastViewHotkeyIndex);
		hotkeyRow(overlay::util::ModuleTextW(kSettingsHotkeyLabelKeys[kToastDeleteHotkeyIndex]),
			  kToastDeleteHotkeyIndex);
		break;
	}
	case SettingsPage::About: {
		const int blockH = std::max(280, (viewportHeight * 75) / 100);
		const int blockTop = viewTop + (viewportHeight - blockH) / 2;
		y = blockTop - m_settingsScrollOffset;

		const int logoSize = std::clamp(blockH * 42 / 100, 148, 240);
		const int nameH = 34;
		const int versionH = 26;
		const int iconBtn = std::clamp(blockH * 10 / 100, 48, 64);
		const int iconPad = std::max(10, iconBtn / 5);
		const int iconGap = 18;
		const int afterLogoGap = std::max(18, blockH * 4 / 100);
		const int afterNameGap = std::max(10, blockH * 2 / 100);
		const int afterVersionGap = std::max(22, blockH * 5 / 100);

		const int fixedH = logoSize + afterLogoGap + nameH + afterNameGap + versionH + afterVersionGap + iconBtn;
		const int topSlack = std::max(0, (blockH - fixedH) / 2);
		y += topSlack;

		if (EnsureAboutLogoLoaded() && m_aboutLogoBitmap) {
			const int logoLeft = contentLeft + ((contentRight - contentLeft) - logoSize) / 2;
			RECT logoRect = {logoLeft, y, logoLeft + logoSize, y + logoSize};
			D2D1_SIZE_F bmpSize = m_aboutLogoBitmap->GetSize();
			float srcAspect = bmpSize.width / (bmpSize.height > 0.0f ? bmpSize.height : 1.0f);
			float drawW = static_cast<float>(logoSize);
			float drawH = static_cast<float>(logoSize);
			if (srcAspect > 1.0f)
				drawH = drawW / srcAspect;
			else
				drawW = drawH * srcAspect;
			float dx = static_cast<float>(logoRect.left) + (logoSize - drawW) * 0.5f;
			float dy = static_cast<float>(logoRect.top) + (logoSize - drawH) * 0.5f;
			target->DrawBitmap(m_aboutLogoBitmap,
					   D2D1::RectF(dx, dy, dx + drawW, dy + drawH), 1.0f,
					   D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
			y += logoSize + afterLogoGap;
		} else {
			y += afterLogoGap;
		}

		RECT nameRect = {contentLeft, y, contentRight, y + nameH};
		RenderText(L"ntchff's Overlay", nameRect, 18.0f, DWRITE_FONT_WEIGHT_BOLD, kWhite,
			   DWRITE_TEXT_ALIGNMENT_CENTER);
		y += nameH + afterNameGap;

		std::wstring versionText = overlay::util::ModuleTextW("About.Version");
		versionText += L" ";
		versionText += overlay::util::Utf8ToWide(PLUGIN_VERSION);
		RECT versionRect = {contentLeft, y, contentRight, y + versionH};
		RenderText(versionText, versionRect, 13.0f, DWRITE_FONT_WEIGHT_NORMAL, kDim,
			   DWRITE_TEXT_ALIGNMENT_CENTER);
		y += versionH + afterVersionGap;

		EnsureAboutSocialIconsLoaded();
		const int socialCount = ABOUT_SOCIAL_COUNT;
		const int rowW = socialCount * iconBtn + (socialCount - 1) * iconGap;
		int x = contentLeft + ((contentRight - contentLeft) - rowW) / 2;
		static const wchar_t *const kSocialFallback[ABOUT_SOCIAL_COUNT] = {L"GH", L"TG", L"TW", L"DA"};
		for (int i = 0; i < socialCount; i++) {
			RECT r = {x, y, x + iconBtn, y + iconBtn};
			int idx = static_cast<int>(m_settingsHits.size());
			bool hover = (idx == m_settingsHoverIndex);
			DrawRoundedRectangle(r, 14.0f, hover ? kRowHover : kRowBg);
			DrawRoundedRectangleBorder(r, 14.0f, 1.0f, hover ? kAccent : kBorder);
			ID2D1Bitmap *iconBmp = m_aboutSocialBitmaps[i];
			if (iconBmp) {
				RECT iconRect = {r.left + iconPad, r.top + iconPad, r.right - iconPad,
						 r.bottom - iconPad};
				float il = static_cast<float>(iconRect.left);
				float it = static_cast<float>(iconRect.top);
				float ir = static_cast<float>(iconRect.right);
				float ib = static_cast<float>(iconRect.bottom);
				target->DrawBitmap(iconBmp, D2D1::RectF(il, it, ir, ib), hover ? 1.0f : 0.88f,
						   D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
			} else {
				RenderText(kSocialFallback[i], r, 12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
					   hover ? kAccent : kWhite, DWRITE_TEXT_ALIGNMENT_CENTER);
			}
			m_settingsHits.push_back({r, SettingsAction::OpenUrl, i});
			x += iconBtn + iconGap;
		}
		y = blockTop - m_settingsScrollOffset + blockH;
		break;
	}
	}

	m_settingsContentTotalHeight = (y + m_settingsScrollOffset) - viewTop;

	target->PopAxisAlignedClip();

	if (m_settingsContentTotalHeight > viewportHeight) {
		RECT trackRect = {shell.right - 10, viewTop, shell.right - 4, viewBottom};
		DrawRoundedRectangle(trackRect, 3.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.06f));
		float ratio = static_cast<float>(viewportHeight) / static_cast<float>(m_settingsContentTotalHeight);
		int thumbH = std::max(30, static_cast<int>(viewportHeight * ratio));
		int maxScroll = m_settingsContentTotalHeight - viewportHeight;
		float scrollRatio = maxScroll > 0 ? static_cast<float>(m_settingsScrollOffset) / maxScroll : 0.0f;
		int thumbTop = viewTop + static_cast<int>((viewportHeight - thumbH) * scrollRatio);
		RECT thumbRect = {trackRect.left, thumbTop, trackRect.right, thumbTop + thumbH};
		DrawRoundedRectangle(thumbRect, 3.0f, AccentSoft());
	}
}

int OverlayRenderer::SettingsHoverIndexAt(const POINT &pt) const
{
	for (size_t i = 0; i < m_settingsHits.size(); i++) {
		const SettingsHit &h = m_settingsHits[i];
		if (!IsPointInRect(pt, h.rect))
			continue;
		if (h.action != SettingsAction::Close && h.action != SettingsAction::Back) {
			if (h.rect.bottom <= m_settingsViewportTop || h.rect.top >= m_settingsViewportBottom)
				continue;
		}
		return static_cast<int>(i);
	}
	return -1;
}

void OverlayRenderer::HandleSettingsClick(int x, int y)
{
	POINT pt = ClientToLayoutPoint(x, y);
	int idx = SettingsHoverIndexAt(pt);
	if (idx < 0) {
		if (m_settingsEditingIntField >= 0) {
			CommitSettingsIntEdit();
			Render();
		}
		return;
	}
	const SettingsHit hit = m_settingsHits[static_cast<size_t>(idx)];

	if (m_settingsCapturingHotkeyIndex >= 0 && hit.action != SettingsAction::HotkeyCapture) {
		m_settingsCapturingHotkeyIndex = -1;
		m_windowManager.SetKeyboardCaptureActive(false);
	}
	if (m_settingsEditingIntField >= 0 &&
	    !(hit.action == SettingsAction::EditInt && hit.field == m_settingsEditingIntField)) {
		CommitSettingsIntEdit();
	}

	switch (hit.action) {
	case SettingsAction::Close:
		CloseSettings();
		return;
	case SettingsAction::Back:
		m_settingsPage = SettingsPage::Categories;
		m_settingsScrollOffset = 0;
		m_settingsHoverIndex = -1;
		m_settingsCapturingHotkeyIndex = -1;
		CancelSettingsIntEdit();
		m_windowManager.SetKeyboardCaptureActive(false);
		Render();
		return;
	case SettingsAction::OpenCategory: {
		SettingsPage page = static_cast<SettingsPage>(hit.field);
		if (page == SettingsPage::Categories)
			return;
		m_settingsPage = page;
		m_settingsScrollOffset = 0;
		m_settingsHoverIndex = -1;
		m_settingsCapturingHotkeyIndex = -1;
		CancelSettingsIntEdit();
		m_windowManager.SetKeyboardCaptureActive(false);
		Render();
		return;
	}
	case SettingsAction::OpenUrl: {
		if (hit.field < 0 || hit.field >= static_cast<int>(sizeof(kAboutUrls) / sizeof(kAboutUrls[0])))
			return;
		ShellExecuteW(NULL, L"open", kAboutUrls[hit.field], NULL, NULL, SW_SHOWDEFAULT);
		return;
	}
	case SettingsAction::Browse: {
		std::wstring picked;
		if (BrowseForExportFolderNative(m_windowManager.GetHWND(), picked)) {
			m_setExportPath = picked;
			PersistSettingsWorkingValues();
		}
		Render();
		return;
	}
	case SettingsAction::BrowseIndicator: {
		const int slot = hit.field;
		if (slot < 0 || slot >= INDICATOR_CUSTOM_SLOT_COUNT)
			return;
		std::wstring picked;
		if (BrowseForIndicatorIconNative(m_windowManager.GetHWND(), picked)) {
			std::wstring stored;
			if (CopyIndicatorIconIntoConfig(slot, picked, stored)) {
				m_setIndicatorIconPath[slot] = stored;
				PersistSettingsWorkingValues();
			}
		}
		Render();
		return;
	}
	case SettingsAction::ClearIndicator: {
		const int slot = hit.field;
		if (slot < 0 || slot >= INDICATOR_CUSTOM_SLOT_COUNT)
			return;
		DeleteStoredIndicatorIcons(slot);
		m_setIndicatorIconPath[slot].clear();
		PersistSettingsWorkingValues();
		Render();
		return;
	}
	case SettingsAction::BrowseSound: {
		const int eventIndex = hit.field;
		if (eventIndex < 0 || eventIndex >= INDICATOR_SOUND_EVENT_COUNT)
			return;
		std::wstring picked;
		if (BrowseForIndicatorSoundNative(m_windowManager.GetHWND(), picked)) {
			std::wstring stored;
			if (CopyIndicatorSoundIntoConfig(eventIndex, picked, stored)) {
				m_setIndicatorSoundPath[eventIndex] = stored;
				PersistSettingsWorkingValues();
			}
		}
		Render();
		return;
	}
	case SettingsAction::ClearSound: {
		const int eventIndex = hit.field;
		if (eventIndex < 0 || eventIndex >= INDICATOR_SOUND_EVENT_COUNT)
			return;
		DeleteStoredIndicatorSound(eventIndex);
		m_setIndicatorSoundPath[eventIndex].clear();
		PersistSettingsWorkingValues();
		Render();
		return;
	}
	case SettingsAction::PickIndicatorColor: {
		const int slot = hit.field;
		if (slot < 0 || slot >= INDICATOR_CUSTOM_SLOT_COUNT)
			return;
		int rgb = m_setIndicatorColor[slot];
		if (PickIndicatorColorNative(m_windowManager.GetHWND(), rgb)) {
			m_setIndicatorColor[slot] = rgb & 0xFFFFFF;
			PersistSettingsWorkingValues();
		}
		Render();
		return;
	}
	case SettingsAction::PickAccentColor: {
		int rgb = m_setAccentColor;
		if (PickIndicatorColorNative(m_windowManager.GetHWND(), rgb)) {
			m_setAccentColor = rgb & 0xFFFFFF;
			m_accentColorRgb = m_setAccentColor;
			ApplyAccentBrushes();
			ApplyToastAccentColor();
			PersistSettingsWorkingValues();
		}
		Render();
		return;
	}
	case SettingsAction::HotkeyCapture:
		CancelSettingsIntEdit();
		m_settingsCapturingHotkeyIndex = hit.field;
		// Grab the keyboard even when focus capture is off, so the bind can be
		// recorded without the overlay holding window focus.
		m_windowManager.SetKeyboardCaptureActive(true);
		Render();
		return;
	case SettingsAction::ToggleBool: {
		switch (static_cast<SettingsField>(hit.field)) {
		case SettingsField::AutoHideEnabled:
			m_setAutoHideEnabled = !m_setAutoHideEnabled;
			break;
		case SettingsField::IndicatorsEnabled:
			m_setIndicatorsEnabled = !m_setIndicatorsEnabled;
#ifdef ENABLE_QT
			overlay_runtime_set_indicators(m_setIndicatorsEnabled, m_setIndicatorsPosition,
						       m_setIndicatorsOled, m_setIndicatorsScale);
#endif
			break;
		case SettingsField::IndicatorsOled:
			m_setIndicatorsOled = !m_setIndicatorsOled;
#ifdef ENABLE_QT
			overlay_runtime_set_indicators(m_setIndicatorsEnabled, m_setIndicatorsPosition,
						       m_setIndicatorsOled, m_setIndicatorsScale);
#endif
			break;
		case SettingsField::IndicatorsTint0:
		case SettingsField::IndicatorsTint1:
		case SettingsField::IndicatorsTint2:
		case SettingsField::IndicatorsTint3: {
			const int slot = static_cast<int>(static_cast<SettingsField>(hit.field)) -
					 static_cast<int>(SettingsField::IndicatorsTint0);
			if (slot >= 0 && slot < INDICATOR_CUSTOM_SLOT_COUNT)
				m_setIndicatorTint[slot] = !m_setIndicatorTint[slot];
			break;
		}
		case SettingsField::RememberReplayBuffer:
			m_setRememberReplayBuffer = !m_setRememberReplayBuffer;
#ifdef ENABLE_QT
			overlay_runtime_set_remember_replay_buffer(m_setRememberReplayBuffer);
#endif
			break;
		case SettingsField::SmartReplay:
			m_setSmartReplay = !m_setSmartReplay;
#ifdef ENABLE_QT
			overlay_runtime_set_smart_replay(m_setSmartReplay);
#endif
			break;
		case SettingsField::SmartReplayClipIgnoreTrim:
			m_setSmartReplayClipIgnoreTrim = !m_setSmartReplayClipIgnoreTrim;
#ifdef ENABLE_QT
			overlay_runtime_set_smart_replay_clip_ignore_trim(m_setSmartReplayClipIgnoreTrim);
#endif
			break;
		case SettingsField::ClipShow0:
		case SettingsField::ClipShow1:
		case SettingsField::ClipShow2: {
			const int slot = static_cast<int>(static_cast<SettingsField>(hit.field)) -
					 static_cast<int>(SettingsField::ClipShow0);
			if (slot >= 0 && slot < CLIP_SAVE_SLOT_COUNT)
				m_setClipShow[slot] = !m_setClipShow[slot];
			break;
		}
		case SettingsField::GalleryInOverlay:
			m_setGalleryInOverlay = !m_setGalleryInOverlay;
			break;
		case SettingsField::ReturnHomeOnHide:
			m_setReturnHomeOnHide = !m_setReturnHomeOnHide;
			m_returnHomeOnHide = m_setReturnHomeOnHide;
			break;
		case SettingsField::CaptureFocus:
			m_setCaptureFocus = !m_setCaptureFocus;
			m_captureFocus = m_setCaptureFocus;
			m_windowManager.SetAllowActivate(m_setCaptureFocus);
			ApplySettingsWindowSize();
			break;
		case SettingsField::ToastsEnabled:
			m_setToastsEnabled = !m_setToastsEnabled;
			ApplyToastOptions();
			break;
		case SettingsField::ToastVideoSaved:
			m_setToastVideoSaved = !m_setToastVideoSaved;
			ApplyToastOptions();
			break;
		case SettingsField::ToastExport:
			m_setToastExport = !m_setToastExport;
			break;
		case SettingsField::ToastOpenHint:
			m_setToastOpenHint = !m_setToastOpenHint;
			ApplyToastOptions();
			break;
		case SettingsField::ToastLowDisk:
			m_setToastLowDisk = !m_setToastLowDisk;
			ApplyToastOptions();
			break;
		default:
			break;
		}
		PersistSettingsWorkingValues();
		Render();
		return;
	}
	case SettingsAction::StepDown:
	case SettingsAction::StepUp: {
		int dir = (hit.action == SettingsAction::StepUp) ? 1 : -1;
		switch (static_cast<SettingsField>(hit.field)) {
		case SettingsField::Position:
			m_setPosition = (m_setPosition + dir + 9) % 9;
			break;
		case SettingsField::Margin:
			m_setMargin = ClampInt(m_setMargin + dir * 5, 0, 200);
			break;
		case SettingsField::Orientation:
			m_setOrientation = (m_setOrientation + dir + 2) % 2;
			break;
		case SettingsField::MonitorMode:
			m_setMonitorMode = (m_setMonitorMode + dir + 3) % 3;
			if (m_settingsOpen)
				ApplySettingsWindowSize();
			break;
		case SettingsField::MonitorIndex: {
			const int monitorCount = std::max(1, m_layoutManager.GetMonitorCount());
			m_setMonitorIndex = (m_setMonitorIndex + dir + monitorCount) % monitorCount;
			if (m_settingsOpen)
				ApplySettingsWindowSize();
			break;
		}
		case SettingsField::Opacity:
			m_setOpacityPct = ClampInt(m_setOpacityPct + dir * 2, 50, 100);
			break;
		case SettingsField::OverlayScale:
			m_setOverlayScale = ClampInt(m_setOverlayScale + dir * 10, 50, 300);
			break;
		case SettingsField::TextScale:
			m_setTextScale = ClampInt(m_setTextScale + dir * 10, 50, 300);
			break;
		case SettingsField::AutoHideSeconds:
			m_setAutoHideSeconds = ClampInt(m_setAutoHideSeconds + dir, 1, 3600);
			break;
		case SettingsField::IndicatorsPosition:
			m_setIndicatorsPosition = (m_setIndicatorsPosition + dir + 9) % 9;
#ifdef ENABLE_QT
			overlay_runtime_set_indicators(m_setIndicatorsEnabled, m_setIndicatorsPosition,
						       m_setIndicatorsOled, m_setIndicatorsScale);
#endif
			break;
		case SettingsField::IndicatorsScale:
			m_setIndicatorsScale = ClampInt(m_setIndicatorsScale + dir * 10, 50, 300);
#ifdef ENABLE_QT
			overlay_runtime_set_indicators(m_setIndicatorsEnabled, m_setIndicatorsPosition,
						       m_setIndicatorsOled, m_setIndicatorsScale);
#endif
			break;
		case SettingsField::SmartReplayMode:
			m_setSmartReplayMode = (m_setSmartReplayMode + dir + 2) % 2;
#ifdef ENABLE_QT
			overlay_runtime_set_smart_replay_mode(m_setSmartReplayMode);
#endif
			break;
		case SettingsField::ToastLowDiskGb:
			m_setToastLowDiskGb = ClampInt(m_setToastLowDiskGb + dir, 1, 100);
			ApplyToastOptions();
			break;
		case SettingsField::ToastDurationSec:
			m_setToastDurationSec = ClampInt(m_setToastDurationSec + dir, 2, 60);
			ApplyToastOptions();
			break;
		case SettingsField::ExportMaxParallel:
			m_setExportMaxParallel = ClampInt(m_setExportMaxParallel + dir, 1, 4);
			EnsureExportInfrastructure();
			m_rebuildPresets.maxParallel = m_setExportMaxParallel;
			m_exportQueue.SetMaxParallel(m_setExportMaxParallel);
			PersistRebuildPresets();
			break;
		case SettingsField::ToastHotkeysScope:
			m_setToastHotkeysScope = (m_setToastHotkeysScope + dir + 2) % 2;
			ApplyToastOptions();
			break;
		default:
			break;
		}
		PersistSettingsWorkingValues();
		Render();
		return;
	}
	case SettingsAction::EditInt: {
		const auto field = static_cast<SettingsField>(hit.field);
		if (field == SettingsField::ClipBufferLength && m_state.replayActive)
			return;
		if (m_settingsEditingIntField == static_cast<int>(field))
			return;
		BeginSettingsIntEdit(field);
		Render();
		return;
	}
	}
}

void OverlayRenderer::BeginSettingsIntEdit(SettingsField field)
{
	int value = 0;
	switch (field) {
	case SettingsField::ClipBufferLength:
		value = m_setBufferLengthSec;
		break;
	case SettingsField::ClipDuration0:
	case SettingsField::ClipDuration1:
	case SettingsField::ClipDuration2: {
		const int slot = static_cast<int>(field) - static_cast<int>(SettingsField::ClipDuration0);
		if (slot < 0 || slot >= CLIP_SAVE_SLOT_COUNT)
			return;
		value = m_setClipSeconds[slot];
		break;
	}
	default:
		return;
	}

	m_settingsCapturingHotkeyIndex = -1;
	m_settingsEditingIntField = static_cast<int>(field);
	m_settingsEditText = std::to_wstring(value < 0 ? 0 : value);
	m_windowManager.SetKeyboardCaptureActive(true);
}

void OverlayRenderer::CancelSettingsIntEdit()
{
	m_settingsEditingIntField = -1;
	m_settingsEditText.clear();
}

bool OverlayRenderer::CommitSettingsIntEdit()
{
	if (m_settingsEditingIntField < 0)
		return false;

	const auto field = static_cast<SettingsField>(m_settingsEditingIntField);
	const std::wstring text = m_settingsEditText;
	CancelSettingsIntEdit();
	if (m_settingsCapturingHotkeyIndex < 0)
		m_windowManager.SetKeyboardCaptureActive(false);

	const wchar_t *p = text.c_str();
	while (*p == L' ' || *p == L'\t')
		++p;
	if (*p == L'\0')
		return false;

	wchar_t *end = nullptr;
	const unsigned long parsed = wcstoul(p, &end, 10);
	if (end == p)
		return false;
	while (*end == L' ' || *end == L'\t')
		++end;
	if (*end != L'\0')
		return false;

	int value = static_cast<int>(
		(std::min)(parsed, static_cast<unsigned long>(OVERLAY_CLIP_SAVE_MAX_SECONDS)));
	value = ClampInt(value, OVERLAY_CLIP_SAVE_MIN_SECONDS, OVERLAY_CLIP_SAVE_MAX_SECONDS);

	switch (field) {
	case SettingsField::ClipBufferLength:
		if (m_state.replayActive)
			return false;
		m_setBufferLengthSec = value;
		overlay_clip_save_set_buffer_length(m_setBufferLengthSec);
		break;
	case SettingsField::ClipDuration0:
	case SettingsField::ClipDuration1:
	case SettingsField::ClipDuration2: {
		const int slot = static_cast<int>(field) - static_cast<int>(SettingsField::ClipDuration0);
		if (slot < 0 || slot >= CLIP_SAVE_SLOT_COUNT)
			return false;
		m_setClipSeconds[slot] = value;
		break;
	}
	default:
		return false;
	}

	PersistSettingsWorkingValues();
	return true;
}

void OverlayRenderer::HandleSettingsKey(int vkCode, int mods)
{
	if (!m_settingsOpen)
		return;

	if (m_settingsEditingIntField >= 0) {
		UNUSED_PARAMETER(mods);
		if (vkCode == VK_ESCAPE) {
			CancelSettingsIntEdit();
			m_windowManager.SetKeyboardCaptureActive(false);
			Render();
			return;
		}
		if (vkCode == VK_RETURN) {
			CommitSettingsIntEdit();
			Render();
			return;
		}
		if (vkCode == VK_BACK) {
			if (!m_settingsEditText.empty())
				m_settingsEditText.pop_back();
			Render();
			return;
		}
		if (vkCode == VK_DELETE) {
			m_settingsEditText.clear();
			Render();
			return;
		}

		int digit = -1;
		if (vkCode >= '0' && vkCode <= '9')
			digit = vkCode - '0';
		else if (vkCode >= VK_NUMPAD0 && vkCode <= VK_NUMPAD9)
			digit = vkCode - VK_NUMPAD0;
		if (digit >= 0) {
			if (m_settingsEditText.size() < 5)
				m_settingsEditText.push_back(static_cast<wchar_t>(L'0' + digit));
			Render();
		}
		return;
	}

	if (m_settingsCapturingHotkeyIndex >= 0) {
		// Ignore standalone modifier keys; wait for a real key.
		if (vkCode == VK_SHIFT || vkCode == VK_CONTROL || vkCode == VK_MENU || vkCode == VK_LWIN ||
		    vkCode == VK_RWIN || (vkCode >= VK_LSHIFT && vkCode <= VK_RMENU) || vkCode == VK_CAPITAL)
			return;
		if (vkCode == VK_ESCAPE) {
			m_settingsCapturingHotkeyIndex = -1;
			m_windowManager.SetKeyboardCaptureActive(false);
			Render();
			return;
		}
		int index = m_settingsCapturingHotkeyIndex;
		if (index >= 0 && index < SETTINGS_HOTKEY_COUNT) {
			m_setHotkeyVk[index] = vkCode;
			m_setHotkeyMod[index] = mods;
		}
		m_settingsCapturingHotkeyIndex = -1;
		m_windowManager.SetKeyboardCaptureActive(false);
		PersistSettingsWorkingValues();
		Render();
		return;
	}

	if (vkCode == VK_ESCAPE) {
		if (m_settingsPage != SettingsPage::Categories) {
			m_settingsPage = SettingsPage::Categories;
			m_settingsScrollOffset = 0;
			m_settingsHoverIndex = -1;
			Render();
			return;
		}
		CloseSettings();
		return;
	}
}

void OverlayRenderer::HandleSettingsWheel(int delta)
{
	if (!m_settingsOpen)
		return;
	int viewportHeight = m_settingsViewportBottom - m_settingsViewportTop;
	int maxScroll = std::max(0, m_settingsContentTotalHeight - viewportHeight);
	int step = (delta / WHEEL_DELTA) * 48;
	if (step == 0)
		step = (delta > 0 ? 48 : -48);
	m_settingsScrollOffset -= step;
	if (m_settingsScrollOffset < 0)
		m_settingsScrollOffset = 0;
	if (m_settingsScrollOffset > maxScroll)
		m_settingsScrollOffset = maxScroll;
	Render();
}

void OverlayRenderer::CloseGalleryContextMenu()
{
	m_galleryContextMenuOpen = false;
	m_galleryContextMenuIndex = -1;
	m_galleryContextMenuHoverIndex = -1;
	m_galleryContextMenuItemRects.clear();
	ZeroMemory(&m_galleryContextMenuRect, sizeof(m_galleryContextMenuRect));
}

void OverlayRenderer::RunPendingOpenWith()
{
	HWND hwnd = m_windowManager.GetHWND();
	if (hwnd) {
		KillTimer(hwnd, kOpenWithTimerId);
	}
	if (m_pendingOpenWithPath.empty()) {
		return;
	}
	std::wstring path = m_pendingOpenWithPath;
	m_pendingOpenWithPath.clear();
	OpenWithDialog(nullptr, path);
}

void OverlayRenderer::RenderGalleryContextMenu(int galleryWidth, int galleryHeight)
{
	m_galleryContextMenuItemRects.clear();
	if (!m_galleryContextMenuOpen) {
		ZeroMemory(&m_galleryContextMenuRect, sizeof(m_galleryContextMenuRect));
		return;
	}

	const int actionCount = static_cast<int>(GalleryContextAction::Count);
	const int pad = 4;
	const int optH = 26;
	const int gap = 2;
	const int menuW = 210;
	const int menuH = pad * 2 + actionCount * (optH + gap) - gap;

	int left = m_galleryContextMenuAnchor.x;
	int top = m_galleryContextMenuAnchor.y;
	if (left + menuW > galleryWidth - 8) {
		left = galleryWidth - 8 - menuW;
	}
	if (top + menuH > galleryHeight - 8) {
		top = galleryHeight - 8 - menuH;
	}
	if (left < 8) {
		left = 8;
	}
	if (top < 8) {
		top = 8;
	}

	RECT panel = {left, top, left + menuW, top + menuH};
	m_galleryContextMenuRect = panel;
	DrawRoundedRectangle(panel, 8.0f, AccentPanel());
	DrawRoundedRectangleBorder(panel, 8.0f, 1.0f,
				   D2D1::ColorF(58.0f / 255.0f, 58.0f / 255.0f, 69.0f / 255.0f, 1.0f));

	for (int i = 0; i < actionCount; ++i) {
		int rowTop = top + pad + i * (optH + gap);
		RECT optRect = {left + pad, rowTop, left + menuW - pad, rowTop + optH};
		m_galleryContextMenuItemRects.push_back(optRect);
		const bool hovered = (i == m_galleryContextMenuHoverIndex);
		DrawRoundedRectangle(optRect, 6.0f, hovered ? AccentSelected() : AccentHover(0.40f));
		RECT textRect = {optRect.left + 10, optRect.top, optRect.right - 6, optRect.bottom};
		RenderText(overlay::util::ModuleTextW(
				   GalleryContextActionLocaleKey(static_cast<GalleryContextAction>(i))),
			   textRect, 9.5f, DWRITE_FONT_WEIGHT_NORMAL, D2D1::ColorF(0.95f, 0.95f, 0.95f),
			   DWRITE_TEXT_ALIGNMENT_LEADING);
	}
}

int OverlayRenderer::GalleryListItemIndexAt(const POINT &pt) const
{
	if (!IsPointInRect(pt, m_galleryListRect)) {
		return -1;
	}
	int itemsAreaBottom = m_galleryListRect.bottom - 34;
	if (pt.y >= itemsAreaBottom) {
		return -1;
	}
	int itemsAreaHeight = itemsAreaBottom - m_galleryListRect.top;
	int itemHeight = 52;
	int itemsPerPage = std::max(1, itemsAreaHeight / itemHeight);
	int startIndex = m_galleryPage * itemsPerPage;
	int localY = pt.y - (m_galleryListRect.top + 8);
	if (localY < 0) {
		return -1;
	}
	int indexInPage = localY / itemHeight;
	int itemIndex = startIndex + indexInPage;
	if (!m_gallery.GetItem(itemIndex)) {
		return -1;
	}
	return itemIndex;
}

bool OverlayRenderer::DeleteGallerySelectedVideo()
{
	const GalleryItem *item = m_gallery.GetItem(m_gallerySelectedIndex);
	if (!item || item->type != GalleryItem::Type::Video) {
		return false;
	}
	EnsureExportInfrastructure();
	if (m_exportQueue.IsPathLocked(item->path)) {
		MessageBoxW(m_windowManager.GetHWND(), overlay::util::ModuleTextW("Export.FileBusy").c_str(),
			    overlay::util::ModuleTextW("DeleteFile").c_str(), MB_OK | MB_ICONWARNING);
		return false;
	}
	int ret = MessageBoxW(m_windowManager.GetHWND(), item->name.c_str(),
			      overlay::util::ModuleTextW("DeleteFile").c_str(), MB_YESNO | MB_ICONQUESTION);
	if (ret != IDYES) {
		return false;
	}
	std::wstring pathToDelete = item->path;
	m_videoPlayer.Close();
	if (!DeleteFileW(pathToDelete.c_str())) {
		return false;
	}
	m_gallery.ReloadCurrent();
	int count = static_cast<int>(m_gallery.GetItems().size());
	if (count == 0) {
		m_gallerySelectedIndex = -1;
	} else {
		m_gallerySelectedIndex =
			(m_gallerySelectedIndex >= count) ? count - 1 : m_gallerySelectedIndex;
		const GalleryItem *nextItem = m_gallery.GetItem(m_gallerySelectedIndex);
		if (nextItem && nextItem->type == GalleryItem::Type::Video) {
			m_videoPlayer.Open(nextItem->path);
			m_galleryTrimIn = 0.0;
			m_galleryTrimOut = m_videoPlayer.GetDuration();
			m_galleryTrimUserSet = false;
		} else {
			m_gallerySelectedIndex = -1;
		}
	}
	return true;
}

bool OverlayRenderer::RenameGalleryContextVideo()
{
	const GalleryItem *item = m_gallery.GetItem(m_galleryContextMenuIndex);
	if (!item || item->type != GalleryItem::Type::Video) {
		return false;
	}
	EnsureExportInfrastructure();
	if (m_exportQueue.IsPathLocked(item->path)) {
		MessageBoxW(m_windowManager.GetHWND(), overlay::util::ModuleTextW("Export.FileBusy").c_str(),
			    overlay::util::ModuleTextW("Gallery.Ctx.Rename").c_str(), MB_OK | MB_ICONWARNING);
		return false;
	}

	std::wstring newName = item->name;
	HWND hwnd = m_windowManager.GetHWND();
	if (!PromptRenameFileName(hwnd, newName)) {
		return false;
	}

	// Trim whitespace
	while (!newName.empty() && (newName.front() == L' ' || newName.front() == L'\t')) {
		newName.erase(newName.begin());
	}
	while (!newName.empty() && (newName.back() == L' ' || newName.back() == L'\t')) {
		newName.pop_back();
	}

	if (!IsValidFileName(newName)) {
		MessageBoxW(hwnd, overlay::util::ModuleTextW("Gallery.Ctx.RenameInvalid").c_str(),
			    overlay::util::ModuleTextW("Gallery.Ctx.Rename").c_str(), MB_OK | MB_ICONWARNING);
		return false;
	}
	if (newName == item->name) {
		return true;
	}

	const size_t slash = item->path.find_last_of(L"\\/");
	if (slash == std::wstring::npos) {
		return false;
	}
	const std::wstring newPath = item->path.substr(0, slash + 1) + newName;
	if (GetFileAttributesW(newPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
		MessageBoxW(hwnd, overlay::util::ModuleTextW("Gallery.Ctx.RenameExists").c_str(),
			    overlay::util::ModuleTextW("Gallery.Ctx.Rename").c_str(), MB_OK | MB_ICONWARNING);
		return false;
	}

	const bool wasOpen = (m_gallerySelectedIndex == m_galleryContextMenuIndex);
	const std::wstring oldPath = item->path;
	if (wasOpen) {
		m_videoPlayer.Close();
	}

	if (!MoveFileW(oldPath.c_str(), newPath.c_str())) {
		if (wasOpen) {
			m_videoPlayer.Open(oldPath);
			m_galleryTrimIn = 0.0;
			m_galleryTrimOut = m_videoPlayer.GetDuration();
			m_galleryTrimUserSet = false;
		}
		MessageBoxW(hwnd, overlay::util::ModuleTextW("Gallery.Ctx.RenameFailed").c_str(),
			    overlay::util::ModuleTextW("Gallery.Ctx.Rename").c_str(), MB_OK | MB_ICONERROR);
		return false;
	}

	m_gallery.ReloadCurrent();
	int found = -1;
	const std::vector<GalleryItem> &items = m_gallery.GetItems();
	for (size_t i = 0; i < items.size(); ++i) {
		if (items[i].type == GalleryItem::Type::Video && items[i].path == newPath) {
			found = static_cast<int>(i);
			break;
		}
	}
	m_gallerySelectedIndex = found;
	m_galleryContextMenuIndex = found;
	if (found >= 0 && wasOpen) {
		m_videoPlayer.Open(newPath);
		m_galleryTrimIn = 0.0;
		m_galleryTrimOut = m_videoPlayer.GetDuration();
		m_galleryTrimUserSet = false;
	}
	return true;
}

bool OverlayRenderer::ExecuteGalleryContextAction(int actionIndex)
{
	const GalleryItem *item = m_gallery.GetItem(m_galleryContextMenuIndex);
	if (!item || item->type != GalleryItem::Type::Video) {
		return false;
	}
	HWND hwnd = m_windowManager.GetHWND();
	switch (static_cast<GalleryContextAction>(actionIndex)) {
	case GalleryContextAction::OpenFolder:
		ShowFileInExplorer(item->path);
		Hide();
		return true;
	case GalleryContextAction::OpenWith: {
		// Wait out the click (LBUTTONUP); PostMessage is too early and the release dismisses the dialog.
		m_pendingOpenWithPath = item->path;
		ReleaseCapture();
		Hide();
		if (hwnd) {
			s_timerInstance = this;
			KillTimer(hwnd, kOpenWithTimerId);
			SetTimer(hwnd, kOpenWithTimerId, kOpenWithDelayMs, TimerProc);
		}
		return true;
	}
	case GalleryContextAction::Copy:
		return CopyFileToClipboard(hwnd, item->path, false);
	case GalleryContextAction::CopyPath:
		return CopyPathToClipboard(hwnd, item->path);
	case GalleryContextAction::Rename:
		return RenameGalleryContextVideo();
	case GalleryContextAction::Delete:
		m_gallerySelectedIndex = m_galleryContextMenuIndex;
		return DeleteGallerySelectedVideo();
	default:
		return false;
	}
}

void OverlayRenderer::HandleGalleryClick(int x, int y)
{
	POINT pt = ClientToLayoutPoint(x, y);
	EnsureExportInfrastructure();

	// Export queue panel interactions (non-blocking).
	if (IsPointInRect(pt, m_galleryQueueRect)) {
		if (IsPointInRect(pt, m_galleryQueueStartRect)) {
			if (m_exportQueue.IsBusy()) {
				m_exportQueue.CancelBatch();
				SyncExportQueueTimer();
				Render();
				return;
			}
			if (m_exportQueue.IsEmpty()) {
				if (m_setToastExport)
					overlay_runtime_show_toast(
						overlay::util::WideToUtf8(overlay::util::ModuleTextW("Export.QueueEmpty"))
							.c_str(),
						3000);
				Render();
				return;
			}
			// Shared export settings for the whole queue — ask once before Start.
			SyncExportQueueFolder();
			m_editingQueueJobId.clear();
			m_rebuildDraft = m_exportQueue.GetBatchPreset();
			if (m_rebuildDraft.id.empty())
				m_rebuildDraft = m_rebuildPresets.GetSelectedOrDefault();
			ApplyDraftTracksToExportUi();
			m_exportSettingsPendingStart = true;
			m_galleryAudioTrackSubmenuOpen = false;
			m_galleryExportSubmenuOpen = true;
			Render();
			return;
		}
		if (IsPointInRect(pt, m_galleryQueueClearRect)) {
			if (!m_exportQueue.IsEmpty()) {
				m_exportQueue.ClearAll();
				m_editingQueueJobId.clear();
				if (m_setToastExport)
					overlay_runtime_show_toast(
						overlay::util::WideToUtf8(overlay::util::ModuleTextW("Export.QueueCleared"))
							.c_str(),
						3000);
				Render();
			}
			return;
		}
		if (IsPointInRect(pt, m_galleryQueueOpenFolderRect)) {
			std::wstring folder;
			if (m_exportQueue.AllOutputsSameFolder(folder) && !folder.empty())
				ShellExecuteW(NULL, L"open", folder.c_str(), NULL, NULL, SW_SHOWDEFAULT);
			return;
		}
		for (size_t i = 0; i < m_queueItemIds.size(); i++) {
			const std::string &id = m_queueItemIds[i];
			if (i < m_queueUpRects.size() && IsPointInRect(pt, m_queueUpRects[i])) {
				m_exportQueue.Reorder(id, static_cast<int>(i) - 1);
				Render();
				return;
			}
			if (i < m_queueDownRects.size() && IsPointInRect(pt, m_queueDownRects[i])) {
				m_exportQueue.Reorder(id, static_cast<int>(i) + 1);
				Render();
				return;
			}
			if (i < m_queueCancelRects.size() && IsPointInRect(pt, m_queueCancelRects[i])) {
				m_exportQueue.CancelJob(id);
				Render();
				return;
			}
			if (i < m_queueRemoveRects.size() && IsPointInRect(pt, m_queueRemoveRects[i])) {
				if (m_editingQueueJobId == id)
					m_editingQueueJobId.clear();
				m_exportQueue.RemoveJob(id);
				Render();
				return;
			}
			if (i < m_queueRetryRects.size() && IsPointInRect(pt, m_queueRetryRects[i])) {
				m_exportQueue.Retry(id);
				Render();
				return;
			}
			if (i < m_queueRenameRects.size() && IsPointInRect(pt, m_queueRenameRects[i])) {
				for (const auto &job : m_exportQueue.GetJobs()) {
					if (job.id != id || job.status == ExportJobStatus::Running)
						continue;
					HWND hwnd = m_windowManager.GetHWND();
					std::wstring newName = job.displayName;
					if (!PromptTextInput(hwnd, overlay::util::ModuleTextW("Export.Rename"), newName,
							     true))
						return;
					while (!newName.empty() && (newName.front() == L' ' || newName.front() == L'\t'))
						newName.erase(newName.begin());
					while (!newName.empty() && (newName.back() == L' ' || newName.back() == L'\t'))
						newName.pop_back();
					if (newName.empty()) {
						MessageBoxW(hwnd,
							    overlay::util::ModuleTextW("Gallery.Ctx.RenameInvalid").c_str(),
							    overlay::util::ModuleTextW("Export.Rename").c_str(),
							    MB_OK | MB_ICONWARNING);
						return;
					}
					const wchar_t *invalid = L"<>:\"/\\|?*";
					if (newName.find_first_of(invalid) != std::wstring::npos) {
						MessageBoxW(hwnd,
							    overlay::util::ModuleTextW("Gallery.Ctx.RenameInvalid").c_str(),
							    overlay::util::ModuleTextW("Export.Rename").c_str(),
							    MB_OK | MB_ICONWARNING);
						return;
					}
					m_exportQueue.RenameJob(id, newName);
					Render();
					return;
				}
				return;
			}
			if (i < m_queueOpenRects.size() && IsPointInRect(pt, m_queueOpenRects[i])) {
				for (const auto &job : m_exportQueue.GetJobs()) {
					if (job.id == id && !job.outputFolder.empty()) {
						ShellExecuteW(NULL, L"open", job.outputFolder.c_str(), NULL, NULL,
							      SW_SHOWDEFAULT);
						break;
					}
				}
				return;
			}
			if (i < m_queueEditRects.size() && IsPointInRect(pt, m_queueEditRects[i])) {
				for (const auto &job : m_exportQueue.GetJobs()) {
					if (job.id == id && job.status != ExportJobStatus::Running) {
						m_editingQueueJobId = id;
						m_rebuildDraft = job.preset;
						// Select source clip and restore its queued trim onto the timeline.
						if (!job.inputPath.empty()) {
							const auto &items = m_gallery.GetItems();
							int found = -1;
							for (size_t gi = 0; gi < items.size(); gi++) {
								if (items[gi].type == GalleryItem::Type::Video &&
								    _wcsicmp(items[gi].path.c_str(), job.inputPath.c_str()) ==
									    0) {
									found = static_cast<int>(gi);
									break;
								}
							}
							if (found >= 0)
								m_gallerySelectedIndex = found;
							const GalleryItem *sel = m_gallery.GetItem(m_gallerySelectedIndex);
							if (!sel || _wcsicmp(sel->path.c_str(), job.inputPath.c_str()) != 0)
								m_videoPlayer.Open(job.inputPath);
							else if (_wcsicmp(m_videoPlayer.GetOpenPath().c_str(),
									  job.inputPath.c_str()) != 0)
								m_videoPlayer.Open(job.inputPath);
						}
						const double mediaDur = m_videoPlayer.GetDuration();
						m_galleryTrimIn = std::max(0.0, job.startSeconds);
						if (job.endSeconds > job.startSeconds)
							m_galleryTrimOut = job.endSeconds;
						else
							m_galleryTrimOut = mediaDur > 0.0 ? mediaDur : (job.startSeconds + 1.0);
						if (mediaDur > 0.0 && m_galleryTrimOut > mediaDur)
							m_galleryTrimOut = mediaDur;
						m_galleryTrimUserSet = true;
						m_videoPlayer.SetTrimIn(m_galleryTrimIn);
						m_videoPlayer.SetTrimOut(m_galleryTrimOut);
						m_exportSettingsPendingStart = false;
						m_galleryExportSubmenuOpen = false;
						if (m_setToastExport)
							overlay_runtime_show_toast(
								overlay::util::WideToUtf8(
									overlay::util::ModuleTextW("Export.EditTrimHint"))
									.c_str(),
								3500);
						Render();
						return;
					}
				}
				return;
			}
		}
		return;
	}

	if (m_galleryContextMenuOpen) {
		if (IsPointInRect(pt, m_galleryContextMenuRect)) {
			for (size_t i = 0; i < m_galleryContextMenuItemRects.size(); ++i) {
				if (IsPointInRect(pt, m_galleryContextMenuItemRects[i])) {
					ExecuteGalleryContextAction(static_cast<int>(i));
					CloseGalleryContextMenu();
					if (IsVisible()) {
						Render();
					}
					return;
				}
			}
			return;
		}
		CloseGalleryContextMenu();
		Render();
		return;
	}

	if (IsPointInRect(pt, m_galleryFullscreenRect)) {
		m_galleryFullscreen = !m_galleryFullscreen;
		overlay_settings_ensure();
		obs_data_set_bool(saved_settings_data, "gallery_fullscreen", m_galleryFullscreen);
		overlay_settings_save();
		ApplyGalleryWindowSize();
		Render();
		return;
	}
	if (IsPointInRect(pt, m_galleryBackRect)) {
		CloseGallery();
		return;
	}
	if (IsPointInRect(pt, m_galleryFolderBackRect)) {
		if (m_gallery.GoUp()) {
			m_galleryPage = 0;
			m_gallerySelectedIndex = -1;
			m_videoPlayer.Close();
			m_galleryTrimIn = 0.0;
			m_galleryTrimOut = 0.0;
			m_galleryTrimUserSet = false;
			m_galleryExportSubmenuOpen = false;
			m_galleryAudioTrackSubmenuOpen = false;
			CloseGalleryContextMenu();
			Render();
		}
		return;
	}

	if (m_galleryExportSubmenuOpen) {
		if (IsPointInRect(pt, m_galleryExportPanelRect)) {
			// Queue Separate/Merge — independent of rebuild preset.
			if (IsPointInRect(pt, m_galleryQueueModeSeparateRect)) {
				m_exportQueue.SetMode(ExportQueueMode::Separate);
				Render();
				return;
			}
			if (IsPointInRect(pt, m_galleryQueueModeMergeRect)) {
				m_exportQueue.SetMode(ExportQueueMode::Merge);
				Render();
				return;
			}
			for (size_t i = 0; i < m_exportTrackKeepRects.size(); i++) {
				if (IsPointInRect(pt, m_exportTrackKeepRects[i]) && i < m_exportTrackKeep.size()) {
					m_exportTrackKeep[i] = !m_exportTrackKeep[i];
					CaptureExportTracksToDraft();
					Render();
					return;
				}
			}
			if (IsPointInRect(pt, m_galleryRebuildModeCopyRect)) {
				m_rebuildDraft.mode = RebuildMode::Copy;
				PersistRebuildPresets();
				Render();
				return;
			}
			if (IsPointInRect(pt, m_galleryRebuildModeReencodeRect)) {
				m_rebuildDraft.mode = RebuildMode::Reencode;
				PersistRebuildPresets();
				Render();
				return;
			}
			auto selectPresetDelta = [&](int delta) {
				if (m_rebuildPresets.presets.empty())
					return;
				int idx = 0;
				for (size_t i = 0; i < m_rebuildPresets.presets.size(); i++) {
					if (m_rebuildPresets.presets[i].id == m_rebuildDraft.id) {
						idx = static_cast<int>(i);
						break;
					}
				}
				idx = (idx + delta + static_cast<int>(m_rebuildPresets.presets.size())) %
				      static_cast<int>(m_rebuildPresets.presets.size());
				m_rebuildDraft = m_rebuildPresets.presets[static_cast<size_t>(idx)];
				m_rebuildPresets.selectedId = m_rebuildDraft.id;
				ApplyDraftTracksToExportUi();
				PersistRebuildPresets();
			};
			auto cycleFormat = [&](int delta) {
				int f = RebuildFormatToInt(m_rebuildDraft.format);
				f = (f + delta + 3) % 3;
				m_rebuildDraft.format = RebuildFormatFromInt(f);
				if (m_rebuildDraft.format == RebuildOutputFormat::Gif && m_rebuildDraft.fps <= 0.5)
					m_rebuildDraft.fps = 15.0;
				if (m_rebuildDraft.format == RebuildOutputFormat::Gif && m_rebuildDraft.width <= 0)
					m_rebuildDraft.width = 480;
			};
			auto cycleRes = [&](int delta) {
				struct Res {
					int w;
					int h;
				};
				static const Res kRes[] = {{0, 0}, {1920, 1080}, {1280, 720}, {854, 480}, {640, 360}};
				int idx = 0;
				for (int i = 0; i < 5; i++) {
					if (m_rebuildDraft.width == kRes[i].w) {
						idx = i;
						break;
					}
				}
				idx = (idx + delta + 5) % 5;
				m_rebuildDraft.width = kRes[idx].w;
				m_rebuildDraft.height = kRes[idx].h;
			};
			auto cycleQuality = [&](int delta) {
				if (m_rebuildDraft.format == RebuildOutputFormat::Gif) {
					static const int kColors[] = {32, 64, 96, 128, 160, 192, 224, 256};
					constexpr int n = 8;
					int idx = 0;
					for (int i = 0; i < n; i++) {
						if (m_rebuildDraft.gifColors == kColors[i]) {
							idx = i;
							break;
						}
					}
					idx = (idx + delta + n) % n;
					m_rebuildDraft.gifColors = kColors[idx];
					if (idx <= 1)
						m_rebuildDraft.quality = RebuildQuality::Low;
					else if (idx <= 3)
						m_rebuildDraft.quality = RebuildQuality::Medium;
					else if (idx <= 5)
						m_rebuildDraft.quality = RebuildQuality::High;
					else
						m_rebuildDraft.quality = RebuildQuality::VeryHigh;
					return;
				}
				static const int kCrf[] = {18, 20, 22, 23, 24, 26, 28, 30, 32, 35, 40};
				constexpr int n = 11;
				int idx = 6; // default near 28
				int bestDist = 1000;
				for (int i = 0; i < n; i++) {
					const int d = std::abs(m_rebuildDraft.Crf() - kCrf[i]);
					if (d < bestDist) {
						bestDist = d;
						idx = i;
					}
				}
				idx = (idx + delta + n) % n;
				m_rebuildDraft.SetCrf(kCrf[idx]);
			};
			auto cycleFps = [&](int delta) {
				static const double kFps[] = {0.0, 60.0, 30.0, 24.0, 15.0, 12.0};
				int idx = 0;
				for (int i = 0; i < 6; i++) {
					if (std::abs(m_rebuildDraft.fps - kFps[i]) < 0.1) {
						idx = i;
						break;
					}
				}
				idx = (idx + delta + 6) % 6;
				m_rebuildDraft.fps = kFps[idx];
			};
			if (IsPointInRect(pt, m_galleryPresetPrevRect)) {
				selectPresetDelta(-1);
				Render();
				return;
			}
			if (IsPointInRect(pt, m_galleryPresetNextRect)) {
				selectPresetDelta(1);
				Render();
				return;
			}
			if (IsPointInRect(pt, m_galleryPresetSaveRect)) {
				HWND hwnd = m_windowManager.GetHWND();
				std::wstring name = m_rebuildDraft.name;
				if (!PromptPresetName(hwnd, name)) {
					Render();
					return;
				}
				m_rebuildDraft.name = name;
				CaptureExportTracksToDraft();
				if (m_rebuildDraft.id.empty())
					m_rebuildDraft.id = m_rebuildPresets.GenerateId();
				m_rebuildPresets.Upsert(m_rebuildDraft);
				m_rebuildPresets.selectedId = m_rebuildDraft.id;
				PersistRebuildPresets();
				Render();
				return;
			}
			if (IsPointInRect(pt, m_galleryPresetSaveAsRect)) {
				HWND hwnd = m_windowManager.GetHWND();
				std::wstring name = m_rebuildDraft.name;
				if (!name.empty())
					name += L" copy";
				if (!PromptPresetName(hwnd, name)) {
					Render();
					return;
				}
				m_rebuildDraft.name = name;
				CaptureExportTracksToDraft();
				m_rebuildDraft.id = m_rebuildPresets.GenerateId();
				m_rebuildPresets.Upsert(m_rebuildDraft);
				m_rebuildPresets.selectedId = m_rebuildDraft.id;
				PersistRebuildPresets();
				Render();
				return;
			}
			if (IsPointInRect(pt, m_galleryPresetDeleteRect)) {
				if (!m_rebuildDraft.id.empty()) {
					m_rebuildPresets.Remove(m_rebuildDraft.id);
					m_rebuildDraft = m_rebuildPresets.GetSelectedOrDefault();
					PersistRebuildPresets();
				}
				Render();
				return;
			}
			if (IsPointInRect(pt, m_galleryFormatPrevRect)) {
				cycleFormat(-1);
				PersistRebuildPresets();
				Render();
				return;
			}
			if (IsPointInRect(pt, m_galleryFormatNextRect)) {
				cycleFormat(1);
				PersistRebuildPresets();
				Render();
				return;
			}
			if (IsPointInRect(pt, m_galleryResPrevRect)) {
				cycleRes(-1);
				PersistRebuildPresets();
				Render();
				return;
			}
			if (IsPointInRect(pt, m_galleryResNextRect)) {
				cycleRes(1);
				PersistRebuildPresets();
				Render();
				return;
			}
			if (IsPointInRect(pt, m_galleryBitratePrevRect)) {
				cycleQuality(-1);
				PersistRebuildPresets();
				Render();
				return;
			}
			if (IsPointInRect(pt, m_galleryBitrateNextRect)) {
				cycleQuality(1);
				PersistRebuildPresets();
				Render();
				return;
			}
			if (IsPointInRect(pt, m_galleryFpsPrevRect)) {
				cycleFps(-1);
				PersistRebuildPresets();
				Render();
				return;
			}
			if (IsPointInRect(pt, m_galleryFpsNextRect)) {
				cycleFps(1);
				PersistRebuildPresets();
				Render();
				return;
			}
			if (IsPointInRect(pt, m_galleryExportConfirmRect)) {
				CaptureExportTracksToDraft();
				if (m_exportTrackStreamIndices.empty())
					ApplyDraftTracksToExportUi();
				bool anyKept = false;
				for (size_t i = 0; i < m_exportTrackKeep.size(); i++) {
					if (m_exportTrackKeep[i]) {
						anyKept = true;
						break;
					}
				}
				if (!m_exportTrackStreamIndices.empty() && !anyKept) {
					if (m_setToastExport)
						overlay_runtime_show_toast("Select at least one track to export", 3500);
					return;
				}
				PersistRebuildPresets();
				m_exportQueue.SetBatchPreset(m_rebuildDraft);
				m_galleryExportSubmenuOpen = false;
				if (m_exportSettingsPendingStart) {
					m_exportSettingsPendingStart = false;
					SyncExportQueueFolder();
					m_exportQueue.SetMaxParallel(m_setExportMaxParallel);
					m_exportQueue.Start();
					SyncExportQueueTimer();
				}
				Render();
				return;
			}
			return;
		}

		m_galleryExportSubmenuOpen = false;
		m_exportSettingsPendingStart = false;
		Render();
		return;
	}

	if (IsPointInRect(pt, m_galleryMuteRect)) {
		m_galleryMuted = !m_galleryMuted;
		m_videoPlayer.SetMuted(m_galleryMuted);
		overlay_settings_ensure();
		obs_data_set_bool(saved_settings_data, "gallery_muted", m_galleryMuted);
		overlay_settings_save();
		Render();
		return;
	}

	if (IsPointInRect(pt, m_galleryVolumeRect)) {
		int width = m_galleryVolumeRect.right - m_galleryVolumeRect.left;
		if (width > 0) {
			int localX = pt.x - m_galleryVolumeRect.left;
			int volume = static_cast<int>((localX / static_cast<double>(width)) * 100.0);
			if (volume < 0)
				volume = 0;
			if (volume > 100)
				volume = 100;
			m_galleryVolume = volume;
			m_videoPlayer.SetVolume(static_cast<double>(m_galleryVolume) / 100.0);
			overlay_settings_ensure();
			obs_data_set_int(saved_settings_data, "gallery_volume", m_galleryVolume);
			overlay_settings_save();
			Render();
		}
		return;
	}

	if (m_videoPlayer.GetAudioTrackCount() > 1) {
		if (m_galleryAudioTrackSubmenuOpen) {
			if (IsPointInRect(pt, m_galleryAudioTrackMenuRect)) {
				for (size_t i = 0; i < m_galleryAudioTrackOptionRects.size(); i++) {
					if (IsPointInRect(pt, m_galleryAudioTrackOptionRects[i])) {
						m_videoPlayer.SetCurrentAudioTrack(static_cast<int>(i));
						m_galleryAudioTrackSubmenuOpen = false;
						Render();
						return;
					}
				}
				// Consume clicks on the audio menu background.
				return;
			}
			if (IsPointInRect(pt, m_galleryAudioTrackButtonRect)) {
				m_galleryAudioTrackSubmenuOpen = false;
				Render();
				return;
			}
			m_galleryAudioTrackSubmenuOpen = false;
			Render();
			return;
		} else if (IsPointInRect(pt, m_galleryAudioTrackButtonRect)) {
			CloseGalleryContextMenu();
			m_galleryAudioTrackSubmenuOpen = true;
			Render();
			return;
		}
	}

	if (IsPointInRect(pt, m_galleryTimelineRect)) {
		double duration = m_videoPlayer.GetDuration();
		if (duration > 0.0) {
			double ratio = static_cast<double>(pt.x - m_galleryTimelineRect.left) /
				       static_cast<double>(m_galleryTimelineRect.right - m_galleryTimelineRect.left);
			if (ratio < 0.0)
				ratio = 0.0;
			if (ratio > 1.0)
				ratio = 1.0;
			m_videoPlayer.Seek(duration * ratio);
			Render();
		}
		return;
	}

	int itemsAreaBottom = m_galleryListRect.bottom - 34;
	int itemsAreaHeight = itemsAreaBottom - m_galleryListRect.top;
	int itemHeight = 52;
	int itemsPerPage = std::max(1, itemsAreaHeight / itemHeight);
	int startIndex = m_galleryPage * itemsPerPage;

	if (IsPointInRect(pt, m_galleryPrevPageRect) && m_galleryPage > 0) {
		m_galleryPage--;
		CloseGalleryContextMenu();
		Render();
		return;
	}
	int maxPage = (static_cast<int>(m_gallery.GetItems().size()) - 1) / itemsPerPage;
	if (IsPointInRect(pt, m_galleryNextPageRect) && m_galleryPage < maxPage) {
		m_galleryPage++;
		CloseGalleryContextMenu();
		Render();
		return;
	}

	if (IsPointInRect(pt, m_galleryListRect) && pt.y < itemsAreaBottom) {
		int localY = pt.y - (m_galleryListRect.top + 8);
		if (localY < 0) {
			return;
		}
		int indexInPage = localY / itemHeight;
		int itemIndex = startIndex + indexInPage;
		const GalleryItem *item = m_gallery.GetItem(itemIndex);
		if (item) {
			CloseGalleryContextMenu();
			if (item->type == GalleryItem::Type::Folder) {
				if (m_gallery.EnterFolder(itemIndex)) {
					m_galleryPage = 0;
					m_gallerySelectedIndex = -1;
					m_videoPlayer.Close();
					m_galleryTrimIn = 0.0;
					m_galleryTrimOut = 0.0;
					m_galleryTrimUserSet = false;
					m_galleryExportSubmenuOpen = false;
					m_galleryAudioTrackSubmenuOpen = false;
				}
			} else {
				if (m_gallerySelectedIndex == itemIndex) {
					Render();
					return;
				}
				m_editingQueueJobId.clear();
				m_gallerySelectedIndex = itemIndex;
				m_videoPlayer.Open(item->path);
				m_galleryTrimIn = 0.0;
				m_galleryTrimOut = m_videoPlayer.GetDuration();
				m_galleryTrimUserSet = false;
				ApplyGalleryWindowSize();
			}
			Render();
		}
		return;
	}

	if (IsPointInRect(pt, m_galleryPlayRect)) {
		if (m_videoPlayer.IsDurationScanActive()) {
			Render();
			return;
		}
		m_videoPlayer.TogglePlaying();
		Render();
		return;
	}

	if (IsPointInRect(pt, m_gallerySetInRect)) {
		m_galleryTrimIn = m_videoPlayer.GetCurrentTime();
		m_videoPlayer.SetTrimIn(m_galleryTrimIn);
		m_galleryTrimUserSet = true;
		if (!m_editingQueueJobId.empty()) {
			for (const auto &job : m_exportQueue.GetJobs()) {
				if (job.id != m_editingQueueJobId)
					continue;
				ExportQueueJob updated = job;
				updated.startSeconds = m_galleryTrimIn;
				updated.endSeconds = m_galleryTrimOut;
				if (updated.endSeconds <= updated.startSeconds)
					updated.endSeconds = updated.startSeconds + 1.0;
				const double mediaDur = m_videoPlayer.GetDuration();
				if (updated.startSeconds <= 0.001 && mediaDur > 0.0 &&
				    updated.endSeconds >= mediaDur - 0.15)
					updated.endSeconds = 0.0;
				m_exportQueue.UpdateJob(m_editingQueueJobId, updated);
				break;
			}
		}
		Render();
		return;
	}

	if (IsPointInRect(pt, m_gallerySetOutRect)) {
		m_galleryTrimOut = m_videoPlayer.GetCurrentTime();
		m_videoPlayer.SetTrimOut(m_galleryTrimOut);
		m_galleryTrimUserSet = true;
		if (!m_editingQueueJobId.empty()) {
			for (const auto &job : m_exportQueue.GetJobs()) {
				if (job.id != m_editingQueueJobId)
					continue;
				ExportQueueJob updated = job;
				updated.startSeconds = m_galleryTrimIn;
				updated.endSeconds = m_galleryTrimOut;
				if (updated.endSeconds <= updated.startSeconds)
					updated.endSeconds = updated.startSeconds + 1.0;
				const double mediaDur = m_videoPlayer.GetDuration();
				if (updated.startSeconds <= 0.001 && mediaDur > 0.0 &&
				    updated.endSeconds >= mediaDur - 0.15)
					updated.endSeconds = 0.0;
				m_exportQueue.UpdateJob(m_editingQueueJobId, updated);
				break;
			}
		}
		Render();
		return;
	}

	if (IsPointInRect(pt, m_galleryTrimRect)) {
		const GalleryItem *selected = m_gallery.GetItem(m_gallerySelectedIndex);
		if (!selected || selected->type != GalleryItem::Type::Video) {
			return;
		}
		CloseGalleryContextMenu();
		m_galleryAudioTrackSubmenuOpen = false;
		m_exportSettingsPendingStart = false;
		m_galleryExportSubmenuOpen = false;

		if (!m_editingQueueJobId.empty()) {
			bool updatedOk = false;
			for (const auto &job : m_exportQueue.GetJobs()) {
				if (job.id != m_editingQueueJobId)
					continue;
				ExportQueueJob updated = job;
				updated.startSeconds = m_galleryTrimIn;
				updated.endSeconds = m_galleryTrimOut;
				if (updated.endSeconds <= updated.startSeconds)
					updated.endSeconds = updated.startSeconds + 1.0;
				const double mediaDur = m_videoPlayer.GetDuration();
				if (updated.startSeconds <= 0.001 && mediaDur > 0.0 &&
				    updated.endSeconds >= mediaDur - 0.15)
					updated.endSeconds = 0.0;
				updatedOk = m_exportQueue.UpdateJob(m_editingQueueJobId, updated);
				break;
			}
			m_editingQueueJobId.clear();
			if (m_setToastExport)
				overlay_runtime_show_toast(
					overlay::util::WideToUtf8(overlay::util::ModuleTextW(
							 updatedOk ? "Export.TrimUpdated" : "Export.EditTrimHint"))
						.c_str(),
					3000);
			Render();
			return;
		}

		AddSelectedClipToExportQueue();
		Render();
		return;
	}

	if (IsPointInRect(pt, m_galleryDeleteRect)) {
		if (DeleteGallerySelectedVideo()) {
			CloseGalleryContextMenu();
			Render();
		}
		return;
	}
}

void OverlayRenderer::HandleGalleryRightClick(int x, int y)
{
	POINT pt = ClientToLayoutPoint(x, y);

	if (m_galleryContextMenuOpen && !IsPointInRect(pt, m_galleryContextMenuRect)) {
		CloseGalleryContextMenu();
	}

	int itemIndex = GalleryListItemIndexAt(pt);
	const GalleryItem *item = m_gallery.GetItem(itemIndex);
	if (!item || item->type != GalleryItem::Type::Video) {
		if (m_galleryContextMenuOpen) {
			CloseGalleryContextMenu();
			Render();
		}
		return;
	}

	m_galleryExportSubmenuOpen = false;
	m_galleryAudioTrackSubmenuOpen = false;

	const bool alreadySelected = (itemIndex == m_gallerySelectedIndex);
	m_gallerySelectedIndex = itemIndex;
	if (!alreadySelected) {
		m_videoPlayer.Open(item->path);
		m_galleryTrimIn = 0.0;
		m_galleryTrimOut = m_videoPlayer.GetDuration();
		m_galleryTrimUserSet = false;
	}

	m_galleryContextMenuOpen = true;
	m_galleryContextMenuIndex = itemIndex;
	m_galleryContextMenuHoverIndex = -1;
	m_galleryContextMenuAnchor = pt;
	Render();
}

bool OverlayRenderer::HandleGalleryMouseDown(int x, int y)
{
	POINT pt = ClientToLayoutPoint(x, y);
	if (m_gallerySelectedIndex < 0) {
		return false;
	}
	if (IsPointInRect(pt, m_galleryListRect) || IsPointInRect(pt, m_galleryPreviewRect)) {
		return true;
	}
	return false;
}

void OverlayRenderer::MaybeStartDrag(int x, int y)
{
	POINT pt = ClientToLayoutPoint(x, y);
	int dx = pt.x - m_dragStart.x;
	int dy = pt.y - m_dragStart.y;
	if (dx * dx + dy * dy < 25) {
		return;
	}
	const GalleryItem *item = m_gallery.GetItem(m_gallerySelectedIndex);
	if (!item || item->type != GalleryItem::Type::Video) {
		return;
	}
	m_dragActive = true;

	SimpleDataObject *dataObj = new SimpleDataObject(item->path);
	SimpleDropSource *dropSource = new SimpleDropSource();
	DWORD effect = DROPEFFECT_COPY;
	DoDragDrop(dataObj, dropSource, DROPEFFECT_COPY, &effect);
	dataObj->Release();
	dropSource->Release();
	m_dragActive = false;
	m_dragCandidate = false;
}

#endif // _WIN32
