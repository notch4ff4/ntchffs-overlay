// Should be refactored. Plan to separate into smaller files. (will be removed after checking)
#ifdef _WIN32

#include "overlay-indicators.h"
#include "overlay-gif-decode.h"
#include "overlay-svg-rasterize.h"
#include <obs-module.h>

extern "C" {
#include <plugin-support.h>
bool overlay_indicators_get_overlay_visible(void);
}
#include <algorithm>
#include <cmath>
#include <dxgiformat.h>
#include <mmsystem.h>
#include <util/base.h>
#include <vector>

#pragma comment(lib, "winmm.lib")

OverlayIndicators *OverlayIndicators::s_timerInstance = nullptr;

namespace {
	const int INDICATOR_SIZE = 20;
	const int INDICATOR_PADDING = 8;
	const int INDICATOR_SPACING = 8;
	const int INDICATOR_MARGIN = 20;
	const int INDICATOR_SVG_PX = 64;
	const ULONGLONG REPLAY_SAVED_DURATION_MS = 2000;
	const ULONGLONG OLED_SHIFT_INTERVAL_MS = 60000;
	const int OLED_SHIFT_PIXELS = 8;
	const float OLED_RESTING_OPACITY = 0.35f;
	const float OLED_PEAK_OPACITY = 1.0f;
	const ULONGLONG OLED_DIM_DELAY_MS = 5000;
	const ULONGLONG OLED_DIM_FADE_MS = 2000;
	const int OLED_PATTERN_COUNT = 8;
	const int OLED_PATTERN[OLED_PATTERN_COUNT][2] = {
		{-1, -1}, {0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0},
	};

	bool HasExtension(const std::wstring &path, const wchar_t *ext)
	{
		const size_t n = wcslen(ext);
		return path.size() >= n && _wcsicmp(path.c_str() + path.size() - n, ext) == 0;
	}

	bool IsSvgPath(const std::wstring &path)
	{
		return HasExtension(path, L".svg");
	}

	float Clamp01(float v)
	{
		if (v < 0.0f)
			return 0.0f;
		if (v > 1.0f)
			return 1.0f;
		return v;
	}
}

OverlayIndicators::OverlayIndicators()
	: m_d2dFactory(nullptr),
	  m_writeFactory(nullptr),
	  m_wicFactory(nullptr),
	  m_wicBitmap(nullptr),
	  m_bitmapRenderTarget(nullptr),
	  m_textBrush(nullptr),
	  m_activeBrush(nullptr),
	  m_position(OverlayPosition::TopRight),
	  m_enabled(true),
	  m_oledProtection(false),
	  m_scalePercent(100),
	  m_oledOffsetX(0),
	  m_oledOffsetY(0),
	  m_oledShiftStartTick(0),
	  m_recordingIndicatorStartTick(0),
	  m_replayIndicatorStartTick(0),
	  m_replaySavingIndicatorStartTick(0),
	  m_replaySavedIndicatorStartTick(0),
	  m_replaySavedUntil(0),
	  m_updateTimer(0),
	  m_timerIntervalMs(500),
	  m_windowWidth(0),
	  m_windowHeight(0) {}

OverlayIndicators::~OverlayIndicators()
{
	Shutdown();
}

void CALLBACK OverlayIndicators::TimerProc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time)
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

bool OverlayIndicators::Initialize(HINSTANCE hInstance)
{
	if (!m_windowManager.Initialize(hInstance)) {
		return false;
	}

	m_windowManager.SetClickThrough(true);

	HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
		obs_log(LOG_ERROR, "Indicators: Failed to initialize COM: 0x%08x", hr);
		return false;
	}

	hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
			      IID_PPV_ARGS(&m_wicFactory));
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "Indicators: Failed to create WIC factory: 0x%08x", hr);
		return false;
	}

	hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_d2dFactory);
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "Indicators: Failed to create D2D factory: 0x%08x", hr);
		return false;
	}

	hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
				 reinterpret_cast<IUnknown **>(&m_writeFactory));
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "Indicators: Failed to create DWrite factory: 0x%08x", hr);
		return false;
	}

	CreateDeviceResources();
	EnsureTimer();
	UpdateStatus();
	Render();

	return true;
}

