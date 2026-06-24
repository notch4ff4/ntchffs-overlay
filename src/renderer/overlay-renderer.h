#pragma once

#ifdef _WIN32

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include "overlay-layout.h"
#include "overlay-state.h"
#include "overlay-window-manager.h"
#include "overlay-state.h"
#include "overlay-gallery.h"
#include "overlay-video-player.h"
#include "overlay-trimmer.h"
#include <unordered_map>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")

#include <wincodec.h>
#include <wincodecsdk.h>

class OverlayRenderer {
public:
	OverlayRenderer();
	~OverlayRenderer();
	
	bool Initialize(HINSTANCE hInstance);
	void Shutdown();
	
	void Show();
	void Hide();
	bool IsVisible() const;
	
	void SetPosition(OverlayPosition position, int margin);
	void SetOrientation(OverlayOrientation orientation);
	void ApplyPosition();
	void SetAutoHide(bool enabled, int seconds);
	void SetGalleryEnabled(bool enabled);
	void SetCaptureFocus(bool capture);
	void SetOverlayBackgroundAlpha(float alpha);

	void UpdateStatus();
	void Render();
	
	void HandleClick(int x, int y);
	void HandleMouseDown(int x, int y);
	void HandleMouseUp(int x, int y);
	
	// --- Getters ---
	OverlayPosition GetPosition() const { return m_position; }
	int GetMargin() const { return m_margin; }
	OverlayOrientation GetOrientation() const { return m_orientation; }
	
private:
	bool InitializeDirect2D();
	void ShutdownDirect2D();
	void CreateDeviceResources();
	void DiscardDeviceResources();
	
	void RenderMainOverlay();
	void RenderDimBackground(int width, int height);
	void RenderSettingsContainer();
	void RenderStatsContainer();
	void RenderPanels();
	void RenderButtons();
	void RenderGallery();
	void RenderGalleryList(const RECT &listRect, int itemHeight, int itemsPerPage);
	void RenderGalleryControls(const RECT &controlsRect);
	void OpenGallery();
	void CloseGallery();
	void ApplyGalleryWindowSize();
	void HandleGalleryClick(int x, int y);
	void HandleGalleryKey(int vkCode, int mods);
	bool HandleGalleryMouseDown(int x, int y);
	void MaybeStartDrag(int x, int y);
	void RenderText(const std::wstring &text, const RECT &rect, 
	                float fontSize, DWRITE_FONT_WEIGHT weight, 
	                D2D1_COLOR_F color, DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_CENTER);
	
	void DrawRoundedRectangle(const RECT &rect, float radius, D2D1_COLOR_F fillColor);
	void DrawRoundedRectangleBorder(const RECT &rect, float radius, float borderWidth, D2D1_COLOR_F borderColor);
	void DrawGalleryIcon(const RECT &rect, int iconType, const D2D1_COLOR_F *tintColor = nullptr);

	bool IsPointInRect(const POINT &pt, const RECT &rect) const;
	void HandleMouseMove(int x, int y);
	void HandleMouseLeave();

	enum class HoverTarget {
		None,
		RecordingPanel,
		ReplayPanel,
		RecordFolderButton,
		ShowOBSButton,
		SettingsButton,
		StatsButton,

		// --- Gallery / player UI ---
		GalleryBack,
		GalleryFolderBack,
		GalleryFullscreen,
		GalleryMute,
		GalleryPlay,
		GallerySetIn,
		GallerySetOut,
		GalleryTrim,
		GalleryDelete
	};

	HoverTarget GetHoverTarget(const POINT &pt) const;
	
	OverlayWindowManager m_windowManager;
	OverlayLayoutManager m_layoutManager;
	OverlayStateManager m_stateManager;
	
	// --- Direct2D resources ---
	ID2D1Factory *m_d2dFactory;
	ID2D1HwndRenderTarget *m_renderTarget;
	IDWriteFactory *m_writeFactory;
	
	// --- WIC resources ---
	IWICImagingFactory *m_wicFactory;
	IWICBitmap *m_wicBitmap;
	ID2D1RenderTarget *m_bitmapRenderTarget;
	
	// --- Brushes ---
	ID2D1SolidColorBrush *m_backgroundBrush;
	ID2D1SolidColorBrush *m_textBrush;
	ID2D1SolidColorBrush *m_activeBrush;
	ID2D1SolidColorBrush *m_borderBrush;
	ID2D1SolidColorBrush *m_statsTitleBrush;

	// Reuse the preview bitmap across frames to avoid per-frame allocation.
	ID2D1Bitmap *m_galleryVideoBitmap = nullptr;
	int m_galleryVideoBitmapW = 0;
	int m_galleryVideoBitmapH = 0;
	
	// --- Text formats ---
	IDWriteTextFormat *m_statusTextFormat;
	IDWriteTextFormat *m_buttonTextFormat;
	IDWriteTextFormat *m_replayActiveTextFormat;
	IDWriteTextFormat *m_statsTitleFormat;
	IDWriteTextFormat *m_statsValueFormat;
	
