#pragma once

#ifdef _WIN32

#include <wincodec.h>
#include <cstdint>
#include <string>
#include <vector>

namespace overlay::gif {

struct DecodedFrame {
	std::vector<uint8_t> bgra;
	UINT w = 0;
	UINT h = 0;
	UINT stride = 0;
	UINT delayMs = 100;
};

// Decode a GIF (or any multi-frame WIC image) into composed premultiplied BGRA frames.
// For single-frame rasters this returns one frame. Returns false on failure.
bool DecodeFile(IWICImagingFactory *wic, const std::wstring &path, std::vector<DecodedFrame> &outFrames);

// True when path looks like a GIF by extension.
bool IsGifPath(const std::wstring &path);

} // namespace overlay::gif

#endif
