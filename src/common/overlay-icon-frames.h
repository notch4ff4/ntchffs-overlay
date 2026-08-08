#pragma once

#ifdef _WIN32

#include <windows.h>
#include <d2d1.h>
#include <wincodec.h>
#include <cstdint>
#include <string>
#include <vector>

namespace overlay::icon {

constexpr UINT kDefaultSvgPx = 64;

struct Frame {
	ID2D1Bitmap *original = nullptr;
	ID2D1Bitmap *tinted = nullptr;
	std::vector<uint8_t> alpha;
	UINT maskW = 0;
	UINT maskH = 0;
	UINT delayMs = 100;
};

struct Slot {
	std::wstring path;
	bool tintEnabled = false;
	int colorRgb = 0xFFFFFF;
	std::vector<Frame> frames;
	size_t frameIndex = 0;
	ULONGLONG frameStartTick = 0;
	bool loadAttempted = false;
	bool loadFailed = false;
};

void ReleaseFrames(std::vector<Frame> &frames);
void InvalidateSlot(Slot &slot);

bool CreateFrameBitmaps(ID2D1RenderTarget *rt, Frame &frame, UINT w, UINT h, const uint8_t *bgra, UINT stride);
void RebuildTintedFrame(ID2D1RenderTarget *rt, Frame &frame, bool tintEnabled, int colorRgb);
void RebuildTintedSlot(ID2D1RenderTarget *rt, Slot &slot);

bool LoadRaster(IWICImagingFactory *wic, ID2D1RenderTarget *rt, Slot &slot, const std::wstring &path);
bool LoadSvg(IWICImagingFactory *wic, ID2D1Factory *d2d, ID2D1RenderTarget *rt, Slot &slot, const std::wstring &path,
	     UINT svgPx = kDefaultSvgPx);

// Sync path/tint from desired values, then lazy-load if needed.
// Returns true when a drawable original bitmap is available.
bool EnsureLoaded(IWICImagingFactory *wic, ID2D1Factory *d2d, ID2D1RenderTarget *rt, Slot &slot,
		  const std::wstring &wantPath, bool wantTint, int wantColorRgb, UINT svgPx = kDefaultSvgPx);

void AdvanceAnimation(Slot &slot, ULONGLONG now);
ID2D1Bitmap *CurrentBitmap(const Slot &slot);

void DrawBuiltin(ID2D1RenderTarget *rt, ID2D1Factory *d2dFactory, ID2D1SolidColorBrush *brush, int slot,
		 const RECT &rect, float strokeWidth);

} // namespace overlay::icon

#endif
