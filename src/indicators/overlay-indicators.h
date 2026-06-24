#pragma once

#ifdef _WIN32

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include "overlay-layout.h"
#include "overlay-state.h"
#include "overlay-window-manager.h"

class OverlayIndicators {
public:
	OverlayIndicators();
	~OverlayIndicators();

	bool Initialize(HINSTANCE hInstance);
	void Shutdown();

	void SetEnabled(bool enabled);
	void SetPosition(OverlayPosition position);
	void SetOledProtection(bool enabled);
	void NotifyReplaySaved();
	void RaiseTopmost();

private:
	void UpdateStatus();
	void Render();
	void UpdateWindowLayout();
	void EnsureTimer();
	void CreateDeviceResources();
	void DiscardDeviceResources();
	void RenderIndicators();
	void UpdateOledOffset();
	float GetOpacityFromStartTick(ULONGLONG startTick, ULONGLONG now) const;
	POINT CalculateWindowPosition(const RECT &workArea, int width, int height) const;

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

	OverlayPosition m_position;
	bool m_enabled;
	bool m_oledProtection;
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
