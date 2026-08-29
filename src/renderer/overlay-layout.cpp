#ifdef _WIN32

#include "overlay-layout.h"
#include <algorithm>

// --- Layout constants (at 100% scale) ---
const int BASE_WIDTH_HORIZONTAL = 490;
const int BASE_HEIGHT_HORIZONTAL = 140;
const int BASE_WIDTH_VERTICAL = 180;
const int BASE_HEIGHT_VERTICAL = 390;

const int PANEL_WIDTH = 140;
const int PANEL_HEIGHT = 110;
const int BUTTON_HEIGHT = 50;
const int BUTTON_SPACING = 10;

const int MAIN_MARGIN_H = 20;
const int MAIN_MARGIN_V = 15;
const int PANEL_SPACING = 15;

const int SETTINGS_GAP = 10;
const int SETTINGS_MARGIN = 10;
const int SETTINGS_BUTTON_SIZE = 26;
const int SETTINGS_CONTAINER_PADDING_H = 6;
const int SETTINGS_CONTAINER_PADDING_V = 4;
const int SETTINGS_BUTTON_SPACING = 4;

const int STATS_SPACING = 8;
const int STATS_HEIGHT = 84;
const int STATS_MARGIN = 10;
const int STATS_WIDTH_HORIZONTAL = 200;
const int STATS_WIDTH_VERTICAL = 160;

namespace {

int ScalePx(int value, int scalePercent)
{
	if (scalePercent == 100)
		return value;
	return (value * scalePercent + 50) / 100;
}

} // namespace

OverlayLayoutManager::OverlayLayoutManager() {
}

RECT OverlayLayoutManager::GetScreenWorkArea() const {
	RECT workArea = {0};
	SystemParametersInfo(SPI_GETWORKAREA, 0, &workArea, 0);
	return workArea;
}

RECT OverlayLayoutManager::GetMonitorBoundsForPoint(POINT screenPoint) const
{
	RECT monitorBounds = GetScreenWorkArea();
	HMONITOR monitor = MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST);
	if (monitor) {
		MONITORINFO monitorInfo = {};
		monitorInfo.cbSize = sizeof(MONITORINFO);
		if (GetMonitorInfo(monitor, &monitorInfo)) {
			monitorBounds = monitorInfo.rcMonitor;
		}
	}
	return monitorBounds;
}

namespace {

struct MonitorEnumContext {
	int targetIndex;
	int currentIndex;
	RECT workArea;
	RECT monitorBounds;
	bool found;
	int count;
};

BOOL CALLBACK OverlayMonitorEnumProc(HMONITOR monitor, HDC, LPRECT, LPARAM lParam)
{
	auto *ctx = reinterpret_cast<MonitorEnumContext *>(lParam);
	if (!ctx)
		return FALSE;

	if (ctx->targetIndex < 0) {
		ctx->count++;
		return TRUE;
	}

	if (ctx->currentIndex == ctx->targetIndex) {
		MONITORINFO info = {};
		info.cbSize = sizeof(MONITORINFO);
		if (GetMonitorInfo(monitor, &info)) {
			ctx->workArea = info.rcWork;
			ctx->monitorBounds = info.rcMonitor;
			ctx->found = true;
		}
		return FALSE;
	}

	ctx->currentIndex++;
	return TRUE;
}

} // namespace

int OverlayLayoutManager::GetMonitorCount() const
{
	MonitorEnumContext ctx = {};
	ctx.targetIndex = -1;
	EnumDisplayMonitors(nullptr, nullptr, OverlayMonitorEnumProc, reinterpret_cast<LPARAM>(&ctx));
	return ctx.count > 0 ? ctx.count : 1;
}

bool OverlayLayoutManager::GetMonitorInfoByIndex(int index, RECT &workArea, RECT &monitorBounds) const
{
	const int count = GetMonitorCount();
	if (count <= 0)
		return false;
	if (index < 0)
		index = 0;
	if (index >= count)
		index = count - 1;

	MonitorEnumContext ctx = {};
	ctx.targetIndex = index;
	EnumDisplayMonitors(nullptr, nullptr, OverlayMonitorEnumProc, reinterpret_cast<LPARAM>(&ctx));
	if (!ctx.found)
		return false;

	workArea = ctx.workArea;
	monitorBounds = ctx.monitorBounds;
	return true;
}

