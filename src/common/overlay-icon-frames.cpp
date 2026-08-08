#ifdef _WIN32

#include "overlay-icon-frames.h"
#include "overlay-gif-decode.h"
#include "overlay-svg-rasterize.h"

#include <dxgiformat.h>
#include <cmath>

namespace overlay::icon {

void ReleaseFrames(std::vector<Frame> &frames)
{
	for (Frame &frame : frames) {
		if (frame.original) {
			frame.original->Release();
			frame.original = nullptr;
		}
		if (frame.tinted) {
			frame.tinted->Release();
			frame.tinted = nullptr;
		}
		frame.alpha.clear();
		frame.maskW = 0;
		frame.maskH = 0;
	}
	frames.clear();
}

void InvalidateSlot(Slot &slot)
{
	ReleaseFrames(slot.frames);
	slot.frameIndex = 0;
	slot.frameStartTick = 0;
	slot.loadAttempted = false;
	slot.loadFailed = false;
}

bool CreateFrameBitmaps(ID2D1RenderTarget *rt, Frame &frame, UINT w, UINT h, const uint8_t *bgra, UINT stride)
{
	if (!rt || !bgra || w == 0 || h == 0)
		return false;

	if (frame.original) {
		frame.original->Release();
		frame.original = nullptr;
	}
	if (frame.tinted) {
		frame.tinted->Release();
		frame.tinted = nullptr;
	}

	frame.maskW = w;
	frame.maskH = h;
	frame.alpha.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
	for (UINT y = 0; y < h; y++) {
		for (UINT x = 0; x < w; x++) {
			const size_t src = static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4;
			frame.alpha[static_cast<size_t>(y) * w + x] = bgra[src + 3];
		}
	}

	D2D1_BITMAP_PROPERTIES props =
		D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
	ID2D1Bitmap *bmp = nullptr;
	if (FAILED(rt->CreateBitmap(D2D1::SizeU(w, h), bgra, stride, &props, &bmp)) || !bmp)
		return false;
	frame.original = bmp;
	return true;
}

void RebuildTintedFrame(ID2D1RenderTarget *rt, Frame &frame, bool tintEnabled, int colorRgb)
{
	if (frame.tinted) {
		frame.tinted->Release();
		frame.tinted = nullptr;
	}
	if (!tintEnabled || frame.alpha.empty() || frame.maskW == 0 || frame.maskH == 0 || !rt)
		return;

	const float cr = ((colorRgb >> 16) & 0xFF) / 255.0f;
	const float cg = ((colorRgb >> 8) & 0xFF) / 255.0f;
	const float cb = (colorRgb & 0xFF) / 255.0f;
	const UINT stride = frame.maskW * 4;
	std::vector<uint8_t> bgra(static_cast<size_t>(stride) * static_cast<size_t>(frame.maskH));
	for (UINT y = 0; y < frame.maskH; y++) {
		for (UINT x = 0; x < frame.maskW; x++) {
			const size_t idx = static_cast<size_t>(y) * frame.maskW + x;
			const float af = frame.alpha[idx] / 255.0f;
			const uint8_t A = static_cast<uint8_t>(af * 255.0f + 0.5f);
			const uint8_t B = static_cast<uint8_t>(cb * 255.0f * af + 0.5f);
			const uint8_t G = static_cast<uint8_t>(cg * 255.0f * af + 0.5f);
			const uint8_t R = static_cast<uint8_t>(cr * 255.0f * af + 0.5f);
			const size_t o = static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4;
			bgra[o + 0] = B;
			bgra[o + 1] = G;
			bgra[o + 2] = R;
			bgra[o + 3] = A;
		}
	}

	D2D1_BITMAP_PROPERTIES props =
		D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
	ID2D1Bitmap *bmp = nullptr;
	if (SUCCEEDED(rt->CreateBitmap(D2D1::SizeU(frame.maskW, frame.maskH), bgra.data(), stride, &props, &bmp)) &&
	    bmp) {
		frame.tinted = bmp;
	}
}

void RebuildTintedSlot(ID2D1RenderTarget *rt, Slot &slot)
{
	if (!rt)
		return;
	for (Frame &frame : slot.frames)
		RebuildTintedFrame(rt, frame, slot.tintEnabled, slot.colorRgb);
}

bool LoadRaster(IWICImagingFactory *wic, ID2D1RenderTarget *rt, Slot &slot, const std::wstring &path)
{
	if (!wic || !rt)
		return false;

	std::vector<overlay::gif::DecodedFrame> decoded;
	if (!overlay::gif::DecodeFile(wic, path, decoded) || decoded.empty())
		return false;

	ReleaseFrames(slot.frames);
	slot.frameIndex = 0;
	slot.frameStartTick = 0;
	slot.frames.reserve(decoded.size());

	for (const overlay::gif::DecodedFrame &src : decoded) {
		Frame frame;
		frame.delayMs = src.delayMs;
		if (!CreateFrameBitmaps(rt, frame, src.w, src.h, src.bgra.data(), src.stride)) {
			ReleaseFrames(slot.frames);
			return false;
		}
		RebuildTintedFrame(rt, frame, slot.tintEnabled, slot.colorRgb);
		slot.frames.push_back(std::move(frame));
	}

	return !slot.frames.empty() && slot.frames[0].original != nullptr;
}

bool LoadSvg(IWICImagingFactory *wic, ID2D1Factory *d2d, ID2D1RenderTarget *rt, Slot &slot, const std::wstring &path,
	     UINT svgPx)
{
	if (!wic || !d2d || !rt)
		return false;

	const UINT sizePx = svgPx > 0 ? svgPx : kDefaultSvgPx;
	std::vector<uint8_t> bgra;
	UINT w = 0, h = 0, stride = 0;
	if (!overlay::svg::RasterizeFile(wic, d2d, path, sizePx, bgra, w, h, stride))
		return false;

	ReleaseFrames(slot.frames);
	slot.frameIndex = 0;
	slot.frameStartTick = 0;

	Frame frame;
	if (!CreateFrameBitmaps(rt, frame, w, h, bgra.data(), stride))
		return false;
	RebuildTintedFrame(rt, frame, slot.tintEnabled, slot.colorRgb);
	slot.frames.push_back(std::move(frame));
	return true;
}

bool EnsureLoaded(IWICImagingFactory *wic, ID2D1Factory *d2d, ID2D1RenderTarget *rt, Slot &slot,
		  const std::wstring &wantPath, bool wantTint, int wantColorRgb, UINT svgPx)
{
	if (!rt)
		return false;

	const int wantColor = wantColorRgb & 0xFFFFFF;
	const bool pathChanged = (slot.path != wantPath);
	const bool tintChanged = (slot.tintEnabled != wantTint) || (slot.colorRgb != wantColor);

	if (pathChanged) {
		InvalidateSlot(slot);
		slot.path = wantPath;
		slot.tintEnabled = wantTint;
		slot.colorRgb = wantColor;
	} else if (tintChanged) {
		slot.tintEnabled = wantTint;
		slot.colorRgb = wantColor;
		if (!slot.frames.empty())
			RebuildTintedSlot(rt, slot);
	}

	if (wantPath.empty())
		return false;
	if (!slot.frames.empty() && slot.frames[0].original)
		return true;
	if (slot.loadAttempted)
		return false;

	slot.loadAttempted = true;
	slot.loadFailed = false;
	const bool isSvg = overlay::svg::IsSvgPath(wantPath);
	const bool ok = isSvg ? LoadSvg(wic, d2d, rt, slot, wantPath, svgPx) : LoadRaster(wic, rt, slot, wantPath);
	if (!ok)
		slot.loadFailed = true;
	return ok && !slot.frames.empty() && slot.frames[0].original != nullptr;
}

void AdvanceAnimation(Slot &slot, ULONGLONG now)
{
	if (slot.frames.size() <= 1)
		return;
	if (slot.frameStartTick == 0)
		slot.frameStartTick = now;

	int guard = 0;
	while (guard++ < 64) {
		const UINT delay = slot.frames[slot.frameIndex].delayMs < 20 ? 20u : slot.frames[slot.frameIndex].delayMs;
		if (now < slot.frameStartTick + delay)
			break;
		slot.frameStartTick += delay;
		slot.frameIndex = (slot.frameIndex + 1) % slot.frames.size();
		if (now > slot.frameStartTick + 2000) {
			slot.frameStartTick = now;
			break;
		}
	}
}

ID2D1Bitmap *CurrentBitmap(const Slot &slot)
{
	if (slot.frames.empty())
		return nullptr;
	size_t idx = slot.frameIndex;
	if (idx >= slot.frames.size())
		idx = 0;
	const Frame &frame = slot.frames[idx];
	if (slot.tintEnabled && frame.tinted)
		return frame.tinted;
	return frame.original;
}

void DrawBuiltin(ID2D1RenderTarget *rt, ID2D1Factory *d2dFactory, ID2D1SolidColorBrush *brush, int slot,
		 const RECT &rect, float strokeWidth)
{
	if (!rt || !brush)
		return;

	auto getCenter = [](const RECT &r) {
		return D2D1_POINT_2F{(r.left + r.right) / 2.0f, (r.top + r.bottom) / 2.0f};
	};
	auto pointOnCircle = [](const D2D1_POINT_2F &center, float radius, float angleRad) {
		return D2D1_POINT_2F{center.x + radius * std::cos(angleRad),
				     center.y + radius * std::sin(angleRad)};
	};

	if (slot == 0) {
		D2D1_POINT_2F center = getCenter(rect);
		float radius = (rect.right - rect.left) * 0.32f;
		rt->DrawEllipse(D2D1::Ellipse(center, radius, radius), brush, strokeWidth);
		return;
	}

	if (slot == 1 && d2dFactory) {
		D2D1_POINT_2F center = getCenter(rect);
		float radius = (rect.right - rect.left) * 0.34f;
		float startAngle = -0.25f * 3.14159265f;
		float endAngle = 1.25f * 3.14159265f;
		D2D1_POINT_2F startPt = pointOnCircle(center, radius, startAngle);
		D2D1_POINT_2F endPt = pointOnCircle(center, radius, endAngle);

		ID2D1PathGeometry *geometry = nullptr;
		if (SUCCEEDED(d2dFactory->CreatePathGeometry(&geometry)) && geometry) {
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
			rt->DrawGeometry(geometry, brush, strokeWidth);
			geometry->Release();
		}

		float arrowAngle = endAngle + 0.5f;
		D2D1_POINT_2F arrowPt1 = pointOnCircle(center, radius * 0.85f, arrowAngle);
		float arrowAngle2 = endAngle - 0.4f;
		D2D1_POINT_2F arrowPt2 = pointOnCircle(center, radius * 0.85f, arrowAngle2);
		rt->DrawLine(arrowPt1, endPt, brush, strokeWidth);
		rt->DrawLine(arrowPt2, endPt, brush, strokeWidth);
		return;
	}

	if (slot == 2) {
		const float s = static_cast<float>(rect.right - rect.left);
		const float inset = s * 0.20f;
		const float labelInset = s * 0.15f;
		const float labelH = s * 0.30f;
		D2D1_RECT_F outer = D2D1::RectF((float)rect.left + inset, (float)rect.top + inset,
						(float)rect.right - inset, (float)rect.bottom - inset);
		D2D1_RECT_F label = D2D1::RectF(outer.left + labelInset, outer.top + labelInset,
						outer.right - labelInset, outer.top + labelInset + labelH);
		rt->DrawRectangle(outer, brush, strokeWidth);
		rt->DrawRectangle(label, brush, strokeWidth);
		return;
	}

	if (slot == 3) {
		const float w = (float)(rect.right - rect.left);
		const float h = (float)(rect.bottom - rect.top);
		D2D1_POINT_2F p1 = {(float)rect.left + w * 0.22f, (float)rect.top + h * 0.52f};
		D2D1_POINT_2F p2 = {(float)rect.left + w * 0.42f, (float)rect.top + h * 0.72f};
		D2D1_POINT_2F p3 = {(float)rect.left + w * 0.78f, (float)rect.top + h * 0.30f};
		rt->DrawLine(p1, p2, brush, strokeWidth);
		rt->DrawLine(p2, p3, brush, strokeWidth);
	}
}

} // namespace overlay::icon

#endif
