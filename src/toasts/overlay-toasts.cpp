// Should be refactored. Plan to separate into smaller files. (will be removed after checking)
// Some parts of this file are written by AI. Should be checked.
#ifdef _WIN32

#include "overlay-toasts.h"
#include "overlay-string-utils.h"
#include <obs-module.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <algorithm>
#include <cmath>
#include <dxgiformat.h>
#include <util/base.h>

#pragma comment(lib, "winmm.lib")

extern "C" {
#include <plugin-support.h>
}

OverlayToasts *OverlayToasts::s_timerInstance = nullptr;

namespace {
	const int TOAST_MARGIN = 24;
	const int TOAST_PAD_X = 20;
	const int TOAST_PAD_Y = 16;
	const int TOAST_ACCENT_W = 4;
	const int TOAST_MAX_WIDTH = 420;
	const float TOAST_FONT_SIZE = 13.0f;
	const float TOAST_RADIUS = 14.0f;
	const ULONGLONG LOW_DISK_COOLDOWN_MS = 30ull * 60ull * 1000ull;
	const int LOW_DISK_THRESHOLD_GB_DEFAULT = 5;
	const int LOW_DISK_THRESHOLD_GB_MIN = 1;
	const int LOW_DISK_THRESHOLD_GB_MAX = 100;
	const ULONGLONG TOAST_DURATION_MS_DEFAULT = 8000;
	const ULONGLONG TOAST_DURATION_MS_MIN = 2000;
	const ULONGLONG TOAST_DURATION_MS_MAX = 60000;
	const ULONGLONG DISK_CHECK_INTERVAL_MS = 5000;
	const ULONGLONG SLIDE_IN_MS = 520;
	const ULONGLONG SLIDE_OUT_MS = 380;
	const UINT TIMER_IDLE_MS = 500;
	const UINT TIMER_ANIM_MS = 8;

	float Clamp01(float v)
	{
		if (v < 0.0f)
			return 0.0f;
		if (v > 1.0f)
			return 1.0f;
		return v;
	}

	// Continuous, gentle ease — soft start and soft landing (less "snap").
	float Smootherstep(float t)
	{
		t = Clamp01(t);
		return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
	}

	float FadeOpacity(float slideProgress)
	{
		// Fade slightly ahead of the slide so the card materializes softly.
		const float t = Clamp01(slideProgress * 1.15f);
		return Smootherstep(t);
	}

	double NowSeconds()
	{
		static LARGE_INTEGER freq = {};
		if (freq.QuadPart == 0)
			QueryPerformanceFrequency(&freq);
		LARGE_INTEGER counter = {};
		QueryPerformanceCounter(&counter);
		return static_cast<double>(counter.QuadPart) / static_cast<double>(freq.QuadPart);
	}
}

OverlayToasts::OverlayToasts()
	: m_d2dFactory(nullptr),
	  m_writeFactory(nullptr),
	  m_wicFactory(nullptr),
	  m_wicBitmap(nullptr),
	  m_bitmapRenderTarget(nullptr),
	  m_bgBrush(nullptr),
	  m_borderBrush(nullptr),
	  m_accentBrush(nullptr),
	  m_textBrush(nullptr),
	  m_hasCurrent(false),
	  m_hotkeysRegistered(false),
	  m_animPhase(AnimPhase::Idle),
	  m_animStartSec(0.0),
	  m_holdUntilSec(0.0),
	  m_slideProgress(0.0f),
	  m_accentColorRgb(0xA882FF),
	  m_enabled(true),
	  m_videoSavedEnabled(true),
	  m_openHintEnabled(true),
	  m_lowDiskEnabled(true),
	  m_lowDiskThresholdGb(LOW_DISK_THRESHOLD_GB_DEFAULT),
	  m_durationMs(TOAST_DURATION_MS_DEFAULT),
	  m_viewVk(VK_F7),
	  m_viewMod(2),
	  m_deleteVk(VK_F8),
	  m_deleteMod(2),
	  m_viewFileHandler(nullptr),
	  m_viewFileUserData(nullptr),
	  m_lastDiskCheckTick(0),
	  m_lastLowDiskToastTick(0),
	  m_updateTimer(0),
	  m_timerIntervalMs(0),
	  m_windowWidth(0),
	  m_windowHeight(0),
	  m_timerPeriodBoosted(false)
{
}

OverlayToasts::~OverlayToasts()
{
	Shutdown();
}