RECT OverlayLayoutManager::GetMonitorWorkAreaByIndex(int index) const
{
	RECT workArea = GetScreenWorkArea();
	RECT monitorBounds = {};
	if (GetMonitorInfoByIndex(index, workArea, monitorBounds))
		return workArea;
	return GetScreenWorkArea();
}

RECT OverlayLayoutManager::GetMonitorWorkAreaAtCursor() const
{
	POINT cursor = {};
	if (!GetCursorPos(&cursor))
		return GetScreenWorkArea();

	HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
	if (!monitor)
		return GetScreenWorkArea();

	MONITORINFO info = {};
	info.cbSize = sizeof(MONITORINFO);
	if (!GetMonitorInfo(monitor, &info))
		return GetScreenWorkArea();
	return info.rcWork;
}

RECT OverlayLayoutManager::ResolveWorkArea(OverlayMonitorMode mode, int specificIndex) const
{
	switch (mode) {
	case OverlayMonitorMode::Specific:
		return GetMonitorWorkAreaByIndex(specificIndex);
	case OverlayMonitorMode::Cursor:
		return GetMonitorWorkAreaAtCursor();
	case OverlayMonitorMode::Primary:
	default:
		return GetScreenWorkArea();
	}
}

void OverlayLayoutManager::CalculateBaseSize(OverlayOrientation orientation, int &width, int &height) {
	if (orientation == OverlayOrientation::Horizontal) {
		width = BASE_WIDTH_HORIZONTAL;
		height = BASE_HEIGHT_HORIZONTAL;
	} else {
		width = BASE_WIDTH_VERTICAL;
		height = BASE_HEIGHT_VERTICAL;
	}
}

void OverlayLayoutManager::CalculateSettingsSize(int &width, int &height) {
	int buttonCount = 2;
	width = buttonCount * SETTINGS_BUTTON_SIZE +
	        (buttonCount - 1) * SETTINGS_BUTTON_SPACING +
	        SETTINGS_CONTAINER_PADDING_H * 2;
	height = SETTINGS_BUTTON_SIZE + SETTINGS_CONTAINER_PADDING_V * 2;
}

void OverlayLayoutManager::CalculateWindowOffset(OverlayPosition position, bool statsVisible,
                                                 const OverlayLayout &layout, int &offsetX, int &offsetY) {
	offsetX = 0;
	offsetY = 0;

	// Anchor side follows screen corner so controls stay on-screen.
	bool settingsOnLeft = (position == OverlayPosition::Right ||
	                      position == OverlayPosition::TopRight ||
	                      position == OverlayPosition::BottomRight);

	bool statsAbove = (position == OverlayPosition::Bottom ||
	                  position == OverlayPosition::BottomLeft ||
	                  position == OverlayPosition::BottomRight);

	if (settingsOnLeft) {
		int settingsWidth, settingsHeight;
		CalculateSettingsSize(settingsWidth, settingsHeight);
		offsetX = SETTINGS_MARGIN + settingsWidth + SETTINGS_GAP;
	}

	if (statsAbove && statsVisible) {
		offsetY = STATS_MARGIN + STATS_HEIGHT + STATS_SPACING;
	}
}