void OverlayIndicators::Shutdown()
{
	if (m_updateTimer) {
		KillTimer(m_windowManager.GetHWND(), m_updateTimer);
		m_updateTimer = 0;
	}

	s_timerInstance = nullptr;

	DiscardDeviceResources();

	if (m_textBrush) {
		m_textBrush->Release();
		m_textBrush = nullptr;
	}
	if (m_activeBrush) {
		m_activeBrush->Release();
		m_activeBrush = nullptr;
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

	m_windowManager.Shutdown();
	CoUninitialize();
}

void OverlayIndicators::SetEnabled(bool enabled)
{
	m_enabled = enabled;
	if (!m_enabled) {
		m_windowManager.Hide();
	} else {
		Render();
	}
}

void OverlayIndicators::SetPosition(OverlayPosition position)
{
	m_position = position;
	UpdateWindowLayout();
}

void OverlayIndicators::SetOledProtection(bool enabled)
{
	if (m_oledProtection == enabled) {
		return;
	}
	m_oledProtection = enabled;
	m_oledShiftStartTick = GetTickCount64();
	UpdateOledOffset();
	const ULONGLONG now = GetTickCount64();
	if (m_state.recordingActive) {
		m_recordingIndicatorStartTick = now;
	}
	if (m_state.replayActive) {
		m_replayIndicatorStartTick = now;
	}
	if (m_state.replaySaving) {
		m_replaySavingIndicatorStartTick = now;
	}
	if (now <= m_replaySavedUntil) {
		m_replaySavedIndicatorStartTick = now;
	}
	if (m_enabled) {
		Render();
	}
}

void OverlayIndicators::SetScalePercent(int scalePercent)
{
	if (scalePercent < 50)
		scalePercent = 50;
	if (scalePercent > 300)
		scalePercent = 300;
	if (m_scalePercent == scalePercent)
		return;
	m_scalePercent = scalePercent;
	DiscardDeviceResources();
	if (m_enabled)
		Render();
}

int OverlayIndicators::ScaledIconSize() const
{
	const int v = INDICATOR_SIZE * m_scalePercent / 100;
	return v < 10 ? 10 : v;
}

int OverlayIndicators::ScaledPadding() const
{
	const int v = INDICATOR_PADDING * m_scalePercent / 100;
	return v < 4 ? 4 : v;
}

int OverlayIndicators::ScaledSpacing() const
{
	const int v = INDICATOR_SPACING * m_scalePercent / 100;
	return v < 4 ? 4 : v;
}

int OverlayIndicators::ScaledMargin() const
{
	const int v = INDICATOR_MARGIN * m_scalePercent / 100;
	return v < 8 ? 8 : v;
}

float OverlayIndicators::ScaledStrokeWidth() const
{
	const float w = 2.0f * static_cast<float>(m_scalePercent) / 100.0f;
	return w < 1.0f ? 1.0f : w;
}

void OverlayIndicators::SetCustom(int slot, const wchar_t *path, bool tintEnabled, int colorRgb)
{
	if (slot < 0 || slot >= CUSTOM_SLOT_COUNT)
		return;

	CustomSlot &s = m_custom[slot];
	std::wstring newPath = path ? path : L"";
	const int newColor = colorRgb & 0xFFFFFF;
	const bool pathChanged = (s.path != newPath);
	const bool tintChanged = (s.tintEnabled != tintEnabled);
	const bool colorChanged = (s.colorRgb != newColor);
	if (!pathChanged && !tintChanged && !colorChanged)
		return;

	s.path = std::move(newPath);
	s.tintEnabled = tintEnabled;
	s.colorRgb = newColor;

	if (pathChanged) {
		InvalidateCustomSlot(slot);
		SyncTimerInterval();
	} else if (!s.frames.empty() && (tintChanged || colorChanged)) {
		RebuildTintedBitmap(slot);
	}

	if (m_enabled)
		Render();
}

void OverlayIndicators::SetSoundPath(int eventIndex, const wchar_t *path)
{
	if (eventIndex < 0 || eventIndex >= SOUND_EVENT_COUNT)
		return;
	m_soundPath[eventIndex] = path ? path : L"";
}

void OverlayIndicators::PlaySoundEvent(int eventIndex)
{
	if (eventIndex < 0 || eventIndex >= SOUND_EVENT_COUNT)
		return;
	const std::wstring &path = m_soundPath[eventIndex];
	if (path.empty())
		return;
	if (!PlaySoundW(path.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT)) {
		obs_log(LOG_WARNING, "Indicators: PlaySound failed for event %d", eventIndex);
	}
}

void OverlayIndicators::NotifyReplaySaved()
{
	const ULONGLONG now = GetTickCount64();
	m_replaySavedUntil = now + REPLAY_SAVED_DURATION_MS;
	m_replaySavedIndicatorStartTick = now;
	PlaySoundEvent(SoundReplaySaved);
	Render();
}

void OverlayIndicators::RaiseTopmost()
{
	if (!m_enabled || !m_windowManager.IsVisible()) {
		return;
	}
	m_windowManager.EnsureTopmost();
}

bool OverlayIndicators::HasAnimatedCustom() const
{
	for (int i = 0; i < CUSTOM_SLOT_COUNT; i++) {
		if (m_custom[i].frames.size() > 1)
			return true;
	}
	return false;
}

void OverlayIndicators::SyncTimerInterval()
{
	const UINT want = HasAnimatedCustom() ? 33u : 500u;
	if (m_updateTimer && m_timerIntervalMs == want)
		return;

	HWND hwnd = m_windowManager.GetHWND();
	if (!hwnd)
		return;

	if (m_updateTimer) {
		KillTimer(hwnd, m_updateTimer);
		m_updateTimer = 0;
	}

	m_timerIntervalMs = want;
	s_timerInstance = this;
	m_updateTimer = SetTimer(hwnd, 2, m_timerIntervalMs, TimerProc);
}

void OverlayIndicators::EnsureTimer()
{
	SyncTimerInterval();
}

void OverlayIndicators::UpdateStatus()
{
	const bool wasRecordingActive = m_state.recordingActive;
	const bool wasReplayActive = m_state.replayActive;
	const bool wasReplaySaving = m_state.replaySaving;
	m_stateManager.UpdateRecordingStatus(m_state);
	m_stateManager.UpdateReplayStatus(m_state);

	if (m_state.recordingActive != wasRecordingActive) {
		PlaySoundEvent(m_state.recordingActive ? SoundRecordingOn : SoundRecordingOff);
	}
	if (m_state.replayActive != wasReplayActive) {
		PlaySoundEvent(m_state.replayActive ? SoundReplayOn : SoundReplayOff);
	}

	auto updateStartTick = [](bool active, bool wasActive, ULONGLONG &startTick) {
		if (active) {
			if (!wasActive || startTick == 0) {
				startTick = GetTickCount64();
			}
		} else {
			startTick = 0;
		}
	};

	updateStartTick(m_state.recordingActive, wasRecordingActive, m_recordingIndicatorStartTick);
	updateStartTick(m_state.replayActive, wasReplayActive, m_replayIndicatorStartTick);
	updateStartTick(m_state.replaySaving, wasReplaySaving, m_replaySavingIndicatorStartTick);

	if (GetTickCount64() > m_replaySavedUntil) {
		m_replaySavedIndicatorStartTick = 0;
	}
}

float OverlayIndicators::GetOpacityFromStartTick(ULONGLONG startTick, ULONGLONG now) const
{
	if (!m_oledProtection) {
		return 1.0f;
	}
	if (startTick == 0) {
		return OLED_RESTING_OPACITY;
	}

	const ULONGLONG elapsed = now - startTick;
	if (elapsed < OLED_DIM_DELAY_MS) {
		return OLED_PEAK_OPACITY;
	}
	if (elapsed >= OLED_DIM_DELAY_MS + OLED_DIM_FADE_MS) {
		return OLED_RESTING_OPACITY;
	}

	const float t = static_cast<float>(elapsed - OLED_DIM_DELAY_MS) /
			static_cast<float>(OLED_DIM_FADE_MS);
	return OLED_PEAK_OPACITY + (OLED_RESTING_OPACITY - OLED_PEAK_OPACITY) * t;
}

void OverlayIndicators::UpdateOledOffset()
{
	if (!m_oledProtection) {
		m_oledOffsetX = 0;
		m_oledOffsetY = 0;
		return;
	}

	if (m_oledShiftStartTick == 0) {
		m_oledShiftStartTick = GetTickCount64();
	}

	const ULONGLONG elapsed = GetTickCount64() - m_oledShiftStartTick;
	const int step = static_cast<int>((elapsed / OLED_SHIFT_INTERVAL_MS) % OLED_PATTERN_COUNT);
	m_oledOffsetX = OLED_PATTERN[step][0] * OLED_SHIFT_PIXELS;
	m_oledOffsetY = OLED_PATTERN[step][1] * OLED_SHIFT_PIXELS;
}

void OverlayIndicators::UpdateWindowLayout()
{
	if (!m_enabled) {
		return;
	}
	if (m_windowWidth <= 0 || m_windowHeight <= 0) {
		return;
	}

	UpdateOledOffset();

	RECT workArea = m_layoutManager.GetScreenWorkArea();
	POINT pos = CalculateWindowPosition(workArea, m_windowWidth, m_windowHeight);
	m_windowManager.SetPosition(pos, m_windowWidth, m_windowHeight);
}

POINT OverlayIndicators::CalculateWindowPosition(const RECT &workArea, int width, int height) const
{
	POINT pos = {0, 0};
	const int margin = ScaledMargin();
	switch (m_position) {
	case OverlayPosition::Top:
		pos.x = (workArea.right - workArea.left - width) / 2;
		pos.y = margin;
		break;
	case OverlayPosition::Bottom:
		pos.x = (workArea.right - workArea.left - width) / 2;
		pos.y = (workArea.bottom - workArea.top) - height - margin;
		break;
	case OverlayPosition::Left:
		pos.x = margin;
		pos.y = (workArea.bottom - workArea.top - height) / 2;
		break;
	case OverlayPosition::Right:
		pos.x = (workArea.right - workArea.left) - width - margin;
		pos.y = (workArea.bottom - workArea.top - height) / 2;
		break;
	case OverlayPosition::TopLeft:
		pos.x = margin;
		pos.y = margin;
		break;
	case OverlayPosition::TopRight:
		pos.x = (workArea.right - workArea.left) - width - margin;
		pos.y = margin;
		break;
	case OverlayPosition::BottomLeft:
		pos.x = margin;
		pos.y = (workArea.bottom - workArea.top) - height - margin;
		break;
	case OverlayPosition::BottomRight:
		pos.x = (workArea.right - workArea.left) - width - margin;
		pos.y = (workArea.bottom - workArea.top) - height - margin;
		break;
	case OverlayPosition::Center:
	default:
		pos.x = (workArea.right - workArea.left - width) / 2;
		pos.y = (workArea.bottom - workArea.top - height) / 2;
		break;
	}

	if (m_oledProtection) {
		pos.x += m_oledOffsetX;
		pos.y += m_oledOffsetY;
		const int workWidth = workArea.right - workArea.left;
		const int workHeight = workArea.bottom - workArea.top;
		const int minX = margin;
		const int minY = margin;
		const int maxX = workWidth - width - margin;
		const int maxY = workHeight - height - margin;
		if (pos.x < minX)
			pos.x = minX;
		else if (pos.x > maxX)
			pos.x = maxX;
		if (pos.y < minY)
			pos.y = minY;
		else if (pos.y > maxY)
			pos.y = maxY;
	}

	return pos;
}

void OverlayIndicators::CreateDeviceResources()
{
	if (m_bitmapRenderTarget) {
		return;
	}

	if (m_windowWidth <= 0 || m_windowHeight <= 0) {
		m_windowWidth = ScaledPadding() * 2 + ScaledIconSize() * 3 + ScaledSpacing() * 2;
		m_windowHeight = ScaledPadding() * 2 + ScaledIconSize();
	}

	HRESULT hr = m_wicFactory->CreateBitmap(m_windowWidth, m_windowHeight, GUID_WICPixelFormat32bppPBGRA,
						WICBitmapCacheOnLoad, &m_wicBitmap);
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "Indicators: Failed to create WIC bitmap: 0x%08x", hr);
		return;
	}

	D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
		D2D1_RENDER_TARGET_TYPE_DEFAULT,
		D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
	hr = m_d2dFactory->CreateWicBitmapRenderTarget(m_wicBitmap, rtProps, &m_bitmapRenderTarget);
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "Indicators: Failed to create render target: 0x%08x", hr);
		return;
	}

	if (m_textBrush) {
		m_textBrush->Release();
		m_textBrush = nullptr;
	}
	if (m_activeBrush) {
		m_activeBrush->Release();
		m_activeBrush = nullptr;
	}
	m_bitmapRenderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f), &m_textBrush);
	m_bitmapRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.66f, 0.30f, 0.30f), &m_activeBrush);
}

