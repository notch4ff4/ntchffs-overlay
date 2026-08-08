#pragma once

#ifdef _WIN32

#include <d2d1.h>
#include <wincodec.h>
#include <cstdint>
#include <string>
#include <vector>

namespace overlay::svg {

// Rasterize an SVG file to premultiplied 32bpp BGRA pixels (square sizePx×sizePx).
bool RasterizeFile(IWICImagingFactory *wic, ID2D1Factory *d2d, const std::wstring &path, UINT sizePx,
		   std::vector<uint8_t> &outBgra, UINT &outW, UINT &outH, UINT &outStride);

// True when path looks like an SVG by extension.
bool IsSvgPath(const std::wstring &path);

} // namespace overlay::svg

#endif