void OverlayLayoutManager::CalculateWindowSize(OverlayPosition position, OverlayOrientation orientation,
                                              bool statsVisible, const OverlayLayout &layout,
                                              int &width, int &height) {
	int baseWidth, baseHeight;
	CalculateBaseSize(orientation, baseWidth, baseHeight);

	int settingsWidth = 0, settingsHeight = 0;
	CalculateSettingsSize(settingsWidth, settingsHeight);

	bool settingsOnLeft = (position == OverlayPosition::Right ||
	                      position == OverlayPosition::TopRight ||
	                      position == OverlayPosition::BottomRight);

	bool statsAbove = (position == OverlayPosition::Bottom ||
	                  position == OverlayPosition::BottomLeft ||
	                  position == OverlayPosition::BottomRight);

	int extraWidthLeft = 0;
	int extraWidthRight = 0;
	if (settingsWidth > 0) {
		if (settingsOnLeft) {
			extraWidthLeft = SETTINGS_GAP + settingsWidth + SETTINGS_MARGIN;
		} else {
			extraWidthRight = SETTINGS_GAP + settingsWidth + SETTINGS_MARGIN;
		}
	}

	int extraHeightTop = 0;
	int extraHeightBottom = 0;
	if (statsVisible) {
		if (statsAbove) {
			extraHeightTop = STATS_SPACING + STATS_HEIGHT + STATS_MARGIN;
		} else {
			extraHeightBottom = STATS_SPACING + STATS_HEIGHT + STATS_MARGIN;
		}
	}

	width = baseWidth + extraWidthLeft + extraWidthRight;
	height = baseHeight + extraHeightTop + extraHeightBottom;
}

void OverlayLayoutManager::CalculateMainOverlayRect(const OverlayLayout &layout, RECT &rect) {
	rect.left = layout.offsetX;
	rect.top = layout.offsetY;
	rect.right = rect.left + layout.baseWidth;
	rect.bottom = rect.top + layout.baseHeight;
}

void OverlayLayoutManager::CalculateSettingsRect(OverlayPosition position, const OverlayLayout &layout, RECT &rect) {
	int settingsWidth, settingsHeight;
	CalculateSettingsSize(settingsWidth, settingsHeight);

	int marginTop = 8;
	int gapFromMain = SETTINGS_GAP;

	bool placeOnLeft = (position == OverlayPosition::Right ||
	                   position == OverlayPosition::TopRight ||
	                   position == OverlayPosition::BottomRight);

	if (placeOnLeft) {
		rect.left = SETTINGS_MARGIN;
		rect.top = layout.offsetY + marginTop;
	} else {
		rect.left = layout.offsetX + layout.baseWidth + gapFromMain;
		rect.top = layout.offsetY + marginTop;
	}

	rect.right = rect.left + settingsWidth;
	rect.bottom = rect.top + settingsHeight;
}

void OverlayLayoutManager::CalculateStatsRect(OverlayPosition position, OverlayOrientation orientation,
                                            bool statsVisible, const OverlayLayout &layout, RECT &rect) {
	if (!statsVisible) {
		rect = {0};
		return;
	}

	int statsWidth = (orientation == OverlayOrientation::Horizontal) ?
	                 STATS_WIDTH_HORIZONTAL : STATS_WIDTH_VERTICAL;
	int statsHeight = STATS_HEIGHT;

	bool placeAbove = (position == OverlayPosition::Bottom ||
	                  position == OverlayPosition::BottomLeft ||
	                  position == OverlayPosition::BottomRight);

	int margin = 10;
	rect.left = layout.offsetX + layout.baseWidth - statsWidth - margin;

	if (placeAbove) {
		rect.top = STATS_MARGIN;
	} else {
		rect.top = layout.offsetY + layout.baseHeight + STATS_SPACING;
	}

	rect.right = rect.left + statsWidth;
	rect.bottom = rect.top + statsHeight;
}

