#ifdef _WIN32

#include "overlay-gif-decode.h"

#include <propidl.h>

namespace overlay::gif {
namespace {

bool HasExtension(const std::wstring &path, const wchar_t *ext)
{
	const size_t n = wcslen(ext);
	return path.size() >= n && _wcsicmp(path.c_str() + path.size() - n, ext) == 0;
}

UINT ReadU16Metadata(IWICMetadataQueryReader *reader, const wchar_t *name, UINT fallback)
{
	if (!reader)
		return fallback;
	PROPVARIANT prop;
	PropVariantInit(&prop);
	UINT value = fallback;
	if (SUCCEEDED(reader->GetMetadataByName(name, &prop))) {
		if (prop.vt == VT_UI2)
			value = prop.uiVal;
		else if (prop.vt == VT_I2 && prop.iVal >= 0)
			value = static_cast<UINT>(prop.iVal);
	}
	PropVariantClear(&prop);
	return value;
}

UINT ReadU8Metadata(IWICMetadataQueryReader *reader, const wchar_t *name, UINT fallback)
{
	if (!reader)
		return fallback;
	PROPVARIANT prop;
	PropVariantInit(&prop);
	UINT value = fallback;
	if (SUCCEEDED(reader->GetMetadataByName(name, &prop))) {
		if (prop.vt == VT_UI1)
			value = prop.bVal;
		else if (prop.vt == VT_UI2)
			value = prop.uiVal;
	}
	PropVariantClear(&prop);
	return value;
}

bool ConvertFrameToPbgra(IWICImagingFactory *wic, IWICBitmapFrameDecode *frame, std::vector<uint8_t> &out,
			 UINT &outW, UINT &outH, UINT &outStride)
{
	IWICFormatConverter *converter = nullptr;
	HRESULT hr = wic->CreateFormatConverter(&converter);
	if (FAILED(hr) || !converter)
		return false;

	hr = converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0f,
				   WICBitmapPaletteTypeCustom);
	if (FAILED(hr)) {
		converter->Release();
		return false;
	}

	UINT w = 0, h = 0;
	converter->GetSize(&w, &h);
	if (w == 0 || h == 0) {
		converter->Release();
		return false;
	}

	const UINT stride = w * 4;
	out.assign(static_cast<size_t>(stride) * static_cast<size_t>(h), 0);
	hr = converter->CopyPixels(nullptr, stride, static_cast<UINT>(out.size()), out.data());
	converter->Release();
	if (FAILED(hr))
		return false;