void CALLBACK OverlayToasts::TimerProc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time)
{
	UNUSED_PARAMETER(hwnd);
	UNUSED_PARAMETER(msg);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(time);

	if (s_timerInstance)
		s_timerInstance->Update();
}

void OverlayToasts::HotkeyCallback(int id, void *userData)
{
	OverlayToasts *self = static_cast<OverlayToasts *>(userData);
	if (self)
		self->OnHotkey(id);
}

bool OverlayToasts::Initialize(HINSTANCE hInstance)
{
	if (!m_windowManager.Initialize(hInstance))
		return false;

	m_windowManager.SetClickThrough(true);
	m_windowManager.SetHotkeyHandler(HotkeyCallback, this);

	HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
		obs_log(LOG_ERROR, "Toasts: Failed to initialize COM: 0x%08x", hr);
		return false;
	}

	hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_wicFactory));
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "Toasts: Failed to create WIC factory: 0x%08x", hr);
		return false;
	}

	hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_d2dFactory);
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "Toasts: Failed to create D2D factory: 0x%08x", hr);
		return false;
	}

	hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
				 reinterpret_cast<IUnknown **>(&m_writeFactory));
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "Toasts: Failed to create DWrite factory: 0x%08x", hr);
		return false;
	}

	EnsureTimer(TIMER_IDLE_MS);
	return true;
}

