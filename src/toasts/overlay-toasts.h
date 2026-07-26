#pragma once

#ifdef _WIN32

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <deque>
#include <string>
#include <vector>
#include "overlay-layout.h"
#include "overlay-state.h"
#include "overlay-window-manager.h"

class OverlayToasts {
public:
	enum ActionKind {
		ActionViewFile = 0,
		ActionDeleteFile = 1
	};

	struct ToastAction {
		int vk = 0;
		int mod = 0;
		ActionKind kind = ActionViewFile;
		int hotkeyId = 0;
	};

	struct ToastItem {
		std::wstring text;
		ULONGLONG durationMs = 0;
		std::wstring filePath;
		std::vector<ToastAction> actions;
		std::string id;
	};

	OverlayToasts();
	~OverlayToasts();

	bool Initialize(HINSTANCE hInstance);
	void Shutdown();

	void SetActionHotkeys(int viewVk, int viewMod, int deleteVk, int deleteMod);
	void SetAccentColor(int colorRgb);
	void SetViewFileHandler(void (*handler)(const wchar_t *path, void *userData), void *userData);
	void SetEnabled(bool enabled);
	void SetVideoSavedEnabled(bool enabled);
	void SetOpenHintEnabled(bool enabled);
	void SetLowDiskEnabled(bool enabled);
	void SetLowDiskThresholdGb(int gigabytes);
	void SetDurationMs(ULONGLONG durationMs);
	void Show(const wchar_t *text, ULONGLONG durationMs, const char *id = nullptr);
	void ShowReplaySaved(const wchar_t *path);
	void ShowOpenHint(const wchar_t *hotkeyLabel);
	void TickDiskSpace();
	void RaiseTopmost();

private:
	enum class AnimPhase { Idle, Enter, Hold, Exit };

	static const int HOTKEY_VIEW = 1;
	static const int HOTKEY_DELETE = 2;

	void Enqueue(ToastItem item);
	void ActivateCurrent();
	void StartExit();
	void FinishExitAndPromote();
	void DismissCurrent();
	void RegisterActionHotkeys();
	void UnregisterActionHotkeys();
	void RefreshCurrentActionHotkeys();
	void OnHotkey(int id);
	void HandleView();
	void HandleDelete();
	void CheckDiskSpace();
	void Update();
	void Render();
	void EnsureTimer(UINT intervalMs);
	void CreateDeviceResources();
	void DiscardDeviceResources();
	void UpdateWindowLayout();
	POINT CalculateWindowPosition(const RECT &workArea, int width, int height) const;
	float SlideProgressAt(double nowSec) const;
	void SetAnimTimerBoost(bool enabled);
	D2D1_SIZE_F MeasureText(const std::wstring &text, float fontSize, float maxWidth) const;
	std::wstring FormatWithPlaceholders(const std::wstring &fmt, const std::wstring &p1,
					    const std::wstring &p2 = std::wstring()) const;
	std::wstring HotkeyName(int vk, int mod) const;
	UINT WinHotkeyMods(int mod) const;
	bool HasId(const std::string &id) const;

	void ApplyAccentBrushes();
	D2D1_COLOR_F AccentColor(float alpha = 1.0f) const;
	D2D1_COLOR_F AccentPanel(float alpha = 1.0f) const;
	D2D1_COLOR_F AccentBorder(float alpha = 1.0f) const;
	static D2D1_COLOR_F ColorFromRgb(int rgb, float alpha = 1.0f);

	static void CALLBACK TimerProc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time);
	static void HotkeyCallback(int id, void *userData);
	static OverlayToasts *s_timerInstance;

	OverlayWindowManager m_windowManager;
	OverlayStateManager m_stateManager;
	OverlayLayoutManager m_layoutManager;

	ID2D1Factory *m_d2dFactory;
	IDWriteFactory *m_writeFactory;
	IWICImagingFactory *m_wicFactory;
	IWICBitmap *m_wicBitmap;
	ID2D1RenderTarget *m_bitmapRenderTarget;
	ID2D1SolidColorBrush *m_bgBrush;
	ID2D1SolidColorBrush *m_borderBrush;
	ID2D1SolidColorBrush *m_accentBrush;
	ID2D1SolidColorBrush *m_textBrush;

	std::deque<ToastItem> m_queue;
	ToastItem m_current;
	bool m_hasCurrent;
	bool m_hotkeysRegistered;
	AnimPhase m_animPhase;
	double m_animStartSec;
	double m_holdUntilSec;
	float m_slideProgress;
	int m_accentColorRgb;
	bool m_enabled;
	bool m_videoSavedEnabled;
	bool m_openHintEnabled;
	bool m_lowDiskEnabled;
	int m_lowDiskThresholdGb;
	ULONGLONG m_durationMs;
	int m_viewVk;
	int m_viewMod;
	int m_deleteVk;
	int m_deleteMod;
	void (*m_viewFileHandler)(const wchar_t *path, void *userData);
	void *m_viewFileUserData;
	ULONGLONG m_lastDiskCheckTick;
	ULONGLONG m_lastLowDiskToastTick;
	UINT_PTR m_updateTimer;
	UINT m_timerIntervalMs;
	int m_windowWidth;
	int m_windowHeight;
	bool m_timerPeriodBoosted;
};

#endif // _WIN32