	// --- State ---
	OverlayState m_state;
	OverlayLayout m_layout;
	OverlayPosition m_position;
	int m_margin;
	OverlayOrientation m_orientation;
	bool m_autoHideEnabled;
	int m_autoHideSeconds;
	ULONGLONG m_lastInteractionTick;
	HoverTarget m_hoverTarget;

	bool m_galleryEnabled;
	bool m_galleryOpen;
	int m_galleryPage;
	int m_gallerySelectedIndex;
	double m_galleryTrimIn;
	double m_galleryTrimOut;
	bool m_galleryTrimUserSet;
	bool m_galleryShareCompress;
	// Preset 0..2 maps to CRF for ffmpeg; kept for backward compatibility with older CRF-only settings.
	int m_galleryShareCompressQuality;
	int m_galleryShareCrf;
	bool m_galleryMuted;
	int m_galleryVolume;
	bool m_galleryExportSubmenuOpen;
	bool m_galleryFullscreen;
	std::wstring m_sharedFolderToOpenOnSuccess;
	// Defer opening until export succeeds.
	OverlayGallery m_gallery;
	OverlayVideoPlayer m_videoPlayer;
	OverlayTrimmer m_trimmer;

	bool m_dragCandidate;
	bool m_dragActive;
	bool m_timelineDragging;
	bool m_suppressNextGalleryClick;
	POINT m_dragStart;

	RECT m_galleryListRect;
	RECT m_galleryPreviewRect;
	RECT m_galleryControlsRect;
	RECT m_galleryTimelineRect;
	RECT m_galleryBackRect;
	RECT m_galleryFolderBackRect;
	RECT m_galleryFullscreenRect;
	RECT m_galleryPrevPageRect;
	RECT m_galleryNextPageRect;
	RECT m_galleryPlayRect;
	RECT m_gallerySetInRect;
	RECT m_gallerySetOutRect;
	RECT m_galleryTrimRect;
	RECT m_galleryDeleteRect;
	RECT m_galleryShareRect;
	RECT m_galleryCompressRect;
	RECT m_galleryExportPanelRect;
	RECT m_galleryExportConfirmRect;
	RECT m_galleryQualityPrevRect;
	RECT m_galleryQualityNextRect;
	RECT m_galleryMuteRect;
	RECT m_galleryVolumeRect;
	RECT m_galleryAudioTrackPrevRect;
	RECT m_galleryAudioTrackNextRect;
	RECT m_galleryAudioTrackButtonRect;
	RECT m_galleryAudioTrackMenuRect;
	bool m_galleryAudioTrackSubmenuOpen;
	std::vector<RECT> m_galleryAudioTrackOptionRects;
	RECT m_galleryExportCancelRect;
	std::vector<int> m_exportTrackStreamIndices;
	std::vector<bool> m_exportTrackKeep;
	std::vector<RECT> m_exportTrackKeepRects;
	std::vector<std::wstring> m_exportTrackLabels;

	ID2D1PathGeometry *m_iconPlayGeometry;

	// Index 0..11 matches icon type; loaded from embedded pack or data/icons/.
	static const int ICON_CACHE_SIZE = 12;
	ID2D1Bitmap *m_iconBitmaps[ICON_CACHE_SIZE];
	bool LoadIconBitmap(int iconType);
	bool LoadIconFromResource(int iconType);
	struct IconMask {
		UINT w = 0;
		UINT h = 0;
		std::vector<uint8_t> alpha;
		// w * h bytes
		bool ready() const { return w > 0 && h > 0 && alpha.size() == (size_t)w * (size_t)h; }
	};
	IconMask m_iconMasks[ICON_CACHE_SIZE];
	std::unordered_map<uint64_t, ID2D1Bitmap *> m_iconTintCache;
	ID2D1Bitmap *GetTintedIconBitmap(int iconType, const D2D1_COLOR_F &tint);
	uint64_t MakeTintKey(int iconType, const D2D1_COLOR_F &tint) const;
	void ReleaseIconBitmaps();

	// Mica-like glass effect; 0.85–0.95 is typical.
	float m_overlayBackgroundAlpha;
	bool m_captureFocus;
	POINT m_dimContentOffset;
	int m_galleryContentWidth;
	int m_galleryContentHeight;

	// --- Timer ---
	UINT_PTR m_updateTimer;
	int m_updateIntervalMs;

	void UpdateAutoHide();
	void SetUpdateInterval(int intervalMs);
	void ApplyWindowGeometry();
	bool IsDimmingActive() const;
	void ApplyDimmingWindowGeometry(POINT contentScreenTopLeft, int contentWidth, int contentHeight);
	POINT ClientToLayoutPoint(int x, int y) const;
	
	static void CALLBACK TimerProc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time);
	static OverlayRenderer *s_timerInstance;
};

#endif // _WIN32
