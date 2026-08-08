#ifdef _WIN32

#include "overlay-indicators.h"
#include <obs-module.h>

extern "C" {
#include <plugin-support.h>
bool overlay_indicators_get_overlay_visible(void);
}
#include <dxgiformat.h>
#include <util/base.h>

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
		m_wicBitmap->Release();
		m_wicBitmap = nullptr;
		return;
	}

	if (m_textBrush) {
		m_textBrush->Release();
		m_textBrush = nullptr;
	}
	m_bitmapRenderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f), &m_textBrush);
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

#endif // _WIN32
