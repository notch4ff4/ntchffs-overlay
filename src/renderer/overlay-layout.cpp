#ifdef _WIN32

#include "overlay-layout.h"
#include <algorithm>

// --- Layout constants ---
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
                                          OverlayLayout &layout) {
	memset(&layout, 0, sizeof(layout));

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
}

POINT OverlayLayoutManager::CalculateWindowPosition(OverlayPosition position, int margin,
                                                    const OverlayLayout &layout) {
	RECT workArea = GetScreenWorkArea();
	POINT windowPos = {0, 0};

	int mainOverlayX = 0, mainOverlayY = 0;

	switch (position) {
	case OverlayPosition::Top:
		mainOverlayX = (workArea.right - workArea.left - layout.baseWidth) / 2;
		mainOverlayY = margin;
		break;
	case OverlayPosition::Bottom:
		mainOverlayX = (workArea.right - workArea.left - layout.baseWidth) / 2;
		mainOverlayY = (workArea.bottom - workArea.top) - layout.baseHeight - margin;
		break;
	case OverlayPosition::Left:
		mainOverlayX = margin;
		mainOverlayY = (workArea.bottom - workArea.top - layout.baseHeight) / 2;
		break;
	case OverlayPosition::Right:
		mainOverlayX = (workArea.right - workArea.left) - layout.baseWidth - margin;
		mainOverlayY = (workArea.bottom - workArea.top - layout.baseHeight) / 2;
		break;
	case OverlayPosition::TopLeft:
		mainOverlayX = margin;
		mainOverlayY = margin;
		break;
	case OverlayPosition::TopRight:
		mainOverlayX = (workArea.right - workArea.left) - layout.baseWidth - margin;
		mainOverlayY = margin;
		break;
	case OverlayPosition::BottomLeft:
		mainOverlayX = margin;
		mainOverlayY = (workArea.bottom - workArea.top) - layout.baseHeight - margin;
		break;
	case OverlayPosition::BottomRight:
		mainOverlayX = (workArea.right - workArea.left) - layout.baseWidth - margin;
		mainOverlayY = (workArea.bottom - workArea.top) - layout.baseHeight - margin;
		break;
	case OverlayPosition::Center:
		mainOverlayX = (workArea.right - workArea.left - layout.baseWidth) / 2;
		mainOverlayY = (workArea.bottom - workArea.top - layout.baseHeight) / 2;
		break;
	}

	// Window origin is main-overlay position minus the adaptive container offset.
	windowPos.x = mainOverlayX - layout.offsetX;
	windowPos.y = mainOverlayY - layout.offsetY;

	return windowPos;
}

#endif // _WIN32
