#pragma once

#ifdef _WIN32

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <string>
#include <vector>
#include "overlay-layout.h"
#include "overlay-state.h"
#include "overlay-window-manager.h"

class OverlayIndicators {
public:
	static const int CUSTOM_SLOT_COUNT = 4;
	static const int SOUND_EVENT_COUNT = 5;

	enum SoundEvent {
		SoundRecordingOn = 0,
		SoundRecordingOff = 1,
		SoundReplayOn = 2,
		SoundReplayOff = 3,
		SoundReplaySaved = 4
	};

	OverlayIndicators();
	~OverlayIndicators();

	bool Initialize(HINSTANCE hInstance);
	void Shutdown();

	void SetEnabled(bool enabled);
	void SetPosition(OverlayPosition position);
	void SetOledProtection(bool enabled);
	void SetScalePercent(int scalePercent);
	void SetCustom(int slot, const wchar_t *path, bool tintEnabled, int colorRgb);
	void SetSoundPath(int eventIndex, const wchar_t *path);
	void NotifyReplaySaved();
	void RaiseTopmost();

private:
	struct CustomSlot {
		std::wstring path;
		bool tintEnabled = false;
		int colorRgb = 0xFFFFFF;
		ID2D1Bitmap *original = nullptr;
		ID2D1Bitmap *tinted = nullptr;
		std::vector<uint8_t> alpha;
		UINT maskW = 0;
		UINT maskH = 0;
		bool loadAttempted = false;
		bool loadFailed = false;
	};

	void UpdateStatus();
	void Render();
	void UpdateWindowLayout();
	void EnsureTimer();
	void CreateDeviceResources();
	void DiscardDeviceResources();
	void ReleaseCustomBitmaps();
	void InvalidateCustomSlot(int slot);
	bool EnsureCustomBitmap(int slot);
	bool LoadRasterCustom(int slot, const std::wstring &path);
	bool LoadSvgCustom(int slot, const std::wstring &path);
	bool CreateBitmapsFromPixels(int slot, UINT w, UINT h, const uint8_t *bgra, UINT stride);
	void RebuildTintedBitmap(int slot);
	bool DrawCustomIcon(int slot, const RECT &rect, float opacity);
	void DrawBuiltinIcon(int slot, const RECT &rect, float opacity);
	void RenderIndicators();
	void UpdateOledOffset();
	float GetOpacityFromStartTick(ULONGLONG startTick, ULONGLONG now) const;
	POINT CalculateWindowPosition(const RECT &workArea, int width, int height) const;
	D2D1_COLOR_F SlotColor(int slot, float opacity) const;
	int ScaledIconSize() const;
	int ScaledPadding() const;
	int ScaledSpacing() const;
	int ScaledMargin() const;
	float ScaledStrokeWidth() const;
	void PlaySoundEvent(int eventIndex);

	OverlayWindowManager m_windowManager;
	OverlayStateManager m_stateManager;
	OverlayState m_state;
	OverlayLayoutManager m_layoutManager;

	ID2D1Factory *m_d2dFactory;
	IDWriteFactory *m_writeFactory;
	IWICImagingFactory *m_wicFactory;
	IWICBitmap *m_wicBitmap;
	ID2D1RenderTarget *m_bitmapRenderTarget;
	ID2D1SolidColorBrush *m_textBrush;
	ID2D1SolidColorBrush *m_activeBrush;

	CustomSlot m_custom[CUSTOM_SLOT_COUNT];
	std::wstring m_soundPath[SOUND_EVENT_COUNT];

	OverlayPosition m_position;
	bool m_enabled;
	bool m_oledProtection;
	int m_scalePercent;
	int m_oledOffsetX;
	int m_oledOffsetY;
	ULONGLONG m_oledShiftStartTick;
	ULONGLONG m_recordingIndicatorStartTick;
	ULONGLONG m_replayIndicatorStartTick;
	ULONGLONG m_replaySavingIndicatorStartTick;
	ULONGLONG m_replaySavedIndicatorStartTick;
	ULONGLONG m_replaySavedUntil;
	UINT_PTR m_updateTimer;
	int m_windowWidth;
	int m_windowHeight;

	static void CALLBACK TimerProc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time);
	static OverlayIndicators *s_timerInstance;
};

#endif // _WIN32