void OverlayToasts::Shutdown()
{
	UnregisterActionHotkeys();
	SetAnimTimerBoost(false);

	if (m_updateTimer) {
		KillTimer(m_windowManager.GetHWND(), m_updateTimer);
		m_updateTimer = 0;
	}
	s_timerInstance = nullptr;

	m_queue.clear();
	m_hasCurrent = false;
	m_animPhase = AnimPhase::Idle;

	DiscardDeviceResources();

	if (m_bgBrush) {
		m_bgBrush->Release();
		m_bgBrush = nullptr;
	}
	if (m_borderBrush) {
		m_borderBrush->Release();
		m_borderBrush = nullptr;
	}
	if (m_accentBrush) {
		m_accentBrush->Release();
		m_accentBrush = nullptr;
	}
	if (m_textBrush) {
		m_textBrush->Release();
		m_textBrush = nullptr;
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

void OverlayToasts::EnsureTimer(UINT intervalMs)
{
	if (m_updateTimer && m_timerIntervalMs == intervalMs)
		return;

	HWND hwnd = m_windowManager.GetHWND();
	if (!hwnd)
		return;

	if (m_updateTimer) {
		KillTimer(hwnd, m_updateTimer);
		m_updateTimer = 0;
	}

	s_timerInstance = this;
	m_timerIntervalMs = intervalMs;
	m_updateTimer = SetTimer(hwnd, 3, intervalMs, TimerProc);
}

void OverlayToasts::SetAnimTimerBoost(bool enabled)
{
	if (enabled == m_timerPeriodBoosted)
		return;
	if (enabled)
		timeBeginPeriod(1);
	else
		timeEndPeriod(1);
	m_timerPeriodBoosted = enabled;
}

std::wstring OverlayToasts::FormatWithPlaceholders(const std::wstring &fmt, const std::wstring &p1,
						   const std::wstring &p2) const
{
	std::wstring out = fmt;
	auto replaceAll = [](std::wstring &s, const wchar_t *from, const std::wstring &to) {
		const size_t fromLen = wcslen(from);
		size_t pos = 0;
		while ((pos = s.find(from, pos)) != std::wstring::npos) {
			s.replace(pos, fromLen, to);
			pos += to.size();
		}
	};
	replaceAll(out, L"%1", p1);
	if (!p2.empty())
		replaceAll(out, L"%2", p2);
	return out;
}

bool OverlayToasts::HasId(const std::string &id) const
{
	if (id.empty())
		return false;
	if (m_hasCurrent && m_current.id == id)
		return true;
	for (const auto &item : m_queue) {
		if (item.id == id)
			return true;
	}
	return false;
}

void OverlayToasts::SetActionHotkeys(int viewVk, int viewMod, int deleteVk, int deleteMod)
{
	m_viewVk = viewVk > 0 ? viewVk : VK_F7;
	m_viewMod = viewMod & 7;
	m_deleteVk = deleteVk > 0 ? deleteVk : VK_F8;
	m_deleteMod = deleteMod & 7;
	RefreshCurrentActionHotkeys();
}

void OverlayToasts::SetAccentColor(int colorRgb)
{
	const int rgb = colorRgb & 0xFFFFFF;
	if (m_accentColorRgb == rgb)
		return;
	m_accentColorRgb = rgb;
	ApplyAccentBrushes();
	if (m_hasCurrent)
		Render();
}

void OverlayToasts::SetViewFileHandler(void (*handler)(const wchar_t *path, void *userData), void *userData)
{
	m_viewFileHandler = handler;
	m_viewFileUserData = userData;
}

void OverlayToasts::SetEnabled(bool enabled)
{
	if (m_enabled == enabled)
		return;
	m_enabled = enabled;
	if (!m_enabled) {
		UnregisterActionHotkeys();
		m_queue.clear();
		m_hasCurrent = false;
		m_current = ToastItem();
		m_animPhase = AnimPhase::Idle;
		m_slideProgress = 0.0f;
		m_windowManager.Hide();
		SetAnimTimerBoost(false);
		EnsureTimer(TIMER_IDLE_MS);
	}
}

void OverlayToasts::SetVideoSavedEnabled(bool enabled)
{
	m_videoSavedEnabled = enabled;
}

void OverlayToasts::SetOpenHintEnabled(bool enabled)
{
	m_openHintEnabled = enabled;
}

void OverlayToasts::SetLowDiskEnabled(bool enabled)
{
	m_lowDiskEnabled = enabled;
}

void OverlayToasts::SetLowDiskThresholdGb(int gigabytes)
{
	if (gigabytes < LOW_DISK_THRESHOLD_GB_MIN)
		gigabytes = LOW_DISK_THRESHOLD_GB_MIN;
	if (gigabytes > LOW_DISK_THRESHOLD_GB_MAX)
		gigabytes = LOW_DISK_THRESHOLD_GB_MAX;
	m_lowDiskThresholdGb = gigabytes;
}

void OverlayToasts::SetDurationMs(ULONGLONG durationMs)
{
	if (durationMs < TOAST_DURATION_MS_MIN)
		durationMs = TOAST_DURATION_MS_MIN;
	if (durationMs > TOAST_DURATION_MS_MAX)
		durationMs = TOAST_DURATION_MS_MAX;
	m_durationMs = durationMs;
}

std::wstring OverlayToasts::HotkeyName(int vk, int mod) const
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

UINT OverlayToasts::WinHotkeyMods(int mod) const
{
	UINT winMods = MOD_NOREPEAT;
	if (mod & 1)
		winMods |= MOD_SHIFT;
	if (mod & 2)
		winMods |= MOD_CONTROL;
	if (mod & 4)
		winMods |= MOD_ALT;
	return winMods;
}

void OverlayToasts::Show(const wchar_t *text, ULONGLONG durationMs, const char *id)
{
	if (!m_enabled || !text || !text[0])
		return;

	ToastItem item;
	item.text = text;
	item.durationMs = durationMs > 0 ? durationMs : m_durationMs;
	if (id)
		item.id = id;
	Enqueue(item);
}

void OverlayToasts::ShowReplaySaved(const wchar_t *path)
{
	if (!m_enabled || !m_videoSavedEnabled || !path || !path[0])
		return;

	ToastItem item;
	item.id = "replay_saved";
	item.filePath = path;
	item.durationMs = m_durationMs;
	item.text = FormatWithPlaceholders(overlay::util::ModuleTextW("Toast.ReplaySaved"),
					   HotkeyName(m_viewVk, m_viewMod), HotkeyName(m_deleteVk, m_deleteMod));

	ToastAction view;
	view.vk = m_viewVk;
	view.mod = m_viewMod;
	view.kind = ActionViewFile;
	view.hotkeyId = HOTKEY_VIEW;
	item.actions.push_back(view);

	ToastAction del;
	del.vk = m_deleteVk;
	del.mod = m_deleteMod;
	del.kind = ActionDeleteFile;
	del.hotkeyId = HOTKEY_DELETE;
	item.actions.push_back(del);

	Enqueue(item);
}

void OverlayToasts::ShowOpenHint(const wchar_t *hotkeyLabel)
{
	if (!m_enabled || !m_openHintEnabled)
		return;
	const wchar_t *label = (hotkeyLabel && hotkeyLabel[0]) ? hotkeyLabel : L"?";
	ToastItem item;
	item.id = "open_hint";
	item.durationMs = m_durationMs;
	item.text = FormatWithPlaceholders(overlay::util::ModuleTextW("Toast.OpenOverlayHint"), label);
	Enqueue(item);
}

void OverlayToasts::TickDiskSpace()
{
	if (!m_enabled || !m_lowDiskEnabled)
		return;
	CheckDiskSpace();
}

void OverlayToasts::RaiseTopmost()
{
	if (m_hasCurrent && m_windowManager.IsVisible())
		m_windowManager.EnsureTopmost();
}

void OverlayToasts::ActivateCurrent()
{
	m_hasCurrent = true;
	m_animPhase = AnimPhase::Enter;
	m_animStartSec = NowSeconds();
	m_slideProgress = 0.0f;
	m_holdUntilSec = 0.0;
	RegisterActionHotkeys();
	SetAnimTimerBoost(true);
	EnsureTimer(TIMER_ANIM_MS);
	Render();
}

void OverlayToasts::Enqueue(ToastItem item)
{
	if (!item.id.empty()) {
		if (m_hasCurrent && m_current.id == item.id && m_animPhase != AnimPhase::Exit) {
			const ULONGLONG duration = item.durationMs > 0 ? item.durationMs : m_current.durationMs;
			UnregisterActionHotkeys();
			m_current = std::move(item);
			m_current.durationMs = duration;
			if (m_animPhase == AnimPhase::Hold)
				m_holdUntilSec = NowSeconds() + (duration / 1000.0);
			RegisterActionHotkeys();
			Render();
			return;
		}
		for (auto &queued : m_queue) {
			if (queued.id == item.id) {
				queued = std::move(item);
				return;
			}
		}
	}

	if (!m_hasCurrent) {
		m_current = std::move(item);
		ActivateCurrent();
		return;
	}

	m_queue.push_back(std::move(item));
}

void OverlayToasts::StartExit()
{
	if (!m_hasCurrent || m_animPhase == AnimPhase::Exit)
		return;

	UnregisterActionHotkeys();
	m_animPhase = AnimPhase::Exit;
	m_animStartSec = NowSeconds();
	SetAnimTimerBoost(true);
	EnsureTimer(TIMER_ANIM_MS);
	Render();
}

void OverlayToasts::FinishExitAndPromote()
{
	m_hasCurrent = false;
	m_current = ToastItem();
	m_animPhase = AnimPhase::Idle;
	m_slideProgress = 0.0f;

	if (m_queue.empty()) {
		m_windowManager.Hide();
		SetAnimTimerBoost(false);
		EnsureTimer(TIMER_IDLE_MS);
		return;
	}

	m_current = std::move(m_queue.front());
	m_queue.pop_front();
	ActivateCurrent();
}

void OverlayToasts::DismissCurrent()
{
	StartExit();
}

void OverlayToasts::RegisterActionHotkeys()
{
	UnregisterActionHotkeys();
	if (!m_hasCurrent || m_current.actions.empty())
		return;

	HWND hwnd = m_windowManager.GetHWND();
	if (!hwnd)
		return;

	for (const ToastAction &action : m_current.actions) {
		if (action.vk <= 0 || action.hotkeyId <= 0)
			continue;
		RegisterHotKey(hwnd, action.hotkeyId, WinHotkeyMods(action.mod), static_cast<UINT>(action.vk));
	}
	m_hotkeysRegistered = true;
}

void OverlayToasts::RefreshCurrentActionHotkeys()
{
	if (!m_hasCurrent || m_current.id != "replay_saved")
		return;

	for (ToastAction &action : m_current.actions) {
		if (action.kind == ActionViewFile) {
			action.vk = m_viewVk;
			action.mod = m_viewMod;
		} else if (action.kind == ActionDeleteFile) {
			action.vk = m_deleteVk;
			action.mod = m_deleteMod;
		}
	}
	m_current.text = FormatWithPlaceholders(overlay::util::ModuleTextW("Toast.ReplaySaved"),
						HotkeyName(m_viewVk, m_viewMod), HotkeyName(m_deleteVk, m_deleteMod));
	RegisterActionHotkeys();
	Render();
}

void OverlayToasts::UnregisterActionHotkeys()
{
	if (!m_hotkeysRegistered)
		return;

	HWND hwnd = m_windowManager.GetHWND();
	if (hwnd) {
		UnregisterHotKey(hwnd, HOTKEY_VIEW);
		UnregisterHotKey(hwnd, HOTKEY_DELETE);
	}
	m_hotkeysRegistered = false;
}

void OverlayToasts::OnHotkey(int id)
{
	if (!m_hasCurrent || m_animPhase == AnimPhase::Exit)
		return;

	for (const ToastAction &action : m_current.actions) {
		if (action.hotkeyId != id)
			continue;
		if (action.kind == ActionViewFile) {
			HandleView();
			return;
		}
		if (action.kind == ActionDeleteFile) {
			HandleDelete();
			return;
		}
	}
}

void OverlayToasts::HandleView()
{
	if (!m_hasCurrent || m_current.filePath.empty())
		return;

	const std::wstring path = m_current.filePath;
	if (m_viewFileHandler)
		m_viewFileHandler(path.c_str(), m_viewFileUserData);
	else
		ShellExecuteW(NULL, L"open", path.c_str(), NULL, NULL, SW_SHOWDEFAULT);
	DismissCurrent();
}

void OverlayToasts::HandleDelete()
{
	if (!m_hasCurrent || m_current.filePath.empty())
		return;

	const std::wstring path = m_current.filePath;
	const std::wstring name = path.substr(path.find_last_of(L"\\/") + 1);

	UnregisterActionHotkeys();

	const int ret = MessageBoxW(NULL, name.c_str(), overlay::util::ModuleTextW("DeleteFile").c_str(),
				    MB_YESNO | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
	if (ret == IDYES) {
		if (DeleteFileW(path.c_str())) {
			obs_log(LOG_INFO, "Toasts: deleted replay file");
		} else {
			obs_log(LOG_WARNING, "Toasts: failed to delete replay file (%lu)", GetLastError());
		}
		DismissCurrent();
		return;
	}

	if (m_hasCurrent && m_animPhase != AnimPhase::Exit)
		RegisterActionHotkeys();
}

void OverlayToasts::CheckDiskSpace()
{
	if (!m_enabled || !m_lowDiskEnabled)
		return;

	const ULONGLONG now = GetTickCount64();
	if (m_lastLowDiskToastTick != 0 && (now - m_lastLowDiskToastTick) < LOW_DISK_COOLDOWN_MS)
		return;
	if (HasId("low_disk"))
		return;

	std::string path = m_stateManager.GetConfiguredRecordingPath();
	if (path.empty())
		path = m_stateManager.GetRecordingPath();
	if (path.empty())
		return;

	WCHAR widePath[MAX_PATH];
	if (MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, widePath, MAX_PATH) <= 0)
		return;

	ULARGE_INTEGER freeBytes = {};
	ULARGE_INTEGER totalBytes = {};
	if (!GetDiskFreeSpaceExW(widePath, &freeBytes, &totalBytes, NULL))
		return;

	const ULONGLONG thresholdBytes =
		static_cast<ULONGLONG>(m_lowDiskThresholdGb) * 1024ull * 1024ull * 1024ull;
	if (freeBytes.QuadPart >= thresholdBytes)
		return;

	m_lastLowDiskToastTick = now;
	Show(overlay::util::ModuleTextW("Toast.LowDiskSpace").c_str(), m_durationMs, "low_disk");
}

float OverlayToasts::SlideProgressAt(double nowSec) const
{
	if (m_animPhase == AnimPhase::Enter) {
		const float t = static_cast<float>((nowSec - m_animStartSec) * 1000.0 / static_cast<double>(SLIDE_IN_MS));
		return Smootherstep(t);
	}
	if (m_animPhase == AnimPhase::Exit) {
		const float t = static_cast<float>((nowSec - m_animStartSec) * 1000.0 / static_cast<double>(SLIDE_OUT_MS));
		return 1.0f - Smootherstep(t);
	}
	if (m_animPhase == AnimPhase::Hold)
		return 1.0f;
	return 0.0f;
}

void OverlayToasts::Update()
{
	const ULONGLONG nowTick = GetTickCount64();
	if (m_enabled && m_lowDiskEnabled &&
	    (m_lastDiskCheckTick == 0 || (nowTick - m_lastDiskCheckTick) >= DISK_CHECK_INTERVAL_MS)) {
		m_lastDiskCheckTick = nowTick;
		CheckDiskSpace();
	}

	if (!m_hasCurrent) {
		SetAnimTimerBoost(false);
		EnsureTimer(TIMER_IDLE_MS);
		return;
	}

	const double nowSec = NowSeconds();

	if (m_animPhase == AnimPhase::Enter) {
		m_slideProgress = SlideProgressAt(nowSec);
		if ((nowSec - m_animStartSec) * 1000.0 >= static_cast<double>(SLIDE_IN_MS)) {
			m_slideProgress = 1.0f;
			m_animPhase = AnimPhase::Hold;
			m_holdUntilSec = nowSec + (m_current.durationMs / 1000.0);
			SetAnimTimerBoost(false);
			EnsureTimer(TIMER_IDLE_MS);
		}
		Render();
		return;
	}

	if (m_animPhase == AnimPhase::Hold) {
		if (nowSec >= m_holdUntilSec) {
			StartExit();
			return;
		}
		EnsureTimer(TIMER_IDLE_MS);
		return;
	}

	if (m_animPhase == AnimPhase::Exit) {
		m_slideProgress = SlideProgressAt(nowSec);
		if ((nowSec - m_animStartSec) * 1000.0 >= static_cast<double>(SLIDE_OUT_MS)) {
			FinishExitAndPromote();
			return;
		}
		Render();
	}
}

D2D1_SIZE_F OverlayToasts::MeasureText(const std::wstring &text, float fontSize, float maxWidth) const
{
	D2D1_SIZE_F size = {0, 0};
	if (!m_writeFactory || text.empty())
		return size;

	IDWriteTextFormat *format = nullptr;
	HRESULT hr = m_writeFactory->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_SEMI_BOLD,
						      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSize,
						      L"en-us", &format);
	if (FAILED(hr) || !format)
		return size;

	format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
	format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
	format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

	IDWriteTextLayout *layout = nullptr;
	hr = m_writeFactory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, maxWidth,
					      400.0f, &layout);
	if (SUCCEEDED(hr) && layout) {
		DWRITE_TEXT_METRICS metrics = {};
		if (SUCCEEDED(layout->GetMetrics(&metrics))) {
			size.width = metrics.widthIncludingTrailingWhitespace;
			size.height = metrics.height;
		}
		layout->Release();
	}
	format->Release();
	return size;
}

