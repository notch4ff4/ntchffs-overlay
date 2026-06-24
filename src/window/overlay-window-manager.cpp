#ifdef _WIN32

#include "overlay-window-manager.h"
#include <obs-module.h>

extern "C" {
#include <plugin-support.h>
}

OverlayWindowManager::OverlayWindowManager()
	: m_hwnd(nullptr), m_hInstance(nullptr),
	  m_clickHandler(nullptr), m_clickUserData(nullptr),
	  m_mouseDownHandler(nullptr), m_mouseDownUserData(nullptr),
	  m_mouseUpHandler(nullptr), m_mouseUpUserData(nullptr),
	  m_mouseMoveHandler(nullptr), m_mouseMoveUserData(nullptr),
	m_mouseLeaveHandler(nullptr), m_mouseLeaveUserData(nullptr),
	  m_keyHandler(nullptr), m_keyHandlerUserData(nullptr),
	  m_trackingMouseLeave(false), m_allowActivate(false) {
}

OverlayWindowManager::~OverlayWindowManager() {
	Shutdown();
}

LRESULT CALLBACK OverlayWindowManager::WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	OverlayWindowManager *manager = reinterpret_cast<OverlayWindowManager *>(
		GetWindowLongPtr(hwnd, GWLP_USERDATA));
	if (msg == WM_NCCREATE) {
		CREATESTRUCT *cs = reinterpret_cast<CREATESTRUCT *>(lParam);
		manager = static_cast<OverlayWindowManager *>(cs->lpCreateParams);
		SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(manager));
	}
	if (!manager) {
		return DefWindowProc(hwnd, msg, wParam, lParam);
	}
	
	switch (msg) {
	case WM_PAINT: {
		PAINTSTRUCT ps;
		BeginPaint(hwnd, &ps);
		EndPaint(hwnd, &ps);
		return 0;
	}
	
	case WM_LBUTTONDOWN: {
		if (manager->m_allowActivate) {
			manager->ForceForeground();
		}
		int x = LOWORD(lParam);
		int y = HIWORD(lParam);
		if (manager->m_mouseDownHandler) {
			manager->m_mouseDownHandler(x, y, manager->m_mouseDownUserData);
		}
		if (manager->m_clickHandler) {
			manager->m_clickHandler(x, y, manager->m_clickUserData);
		}
		return 0;
	}

	case WM_LBUTTONUP: {
		int x = LOWORD(lParam);
		int y = HIWORD(lParam);
		if (manager->m_mouseUpHandler) {
			manager->m_mouseUpHandler(x, y, manager->m_mouseUpUserData);
		}
		return 0;
	}
	
	case WM_MOUSEMOVE: {
		int x = LOWORD(lParam);
		int y = HIWORD(lParam);
		if (manager->m_mouseMoveHandler) {
			manager->m_mouseMoveHandler(x, y, manager->m_mouseMoveUserData);
		}
		if (!manager->m_trackingMouseLeave) {
			TRACKMOUSEEVENT tme = {};
			tme.cbSize = sizeof(TRACKMOUSEEVENT);
			tme.dwFlags = TME_LEAVE;
			tme.hwndTrack = hwnd;
			if (TrackMouseEvent(&tme)) {
				manager->m_trackingMouseLeave = true;
			}
		}
		SetCursor(LoadCursor(NULL, IDC_ARROW));
		return 0;
	}
	
	case WM_MOUSELEAVE: {
		manager->m_trackingMouseLeave = false;
		if (manager->m_mouseLeaveHandler) {
			manager->m_mouseLeaveHandler(manager->m_mouseLeaveUserData);
		}
		return 0;
	}
	
	case WM_MOUSEACTIVATE: {
		if (manager->m_allowActivate) {
			manager->ForceForeground();
			return MA_ACTIVATE;
		}
		return DefWindowProc(hwnd, msg, wParam, lParam);
	}
	
	case WM_NCHITTEST: {
		POINT pt = {LOWORD(lParam), HIWORD(lParam)};
		ScreenToClient(hwnd, &pt);

		return HTCLIENT;
	}
	
	case WM_TIMER:
		return 0;

	case WM_KEYDOWN:
	case WM_SYSKEYDOWN: {
		if (manager->m_keyHandler) {
			int vk = static_cast<int>(wParam);
			int mods = 0;
			if (GetKeyState(VK_SHIFT) & 0x8000)
				mods |= 1;
			if (GetKeyState(VK_CONTROL) & 0x8000)
				mods |= 2;
			if (GetKeyState(VK_MENU) & 0x8000)
				mods |= 4;
			manager->m_keyHandler(vk, mods, manager->m_keyHandlerUserData);
		}
		return 0;
	}
	
	case WM_DESTROY:
		return 0;
	
	default:
		return DefWindowProc(hwnd, msg, wParam, lParam);
	}
}

bool OverlayWindowManager::Initialize(HINSTANCE hInstance) {
	m_hInstance = hInstance;
	
	WNDCLASSEX wc = {0};
	wc.cbSize = sizeof(WNDCLASSEX);
	wc.lpfnWndProc = WindowProc;
	wc.hInstance = hInstance;
	wc.lpszClassName = L"OBSOverlayWindow";
	wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	
	// Re-register on hot reload so a stale class does not block creation.
	UnregisterClassW(L"OBSOverlayWindow", hInstance);
	
	if (!RegisterClassExW(&wc)) {
		DWORD error = GetLastError();
		if (error != ERROR_CLASS_ALREADY_EXISTS) {
			obs_log(LOG_ERROR, "Failed to register window class: %lu", error);
			return false;
		}
	}
	
	// Layered topmost popup stays above games without activating or appearing in Alt+Tab.
	m_hwnd = CreateWindowExW(
		WS_EX_LAYERED |
		WS_EX_TOPMOST |
		WS_EX_NOACTIVATE |
		WS_EX_TOOLWINDOW,
		L"OBSOverlayWindow",
		NULL,
		WS_POPUP,
		0, 0, 800, 600,
		NULL, NULL, hInstance, this
	);
	
	if (!m_hwnd) {
		DWORD error = GetLastError();
		obs_log(LOG_ERROR, "Failed to create overlay window: %lu", error);
		return false;
	}
	
	// UpdateLayeredWindow preserves per-pixel alpha that SetLayeredWindowAttributes cannot provide.
	
	obs_log(LOG_INFO, "Overlay window created successfully");
	return true;
}

