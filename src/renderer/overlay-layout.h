#pragma once

#ifdef _WIN32

#include <windows.h>

enum class OverlayPosition {
	Top = 0,
	Bottom = 1,
	Left = 2,
	Right = 3,
	TopLeft = 4,
	TopRight = 5,
	BottomLeft = 6,
	BottomRight = 7,
	Center = 8
};

enum class OverlayOrientation {
	Horizontal = 0,
	Vertical = 1
};

struct OverlayLayout {
	// --- Core dimensions ---
	int baseWidth;
	int baseHeight;
	
	// --- Window bounds ---
	int windowWidth;
	int windowHeight;
	
	// --- Adaptive container offset ---
	int offsetX;
	int offsetY;
	
	// --- Main overlay region ---
	RECT mainOverlayRect;
	
	// --- Settings container ---
	RECT settingsRect;
	bool settingsOnLeft;
	
	// --- Stats container ---
	RECT statsRect;
	bool statsAbove;
	
	// --- Control panels ---
	RECT recordingPanelRect;
	RECT replayPanelRect;
	RECT buttonsContainerRect;
	
	// --- Panel buttons ---
	RECT recordFolderButtonRect;
	RECT showOBSButtonRect;
	
	// --- Settings toolbar buttons ---
	RECT settingsButtonRect;
	RECT statsButtonRect;
};

class OverlayLayoutManager {
public:
	OverlayLayoutManager();
	
	void CalculateLayout(OverlayPosition position, int margin, 
	                     OverlayOrientation orientation, 
	                     bool statsVisible, 
	                     OverlayLayout &layout);
	
	POINT CalculateWindowPosition(OverlayPosition position, int margin,
	                              const OverlayLayout &layout);
	
	RECT GetScreenWorkArea() const;
	RECT GetMonitorBoundsForPoint(POINT screenPoint) const;
	
private:
	void CalculateBaseSize(OverlayOrientation orientation, int &width, int &height);
	void CalculateSettingsSize(int &width, int &height);
	void CalculateWindowSize(OverlayPosition position, OverlayOrientation orientation,
	                        bool statsVisible, const OverlayLayout &layout,
	                        int &width, int &height);
	void CalculateWindowOffset(OverlayPosition position, bool statsVisible,
	                          const OverlayLayout &layout, int &offsetX, int &offsetY);
	void CalculateMainOverlayRect(const OverlayLayout &layout, RECT &rect);
	void CalculateSettingsRect(OverlayPosition position, const OverlayLayout &layout, RECT &rect);
	void CalculateStatsRect(OverlayPosition position, OverlayOrientation orientation,
	                      bool statsVisible, const OverlayLayout &layout, RECT &rect);
	void CalculatePanelRects(OverlayOrientation orientation, const OverlayLayout &layout);
	
};

#endif // _WIN32
