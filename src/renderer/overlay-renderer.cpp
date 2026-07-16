#ifdef _WIN32

#include "overlay-renderer.h"
#include "overlay-icons-resource.h"
#include "overlay-string-utils.h"
#include "overlay-indicators-c.h"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <sstream>
#include <iomanip>
#include <shellapi.h>
#include <objbase.h>
#include <shlobj.h>
#include <comdef.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdlib>

#ifdef ENABLE_QT
#include <QMetaObject>
#include <QWidget>
#endif

extern "C" {
#include <plugin-support.h>
#include "overlay-settings-storage.h"
#include "overlay-runtime-apply.h"
}

OverlayRenderer *OverlayRenderer::s_timerInstance = nullptr;

// Anchor symbol so GetModuleHandleEx can resolve this DLL for embedded icons.
static int g_overlayIconsModuleAnchor = 0;

namespace {

constexpr int kCompressQualityStrongCompression = 0;
constexpr int kCompressQualityBalanced = 1;
constexpr int kCompressQualityQuality = 2;

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
	  m_position(OverlayPosition::Top),
	  m_margin(20),
	  m_orientation(OverlayOrientation::Horizontal),
	  m_autoHideEnabled(false),
	  m_autoHideSeconds(5),
	  m_lastInteractionTick(0),
	  m_hoverTarget(HoverTarget::None),
	  m_galleryEnabled(false),
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
	  m_dragCandidate(false),
	  m_dragActive(false),
	  m_timelineDragging(false),
	  m_suppressNextGalleryClick(false),
	  m_overlayBackgroundAlpha(0.88f),
	  m_captureFocus(true),
	  m_dimContentOffset{0, 0},
	  m_galleryContentWidth(0),
	  m_galleryContentHeight(0),
	  m_updateTimer(0),
	  m_updateIntervalMs(500)
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
	for (int i = 0; i < ICON_CACHE_SIZE; i++) {
		m_iconBitmaps[i] = nullptr;
		m_iconMasks[i] = IconMask{};
	}

	m_settingsOpen = false;
	m_settingsContentWidth = 0;
	m_settingsContentHeight = 0;
	m_settingsScrollOffset = 0;
	m_settingsContentTotalHeight = 0;
	m_settingsViewportTop = 0;
	m_settingsViewportBottom = 0;
	m_settingsCapturingHotkeyIndex = -1;
	m_settingsHoverIndex = -1;
	m_setPosition = 0;
	m_setMargin = 20;
	m_setOrientation = 0;
	m_setOpacityPct = 88;
	m_setAutoHideEnabled = false;
	m_setAutoHideSeconds = 5;
	m_setIndicatorsEnabled = true;
	m_setIndicatorsPosition = 5;
	m_setIndicatorsOled = false;
	m_setSmartReplay = true;
	m_setSmartReplayMode = 0;
	m_setGalleryInOverlay = false;
	m_setCaptureFocus = true;
	for (int i = 0; i < 7; i++) {
		m_setHotkeyVk[i] = 0;
		m_setHotkeyMod[i] = 0;
	}
}

OverlayRenderer::~OverlayRenderer()
{
	Shutdown();
}