void OverlayWindowManager::Shutdown() {
	if (m_hwnd) {
		DestroyWindow(m_hwnd);
		m_hwnd = nullptr;
	}
}

bool OverlayWindowManager::IsVisible() const {
	if (!m_hwnd) {
		return false;
	}
	return IsWindowVisible(m_hwnd) != 0;
}

void OverlayWindowManager::Show() {
	if (!m_hwnd) {
		return;
	}
	
	if (m_allowActivate) {
		ShowWindow(m_hwnd, SW_SHOW);
		SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
		             SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
		ForceForeground();
	} else {
		ShowWindow(m_hwnd, SW_SHOWNA);
		SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
		             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
	}
	
	InvalidateRect(m_hwnd, NULL, TRUE);
	UpdateWindow(m_hwnd);
}

void OverlayWindowManager::Hide() {
	if (!m_hwnd) {
		return;
	}
	
	ShowWindow(m_hwnd, SW_HIDE);
}

void OverlayWindowManager::SetPosition(const POINT &pos, int width, int height) {
	if (!m_hwnd) {
		return;
	}
	SetWindowPos(m_hwnd, HWND_TOPMOST, pos.x, pos.y, width, height,
	             SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void OverlayWindowManager::SetClickHandler(void (*handler)(int x, int y, void *userData), void *userData) {
	m_clickHandler = handler;
	m_clickUserData = userData;
}

void OverlayWindowManager::SetMouseDownHandler(void (*handler)(int x, int y, void *userData), void *userData) {
	m_mouseDownHandler = handler;
	m_mouseDownUserData = userData;
}

void OverlayWindowManager::SetMouseUpHandler(void (*handler)(int x, int y, void *userData), void *userData) {
	m_mouseUpHandler = handler;
	m_mouseUpUserData = userData;
}

void OverlayWindowManager::SetMouseMoveHandler(void (*handler)(int x, int y, void *userData), void *userData) {
	m_mouseMoveHandler = handler;
	m_mouseMoveUserData = userData;
}

void OverlayWindowManager::SetMouseLeaveHandler(void (*handler)(void *userData), void *userData) {
	m_mouseLeaveHandler = handler;
	m_mouseLeaveUserData = userData;
}

void OverlayWindowManager::SetKeyHandler(void (*handler)(int vkCode, int mods, void *userData), void *userData) {
	m_keyHandler = handler;
	m_keyHandlerUserData = userData;
}

void OverlayWindowManager::SetClickThrough(bool enabled) {
	if (!m_hwnd) {
		return;
	}
	LONG_PTR exStyle = GetWindowLongPtr(m_hwnd, GWL_EXSTYLE);
	if (enabled) {
		exStyle |= WS_EX_TRANSPARENT;
	} else {
		exStyle &= ~WS_EX_TRANSPARENT;
	}
	SetWindowLongPtr(m_hwnd, GWL_EXSTYLE, exStyle);
	SetWindowPos(m_hwnd, NULL, 0, 0, 0, 0,
	             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
		             SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void OverlayWindowManager::SetAllowActivate(bool allow) {
	m_allowActivate = allow;
	if (!m_hwnd) {
		return;
	}
	LONG_PTR exStyle = GetWindowLongPtr(m_hwnd, GWL_EXSTYLE);
	if (allow) {
		exStyle &= ~WS_EX_NOACTIVATE;
	} else {
		exStyle |= WS_EX_NOACTIVATE;
	}
	SetWindowLongPtr(m_hwnd, GWL_EXSTYLE, exStyle);
	SetWindowPos(m_hwnd, NULL, 0, 0, 0, 0,
	             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
		             SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void OverlayWindowManager::EnsureTopmost() {
	if (!m_hwnd || !IsWindowVisible(m_hwnd)) {
		return;
	}
	SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
	             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	if (m_allowActivate) {
		ForceForeground();
	}
}

void OverlayWindowManager::ForceForeground() {
	if (!m_hwnd || !IsWindow(m_hwnd)) {
		return;
	}
	HWND hFore = GetForegroundWindow();
	if (hFore == m_hwnd) {
		return;
	}
	// Avoid stealing focus from in-process dialogs such as settings or delete confirmation.
	if (hFore) {
		DWORD forePid = 0;
		GetWindowThreadProcessId(hFore, &forePid);
		if (forePid == GetCurrentProcessId()) {
			return;
		}
	}
	DWORD foreThread = 0;
	DWORD ourThread = GetCurrentThreadId();
	if (hFore) {
		foreThread = GetWindowThreadProcessId(hFore, NULL);
	}
	if (foreThread != 0 && foreThread != ourThread) {
		AttachThreadInput(ourThread, foreThread, TRUE);
	}
	BringWindowToTop(m_hwnd);
	ShowWindow(m_hwnd, SW_SHOW);
	SetForegroundWindow(m_hwnd);
	if (foreThread != 0 && foreThread != ourThread) {
		AttachThreadInput(ourThread, foreThread, FALSE);
	}
}

#endif // _WIN32