void OverlayLayoutManager::CalculatePanelRects(OverlayOrientation orientation, const OverlayLayout &layout) {
	RECT &recRect = const_cast<OverlayLayout&>(layout).recordingPanelRect;
	RECT &repRect = const_cast<OverlayLayout&>(layout).replayPanelRect;
	RECT &buttonsRect = const_cast<OverlayLayout&>(layout).buttonsContainerRect;
	RECT &folderRect = const_cast<OverlayLayout&>(layout).recordFolderButtonRect;
	RECT &obsRect = const_cast<OverlayLayout&>(layout).showOBSButtonRect;

	if (orientation == OverlayOrientation::Horizontal) {
		recRect.left = layout.offsetX + MAIN_MARGIN_H;
		recRect.top = layout.offsetY + MAIN_MARGIN_V;
		recRect.right = recRect.left + PANEL_WIDTH;
		recRect.bottom = recRect.top + PANEL_HEIGHT;

		repRect.left = recRect.right + PANEL_SPACING;
		repRect.top = recRect.top;
		repRect.right = repRect.left + PANEL_WIDTH;
		repRect.bottom = repRect.top + PANEL_HEIGHT;

		buttonsRect.left = repRect.right + PANEL_SPACING;
		buttonsRect.top = recRect.top;
		buttonsRect.right = buttonsRect.left + PANEL_WIDTH;
		buttonsRect.bottom = buttonsRect.top + PANEL_HEIGHT;
	} else {
		recRect.left = layout.offsetX + MAIN_MARGIN_H;
		recRect.top = layout.offsetY + MAIN_MARGIN_V;
		recRect.right = recRect.left + PANEL_WIDTH;
		recRect.bottom = recRect.top + PANEL_HEIGHT;

		repRect.left = recRect.left;
		repRect.top = recRect.bottom + PANEL_SPACING;
		repRect.right = repRect.left + PANEL_WIDTH;
		repRect.bottom = repRect.top + PANEL_HEIGHT;

		buttonsRect.left = recRect.left;
		buttonsRect.top = repRect.bottom + PANEL_SPACING;
		buttonsRect.right = buttonsRect.left + PANEL_WIDTH;
		buttonsRect.bottom = buttonsRect.top + PANEL_HEIGHT;
	}

	folderRect.left = buttonsRect.left;
	folderRect.top = buttonsRect.top;
	folderRect.right = folderRect.left + PANEL_WIDTH;
	folderRect.bottom = folderRect.top + BUTTON_HEIGHT;

	obsRect.left = buttonsRect.left;
	obsRect.top = folderRect.bottom + BUTTON_SPACING;
	obsRect.right = obsRect.left + PANEL_WIDTH;
	obsRect.bottom = obsRect.top + BUTTON_HEIGHT;

	RECT &settingsBtnRect = const_cast<OverlayLayout&>(layout).settingsButtonRect;
	RECT &statsBtnRect = const_cast<OverlayLayout&>(layout).statsButtonRect;

	if (layout.settingsRect.right > 0) {
		settingsBtnRect.left = layout.settingsRect.left + SETTINGS_CONTAINER_PADDING_H;
		settingsBtnRect.top = layout.settingsRect.top + SETTINGS_CONTAINER_PADDING_V;
		settingsBtnRect.right = settingsBtnRect.left + SETTINGS_BUTTON_SIZE;
		settingsBtnRect.bottom = settingsBtnRect.top + SETTINGS_BUTTON_SIZE;

		statsBtnRect.left = settingsBtnRect.right + SETTINGS_BUTTON_SPACING;
		statsBtnRect.top = settingsBtnRect.top;
		statsBtnRect.right = statsBtnRect.left + SETTINGS_BUTTON_SIZE;
		statsBtnRect.bottom = statsBtnRect.top + SETTINGS_BUTTON_SIZE;
	}
}