void OverlayIndicators::ReleaseCustomBitmaps()
{
	for (int i = 0; i < CUSTOM_SLOT_COUNT; i++) {
		InvalidateCustomSlot(i);
	}
}

void OverlayIndicators::InvalidateCustomSlot(int slot)
{
	if (slot < 0 || slot >= CUSTOM_SLOT_COUNT)
		return;
	CustomSlot &s = m_custom[slot];
	for (CustomFrame &frame : s.frames) {
		if (frame.original) {
			frame.original->Release();
			frame.original = nullptr;
		}
		if (frame.tinted) {
			frame.tinted->Release();
			frame.tinted = nullptr;
		}
		frame.alpha.clear();
		frame.maskW = 0;
		frame.maskH = 0;
	}
	s.frames.clear();
	s.frameIndex = 0;
	s.frameStartTick = 0;
	s.loadAttempted = false;
	s.loadFailed = false;
}

void OverlayIndicators::DiscardDeviceResources()
{
	ReleaseCustomBitmaps();
	if (m_bitmapRenderTarget) {
		m_bitmapRenderTarget->Release();
		m_bitmapRenderTarget = nullptr;
	}
	if (m_wicBitmap) {
		m_wicBitmap->Release();
		m_wicBitmap = nullptr;
	}
}