void CALLBACK OverlayRenderer::TimerProc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time)
{
	UNUSED_PARAMETER(hwnd);
	UNUSED_PARAMETER(msg);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(time);

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
		int64_t position = obs_data_get_int(saved_settings_data, "position");
		if (position >= 0 && position <= 8) {
			m_position = static_cast<OverlayPosition>(static_cast<int>(position));
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
	}

	m_windowManager.SetClickHandler(
		[](int x, int y, void *userData) {
			OverlayRenderer *renderer = static_cast<OverlayRenderer *>(userData);
			if (renderer) {
				renderer->HandleClick(x, y);
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

	// Theme: bg #141419, accent #a882ff, border #3a3a45
	// Theme colors for overlay UI.
	D2D1_COLOR_F bgColor = D2D1::ColorF(20.0f / 255.0f, 20.0f / 255.0f, 25.0f / 255.0f, 1.0f);
	target->CreateSolidColorBrush(bgColor, &m_backgroundBrush);

	D2D1_COLOR_F textColor = D2D1::ColorF(1.0f, 1.0f, 1.0f);
	target->CreateSolidColorBrush(textColor, &m_textBrush);

	D2D1_COLOR_F activeColor = D2D1::ColorF(168.0f / 255.0f, 130.0f / 255.0f, 255.0f / 255.0f, 1.0f);
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
	if (m_updateTimer) {
		KillTimer(m_windowManager.GetHWND(), m_updateTimer);
		m_updateTimer = 0;
	}

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
	m_layoutManager.CalculateLayout(m_position, m_margin, m_orientation, m_state.statsVisible, m_layout);

	if (IsDimmingActive()) {
		POINT normalPos = m_layoutManager.CalculateWindowPosition(m_position, m_margin, m_layout);
		ApplyDimmingWindowGeometry(normalPos, m_layout.windowWidth, m_layout.windowHeight);
	} else {
		m_dimContentOffset = {0, 0};
		POINT windowPos = m_layoutManager.CalculateWindowPosition(m_position, m_margin, m_layout);
		m_windowManager.SetPosition(windowPos, m_layout.windowWidth, m_layout.windowHeight);
	}
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

void OverlayRenderer::UpdateStatus()
{
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
	const D2D1_COLOR_F hoverColor = D2D1::ColorF(0.48f, 0.42f, 0.65f);
	const D2D1_COLOR_F hoverBgColor = D2D1::ColorF(0.48f, 0.42f, 0.65f, 0.35f);

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

	int padding = 10;
	int lineHeight = 16;
	int startY = m_layout.statsRect.top + 8;

	RECT titleRect = {m_layout.statsRect.left + padding, startY, m_layout.statsRect.right - padding,
			  startY + lineHeight};
	RenderText(overlay::util::ModuleTextW("Stats"), titleRect, 12.0f, DWRITE_FONT_WEIGHT_BOLD,
		   D2D1::ColorF(168.0f / 255.0f, 130.0f / 255.0f, 255.0f / 255.0f, 1.0f),
		   DWRITE_TEXT_ALIGNMENT_LEADING);

	std::wostringstream fpsText;
	fpsText << overlay::util::ModuleTextW("Stats.Fps") << L" " << std::fixed << std::setprecision(1) << m_state.fps;
	RECT fpsRect = {m_layout.statsRect.left + padding, startY + lineHeight + 4, m_layout.statsRect.right - padding,
			startY + lineHeight * 2 + 4};
	RenderText(fpsText.str(), fpsRect, 11.0f, DWRITE_FONT_WEIGHT_NORMAL, D2D1::ColorF(1.0f, 1.0f, 1.0f),
		   DWRITE_TEXT_ALIGNMENT_LEADING);

	std::wostringstream droppedText;
	droppedText << overlay::util::ModuleTextW("Stats.Dropped") << L" " << m_state.droppedFrames;
	RECT droppedRect = {m_layout.statsRect.left + padding, startY + lineHeight * 2 + 8,
			    m_layout.statsRect.right - padding, startY + lineHeight * 3 + 8};
	RenderText(droppedText.str(), droppedRect, 11.0f, DWRITE_FONT_WEIGHT_NORMAL, D2D1::ColorF(1.0f, 1.0f, 1.0f),
		   DWRITE_TEXT_ALIGNMENT_LEADING);

	std::wstring freeSpaceW = overlay::util::Utf8ToWide(m_state.freeSpaceText);
	RECT freeRect = {m_layout.statsRect.left + padding, startY + lineHeight * 3 + 12,
			 m_layout.statsRect.right - padding, startY + lineHeight * 4 + 12};
	RenderText(freeSpaceW, freeRect, 11.0f, DWRITE_FONT_WEIGHT_NORMAL, D2D1::ColorF(1.0f, 1.0f, 1.0f),
		   DWRITE_TEXT_ALIGNMENT_LEADING);
}

void OverlayRenderer::RenderPanels()
{
	RECT recRect = m_layout.recordingPanelRect;
	const bool hoverRecording = (m_hoverTarget == HoverTarget::RecordingPanel);
	const D2D1_COLOR_F hoverColor = D2D1::ColorF(0.48f, 0.42f, 0.65f);

	float centerX = (recRect.left + recRect.right) / 2.0f;
	float centerY = recRect.top + 30.0f;
	float radius = 18.0f;

	D2D1_ELLIPSE ellipse = D2D1::Ellipse(D2D1::Point2F(centerX, centerY), radius, radius);

	ID2D1RenderTarget *target = m_bitmapRenderTarget ? m_bitmapRenderTarget : m_renderTarget;
	if (m_state.recordingActive || hoverRecording) {
		target->FillEllipse(ellipse, m_activeBrush);
	} else {
		// RenderText may leave m_textBrush tinted; reset before drawing the idle dot.
		m_textBrush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
		target->FillEllipse(ellipse, m_textBrush);
	}

	RECT statusRect = {recRect.left, (int)(centerY + radius + 10), recRect.right, recRect.bottom};
	std::wstring statusText = m_state.recordingActive ? overlay::util::ModuleTextW("Recording")
							  : overlay::util::ModuleTextW("NotRecording");
	D2D1_COLOR_F statusColor = (m_state.recordingActive || hoverRecording)
					   ? (m_state.recordingActive ? D2D1::ColorF(168.0f / 255.0f, 130.0f / 255.0f,
										     255.0f / 255.0f, 1.0f)
								      : hoverColor)
					   : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
	RenderText(statusText, statusRect, 13.0f, DWRITE_FONT_WEIGHT_NORMAL, statusColor);

	if (m_state.recordingActive || hoverRecording) {
		RECT borderRect = {recRect.left - 3, recRect.top - 3, recRect.right + 3, recRect.bottom + 3};
		DrawRoundedRectangleBorder(borderRect, 10.0f, 3.0f,
					   D2D1::ColorF(168.0f / 255.0f, 130.0f / 255.0f, 255.0f / 255.0f, 1.0f));
	}

	RECT repRect = m_layout.replayPanelRect;
	const bool hoverReplay = (m_hoverTarget == HoverTarget::ReplayPanel);
	const D2D1_COLOR_F hoverReplayColor = D2D1::ColorF(0.48f, 0.42f, 0.65f);

	if (m_state.replayActive) {
		RECT borderRect = {repRect.left - 3, repRect.top - 3, repRect.right + 3, repRect.bottom + 3};
		DrawRoundedRectangleBorder(borderRect, 10.0f, 3.0f,
					   D2D1::ColorF(168.0f / 255.0f, 130.0f / 255.0f, 255.0f / 255.0f, 1.0f));

		int halfHeight = (repRect.bottom - repRect.top) / 2;
		RECT upperRect = {repRect.left + 5, repRect.top + 5, repRect.right - 5, repRect.top + halfHeight - 5};
		RECT lowerRect = {repRect.left + 5, repRect.top + halfHeight + 5, repRect.right - 5,
				  repRect.bottom - 5};

		RenderText(overlay::util::ModuleTextW("Replay.TurnOff"), upperRect, 11.0f, DWRITE_FONT_WEIGHT_NORMAL,
			   D2D1::ColorF(168.0f / 255.0f, 130.0f / 255.0f, 255.0f / 255.0f, 1.0f));

		ID2D1RenderTarget *target = m_bitmapRenderTarget ? m_bitmapRenderTarget : m_renderTarget;
		D2D1_POINT_2F lineStart = D2D1::Point2F((float)(repRect.left + 5), (float)(repRect.top + halfHeight));
		D2D1_POINT_2F lineEnd = D2D1::Point2F((float)(repRect.right - 5), (float)(repRect.top + halfHeight));
		target->DrawLine(lineStart, lineEnd, m_activeBrush, 1.0f);

		if (m_state.replaySaving) {
			RenderText(overlay::util::ModuleTextW("Saving"), lowerRect, 11.0f, DWRITE_FONT_WEIGHT_NORMAL,
				   D2D1::ColorF(0.6f, 0.6f, 0.6f, 1.0f));
		} else {
			RenderText(overlay::util::ModuleTextW("Save"), lowerRect, 11.0f, DWRITE_FONT_WEIGHT_NORMAL,
				   D2D1::ColorF(168.0f / 255.0f, 130.0f / 255.0f, 255.0f / 255.0f, 1.0f));
		}
	} else {
		float centerX = (repRect.left + repRect.right) / 2.0f;
		float centerY = repRect.top + 30.0f;
		std::wstring replayIcon = L"⟲";
		RECT iconRect = {(int)(centerX - 20), (int)(centerY - 20), (int)(centerX + 20), (int)(centerY + 20)};
		RenderText(replayIcon, iconRect, 36.0f, DWRITE_FONT_WEIGHT_NORMAL,
			   hoverReplay ? hoverReplayColor : D2D1::ColorF(1.0f, 1.0f, 1.0f));

		RECT statusRect = {repRect.left, (int)(centerY + 20), repRect.right, repRect.bottom};
		RenderText(overlay::util::ModuleTextW("Off"), statusRect, 13.0f, DWRITE_FONT_WEIGHT_NORMAL,
			   hoverReplay ? hoverReplayColor : D2D1::ColorF(1.0f, 1.0f, 1.0f));
	}
}

void OverlayRenderer::RenderButtons()
{
	RECT folderRect = m_layout.recordFolderButtonRect;
	const bool galleryAvailable = m_state.recordingPathConfigured;
	const bool hoverFolder = galleryAvailable && (m_hoverTarget == HoverTarget::RecordFolderButton);
	const D2D1_COLOR_F hoverColor = D2D1::ColorF(0.48f, 0.42f, 0.65f);
	const D2D1_COLOR_F disabledColor = D2D1::ColorF(0.45f, 0.45f, 0.45f, 1.0f);
	const D2D1_COLOR_F folderColor =
		!galleryAvailable ? disabledColor : (hoverFolder ? hoverColor : D2D1::ColorF(1.0f, 1.0f, 1.0f));
	RenderText(overlay::util::ModuleTextW("Gallery"), folderRect, 10.0f, DWRITE_FONT_WEIGHT_NORMAL, folderColor);

	RECT obsRect = m_layout.showOBSButtonRect;
	const bool hoverShowObs = (m_hoverTarget == HoverTarget::ShowOBSButton);
	RenderText(overlay::util::ModuleTextW("ShowObs"), obsRect, 10.0f, DWRITE_FONT_WEIGHT_NORMAL,
		   hoverShowObs ? hoverColor : D2D1::ColorF(1.0f, 1.0f, 1.0f));
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
	if (color.r > 0.6f && color.g < 0.6f && color.b > 0.6f) {
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
	static const int resourceIds[] = {IDR_ICON_PLAY,        IDR_ICON_PAUSE,      IDR_ICON_CARET_LEFT,
					  IDR_ICON_CARET_RIGHT, IDR_ICON_DOWNLOAD,   IDR_ICON_TRASH,
					  IDR_ICON_SPEAKER,     IDR_ICON_ARROW_LEFT, IDR_ICON_ARROWS_OUT,
					  IDR_ICON_ARROWS_IN,   IDR_ICON_GEAR,       IDR_ICON_CHART};
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
						"icons/download-simple.png",
						"icons/trash-simple.png",
						"icons/speaker-high.png",
						"icons/arrow-left.png",
						"icons/arrows-out.png",
						"icons/arrows-in.png",
						"icons/gear.png",
						"icons/chart-line.png"};
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
	if (m_trimmer.GetStatus() == OverlayTrimmer::Status::Trimming)
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
				}
				break;
			}
		}
	}
	Render();
}