void OverlayToasts::UpdateWindowLayout()
{
	if (m_windowWidth <= 0 || m_windowHeight <= 0)
		return;

	RECT workArea = m_layoutManager.GetScreenWorkArea();
	POINT pos = CalculateWindowPosition(workArea, m_windowWidth, m_windowHeight);
	m_windowManager.SetPosition(pos, m_windowWidth, m_windowHeight);
}

POINT OverlayToasts::CalculateWindowPosition(const RECT &workArea, int width, int height) const
{
	const float progress = Clamp01(m_slideProgress);
	const float hidden = 1.0f - progress;

	POINT pos = {};
	pos.x = workArea.left + TOAST_MARGIN;
	pos.y = workArea.top + TOAST_MARGIN;

	// Slide in from the left edge with a short soft travel distance.
	const float travelX = static_cast<float>(width) * 0.55f + static_cast<float>(TOAST_MARGIN);
	const float travelY = 14.0f;
	pos.x -= static_cast<int>(std::lround(travelX * hidden));
	pos.y -= static_cast<int>(std::lround(travelY * hidden));
	return pos;
}

namespace {

void ToastRgbToHsl(float r, float g, float b, float &h, float &s, float &l)
{
	const float maxc = (std::max)(r, (std::max)(g, b));
	const float minc = (std::min)(r, (std::min)(g, b));
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

float ToastHueToRgb(float p, float q, float t)
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

void ToastHslToRgb(float h, float s, float l, float &r, float &g, float &b)
{
	if (s <= 0.0001f) {
		r = g = b = l;
		return;
	}
	const float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
	const float p = 2.0f * l - q;
	r = ToastHueToRgb(p, q, h + 1.0f / 3.0f);
	g = ToastHueToRgb(p, q, h);
	b = ToastHueToRgb(p, q, h - 1.0f / 3.0f);
}

D2D1_COLOR_F ToastDeriveAccentShade(int rgb, float satMul, float lightMul, float alpha)
{
	float r = ((rgb >> 16) & 0xFF) / 255.0f;
	float g = ((rgb >> 8) & 0xFF) / 255.0f;
	float b = (rgb & 0xFF) / 255.0f;
	float h = 0.0f, s = 0.0f, l = 0.0f;
	ToastRgbToHsl(r, g, b, h, s, l);
	s = (std::max)(0.0f, (std::min)(1.0f, s * satMul));
	l = (std::max)(0.0f, (std::min)(1.0f, l * lightMul));
	ToastHslToRgb(h, s, l, r, g, b);
	return D2D1::ColorF(r, g, b, alpha);
}

} // namespace

D2D1_COLOR_F OverlayToasts::ColorFromRgb(int rgb, float alpha)
{
	return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha);
}

