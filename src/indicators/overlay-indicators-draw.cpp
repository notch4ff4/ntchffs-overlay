#ifdef _WIN32

#include "overlay-indicators.h"
#include <obs-module.h>

extern "C" {
#include <plugin-support.h>
}
#include <util/base.h>

namespace {
	float Clamp01(float v)
	{
		if (v < 0.0f)
			return 0.0f;
		if (v > 1.0f)
			return 1.0f;
		return v;
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

bool OverlayIndicators::EnsureCustomBitmap(int slot)
{
	if (slot < 0 || slot >= CUSTOM_SLOT_COUNT || !m_bitmapRenderTarget)
		return false;

	overlay::icon::Slot &s = m_custom[slot];
	const bool alreadyAttempted = s.loadAttempted;
	const bool ok = overlay::icon::EnsureLoaded(m_wicFactory, m_d2dFactory, m_bitmapRenderTarget, s, s.path,
						    s.tintEnabled, s.colorRgb, SvgRasterPx());
	if (!ok && s.loadFailed && !alreadyAttempted) {
		obs_log(LOG_WARNING, "Indicators: failed to load custom icon for slot %d", slot);
	}
	if (ok)
		SyncTimerInterval();
	return ok;
}

void OverlayIndicators::AdvanceCustomAnimation(int slot, ULONGLONG now)
{
	if (slot < 0 || slot >= CUSTOM_SLOT_COUNT)
		return;
	overlay::icon::AdvanceAnimation(m_custom[slot], now);
}

bool OverlayIndicators::DrawCustomIcon(int slot, const RECT &rect, float opacity)
{
	if (!EnsureCustomBitmap(slot))
		return false;

	overlay::icon::Slot &s = m_custom[slot];
	AdvanceCustomAnimation(slot, GetTickCount64());
	ID2D1Bitmap *bmp = overlay::icon::CurrentBitmap(s);
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
	overlay::icon::DrawBuiltin(m_bitmapRenderTarget, m_d2dFactory, m_textBrush, slot, rect, ScaledStrokeWidth());
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