void OverlayLayoutManager::CalculateLayout(OverlayPosition position, int margin,
                                          OverlayOrientation orientation,
                                          bool statsVisible,
                                          int scalePercent,
                                          OverlayLayout &layout) {
	memset(&layout, 0, sizeof(layout));

	if (scalePercent < 50)
		scalePercent = 50;
	if (scalePercent > 300)
		scalePercent = 300;

	CalculateBaseSize(orientation, layout.baseWidth, layout.baseHeight);

	int settingsWidth, settingsHeight;
	CalculateSettingsSize(settingsWidth, settingsHeight);

	CalculateWindowSize(position, orientation, statsVisible, layout,
	                   layout.windowWidth, layout.windowHeight);

	CalculateWindowOffset(position, statsVisible, layout, layout.offsetX, layout.offsetY);

	CalculateMainOverlayRect(layout, layout.mainOverlayRect);

	layout.settingsOnLeft = (position == OverlayPosition::Right ||
	                        position == OverlayPosition::TopRight ||
	                        position == OverlayPosition::BottomRight);

	layout.statsAbove = (position == OverlayPosition::Bottom ||
	                    position == OverlayPosition::BottomLeft ||
	                    position == OverlayPosition::BottomRight);

	CalculateSettingsRect(position, layout, layout.settingsRect);
	CalculateStatsRect(position, orientation, statsVisible, layout, layout.statsRect);

	CalculatePanelRects(orientation, layout);

	if (scalePercent == 100)
		return;

	auto scaleRect = [&](RECT &r) {
		if (r.right <= r.left && r.bottom <= r.top)
			return;
		r.left = ScalePx(r.left, scalePercent);
		r.top = ScalePx(r.top, scalePercent);
		r.right = ScalePx(r.right, scalePercent);
		r.bottom = ScalePx(r.bottom, scalePercent);
	};

	layout.baseWidth = ScalePx(layout.baseWidth, scalePercent);
	layout.baseHeight = ScalePx(layout.baseHeight, scalePercent);
	layout.windowWidth = ScalePx(layout.windowWidth, scalePercent);
	layout.windowHeight = ScalePx(layout.windowHeight, scalePercent);
	layout.offsetX = ScalePx(layout.offsetX, scalePercent);
	layout.offsetY = ScalePx(layout.offsetY, scalePercent);

	scaleRect(layout.mainOverlayRect);
	scaleRect(layout.settingsRect);
	scaleRect(layout.statsRect);
	scaleRect(layout.recordingPanelRect);
	scaleRect(layout.replayPanelRect);
	scaleRect(layout.buttonsContainerRect);
	scaleRect(layout.recordFolderButtonRect);
	scaleRect(layout.showOBSButtonRect);
	scaleRect(layout.settingsButtonRect);
	scaleRect(layout.statsButtonRect);
}

POINT OverlayLayoutManager::CalculateWindowPosition(OverlayPosition position, int margin,
                                                    const OverlayLayout &layout) {
	return CalculateWindowPosition(position, margin, layout, GetScreenWorkArea());
}

POINT OverlayLayoutManager::CalculateWindowPosition(OverlayPosition position, int margin,
                                                    const OverlayLayout &layout, const RECT &workArea) {
	POINT windowPos = {0, 0};

	int mainOverlayX = 0, mainOverlayY = 0;
	const int workWidth = workArea.right - workArea.left;
	const int workHeight = workArea.bottom - workArea.top;

	switch (position) {
	case OverlayPosition::Top:
		mainOverlayX = (workWidth - layout.baseWidth) / 2;
		mainOverlayY = margin;
		break;
	case OverlayPosition::Bottom:
		mainOverlayX = (workWidth - layout.baseWidth) / 2;
		mainOverlayY = workHeight - layout.baseHeight - margin;
		break;
	case OverlayPosition::Left:
		mainOverlayX = margin;
		mainOverlayY = (workHeight - layout.baseHeight) / 2;
		break;
	case OverlayPosition::Right:
		mainOverlayX = workWidth - layout.baseWidth - margin;
		mainOverlayY = (workHeight - layout.baseHeight) / 2;
		break;
	case OverlayPosition::TopLeft:
		mainOverlayX = margin;
		mainOverlayY = margin;
		break;
	case OverlayPosition::TopRight:
		mainOverlayX = workWidth - layout.baseWidth - margin;
		mainOverlayY = margin;
		break;
	case OverlayPosition::BottomLeft:
		mainOverlayX = margin;
		mainOverlayY = workHeight - layout.baseHeight - margin;
		break;
	case OverlayPosition::BottomRight:
		mainOverlayX = workWidth - layout.baseWidth - margin;
		mainOverlayY = workHeight - layout.baseHeight - margin;
		break;
	case OverlayPosition::Center:
		mainOverlayX = (workWidth - layout.baseWidth) / 2;
		mainOverlayY = (workHeight - layout.baseHeight) / 2;
		break;
	}

	// Window origin is main-overlay position minus the adaptive container offset.
	windowPos.x = workArea.left + mainOverlayX - layout.offsetX;
	windowPos.y = workArea.top + mainOverlayY - layout.offsetY;

	return windowPos;
}

#endif // _WIN32
