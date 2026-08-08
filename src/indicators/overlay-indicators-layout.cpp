#ifdef _WIN32

#include "overlay-indicators.h"

namespace {
	const int INDICATOR_SIZE = 20;
	const int INDICATOR_PADDING = 8;
	const int INDICATOR_SPACING = 8;
	const int INDICATOR_MARGIN = 20;
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

int OverlayIndicators::ScaledIconSize() const
{
	const int v = INDICATOR_SIZE * m_scalePercent / 100;
	return v < 10 ? 10 : v;
}

int OverlayIndicators::ScaledPadding() const
{
	const int v = INDICATOR_PADDING * m_scalePercent / 100;
	return v < 4 ? 4 : v;
}

int OverlayIndicators::ScaledSpacing() const
{
	const int v = INDICATOR_SPACING * m_scalePercent / 100;
	return v < 4 ? 4 : v;
}

int OverlayIndicators::ScaledMargin() const
{
	const int v = INDICATOR_MARGIN * m_scalePercent / 100;
	return v < 8 ? 8 : v;
}

float OverlayIndicators::ScaledStrokeWidth() const
{
	const float w = 2.0f * static_cast<float>(m_scalePercent) / 100.0f;
	return w < 1.0f ? 1.0f : w;
}

UINT OverlayIndicators::SvgRasterPx() const
{
	const int want = ScaledIconSize() * 2;
	const UINT px = want > 0 ? static_cast<UINT>(want) : overlay::icon::kDefaultSvgPx;
	return px < overlay::icon::kDefaultSvgPx ? overlay::icon::kDefaultSvgPx : px;
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

	const float t = static_cast<float>(elapsed - OLED_DIM_DELAY_MS) / static_cast<float>(OLED_DIM_FADE_MS);
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
	const int margin = ScaledMargin();
	switch (m_position) {
	case OverlayPosition::Top:
		pos.x = (workArea.right - workArea.left - width) / 2;
		pos.y = margin;
		break;
	case OverlayPosition::Bottom:
		pos.x = (workArea.right - workArea.left - width) / 2;
		pos.y = (workArea.bottom - workArea.top) - height - margin;
		break;
	case OverlayPosition::Left:
		pos.x = margin;
		pos.y = (workArea.bottom - workArea.top - height) / 2;
		break;
	case OverlayPosition::Right:
		pos.x = (workArea.right - workArea.left) - width - margin;
		pos.y = (workArea.bottom - workArea.top - height) / 2;
		break;
	case OverlayPosition::TopLeft:
		pos.x = margin;
		pos.y = margin;
		break;
	case OverlayPosition::TopRight:
		pos.x = (workArea.right - workArea.left) - width - margin;
		pos.y = margin;
		break;
	case OverlayPosition::BottomLeft:
		pos.x = margin;
		pos.y = (workArea.bottom - workArea.top) - height - margin;
		break;
	case OverlayPosition::BottomRight:
		pos.x = (workArea.right - workArea.left) - width - margin;
		pos.y = (workArea.bottom - workArea.top) - height - margin;
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
		const int minX = margin;
		const int minY = margin;
		const int maxX = workWidth - width - margin;
		const int maxY = workHeight - height - margin;
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

#endif // _WIN32
