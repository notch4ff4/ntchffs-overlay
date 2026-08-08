#ifdef _WIN32

#include "overlay-indicators.h"
#include <obs-module.h>

extern "C" {
#include <plugin-support.h>
bool overlay_indicators_get_overlay_visible(void);
}
#include <mmsystem.h>
#include <util/base.h>

#pragma comment(lib, "winmm.lib")

OverlayIndicators *OverlayIndicators::s_timerInstance = nullptr;

namespace {
	const ULONGLONG REPLAY_SAVED_DURATION_MS = 2000;
}

OverlayIndicators::OverlayIndicators()
	: m_d2dFactory(nullptr),
	  m_wicFactory(nullptr),
	  m_wicBitmap(nullptr),
	  m_bitmapRenderTarget(nullptr),
	  m_textBrush(nullptr),
	  m_comInitialized(false),
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
	  m_windowHeight(0)
{
}

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
	m_comInitialized = SUCCEEDED(hr);

	hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_wicFactory));
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "Indicators: Failed to create WIC factory: 0x%08x", hr);
		return false;
	}

	hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_d2dFactory);
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "Indicators: Failed to create D2D factory: 0x%08x", hr);
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
	if (m_wicFactory) {
		m_wicFactory->Release();
		m_wicFactory = nullptr;
	}
	if (m_d2dFactory) {
		m_d2dFactory->Release();
		m_d2dFactory = nullptr;
	}

	m_windowManager.Shutdown();
	if (m_comInitialized) {
		CoUninitialize();
		m_comInitialized = false;
	}
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

void OverlayIndicators::SetCustom(int slot, const wchar_t *path, bool tintEnabled, int colorRgb)
{
	if (slot < 0 || slot >= CUSTOM_SLOT_COUNT)
		return;

	overlay::icon::Slot &s = m_custom[slot];
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
		overlay::icon::RebuildTintedSlot(m_bitmapRenderTarget, s);
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
	overlay::icon::InvalidateSlot(m_custom[slot]);
}

#endif // _WIN32