void OverlayRenderer::CloseGallery()
{
	m_galleryOpen = false;
	m_galleryFullscreen = false;
	m_galleryExportSubmenuOpen = false;
	m_galleryAudioTrackSubmenuOpen = false;
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

void OverlayRenderer::ApplyGalleryWindowSize()
{
	if (!m_galleryOpen)
		return;
	RECT workArea = m_layoutManager.GetScreenWorkArea();
	int workWidth = workArea.right - workArea.left;
	int workHeight = workArea.bottom - workArea.top;

	int targetWidth;
	int targetHeight;
	POINT contentPos;

	if (m_galleryFullscreen) {
		targetWidth = workWidth;
		targetHeight = workHeight;
		contentPos = {workArea.left, workArea.top};
	} else {
		targetWidth = std::min(1200, workWidth - 40);
		targetHeight = std::min(820, workHeight - 40);
		if (targetWidth < 800)
			targetWidth = std::max(640, workWidth - 20);
		if (targetHeight < 500)
			targetHeight = std::max(420, workHeight - 20);
		contentPos = {workArea.left + (workWidth - targetWidth) / 2,
			     workArea.top + (workHeight - targetHeight) / 2};
	}

	m_galleryContentWidth = targetWidth;
	m_galleryContentHeight = targetHeight;

	if (IsDimmingActive()) {
		ApplyDimmingWindowGeometry(contentPos, targetWidth, targetHeight);
	} else {
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

	int outerPadding = 3;
	int sectionGap = 16;
	int contentInsetH = 14;
	int headerHeight = 30;
	int sidebarWidth = std::max(240, std::min(340, width / 3));
	int controlsHeight = 42;
	int timelineHeight = 30;

	const RECT shellRect = {outerPadding, outerPadding, width - outerPadding, height - outerPadding};
	const RECT headerRect = {shellRect.left + contentInsetH, shellRect.top + 12, shellRect.right - contentInsetH,
				 shellRect.top + 12 + headerHeight};
	const int contentTop = headerRect.bottom + 12;
	const int contentBottom = shellRect.bottom - contentInsetH;

	m_galleryListRect = {shellRect.left + contentInsetH, contentTop, shellRect.left + contentInsetH + sidebarWidth,
			     contentBottom};
	const int previewColumnLeft = m_galleryListRect.right + sectionGap;
	const int previewColumnRight = shellRect.right - contentInsetH;

	m_galleryControlsRect = {previewColumnLeft, contentBottom - controlsHeight, previewColumnRight, contentBottom};
	m_galleryTimelineRect = {previewColumnLeft, m_galleryControlsRect.top - sectionGap - timelineHeight,
				 previewColumnRight, m_galleryControlsRect.top - sectionGap};
	m_galleryPreviewRect = {previewColumnLeft, contentTop, previewColumnRight, m_galleryTimelineRect.top - sectionGap};

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
	const D2D1_COLOR_F hoverColor = D2D1::ColorF(0.48f, 0.42f, 0.65f);
	const D2D1_COLOR_F hoverBgColor = D2D1::ColorF(0.48f, 0.42f, 0.65f, 0.25f);

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

	// Preview
	DrawRoundedRectangle(m_galleryPreviewRect, 12.0f, D2D1::ColorF(30.0f / 255.0f, 30.0f / 255.0f, 38.0f / 255.0f, 1.0f));
	DrawRoundedRectangleBorder(m_galleryPreviewRect, 12.0f, 1.0f,
				   D2D1::ColorF(58.0f / 255.0f, 58.0f / 255.0f, 69.0f / 255.0f, 1.0f));
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
				float destAspect = destW / destH;
				D2D1_RECT_F destRect = D2D1::RectF(static_cast<float>(m_galleryPreviewRect.left),
								   static_cast<float>(m_galleryPreviewRect.top),
								   static_cast<float>(m_galleryPreviewRect.right),
								   static_cast<float>(m_galleryPreviewRect.bottom));
				if (srcAspect > destAspect) {
					float newH = destW / srcAspect;
					float top =
						static_cast<float>(m_galleryPreviewRect.top) + (destH - newH) * 0.5f;
					destRect.top = top;
					destRect.bottom = top + newH;
				} else {
					float newW = destH * srcAspect;
					float left =
						static_cast<float>(m_galleryPreviewRect.left) + (destW - newW) * 0.5f;
					destRect.left = left;
					destRect.right = left + newW;
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

	// Open the export folder only after a successful trim completes.
	if (m_trimmer.GetStatus() == OverlayTrimmer::Status::Success && !m_sharedFolderToOpenOnSuccess.empty()) {
		ShellExecuteW(NULL, L"open", m_sharedFolderToOpenOnSuccess.c_str(), NULL, NULL, SW_SHOWDEFAULT);
		m_sharedFolderToOpenOnSuccess.clear();
	}
	if (m_trimmer.GetStatus() == OverlayTrimmer::Status::Error) {
		m_sharedFolderToOpenOnSuccess.clear();
	}

	// Export progress overlay
	if (m_trimmer.GetStatus() == OverlayTrimmer::Status::Trimming) {
		DrawRoundedRectangle(m_galleryPreviewRect, 10.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.75f));
		int pw = m_galleryPreviewRect.right - m_galleryPreviewRect.left;
		int ph = m_galleryPreviewRect.bottom - m_galleryPreviewRect.top;
		RECT statusTextRect = {m_galleryPreviewRect.left, m_galleryPreviewRect.top + ph / 2 - 36,
				       m_galleryPreviewRect.right, m_galleryPreviewRect.top + ph / 2 - 16};
		RenderText(overlay::util::ModuleTextW("Processing"), statusTextRect, 14.0f, DWRITE_FONT_WEIGHT_BOLD,
			   D2D1::ColorF(1.0f, 1.0f, 1.0f));

		// Spinner
		float cx = m_galleryPreviewRect.left + pw / 2.0f;
		float cy = m_galleryPreviewRect.top + ph / 2.0f + 2.0f;
		float radius = 14.0f;
		double angle = (GetTickCount64() % 2000) / 2000.0 * (2.0 * 3.141592653589793);
		ID2D1RenderTarget *target = m_bitmapRenderTarget ? m_bitmapRenderTarget : m_renderTarget;
		if (target && m_textBrush) {
			for (int i = 0; i < 4; i++) {
				double a = angle + i * (2.0 * 3.141592653589793 / 4.0);
				float ex = cx + radius * static_cast<float>(cos(a));
				float ey = cy + radius * static_cast<float>(sin(a));
				target->DrawLine(D2D1::Point2F(cx, cy), D2D1::Point2F(ex, ey), m_textBrush, 2.5f);
			}
		}

		int cancelBtnW = 80;
		int cancelBtnH = 28;
		int cancelBtnTop = m_galleryPreviewRect.top + ph / 2 + 24;
		m_galleryExportCancelRect = {m_galleryPreviewRect.left + (pw - cancelBtnW) / 2, cancelBtnTop,
					     m_galleryPreviewRect.left + (pw + cancelBtnW) / 2,
					     cancelBtnTop + cancelBtnH};
		DrawRoundedRectangle(m_galleryExportCancelRect, 6.0f, D2D1::ColorF(0.4f, 0.2f, 0.2f, 1.0f));
		RenderText(overlay::util::ModuleTextW("Cancel"), m_galleryExportCancelRect, 11.0f, DWRITE_FONT_WEIGHT_NORMAL,
			   D2D1::ColorF(1.0f, 1.0f, 1.0f));
	} else {
		m_galleryExportCancelRect = RECT{0, 0, 0, 0};
	}

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
		double startRatio = std::max(0.0, std::min(1.0, trimIn / displayDuration));
		double endRatio = std::max(0.0, std::min(1.0, trimOut / displayDuration));
		double curRatio = std::max(0.0, std::min(1.0, current / displayDuration));
		int left = m_galleryTimelineRect.left + 6;
		int right = m_galleryTimelineRect.right - 6;
		int top = m_galleryTimelineRect.top + 10;
		int bottom = m_galleryTimelineRect.bottom - 10;
		int trackWidth = right - left;
		int trimLeft = left + static_cast<int>(trackWidth * startRatio);
		int trimRight = left + static_cast<int>(trackWidth * endRatio);
		RECT trimRect = {trimLeft, top, std::max(trimLeft + 4, trimRight), bottom};
		DrawRoundedRectangle(trimRect, 4.0f,
				     D2D1::ColorF(130.0f / 255.0f, 110.0f / 255.0f, 200.0f / 255.0f, 1.0f));
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
			DrawRoundedRectangle(itemRect, 8.0f, D2D1::ColorF(70.0f / 255.0f, 60.0f / 255.0f, 110.0f / 255.0f, 1.0f));
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
			DrawRoundedRectangle(subPanel, 8.0f,
					     D2D1::ColorF(35.0f / 255.0f, 32.0f / 255.0f, 50.0f / 255.0f, 1.0f));
			DrawRoundedRectangleBorder(subPanel, 8.0f, 1.0f,
						   D2D1::ColorF(58.0f / 255.0f, 58.0f / 255.0f, 69.0f / 255.0f, 1.0f));
			for (int i = 0; i < audioTrackCount; i++) {
				int rowTop = subTop + pad + i * (optH + 2);
				RECT optRect = {subLeft + pad, rowTop, subLeft + subW - pad, rowTop + optH};
				m_galleryAudioTrackOptionRects.push_back(optRect);
				float radius = optH / 2.0f;
				bool isCurrent = (i == m_videoPlayer.GetCurrentAudioTrackIndex());
				DrawRoundedRectangle(
					optRect, radius,
					isCurrent ? D2D1::ColorF(70.0f / 255.0f, 55.0f / 255.0f, 120.0f / 255.0f, 1.0f)
						  : D2D1::ColorF(45.0f / 255.0f, 42.0f / 255.0f, 58.0f / 255.0f, 1.0f));
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

	const D2D1_COLOR_F hoverColor = D2D1::ColorF(0.48f, 0.42f, 0.65f);
	const D2D1_COLOR_F normalIcon = D2D1::ColorF(0.85f, 0.85f, 0.9f);

	const bool hoverMute = (m_hoverTarget == HoverTarget::GalleryMute);
	const D2D1_COLOR_F muteTint = hoverMute ? hoverColor : normalIcon;
	DrawGalleryIcon(m_galleryMuteRect, 6, &muteTint);
	DrawRoundedRectangle(m_galleryVolumeRect, 4.0f,
			     D2D1::ColorF(40.0f / 255.0f, 40.0f / 255.0f, 50.0f / 255.0f, 1.0f));
	int volWidth = m_galleryVolumeRect.right - m_galleryVolumeRect.left - 4;
	int volFill = static_cast<int>(volWidth * (m_galleryVolume / 100.0));
	RECT volRect = {m_galleryVolumeRect.left + 2, m_galleryVolumeRect.top + 2,
			m_galleryVolumeRect.left + 2 + volFill, m_galleryVolumeRect.bottom - 2};
	DrawRoundedRectangle(volRect, 3.0f, D2D1::ColorF(130.0f / 255.0f, 110.0f / 255.0f, 200.0f / 255.0f, 1.0f));

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

	// Export submenu
	if (m_galleryExportSubmenuOpen) {
		int pad = 12;
		int row1H = 24;
		int row2H = 24;
		int row3H = 32;
		int trackRowH = 16;
		int tracksHeight = static_cast<int>(m_exportTrackStreamIndices.size()) * trackRowH;
		if (tracksHeight > 0)
			tracksHeight += 4;
		int boxSize = 184 + tracksHeight;
		int gap = 8;
		int panelLeft = (controlsRect.left + controlsRect.right) / 2 - boxSize / 2;
		int panelTop = controlsRect.top - gap - boxSize;
		RECT exportBlockRect = {panelLeft, panelTop, panelLeft + boxSize, panelTop + boxSize};
		m_galleryExportPanelRect = exportBlockRect;
		DrawRoundedRectangle(exportBlockRect, 10.0f,
				     D2D1::ColorF(35.0f / 255.0f, 32.0f / 255.0f, 50.0f / 255.0f, 1.0f));
		DrawRoundedRectangleBorder(exportBlockRect, 10.0f, 1.0f,
					   D2D1::ColorF(58.0f / 255.0f, 58.0f / 255.0f, 69.0f / 255.0f, 1.0f));

		int contentTop = exportBlockRect.top + pad;
		m_exportTrackKeepRects.clear();
		for (size_t i = 0; i < m_exportTrackStreamIndices.size(); i++) {
			int rowTop = contentTop + static_cast<int>(i) * trackRowH;
			int cbSize = 12;
			RECT keepRect = {exportBlockRect.left + pad, rowTop, exportBlockRect.left + pad + cbSize,
					 rowTop + trackRowH};
			m_exportTrackKeepRects.push_back(keepRect);
			DrawRoundedRectangle(keepRect, 2.0f, D2D1::ColorF(0.2f, 0.2f, 0.25f, 1.0f));
			if (i < m_exportTrackKeep.size() && m_exportTrackKeep[i]) {
				RECT checkR = {keepRect.left + 2, keepRect.top + 2, keepRect.right - 2,
					       keepRect.bottom - 2};
				DrawRoundedRectangle(checkR, 1.0f,
						     D2D1::ColorF(130.0f / 255.0f, 110.0f / 255.0f, 200.0f / 255.0f,
								  1.0f));
			}
			RECT labelRect = {exportBlockRect.left + pad + cbSize + 4, rowTop, exportBlockRect.right - pad,
					  rowTop + trackRowH};
			RenderText(i < m_exportTrackLabels.size() ? m_exportTrackLabels[i] : L"", labelRect, 9.0f,
				   DWRITE_FONT_WEIGHT_NORMAL, D2D1::ColorF(0.9f, 0.9f, 0.9f),
				   DWRITE_TEXT_ALIGNMENT_LEADING);
		}
		if (tracksHeight > 0)
			contentTop += tracksHeight;

		int cbSize = 14;
		UNUSED_PARAMETER(cbSize);

		// Compress row
		m_galleryCompressRect = {exportBlockRect.left + pad, contentTop,
					 exportBlockRect.left + pad + cbSize + 4 + 70, contentTop + row1H};
		DrawRoundedRectangle(RECT{exportBlockRect.left + pad, contentTop + (row1H - cbSize) / 2,
					  exportBlockRect.left + pad + cbSize,
					  contentTop + (row1H - cbSize) / 2 + cbSize},
				     3.0f, D2D1::ColorF(0.2f, 0.2f, 0.25f, 1.0f));
		if (m_galleryShareCompress) {
			RECT checkR = {exportBlockRect.left + pad + 2, contentTop + (row1H - cbSize) / 2 + 2,
				       exportBlockRect.left + pad + cbSize - 2,
				       contentTop + (row1H - cbSize) / 2 + cbSize - 2};
			DrawRoundedRectangle(checkR, 2.0f,
					     D2D1::ColorF(130.0f / 255.0f, 110.0f / 255.0f, 200.0f / 255.0f, 1.0f));
		}
		RenderText(overlay::util::ModuleTextW("Compress"),
			   RECT{exportBlockRect.left + pad + cbSize + 4, contentTop, exportBlockRect.right - pad,
				contentTop + row1H},
			   10.0f, DWRITE_FONT_WEIGHT_NORMAL, D2D1::ColorF(0.9f, 0.9f, 0.9f),
			   DWRITE_TEXT_ALIGNMENT_LEADING);

		// Quality row
		if (m_galleryShareCompress) {
			int qRowTop = contentTop + row1H + 4;
			int qBtnW = 28;
			m_galleryQualityPrevRect = {exportBlockRect.left + pad, qRowTop,
						    exportBlockRect.left + pad + qBtnW, qRowTop + row2H};
			m_galleryQualityNextRect = {exportBlockRect.right - pad - qBtnW, qRowTop,
						    exportBlockRect.right - pad, qRowTop + row2H};
			RenderText(L"-", m_galleryQualityPrevRect, 11.0f, DWRITE_FONT_WEIGHT_NORMAL,
				   D2D1::ColorF(0.9f, 0.9f, 0.9f));
			const std::wstring qualityText =
				overlay::util::ModuleTextW("CompressionQuality") + L" " +
				CompressQualityLabel(m_galleryShareCompressQuality);
			RenderText(qualityText,
				   RECT{m_galleryQualityPrevRect.right + 4, qRowTop, m_galleryQualityNextRect.left - 4,
					qRowTop + row2H},
				   9.5f, DWRITE_FONT_WEIGHT_NORMAL, D2D1::ColorF(0.85f, 0.85f, 0.85f));
			RenderText(L"+", m_galleryQualityNextRect, 11.0f, DWRITE_FONT_WEIGHT_NORMAL,
				   D2D1::ColorF(0.9f, 0.9f, 0.9f));
		} else {
			m_galleryQualityPrevRect = RECT{0, 0, 0, 0};
			m_galleryQualityNextRect = RECT{0, 0, 0, 0};
		}

		// Export button
		int exportBtnTop = exportBlockRect.bottom - pad - row3H;
		m_galleryExportConfirmRect = {exportBlockRect.left + pad, exportBtnTop, exportBlockRect.right - pad,
					      exportBlockRect.bottom - pad};
		DrawRoundedRectangle(m_galleryExportConfirmRect, 6.0f,
				     D2D1::ColorF(70.0f / 255.0f, 55.0f / 255.0f, 120.0f / 255.0f, 1.0f));
		RenderText(overlay::util::ModuleTextW("Save"), m_galleryExportConfirmRect, 11.0f, DWRITE_FONT_WEIGHT_NORMAL,
			   D2D1::ColorF(1.0f, 1.0f, 1.0f));
	} else {
		m_galleryCompressRect = RECT{0, 0, 0, 0};
		m_galleryExportPanelRect = RECT{0, 0, 0, 0};
		m_galleryQualityPrevRect = RECT{0, 0, 0, 0};
		m_galleryQualityNextRect = RECT{0, 0, 0, 0};
		m_galleryExportConfirmRect = RECT{0, 0, 0, 0};
	}

	std::wstring status = L"";
	OverlayTrimmer::Status trimStatus = m_trimmer.GetStatus();
	if (trimStatus == OverlayTrimmer::Status::Trimming) {
		status = overlay::util::ModuleTextW("Processing");
	} else if (trimStatus == OverlayTrimmer::Status::Success) {
		status = overlay::util::ModuleTextW("Done");
	} else if (trimStatus == OverlayTrimmer::Status::Error) {
		status = overlay::util::ModuleTextW("Failed");
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
	if (!m_galleryOpen || m_gallerySelectedIndex < 0) {
		return;
	}
	if (m_trimmer.GetStatus() == OverlayTrimmer::Status::Trimming) {
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

const char *const kSettingsHotkeyKeys[7] = {
	"hotkey_play",         "hotkey_seek_forward_5", "hotkey_seek_back_5", "hotkey_frame_forward",
	"hotkey_frame_back",   "hotkey_go_in",          "hotkey_go_out"};
const int kSettingsHotkeyDefaultVk[7] = {32, 39, 37, 190, 188, 36, 35};
const char *const kSettingsHotkeyLabelKeys[7] = {
	"Hotkeys.PlayPause", "Hotkeys.SeekForward5", "Hotkeys.SeekBack5", "Hotkeys.FrameForward",
	"Hotkeys.FrameBack", "Hotkeys.GoIn",         "Hotkeys.GoOut"};

const char *const kSettingsPositionKeys[9] = {
	"Position.Top",        "Position.Bottom",     "Position.Left",
	"Position.Right",      "Position.TopLeft",    "Position.TopRight",
	"Position.BottomLeft", "Position.BottomRight", "Position.Center"};
const char *const kSettingsOrientationKeys[2] = {"Orientation.Horizontal", "Orientation.Vertical"};
const char *const kSettingsSmartReplayModeKeys[2] = {"SmartReplay.Mode.Legacy", "SmartReplay.Mode.TimestampTrim"};

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

	m_setPosition = ClampInt(getIntDef("position", 0), 0, 8);
	m_setMargin = ClampInt(getIntDef("margin", 20), 0, 200);
	int orient = getIntDef("orientation", 0);
	m_setOrientation = (orient == 1) ? 1 : 0;
	double alpha = (d && obs_data_has_user_value(d, "overlay_background_alpha"))
			       ? obs_data_get_double(d, "overlay_background_alpha")
			       : 0.88;
	m_setOpacityPct = ClampInt(static_cast<int>(alpha * 100.0 + 0.5), 50, 100);
	m_setAutoHideEnabled = getBoolDef("auto_hide_enabled", false);
	m_setAutoHideSeconds = ClampInt(getIntDef("auto_hide_seconds", 5), 1, 3600);
	m_setIndicatorsEnabled = getBoolDef("indicators_enabled", true);
	m_setIndicatorsPosition = ClampInt(getIntDef("indicators_position", 5), 0, 8);
	m_setIndicatorsOled = getBoolDef("indicators_oled_protection", false);
	m_setSmartReplay = getBoolDef("smart_replay", true);
	m_setSmartReplayMode = ClampInt(getIntDef("smart_replay_mode", 0), 0, 1);
	m_setGalleryInOverlay = getBoolDef("gallery_in_overlay", false);
	m_setCaptureFocus = getBoolDef("capture_focus", true);
	if (d && obs_data_has_user_value(d, "gallery_export_path"))
		m_setExportPath = overlay::util::Utf8ToWide(obs_data_get_string(d, "gallery_export_path"));
	else
		m_setExportPath.clear();

	for (int i = 0; i < 7; i++) {
		int hkVk, hkMod;
		getHotkeyVkMod(d, kSettingsHotkeyKeys[i], kSettingsHotkeyDefaultVk[i], 0, hkVk, hkMod);
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
	obs_data_set_double(d, "overlay_background_alpha", m_setOpacityPct / 100.0);
	obs_data_set_bool(d, "auto_hide_enabled", m_setAutoHideEnabled);
	obs_data_set_int(d, "auto_hide_seconds", m_setAutoHideSeconds);
	obs_data_set_bool(d, "indicators_enabled", m_setIndicatorsEnabled);
	obs_data_set_int(d, "indicators_position", m_setIndicatorsPosition);
	obs_data_set_bool(d, "indicators_oled_protection", m_setIndicatorsOled);
	obs_data_set_bool(d, "smart_replay", m_setSmartReplay);
	obs_data_set_int(d, "smart_replay_mode", m_setSmartReplayMode);
	obs_data_set_bool(d, "gallery_in_overlay", m_setGalleryInOverlay);
	obs_data_set_bool(d, "capture_focus", m_setCaptureFocus);
	obs_data_set_string(d, "gallery_export_path", overlay::util::WideToUtf8(m_setExportPath).c_str());
	for (int i = 0; i < 7; i++) {
		obs_data_set_int(d, (std::string(kSettingsHotkeyKeys[i]) + "_vk").c_str(), m_setHotkeyVk[i]);
		obs_data_set_int(d, (std::string(kSettingsHotkeyKeys[i]) + "_mod").c_str(), m_setHotkeyMod[i]);
	}
}

void OverlayRenderer::OpenSettings()
{
	if (m_settingsOpen)
		return;
	if (m_galleryOpen)
		CloseGallery();

	m_settingsOpen = true;
	m_settingsScrollOffset = 0;
	m_settingsCapturingHotkeyIndex = -1;
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
	m_settingsOpen = false;
	m_settingsCapturingHotkeyIndex = -1;
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
	m_overlayBackgroundAlpha = static_cast<float>(m_setOpacityPct) / 100.0f;
	m_autoHideEnabled = m_setAutoHideEnabled;
	m_autoHideSeconds = ClampInt(m_setAutoHideSeconds, 1, 3600);
	m_galleryEnabled = m_setGalleryInOverlay;
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
	RECT workArea = m_layoutManager.GetScreenWorkArea();
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
	const D2D1_COLOR_F kAccent = D2D1::ColorF(0.66f, 0.51f, 1.0f);
	const D2D1_COLOR_F kRowBg = D2D1::ColorF(30.0f / 255.0f, 30.0f / 255.0f, 38.0f / 255.0f, 0.9f);
	const D2D1_COLOR_F kRowHover = D2D1::ColorF(0.48f, 0.42f, 0.65f, 0.30f);
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
	RenderText(overlay::util::ModuleTextW("OverlaySettings"),
		   {headerRect.left, headerRect.top, headerRect.right - 40, headerRect.bottom}, 15.0f,
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
		m_settingsHits.push_back({closeRect, SettingsAction::Close, 0});
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

	auto sectionHeader = [&](const std::wstring &title) {
		const int h = 28;
		RECT r = {contentLeft + 4, y, contentRight, y + h};
		RenderText(title, r, 12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, kAccent, DWRITE_TEXT_ALIGNMENT_LEADING);
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

	sectionHeader(overlay::util::ModuleTextW("Tab.General"));
	stepperRow(overlay::util::ModuleTextW("Position.Label"),
		   overlay::util::ModuleTextW(kSettingsPositionKeys[m_setPosition]), SettingsField::Position);
	stepperRow(overlay::util::ModuleTextW("Margin"), std::to_wstring(m_setMargin) + L" px", SettingsField::Margin);
	stepperRow(overlay::util::ModuleTextW("Layout"),
		   overlay::util::ModuleTextW(kSettingsOrientationKeys[m_setOrientation]), SettingsField::Orientation);
	stepperRow(overlay::util::ModuleTextW("OverlayOpacity"), std::to_wstring(m_setOpacityPct) + L"%",
		   SettingsField::Opacity);
	checkboxRow(overlay::util::ModuleTextW("AutoHide.Enable"), m_setAutoHideEnabled,
		    SettingsField::AutoHideEnabled);
	stepperRow(overlay::util::ModuleTextW("AutoHide.Delay"), std::to_wstring(m_setAutoHideSeconds) + L" s",
		   SettingsField::AutoHideSeconds);
	checkboxRow(overlay::util::ModuleTextW("Gallery.InOverlay"), m_setGalleryInOverlay,
		    SettingsField::GalleryInOverlay);
	pathRow();
	checkboxRow(overlay::util::ModuleTextW("CaptureFocus"), m_setCaptureFocus, SettingsField::CaptureFocus);

	sectionHeader(overlay::util::ModuleTextW("Indicators"));
	checkboxRow(overlay::util::ModuleTextW("Indicators.Show"), m_setIndicatorsEnabled,
		    SettingsField::IndicatorsEnabled);
	stepperRow(overlay::util::ModuleTextW("Position.Label"),
		   overlay::util::ModuleTextW(kSettingsPositionKeys[m_setIndicatorsPosition]),
		   SettingsField::IndicatorsPosition);
	checkboxRow(overlay::util::ModuleTextW("Indicators.OledProtection"), m_setIndicatorsOled,
		    SettingsField::IndicatorsOled);

	sectionHeader(overlay::util::ModuleTextW("Replay"));
	checkboxRow(overlay::util::ModuleTextW("SmartReplay"), m_setSmartReplay, SettingsField::SmartReplay);
	stepperRow(overlay::util::ModuleTextW("SmartReplay.Mode"),
		   overlay::util::ModuleTextW(kSettingsSmartReplayModeKeys[m_setSmartReplayMode]),
		   SettingsField::SmartReplayMode);

	sectionHeader(overlay::util::ModuleTextW("Tab.Hotkeys"));
	for (int i = 0; i < 7; i++) {
		hotkeyRow(overlay::util::ModuleTextW(kSettingsHotkeyLabelKeys[i]), i);
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
		DrawRoundedRectangle(thumbRect, 3.0f, D2D1::ColorF(0.66f, 0.51f, 1.0f, 0.55f));
	}
}

int OverlayRenderer::SettingsHoverIndexAt(const POINT &pt) const
{
	for (size_t i = 0; i < m_settingsHits.size(); i++) {
		const SettingsHit &h = m_settingsHits[i];
		if (!IsPointInRect(pt, h.rect))
			continue;
		if (h.action != SettingsAction::Close) {
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
	if (idx < 0)
		return;
	const SettingsHit hit = m_settingsHits[static_cast<size_t>(idx)];

	if (m_settingsCapturingHotkeyIndex >= 0 && hit.action != SettingsAction::HotkeyCapture) {
		m_settingsCapturingHotkeyIndex = -1;
		m_windowManager.SetKeyboardCaptureActive(false);
	}

	switch (hit.action) {
	case SettingsAction::Close:
		CloseSettings();
		return;
	case SettingsAction::Browse: {
		std::wstring picked;
		if (BrowseForExportFolderNative(m_windowManager.GetHWND(), picked)) {
			m_setExportPath = picked;
			PersistSettingsWorkingValues();
		}
		Render();
		return;
	}
	case SettingsAction::HotkeyCapture:
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
						       m_setIndicatorsOled);
#endif
			break;
		case SettingsField::IndicatorsOled:
			m_setIndicatorsOled = !m_setIndicatorsOled;
#ifdef ENABLE_QT
			overlay_runtime_set_indicators(m_setIndicatorsEnabled, m_setIndicatorsPosition,
						       m_setIndicatorsOled);
#endif
			break;
		case SettingsField::SmartReplay:
			m_setSmartReplay = !m_setSmartReplay;
#ifdef ENABLE_QT
			overlay_runtime_set_smart_replay(m_setSmartReplay);
#endif
			break;
		case SettingsField::GalleryInOverlay:
			m_setGalleryInOverlay = !m_setGalleryInOverlay;
			break;
		case SettingsField::CaptureFocus:
			m_setCaptureFocus = !m_setCaptureFocus;
			m_captureFocus = m_setCaptureFocus;
			m_windowManager.SetAllowActivate(m_setCaptureFocus);
			ApplySettingsWindowSize();
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
		case SettingsField::Opacity:
			m_setOpacityPct = ClampInt(m_setOpacityPct + dir * 2, 50, 100);
			break;
		case SettingsField::AutoHideSeconds:
			m_setAutoHideSeconds = ClampInt(m_setAutoHideSeconds + dir, 1, 3600);
			break;
		case SettingsField::IndicatorsPosition:
			m_setIndicatorsPosition = (m_setIndicatorsPosition + dir + 9) % 9;
#ifdef ENABLE_QT
			overlay_runtime_set_indicators(m_setIndicatorsEnabled, m_setIndicatorsPosition,
						       m_setIndicatorsOled);
#endif
			break;
		case SettingsField::SmartReplayMode:
			m_setSmartReplayMode = (m_setSmartReplayMode + dir + 2) % 2;
#ifdef ENABLE_QT
			overlay_runtime_set_smart_replay_mode(m_setSmartReplayMode);
#endif
			break;
		default:
			break;
		}
		PersistSettingsWorkingValues();
		Render();
		return;
	}
	}
}

void OverlayRenderer::HandleSettingsKey(int vkCode, int mods)
{
	if (!m_settingsOpen)
		return;

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
		if (index >= 0 && index < 7) {
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

void OverlayRenderer::HandleGalleryClick(int x, int y)
{
	POINT pt = ClientToLayoutPoint(x, y);
	if (m_trimmer.GetStatus() == OverlayTrimmer::Status::Trimming) {
		if (IsPointInRect(pt, m_galleryExportCancelRect)) {
			m_trimmer.RequestCancel();
		}
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
			Render();
		}
		return;
	}

	if (m_galleryExportSubmenuOpen) {
		if (IsPointInRect(pt, m_galleryExportPanelRect)) {
			for (size_t i = 0; i < m_exportTrackKeepRects.size(); i++) {
				if (IsPointInRect(pt, m_exportTrackKeepRects[i]) && i < m_exportTrackKeep.size()) {
					m_exportTrackKeep[i] = !m_exportTrackKeep[i];
					Render();
					return;
				}
			}
			if (IsPointInRect(pt, m_galleryExportConfirmRect)) {
				const GalleryItem *item = m_gallery.GetItem(m_gallerySelectedIndex);
				if (item && item->type == GalleryItem::Type::Video) {
					double start = m_galleryTrimIn;
					double end = m_galleryTrimOut;
					if (end <= start) {
						end = start + 1.0;
					}
					std::string recordingPath = m_stateManager.GetRecordingPath();
					std::wstring sharedFolder = BuildGalleryExportFolder(recordingPath);
					if (!sharedFolder.empty()) {
						if (!CreateDirectoryW(sharedFolder.c_str(), NULL)) {
							DWORD mkdirErr = GetLastError();
							if (mkdirErr != ERROR_ALREADY_EXISTS) {
								obs_log(LOG_ERROR,
									"Gallery export: failed to create output folder '%s' (error %lu)",
									overlay::util::WideToUtf8(sharedFolder).c_str(), mkdirErr);
							}
						}
						std::wstring sharePath =
							m_galleryShareCompress
								? BuildSharedOutputPathExt(sharedFolder, item->path, L"_share",
											   L".mp4")
								: BuildSharedOutputPath(sharedFolder, item->path, L"_share");
						m_sharedFolderToOpenOnSuccess = sharedFolder;
						std::vector<int> streamIndicesToKeep;
						for (size_t i = 0; i < m_exportTrackStreamIndices.size(); i++) {
							if (i < m_exportTrackKeep.size() && m_exportTrackKeep[i]) {
								streamIndicesToKeep.push_back(m_exportTrackStreamIndices[i]);
							}
						}
						if (!m_trimmer.StartExport(item->path, sharePath, start, end, m_galleryShareCompress,
									   m_galleryShareCrf, streamIndicesToKeep,
									   /*customFfmpegArgs=*/L"",
									   /*useCpuEncoder=*/true)) {
							m_sharedFolderToOpenOnSuccess.clear();
							obs_log(LOG_WARNING,
								"Gallery export could not start: another export is already in progress");
						}
					} else {
						obs_log(LOG_ERROR,
							"Gallery export failed: export folder path is empty (configure gallery export path or recording directory)");
					}
				}
				m_galleryExportSubmenuOpen = false;
				Render();
				return;
			}
			if (IsPointInRect(pt, m_galleryCompressRect)) {
				m_galleryShareCompress = !m_galleryShareCompress;
				overlay_settings_ensure();
				obs_data_set_bool(saved_settings_data, "gallery_share_compress", m_galleryShareCompress);
				overlay_settings_save();
				Render();
				return;
			}
			if (IsPointInRect(pt, m_galleryQualityPrevRect)) {
				m_galleryShareCompressQuality =
					std::max(kCompressQualityStrongCompression, m_galleryShareCompressQuality - 1);
				m_galleryShareCrf = CrfForCompressQuality(m_galleryShareCompressQuality);
				overlay_settings_ensure();
				obs_data_set_int(saved_settings_data, "gallery_share_compress_quality",
						 m_galleryShareCompressQuality);
				obs_data_set_int(saved_settings_data, "gallery_share_crf", m_galleryShareCrf);
				overlay_settings_save();
				Render();
				return;
			}
			if (IsPointInRect(pt, m_galleryQualityNextRect)) {
				m_galleryShareCompressQuality =
					std::min(kCompressQualityQuality, m_galleryShareCompressQuality + 1);
				m_galleryShareCrf = CrfForCompressQuality(m_galleryShareCompressQuality);
				overlay_settings_ensure();
				obs_data_set_int(saved_settings_data, "gallery_share_compress_quality",
						 m_galleryShareCompressQuality);
				obs_data_set_int(saved_settings_data, "gallery_share_crf", m_galleryShareCrf);
				overlay_settings_save();
				Render();
				return;
			}
			// Consume clicks on the popup background so they do not reach controls below.
			return;
		}

		m_galleryExportSubmenuOpen = false;
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
		Render();
		return;
	}
	int maxPage = (static_cast<int>(m_gallery.GetItems().size()) - 1) / itemsPerPage;
	if (IsPointInRect(pt, m_galleryNextPageRect) && m_galleryPage < maxPage) {
		m_galleryPage++;
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
				m_gallerySelectedIndex = itemIndex;
				m_videoPlayer.Open(item->path);
				m_galleryTrimIn = 0.0;
				m_galleryTrimOut = m_videoPlayer.GetDuration();
				m_galleryTrimUserSet = false;
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
		Render();
		return;
	}

	if (IsPointInRect(pt, m_gallerySetOutRect)) {
		m_galleryTrimOut = m_videoPlayer.GetCurrentTime();
		m_videoPlayer.SetTrimOut(m_galleryTrimOut);
		m_galleryTrimUserSet = true;
		Render();
		return;
	}

	if (IsPointInRect(pt, m_galleryTrimRect)) {
		const GalleryItem *selected = m_gallery.GetItem(m_gallerySelectedIndex);
		if (!selected || selected->type != GalleryItem::Type::Video) {
			return;
		}
		m_galleryExportSubmenuOpen = true;
		m_galleryAudioTrackSubmenuOpen = false;
		m_exportTrackStreamIndices.clear();
		m_exportTrackKeep.clear();
		m_exportTrackLabels.clear();
		int trackNum = 0;
		int videoIdx = m_videoPlayer.GetVideoStreamIndex();
		if (videoIdx >= 0) {
			trackNum++;
			m_exportTrackStreamIndices.push_back(videoIdx);
			m_exportTrackKeep.push_back(true);
			std::wstring videoName = WideFromUtf8(m_videoPlayer.GetVideoStreamName());
			m_exportTrackLabels.push_back(std::to_wstring(trackNum) + L": " +
						      (videoName.empty() ? overlay::util::ModuleTextW("Export.Video") : videoName));
		}
		int audioCount = m_videoPlayer.GetAudioTrackCount();
		for (int i = 0; i < audioCount; i++) {
			OverlayVideoPlayer::AudioTrackInfo info;
			m_videoPlayer.GetAudioTrackInfo(i, info);
			trackNum++;
			m_exportTrackStreamIndices.push_back(info.streamIndex);
			m_exportTrackKeep.push_back(true);
			std::wstring name = WideFromUtf8(info.name);
			m_exportTrackLabels.push_back(std::to_wstring(trackNum) + L": " +
						      (name.empty() ? overlay::util::ModuleTextW("Export.Audio") + L" " + std::to_wstring(i + 1)
								    : name));
		}
		Render();
		return;
	}

	if (IsPointInRect(pt, m_galleryDeleteRect)) {
		const GalleryItem *item = m_gallery.GetItem(m_gallerySelectedIndex);
		if (item && item->type == GalleryItem::Type::Video) {
			int ret = MessageBoxW(m_windowManager.GetHWND(), item->name.c_str(),
					      overlay::util::ModuleTextW("DeleteFile").c_str(),
					      MB_YESNO | MB_ICONQUESTION);
			if (ret == IDYES) {
				std::wstring pathToDelete = item->path;
				m_videoPlayer.Close();
				if (DeleteFileW(pathToDelete.c_str())) {
					m_gallery.ReloadCurrent();
					int count = static_cast<int>(m_gallery.GetItems().size());
					if (count == 0) {
						m_gallerySelectedIndex = -1;
					} else {
						m_gallerySelectedIndex = (m_gallerySelectedIndex >= count)
										 ? count - 1
										 : m_gallerySelectedIndex;
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
				}
				Render();
			}
		}
		return;
	}
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
