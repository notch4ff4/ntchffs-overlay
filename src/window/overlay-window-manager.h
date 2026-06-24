#pragma once

#ifdef _WIN32

#include <windows.h>
#include "overlay-layout.h"

class OverlayWindowManager {
public:
	OverlayWindowManager();
	~OverlayWindowManager();
	
	bool Initialize(HINSTANCE hInstance);
	void Shutdown();
	
	HWND GetHWND() const { return m_hwnd; }
	bool IsVisible() const;
	
	void Show();
	void Hide();
	void SetPosition(const POINT &pos, int width, int height);
	
	void SetClickHandler(void (*handler)(int x, int y, void *userData), void *userData);
	void SetMouseDownHandler(void (*handler)(int x, int y, void *userData), void *userData);
	void SetMouseUpHandler(void (*handler)(int x, int y, void *userData), void *userData);
	void SetMouseMoveHandler(void (*handler)(int x, int y, void *userData), void *userData);
	void SetMouseLeaveHandler(void (*handler)(void *userData), void *userData);
	void SetClickThrough(bool enabled);

	// Allow focus so the overlay receives clicks over full-screen games.
	void SetAllowActivate(bool allow);

	// Re-apply topmost periodically because games may raise themselves above the overlay.
	void EnsureTopmost();

	// EN: Handle keys only while the overlay has focus; vkCode is the virtual key, mods are 1=Shift, 2=Ctrl, 4=Alt.
	// RU: Обрабатывать клавиши только при фокусе оверлея; vkCode — виртуальная клавиша, mods: 1=Shift, 2=Ctrl, 4=Alt.
	void SetKeyHandler(void (*handler)(int vkCode, int mods, void *userData), void *userData);

private:
	static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	
	HWND m_hwnd;
	HINSTANCE m_hInstance;
	
	void (*m_clickHandler)(int x, int y, void *userData);
	void *m_clickUserData;
	void (*m_mouseDownHandler)(int x, int y, void *userData);
	void *m_mouseDownUserData;
	void (*m_mouseUpHandler)(int x, int y, void *userData);
	void *m_mouseUpUserData;
	void (*m_mouseMoveHandler)(int x, int y, void *userData);
	void *m_mouseMoveUserData;
	void (*m_mouseLeaveHandler)(void *userData);
	void *m_mouseLeaveUserData;
	void (*m_keyHandler)(int vkCode, int mods, void *userData);
	void *m_keyHandlerUserData;
	bool m_trackingMouseLeave;
	bool m_allowActivate;

	void ForceForeground();
};

#endif // _WIN32
