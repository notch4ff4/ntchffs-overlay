#ifdef _WIN32

#include "overlay-indicators.h"
#include <obs-module.h>

extern "C" {
#include <plugin-support.h>
bool overlay_indicators_get_overlay_visible(void);
}
#include <algorithm>
#include <cmath>
#include <dxgiformat.h>
#include <util/base.h>

OverlayIndicators *OverlayIndicators::s_timerInstance = nullptr;

namespace {
	const int INDICATOR_SIZE = 20;
	const int INDICATOR_PADDING = 8;
	const int INDICATOR_SPACING = 8;
	const int INDICATOR_MARGIN = 20;
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
	  m_oledOffsetX(0),
	  m_oledOffsetY(0),
	  m_oledShiftStartTick(0),
	  m_recordingIndicatorStartTick(0),
	  m_replayIndicatorStartTick(0),
	  m_replaySavingIndicatorStartTick(0),
	  m_replaySavedIndicatorStartTick(0),
	  m_replaySavedUntil(0),
	  m_updateTimer(0),
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

void OverlayIndicators::NotifyReplaySaved()
{
	const ULONGLONG now = GetTickCount64();
	m_replaySavedUntil = now + REPLAY_SAVED_DURATION_MS;
	m_replaySavedIndicatorStartTick = now;
	Render();
}

void OverlayIndicators::RaiseTopmost()
{
	if (!m_enabled || !m_windowManager.IsVisible()) {
		return;
	}
	m_windowManager.EnsureTopmost();
}

void OverlayIndicators::EnsureTimer()
{
	if (!m_updateTimer) {
		s_timerInstance = this;
		m_updateTimer = SetTimer(m_windowManager.GetHWND(), 2, 500, TimerProc);
	}
}

void OverlayIndicators::UpdateStatus()
{
	const bool wasRecordingActive = m_state.recordingActive;
	const bool wasReplayActive = m_state.replayActive;
	const bool wasReplaySaving = m_state.replaySaving;
	m_stateManager.UpdateRecordingStatus(m_state);
	m_stateManager.UpdateReplayStatus(m_state);

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
	switch (m_position) {
	case OverlayPosition::Top:
		pos.x = (workArea.right - workArea.left - width) / 2;
		pos.y = INDICATOR_MARGIN;
		break;
	case OverlayPosition::Bottom:
		pos.x = (workArea.right - workArea.left - width) / 2;
		pos.y = (workArea.bottom - workArea.top) - height - INDICATOR_MARGIN;
		break;
	case OverlayPosition::Left:
		pos.x = INDICATOR_MARGIN;
		pos.y = (workArea.bottom - workArea.top - height) / 2;
		break;
	case OverlayPosition::Right:
		pos.x = (workArea.right - workArea.left) - width - INDICATOR_MARGIN;
		pos.y = (workArea.bottom - workArea.top - height) / 2;
		break;
	case OverlayPosition::TopLeft:
		pos.x = INDICATOR_MARGIN;
		pos.y = INDICATOR_MARGIN;
		break;
	case OverlayPosition::TopRight:
		pos.x = (workArea.right - workArea.left) - width - INDICATOR_MARGIN;
		pos.y = INDICATOR_MARGIN;
		break;
	case OverlayPosition::BottomLeft:
		pos.x = INDICATOR_MARGIN;
		pos.y = (workArea.bottom - workArea.top) - height - INDICATOR_MARGIN;
		break;
	case OverlayPosition::BottomRight:
		pos.x = (workArea.right - workArea.left) - width - INDICATOR_MARGIN;
		pos.y = (workArea.bottom - workArea.top) - height - INDICATOR_MARGIN;
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
		const int minX = INDICATOR_MARGIN;
		const int minY = INDICATOR_MARGIN;
		const int maxX = workWidth - width - INDICATOR_MARGIN;
		const int maxY = workHeight - height - INDICATOR_MARGIN;
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
		m_windowWidth = INDICATOR_PADDING * 2 + INDICATOR_SIZE * 3 + INDICATOR_SPACING * 2;
		m_windowHeight = INDICATOR_PADDING * 2 + INDICATOR_SIZE;
	}

	HRESULT hr = m_wicFactory->CreateBitmap(m_windowWidth, m_windowHeight,
	                                        GUID_WICPixelFormat32bppPBGRA,
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

	m_bitmapRenderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f), &m_textBrush);
	m_bitmapRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.66f, 0.30f, 0.30f), &m_activeBrush);
}