D2D1_COLOR_F OverlayToasts::AccentColor(float alpha) const
{
	return ColorFromRgb(m_accentColorRgb, alpha);
}

D2D1_COLOR_F OverlayToasts::AccentPanel(float alpha) const
{
	return ToastDeriveAccentShade(m_accentColorRgb, 0.45f, 0.15f, alpha);
}

D2D1_COLOR_F OverlayToasts::AccentBorder(float alpha) const
{
	return ToastDeriveAccentShade(m_accentColorRgb, 0.55f, 0.42f, alpha);
}

void OverlayToasts::ApplyAccentBrushes()
{
	if (!m_bitmapRenderTarget)
		return;
	if (m_bgBrush)
		m_bgBrush->SetColor(AccentPanel(0.96f));
	if (m_borderBrush)
		m_borderBrush->SetColor(AccentBorder(0.95f));
	if (m_accentBrush)
		m_accentBrush->SetColor(AccentColor());
}

void OverlayToasts::CreateDeviceResources()
{
	if (m_bitmapRenderTarget)
		return;
	if (m_windowWidth <= 0 || m_windowHeight <= 0)
		return;

	HRESULT hr = m_wicFactory->CreateBitmap(m_windowWidth, m_windowHeight, GUID_WICPixelFormat32bppPBGRA,
						WICBitmapCacheOnLoad, &m_wicBitmap);
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "Toasts: Failed to create WIC bitmap: 0x%08x", hr);
		return;
	}

	D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
		D2D1_RENDER_TARGET_TYPE_DEFAULT,
		D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
	hr = m_d2dFactory->CreateWicBitmapRenderTarget(m_wicBitmap, rtProps, &m_bitmapRenderTarget);
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "Toasts: Failed to create render target: 0x%08x", hr);
		return;
	}

	if (m_bgBrush) {
		m_bgBrush->Release();
		m_bgBrush = nullptr;
	}
	if (m_borderBrush) {
		m_borderBrush->Release();
		m_borderBrush = nullptr;
	}
	if (m_accentBrush) {
		m_accentBrush->Release();
		m_accentBrush = nullptr;
	}
	if (m_textBrush) {
		m_textBrush->Release();
		m_textBrush = nullptr;
	}

	m_bitmapRenderTarget->CreateSolidColorBrush(AccentPanel(0.96f), &m_bgBrush);
	m_bitmapRenderTarget->CreateSolidColorBrush(AccentBorder(0.95f), &m_borderBrush);
	m_bitmapRenderTarget->CreateSolidColorBrush(AccentColor(), &m_accentBrush);
	m_bitmapRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.92f, 0.94f, 0.98f, 1.0f), &m_textBrush);
}