	outW = w;
	outH = h;
	outStride = stride;
	return true;
}

void BlitFrameOntoCanvas(std::vector<uint8_t> &canvas, UINT canvasW, UINT canvasH, const uint8_t *src, UINT srcW,
			 UINT srcH, UINT srcStride, UINT left, UINT top)
{
	for (UINT y = 0; y < srcH; y++) {
		const UINT dy = top + y;
		if (dy >= canvasH)
			break;
		for (UINT x = 0; x < srcW; x++) {
			const UINT dx = left + x;
			if (dx >= canvasW)
				break;
			const size_t si = static_cast<size_t>(y) * srcStride + static_cast<size_t>(x) * 4;
			const uint8_t a = src[si + 3];
			if (a == 0)
				continue;
			const size_t di = (static_cast<size_t>(dy) * canvasW + dx) * 4;
			canvas[di + 0] = src[si + 0];
			canvas[di + 1] = src[si + 1];
			canvas[di + 2] = src[si + 2];
			canvas[di + 3] = a;
		}
	}
}

void ClearRect(std::vector<uint8_t> &canvas, UINT canvasW, UINT canvasH, UINT left, UINT top, UINT w, UINT h)
{
	for (UINT y = 0; y < h; y++) {
		const UINT dy = top + y;
		if (dy >= canvasH)
			break;
		for (UINT x = 0; x < w; x++) {
			const UINT dx = left + x;
			if (dx >= canvasW)
				break;
			const size_t di = (static_cast<size_t>(dy) * canvasW + dx) * 4;
			canvas[di + 0] = 0;
			canvas[di + 1] = 0;
			canvas[di + 2] = 0;
			canvas[di + 3] = 0;
		}
	}
}

UINT DelayMsFromMetadata(IWICMetadataQueryReader *reader)
{
	// GIF delay is in 1/100 second. Zero is commonly treated as 10 (100 ms).
	const UINT delayCs = ReadU16Metadata(reader, L"/grctlext/Delay", 10);
	UINT ms = (delayCs == 0 ? 10u : delayCs) * 10u;
	if (ms < 20)
		ms = 20;
	return ms;
}

bool ResolveCanvasSize(IWICBitmapDecoder *decoder, UINT frameCount, UINT &canvasW, UINT &canvasH)
{
	canvasW = 0;
	canvasH = 0;

	IWICMetadataQueryReader *decoderMeta = nullptr;
	if (SUCCEEDED(decoder->GetMetadataQueryReader(&decoderMeta)) && decoderMeta) {
		canvasW = ReadU16Metadata(decoderMeta, L"/logscrdesc/Width", 0);
		canvasH = ReadU16Metadata(decoderMeta, L"/logscrdesc/Height", 0);
		decoderMeta->Release();
	}

	if (canvasW > 0 && canvasH > 0)
		return true;

	// Fallback: union of all frame rects (left/top + size).
	for (UINT i = 0; i < frameCount; i++) {
		IWICBitmapFrameDecode *frame = nullptr;
		if (FAILED(decoder->GetFrame(i, &frame)) || !frame)
			return false;

		UINT fw = 0, fh = 0;
		frame->GetSize(&fw, &fh);
		IWICMetadataQueryReader *meta = nullptr;
		frame->GetMetadataQueryReader(&meta);
		const UINT left = ReadU16Metadata(meta, L"/imgdesc/Left", 0);
		const UINT top = ReadU16Metadata(meta, L"/imgdesc/Top", 0);
		if (meta)
			meta->Release();
		frame->Release();

		const UINT right = left + fw;
		const UINT bottom = top + fh;
		if (right > canvasW)
			canvasW = right;
		if (bottom > canvasH)
			canvasH = bottom;
	}

	return canvasW > 0 && canvasH > 0;
}

bool DecodeMultiFrame(IWICImagingFactory *wic, IWICBitmapDecoder *decoder, UINT frameCount,
		      std::vector<DecodedFrame> &outFrames)
{
	UINT canvasW = 0, canvasH = 0;
	if (!ResolveCanvasSize(decoder, frameCount, canvasW, canvasH))
		return false;

	std::vector<uint8_t> canvas(static_cast<size_t>(canvasW) * static_cast<size_t>(canvasH) * 4, 0);
	std::vector<uint8_t> backup;
	UINT prevDisposal = 0;
	UINT prevLeft = 0, prevTop = 0, prevW = 0, prevH = 0;

	outFrames.clear();
	outFrames.reserve(frameCount);

	for (UINT i = 0; i < frameCount; i++) {
		IWICBitmapFrameDecode *frame = nullptr;
		if (FAILED(decoder->GetFrame(i, &frame)) || !frame)
			return false;

		IWICMetadataQueryReader *meta = nullptr;
		frame->GetMetadataQueryReader(&meta);

		const UINT left = ReadU16Metadata(meta, L"/imgdesc/Left", 0);
		const UINT top = ReadU16Metadata(meta, L"/imgdesc/Top", 0);
		const UINT disposal = ReadU8Metadata(meta, L"/grctlext/Disposal", 0);
		const UINT delayMs = DelayMsFromMetadata(meta);

		if (i > 0) {
			if (prevDisposal == 2) {
				ClearRect(canvas, canvasW, canvasH, prevLeft, prevTop, prevW, prevH);
			} else if (prevDisposal == 3 && !backup.empty()) {
				canvas = backup;
			}
		}

		if (disposal == 3)
			backup = canvas;

		std::vector<uint8_t> framePixels;
		UINT fw = 0, fh = 0, fstride = 0;
		if (!ConvertFrameToPbgra(wic, frame, framePixels, fw, fh, fstride)) {
			if (meta)
				meta->Release();
			frame->Release();
			return false;
		}

		BlitFrameOntoCanvas(canvas, canvasW, canvasH, framePixels.data(), fw, fh, fstride, left, top);

		DecodedFrame out;
		out.w = canvasW;
		out.h = canvasH;
		out.stride = canvasW * 4;
		out.delayMs = delayMs;
		out.bgra = canvas;
		outFrames.push_back(std::move(out));

		prevDisposal = disposal;
		prevLeft = left;
		prevTop = top;
		prevW = fw;
		prevH = fh;

		if (meta)
			meta->Release();
		frame->Release();
	}

	return !outFrames.empty();
}

} // namespace

bool IsGifPath(const std::wstring &path)
{
	return HasExtension(path, L".gif");
}

bool DecodeFile(IWICImagingFactory *wic, const std::wstring &path, std::vector<DecodedFrame> &outFrames)
{
	outFrames.clear();
	if (!wic || path.empty())
		return false;

	IWICBitmapDecoder *decoder = nullptr;
	HRESULT hr = wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
						    WICDecodeMetadataCacheOnLoad, &decoder);
	if (FAILED(hr) || !decoder)
		return false;

	UINT frameCount = 0;
	decoder->GetFrameCount(&frameCount);
	if (frameCount == 0) {
		decoder->Release();
		return false;
	}

	GUID container = GUID_NULL;
	decoder->GetContainerFormat(&container);
	const bool isGif = IsEqualGUID(container, GUID_ContainerFormatGif);

	bool ok = false;
	if (isGif && frameCount > 1) {
		ok = DecodeMultiFrame(wic, decoder, frameCount, outFrames);
	} else {
		IWICBitmapFrameDecode *frame = nullptr;
		hr = decoder->GetFrame(0, &frame);
		if (SUCCEEDED(hr) && frame) {
			DecodedFrame out;
			ok = ConvertFrameToPbgra(wic, frame, out.bgra, out.w, out.h, out.stride);
			if (ok) {
				if (isGif) {
					IWICMetadataQueryReader *meta = nullptr;
					frame->GetMetadataQueryReader(&meta);
					out.delayMs = DelayMsFromMetadata(meta);
					if (meta)
						meta->Release();
				}
				outFrames.push_back(std::move(out));
			}
			frame->Release();
		}
	}

	decoder->Release();
	return ok && !outFrames.empty();
}

} // namespace overlay::gif

#endif