void OverlayIndicators::DiscardDeviceResources()
{
	if (m_bitmapRenderTarget) {
		m_bitmapRenderTarget->Release();
		m_bitmapRenderTarget = nullptr;
	}
	if (m_wicBitmap) {
		m_wicBitmap->Release();
		m_wicBitmap = nullptr;
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

	m_windowWidth = INDICATOR_PADDING * 2 + INDICATOR_SIZE * count +
	                INDICATOR_SPACING * (count > 1 ? (count - 1) : 0);
	m_windowHeight = INDICATOR_PADDING * 2 + INDICATOR_SIZE;

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

	m_bitmapRenderTarget->BeginDraw();
	m_bitmapRenderTarget->SetTransform(D2D1::Matrix3x2F::Identity());
	m_bitmapRenderTarget->Clear(D2D1::ColorF(0, 0, 0, 0));

	const ULONGLONG now = GetTickCount64();
	const bool showReplaySaved = now <= m_replaySavedUntil;

	int index = 0;
	auto drawIconRect = [&](int idx) {
		int x = INDICATOR_PADDING + idx * (INDICATOR_SIZE + INDICATOR_SPACING);
		int y = INDICATOR_PADDING;
		RECT rect = {x, y, x + INDICATOR_SIZE, y + INDICATOR_SIZE};
		return rect;
	};
	auto getCenter = [](const RECT &rect) {
		D2D1_POINT_2F center = {
			(rect.left + rect.right) / 2.0f,
			(rect.top + rect.bottom) / 2.0f
		};
		return center;
	};
	auto pointOnCircle = [](const D2D1_POINT_2F &center, float radius, float angleRad) {
		D2D1_POINT_2F pt = {
			center.x + radius * std::cos(angleRad),
			center.y + radius * std::sin(angleRad)
		};
		return pt;
	};
	const float strokeWidth = 2.0f;
	auto setIndicatorColor = [&](float opacity) {
		m_textBrush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, opacity));
	};

	if (m_state.recordingActive) {
		setIndicatorColor(GetOpacityFromStartTick(m_recordingIndicatorStartTick, now));
		RECT rect = drawIconRect(index++);
		D2D1_POINT_2F center = getCenter(rect);
		float radius = (rect.right - rect.left) * 0.32f;
		D2D1_ELLIPSE ellipse = D2D1::Ellipse(center, radius, radius);
		m_bitmapRenderTarget->DrawEllipse(ellipse, m_textBrush, strokeWidth);
	}

	if (m_state.replayActive) {
		setIndicatorColor(GetOpacityFromStartTick(m_replayIndicatorStartTick, now));
		RECT rect = drawIconRect(index++);
		if (m_d2dFactory) {
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
		}
	}

	if (m_state.replaySaving) {
		setIndicatorColor(GetOpacityFromStartTick(m_replaySavingIndicatorStartTick, now));
		RECT rect = drawIconRect(index++);
		D2D1_RECT_F outer = D2D1::RectF((float)rect.left + 4.0f, (float)rect.top + 4.0f,
		                               (float)rect.right - 4.0f, (float)rect.bottom - 4.0f);
		D2D1_RECT_F label = D2D1::RectF(outer.left + 3.0f, outer.top + 3.0f,
		                               outer.right - 3.0f, outer.top + 9.0f);
		m_bitmapRenderTarget->DrawRectangle(outer, m_textBrush, strokeWidth);
		m_bitmapRenderTarget->DrawRectangle(label, m_textBrush, strokeWidth);
	}

	if (showReplaySaved) {
		setIndicatorColor(GetOpacityFromStartTick(m_replaySavedIndicatorStartTick, now));
		RECT rect = drawIconRect(index++);
		D2D1_POINT_2F p1 = {(float)rect.left + 4.0f, (float)rect.top + 10.0f};
		D2D1_POINT_2F p2 = {(float)rect.left + 8.0f, (float)rect.top + 14.0f};
		D2D1_POINT_2F p3 = {(float)rect.left + 16.0f, (float)rect.top + 6.0f};
		m_bitmapRenderTarget->DrawLine(p1, p2, m_textBrush, strokeWidth);
		m_bitmapRenderTarget->DrawLine(p2, p3, m_textBrush, strokeWidth);
	}

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

			UpdateLayeredWindow(hwnd, hdcScreen, &ptDst, &sizeWnd,
			                   hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);

			SelectObject(hdcMem, hOldBitmap);
			DeleteObject(hBitmap);
		}

		DeleteDC(hdcMem);
		ReleaseDC(NULL, hdcScreen);
	}
	pLock->Release();
}

#endif // _WIN32