D2D1_COLOR_F OverlayIndicators::SlotColor(int slot, float opacity) const
{
	if (slot < 0 || slot >= CUSTOM_SLOT_COUNT || !m_custom[slot].tintEnabled)
		return D2D1::ColorF(1.0f, 1.0f, 1.0f, opacity);
	const int rgb = m_custom[slot].colorRgb;
	const float r = ((rgb >> 16) & 0xFF) / 255.0f;
	const float g = ((rgb >> 8) & 0xFF) / 255.0f;
	const float b = (rgb & 0xFF) / 255.0f;
	return D2D1::ColorF(r, g, b, opacity);
}

void OverlayIndicators::RebuildTintedFrame(CustomFrame &frame, bool tintEnabled, int colorRgb)
{
	if (frame.tinted) {
		frame.tinted->Release();
		frame.tinted = nullptr;
	}
	if (!tintEnabled || frame.alpha.empty() || frame.maskW == 0 || frame.maskH == 0 || !m_bitmapRenderTarget)
		return;

	const float cr = ((colorRgb >> 16) & 0xFF) / 255.0f;
	const float cg = ((colorRgb >> 8) & 0xFF) / 255.0f;
	const float cb = (colorRgb & 0xFF) / 255.0f;
	const UINT stride = frame.maskW * 4;
	std::vector<uint8_t> bgra(static_cast<size_t>(stride) * static_cast<size_t>(frame.maskH));
	for (UINT y = 0; y < frame.maskH; y++) {
		for (UINT x = 0; x < frame.maskW; x++) {
			const size_t idx = static_cast<size_t>(y) * frame.maskW + x;
			const float af = frame.alpha[idx] / 255.0f;
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
	if (SUCCEEDED(m_bitmapRenderTarget->CreateBitmap(D2D1::SizeU(frame.maskW, frame.maskH), bgra.data(), stride,
							 &props, &bmp)) &&
	    bmp) {
		frame.tinted = bmp;
	}
}

void OverlayIndicators::RebuildTintedBitmap(int slot)
{
	if (slot < 0 || slot >= CUSTOM_SLOT_COUNT || !m_bitmapRenderTarget)
		return;
	CustomSlot &s = m_custom[slot];
	for (CustomFrame &frame : s.frames)
		RebuildTintedFrame(frame, s.tintEnabled, s.colorRgb);
}

bool OverlayIndicators::CreateFrameBitmaps(int slot, CustomFrame &frame, UINT w, UINT h, const uint8_t *bgra,
					   UINT stride)
{
	UNUSED_PARAMETER(slot);
	if (!m_bitmapRenderTarget || !bgra || w == 0 || h == 0)
		return false;

	if (frame.original) {
		frame.original->Release();
		frame.original = nullptr;
	}
	if (frame.tinted) {
		frame.tinted->Release();
		frame.tinted = nullptr;
	}

	frame.maskW = w;
	frame.maskH = h;
	frame.alpha.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
	for (UINT y = 0; y < h; y++) {
		for (UINT x = 0; x < w; x++) {
			const size_t src = static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4;
			frame.alpha[static_cast<size_t>(y) * w + x] = bgra[src + 3];
		}
	}

	D2D1_BITMAP_PROPERTIES props =
		D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
	ID2D1Bitmap *bmp = nullptr;
	if (FAILED(m_bitmapRenderTarget->CreateBitmap(D2D1::SizeU(w, h), bgra, stride, &props, &bmp)) || !bmp)
		return false;
	frame.original = bmp;
	return true;
}

bool OverlayIndicators::LoadRasterCustom(int slot, const std::wstring &path)
{
	if (!m_wicFactory || !m_bitmapRenderTarget)
		return false;

	std::vector<overlay::gif::DecodedFrame> decoded;
	if (!overlay::gif::DecodeFile(m_wicFactory, path, decoded) || decoded.empty())
		return false;

	CustomSlot &s = m_custom[slot];
	s.frames.clear();
	s.frames.reserve(decoded.size());
	s.frameIndex = 0;
	s.frameStartTick = 0;

	for (const overlay::gif::DecodedFrame &src : decoded) {
		CustomFrame frame;
		frame.delayMs = src.delayMs;
		if (!CreateFrameBitmaps(slot, frame, src.w, src.h, src.bgra.data(), src.stride)) {
			for (CustomFrame &f : s.frames) {
				if (f.original) {
					f.original->Release();
					f.original = nullptr;
				}
				if (f.tinted) {
					f.tinted->Release();
					f.tinted = nullptr;
				}
			}
			s.frames.clear();
			return false;
		}
		RebuildTintedFrame(frame, s.tintEnabled, s.colorRgb);
		s.frames.push_back(std::move(frame));
	}

	SyncTimerInterval();
	return !s.frames.empty() && s.frames[0].original != nullptr;
}

bool OverlayIndicators::LoadSvgCustom(int slot, const std::wstring &path)
{
	if (!m_wicFactory || !m_d2dFactory || !m_bitmapRenderTarget)
		return false;

	std::vector<uint8_t> bgra;
	UINT w = 0, h = 0, stride = 0;
	if (!overlay::svg::RasterizeFile(m_wicFactory, m_d2dFactory, path, INDICATOR_SVG_PX, bgra, w, h, stride)) {
		obs_log(LOG_WARNING, "Indicators: SVG rasterize failed for slot %d", slot);
		return false;
	}

	CustomSlot &s = m_custom[slot];
	s.frames.clear();
	s.frameIndex = 0;
	s.frameStartTick = 0;

	CustomFrame frame;
	if (!CreateFrameBitmaps(slot, frame, w, h, bgra.data(), stride))
		return false;
	RebuildTintedFrame(frame, s.tintEnabled, s.colorRgb);
	s.frames.push_back(std::move(frame));
	SyncTimerInterval();
	return true;
}

bool OverlayIndicators::EnsureCustomBitmap(int slot)
{
	if (slot < 0 || slot >= CUSTOM_SLOT_COUNT || !m_bitmapRenderTarget)
		return false;

	CustomSlot &s = m_custom[slot];
	if (s.path.empty())
		return false;
	if (!s.frames.empty() && s.frames[0].original)
		return true;
	if (s.loadAttempted)
		return false;

	s.loadAttempted = true;
	s.loadFailed = false;
	bool ok = IsSvgPath(s.path) ? LoadSvgCustom(slot, s.path) : LoadRasterCustom(slot, s.path);
	if (!ok) {
		s.loadFailed = true;
		obs_log(LOG_WARNING, "Indicators: failed to load custom icon for slot %d", slot);
	}
	return ok && !s.frames.empty() && s.frames[0].original != nullptr;
}

void OverlayIndicators::AdvanceCustomAnimation(int slot, ULONGLONG now)
{
	if (slot < 0 || slot >= CUSTOM_SLOT_COUNT)
		return;
	CustomSlot &s = m_custom[slot];
	if (s.frames.size() <= 1)
		return;
	if (s.frameStartTick == 0)
		s.frameStartTick = now;

	int guard = 0;
	while (guard++ < 64) {
		const UINT delay = s.frames[s.frameIndex].delayMs < 20 ? 20u : s.frames[s.frameIndex].delayMs;
		if (now < s.frameStartTick + delay)
			break;
		s.frameStartTick += delay;
		s.frameIndex = (s.frameIndex + 1) % s.frames.size();
		if (now > s.frameStartTick + 2000) {
			s.frameStartTick = now;
			break;
		}
	}
}

bool OverlayIndicators::DrawCustomIcon(int slot, const RECT &rect, float opacity)
{
	if (!EnsureCustomBitmap(slot))
		return false;

	CustomSlot &s = m_custom[slot];
	AdvanceCustomAnimation(slot, GetTickCount64());
	if (s.frames.empty())
		return false;
	if (s.frameIndex >= s.frames.size())
		s.frameIndex = 0;

	CustomFrame &frame = s.frames[s.frameIndex];
	ID2D1Bitmap *bmp = (s.tintEnabled && frame.tinted) ? frame.tinted : frame.original;
	if (!bmp)
		return false;

	D2D1_RECT_F dest = D2D1::RectF((float)rect.left, (float)rect.top, (float)rect.right, (float)rect.bottom);
	m_bitmapRenderTarget->DrawBitmap(bmp, dest, Clamp01(opacity), D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
	return true;
}

void OverlayIndicators::DrawBuiltinIcon(int slot, const RECT &rect, float opacity)
{
	if (!m_textBrush)
		return;

	m_textBrush->SetColor(SlotColor(slot, opacity));
	const float strokeWidth = ScaledStrokeWidth();

	auto getCenter = [](const RECT &r) {
		return D2D1_POINT_2F{(r.left + r.right) / 2.0f, (r.top + r.bottom) / 2.0f};
	};
	auto pointOnCircle = [](const D2D1_POINT_2F &center, float radius, float angleRad) {
		return D2D1_POINT_2F{center.x + radius * std::cos(angleRad),
				     center.y + radius * std::sin(angleRad)};
	};

	if (slot == 0) {
		D2D1_POINT_2F center = getCenter(rect);
		float radius = (rect.right - rect.left) * 0.32f;
		m_bitmapRenderTarget->DrawEllipse(D2D1::Ellipse(center, radius, radius), m_textBrush, strokeWidth);
		return;
	}

	if (slot == 1 && m_d2dFactory) {
		D2D1_POINT_2F center = getCenter(rect);
		float radius = (rect.right - rect.left) * 0.34f;
		float startAngle = -0.25f * 3.14159265f;
		float endAngle = 1.25f * 3.14159265f;
		D2D1_POINT_2F startPt = pointOnCircle(center, radius, startAngle);
		D2D1_POINT_2F endPt = pointOnCircle(center, radius, endAngle);

		ID2D1PathGeometry *geometry = nullptr;
		if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(&geometry)) && geometry) {
			ID2D1GeometrySink *sink = nullptr;
			if (SUCCEEDED(geometry->Open(&sink)) && sink) {
				sink->BeginFigure(startPt, D2D1_FIGURE_BEGIN_HOLLOW);
				D2D1_ARC_SEGMENT arc = {};
				arc.point = endPt;
				arc.size = D2D1::SizeF(radius, radius);
				arc.rotationAngle = 0.0f;
				arc.sweepDirection = D2D1_SWEEP_DIRECTION_CLOCKWISE;
				arc.arcSize = D2D1_ARC_SIZE_LARGE;
				sink->AddArc(arc);
				sink->EndFigure(D2D1_FIGURE_END_OPEN);
				sink->Close();
				sink->Release();
			}
			m_bitmapRenderTarget->DrawGeometry(geometry, m_textBrush, strokeWidth);
			geometry->Release();
		}

		float arrowAngle = endAngle + 0.5f;
		D2D1_POINT_2F arrowPt1 = pointOnCircle(center, radius * 0.85f, arrowAngle);
		float arrowAngle2 = endAngle - 0.4f;
		D2D1_POINT_2F arrowPt2 = pointOnCircle(center, radius * 0.85f, arrowAngle2);
		m_bitmapRenderTarget->DrawLine(arrowPt1, endPt, m_textBrush, strokeWidth);
		m_bitmapRenderTarget->DrawLine(arrowPt2, endPt, m_textBrush, strokeWidth);
		return;
	}

	if (slot == 2) {
		const float s = static_cast<float>(rect.right - rect.left);
		const float inset = s * 0.20f;
		const float labelInset = s * 0.15f;
		const float labelH = s * 0.30f;
		D2D1_RECT_F outer = D2D1::RectF((float)rect.left + inset, (float)rect.top + inset,
						(float)rect.right - inset, (float)rect.bottom - inset);
		D2D1_RECT_F label = D2D1::RectF(outer.left + labelInset, outer.top + labelInset,
						outer.right - labelInset, outer.top + labelInset + labelH);
		m_bitmapRenderTarget->DrawRectangle(outer, m_textBrush, strokeWidth);
		m_bitmapRenderTarget->DrawRectangle(label, m_textBrush, strokeWidth);
		return;
	}

	if (slot == 3) {
		D2D1_POINT_2F p1 = {(float)rect.left + 4.0f, (float)rect.top + 10.0f};
		D2D1_POINT_2F p2 = {(float)rect.left + 8.0f, (float)rect.top + 14.0f};
		D2D1_POINT_2F p3 = {(float)rect.left + 16.0f, (float)rect.top + 6.0f};
		m_bitmapRenderTarget->DrawLine(p1, p2, m_textBrush, strokeWidth);
		m_bitmapRenderTarget->DrawLine(p2, p3, m_textBrush, strokeWidth);
	}
}

void OverlayIndicators::Render()
{
	if (!m_enabled) {
		return;
	}

	const ULONGLONG now = GetTickCount64();
	const bool showReplaySaved = now <= m_replaySavedUntil;

	int count = 0;
	if (m_state.recordingActive)
		count++;
	if (m_state.replayActive)
		count++;
	if (m_state.replaySaving)
		count++;
	if (showReplaySaved)
		count++;

	if (count == 0) {
		m_windowManager.Hide();
		return;
	}

	m_windowWidth = ScaledPadding() * 2 + ScaledIconSize() * count +
			ScaledSpacing() * (count > 1 ? (count - 1) : 0);
	m_windowHeight = ScaledPadding() * 2 + ScaledIconSize();

	UpdateWindowLayout();

	if (m_wicBitmap) {
		UINT bitmapWidth = 0;
		UINT bitmapHeight = 0;
		m_wicBitmap->GetSize(&bitmapWidth, &bitmapHeight);
		if ((int)bitmapWidth != m_windowWidth || (int)bitmapHeight != m_windowHeight) {
			DiscardDeviceResources();
		}
	}

	if (!m_bitmapRenderTarget) {
		DiscardDeviceResources();
		CreateDeviceResources();
		if (!m_bitmapRenderTarget) {
			return;
		}
	}

	RenderIndicators();
	if (!m_windowManager.IsVisible()) {
		m_windowManager.Show();
	} else if (!overlay_indicators_get_overlay_visible()) {
		m_windowManager.EnsureTopmost();
	}
}

void OverlayIndicators::RenderIndicators()
{
	if (!m_bitmapRenderTarget || !m_wicBitmap) {
		return;
	}

	const ULONGLONG now = GetTickCount64();
	const bool showReplaySaved = now <= m_replaySavedUntil;

	// Rasterize custom icons (esp. SVG) before BeginDraw — nested RT creation
	// during an active draw often fails on WIC/D2D.
	if (m_state.recordingActive)
		EnsureCustomBitmap(0);
	if (m_state.replayActive)
		EnsureCustomBitmap(1);
	if (m_state.replaySaving)
		EnsureCustomBitmap(2);
	if (showReplaySaved)
		EnsureCustomBitmap(3);

	m_bitmapRenderTarget->BeginDraw();
	m_bitmapRenderTarget->SetTransform(D2D1::Matrix3x2F::Identity());
	m_bitmapRenderTarget->Clear(D2D1::ColorF(0, 0, 0, 0));

	int index = 0;
	auto drawIconRect = [&](int idx) {
		const int pad = ScaledPadding();
		const int size = ScaledIconSize();
		const int spacing = ScaledSpacing();
		int x = pad + idx * (size + spacing);
		int y = pad;
		RECT rect = {x, y, x + size, y + size};
		return rect;
	};

	auto drawSlot = [&](int slot, ULONGLONG startTick) {
		const float opacity = GetOpacityFromStartTick(startTick, now);
		RECT rect = drawIconRect(index++);
		if (!DrawCustomIcon(slot, rect, opacity))
			DrawBuiltinIcon(slot, rect, opacity);
	};

	if (m_state.recordingActive)
		drawSlot(0, m_recordingIndicatorStartTick);
	if (m_state.replayActive)
		drawSlot(1, m_replayIndicatorStartTick);
	if (m_state.replaySaving)
		drawSlot(2, m_replaySavingIndicatorStartTick);
	if (showReplaySaved)
		drawSlot(3, m_replaySavedIndicatorStartTick);

	HRESULT hr = m_bitmapRenderTarget->EndDraw();
	if (FAILED(hr)) {
		DiscardDeviceResources();
		return;
	}

	IWICBitmapLock *pLock = nullptr;
	WICRect rect = {0, 0, m_windowWidth, m_windowHeight};
	hr = m_wicBitmap->Lock(&rect, WICBitmapLockRead, &pLock);
	if (FAILED(hr)) {
		return;
	}

	UINT bufferSize = 0;
	BYTE *pData = nullptr;
	UINT stride = 0;
	hr = pLock->GetDataPointer(&bufferSize, &pData);
	if (SUCCEEDED(hr)) {
		pLock->GetStride(&stride);

		POINT ptSrc = {0, 0};
		POINT ptDst;
		RECT windowRect;
		HWND hwnd = m_windowManager.GetHWND();
		GetWindowRect(hwnd, &windowRect);
		ptDst.x = windowRect.left;
		ptDst.y = windowRect.top;
		SIZE sizeWnd = {m_windowWidth, m_windowHeight};

		HDC hdcScreen = GetDC(NULL);
		BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};

		HDC hdcMem = CreateCompatibleDC(hdcScreen);
		BITMAPINFO bmi = {0};
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = m_windowWidth;
		bmi.bmiHeader.biHeight = -m_windowHeight;
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 32;
		bmi.bmiHeader.biCompression = BI_RGB;

		BYTE *pBits = nullptr;
		int dibStride = m_windowWidth * 4;
		HBITMAP hBitmap = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, (void **)&pBits, NULL, 0);
		if (hBitmap) {
			HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);
			BYTE *pSrc = pData;
			BYTE *pDst = pBits;
			for (int y = 0; y < m_windowHeight; y++) {
				memcpy(pDst, pSrc, m_windowWidth * 4);
				pSrc += stride;
				pDst += dibStride;
			}

			UpdateLayeredWindow(hwnd, hdcScreen, &ptDst, &sizeWnd, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);

			SelectObject(hdcMem, hOldBitmap);
			DeleteObject(hBitmap);
		}

		DeleteDC(hdcMem);
		ReleaseDC(NULL, hdcScreen);
	}
	pLock->Release();
}

#endif // _WIN32