void OverlayToasts::DiscardDeviceResources()
{
	if (m_bitmapRenderTarget) {
		m_bitmapRenderTarget->Release();
		m_bitmapRenderTarget = nullptr;
	}
	if (m_wicBitmap) {
		m_wicBitmap->Release();
		m_wicBitmap = nullptr;
	}
	if (m_bgBrush) {
		m_bgBrush->Release();
		m_bgBrush = nullptr;
	}
	if (m_borderBrush) {
		m_borderBrush->Release();
		m_borderBrush = nullptr;
	}
	if (m_accentBrush) {
		m_accentBrush->Release();
		m_accentBrush = nullptr;
	}
	if (m_textBrush) {
		m_textBrush->Release();
		m_textBrush = nullptr;
	}
}

void OverlayToasts::Render()
{
	if (!m_hasCurrent) {
		m_windowManager.Hide();
		return;
	}

	const float maxTextWidth = static_cast<float>(TOAST_MAX_WIDTH - TOAST_PAD_X * 2 - TOAST_ACCENT_W);
	D2D1_SIZE_F textSize = MeasureText(m_current.text, TOAST_FONT_SIZE, maxTextWidth);
	const int textW = static_cast<int>(std::ceil(textSize.width));
	const int textH = static_cast<int>(std::ceil(textSize.height));
	m_windowWidth = (std::max)(220, textW + TOAST_PAD_X * 2 + TOAST_ACCENT_W);
	m_windowHeight = (std::max)(56, textH + TOAST_PAD_Y * 2);
	if (m_windowWidth > TOAST_MAX_WIDTH)
		m_windowWidth = TOAST_MAX_WIDTH;

	UpdateWindowLayout();

	if (m_wicBitmap) {
		UINT bitmapWidth = 0;
		UINT bitmapHeight = 0;
		m_wicBitmap->GetSize(&bitmapWidth, &bitmapHeight);
		if ((int)bitmapWidth != m_windowWidth || (int)bitmapHeight != m_windowHeight)
			DiscardDeviceResources();
	}

	if (!m_bitmapRenderTarget) {
		CreateDeviceResources();
		if (!m_bitmapRenderTarget)
			return;
	}

	const float opacity = FadeOpacity(m_slideProgress);

	m_bitmapRenderTarget->BeginDraw();
	m_bitmapRenderTarget->SetTransform(D2D1::Matrix3x2F::Identity());
	m_bitmapRenderTarget->Clear(D2D1::ColorF(0, 0, 0, 0));

	D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
		D2D1::RectF(0.5f, 0.5f, static_cast<float>(m_windowWidth) - 0.5f,
			    static_cast<float>(m_windowHeight) - 0.5f),
		TOAST_RADIUS, TOAST_RADIUS);

	if (m_bgBrush) {
		m_bgBrush->SetOpacity(opacity);
		m_bitmapRenderTarget->FillRoundedRectangle(rr, m_bgBrush);
	}
	if (m_borderBrush) {
		m_borderBrush->SetOpacity(opacity);
		m_bitmapRenderTarget->DrawRoundedRectangle(rr, m_borderBrush, 1.0f);
	}

	if (m_accentBrush) {
		m_accentBrush->SetOpacity(opacity);
		D2D1_ROUNDED_RECT accent = D2D1::RoundedRect(
			D2D1::RectF(1.0f, 8.0f, 1.0f + static_cast<float>(TOAST_ACCENT_W),
				    static_cast<float>(m_windowHeight) - 8.0f),
			2.0f, 2.0f);
		m_bitmapRenderTarget->FillRoundedRectangle(accent, m_accentBrush);
	}

	if (m_writeFactory && m_textBrush) {
		m_textBrush->SetOpacity(opacity);
		IDWriteTextFormat *format = nullptr;
		HRESULT hr = m_writeFactory->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_SEMI_BOLD,
							      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
							      TOAST_FONT_SIZE, L"en-us", &format);
		if (SUCCEEDED(hr) && format) {
			format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
			format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
			format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
			D2D1_RECT_F textRect = D2D1::RectF(static_cast<float>(TOAST_PAD_X + TOAST_ACCENT_W),
							  static_cast<float>(TOAST_PAD_Y),
							  static_cast<float>(m_windowWidth - TOAST_PAD_X),
							  static_cast<float>(m_windowHeight - TOAST_PAD_Y));
			m_bitmapRenderTarget->DrawTextW(m_current.text.c_str(),
							static_cast<UINT32>(m_current.text.size()), format, textRect,
							m_textBrush);
			format->Release();
		}
	}

	HRESULT hr = m_bitmapRenderTarget->EndDraw();
	if (FAILED(hr)) {
		DiscardDeviceResources();
		return;
	}

	IWICBitmapLock *pLock = nullptr;
	WICRect rect = {0, 0, m_windowWidth, m_windowHeight};
	hr = m_wicBitmap->Lock(&rect, WICBitmapLockRead, &pLock);
	if (FAILED(hr))
		return;

	UINT bufferSize = 0;
	BYTE *pData = nullptr;
	UINT stride = 0;
	hr = pLock->GetDataPointer(&bufferSize, &pData);
	if (SUCCEEDED(hr)) {
		pLock->GetStride(&stride);

		RECT workArea = m_layoutManager.GetScreenWorkArea();
		POINT ptDst = CalculateWindowPosition(workArea, m_windowWidth, m_windowHeight);
		POINT ptSrc = {0, 0};
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
		const int dibStride = m_windowWidth * 4;
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
			UpdateLayeredWindow(m_windowManager.GetHWND(), hdcScreen, &ptDst, &sizeWnd, hdcMem, &ptSrc, 0,
					    &blend, ULW_ALPHA);
			SelectObject(hdcMem, hOldBitmap);
			DeleteObject(hBitmap);
		}
		DeleteDC(hdcMem);
		ReleaseDC(NULL, hdcScreen);
	}
	pLock->Release();

	if (!m_windowManager.IsVisible())
		m_windowManager.Show();
	else
		m_windowManager.EnsureTopmost();
}

#endif // _WIN32
