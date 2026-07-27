// Write by AI. Should be checked. (will be removed after checking)
#ifdef _WIN32

#include "overlay-svg-rasterize.h"
#include <obs-module.h>

extern "C" {
#include <plugin-support.h>
}

#include <d2d1_1.h>
#include <d2d1_3.h>
#include <d2d1svg.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgiformat.h>
#include <shlwapi.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace overlay::svg {
namespace {

bool ReadFileUtf8(const std::wstring &path, std::string &out)
{
	out.clear();
	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
				  FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;

	LARGE_INTEGER size = {};
	if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 8 * 1024 * 1024) {
		CloseHandle(file);
		return false;
	}

	out.resize(static_cast<size_t>(size.QuadPart));
	DWORD read = 0;
	const BOOL ok = ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
	CloseHandle(file);
	if (!ok || read != out.size()) {
		out.clear();
		return false;
	}
	return true;
}

// D2D viewport is sizePx×sizePx. Absolute width/height like "800px" make viewBox
// content render off-canvas. Keep viewBox, force width/height to the raster size.
void NormalizeSvgMarkup(std::string &svg, UINT sizePx)
{
	auto replaceAttr = [&](const char *name) {
		const std::string key = std::string(name) + "=\"";
		size_t pos = 0;
		while ((pos = svg.find(key, pos)) != std::string::npos) {
			const size_t start = pos;
			const size_t val = pos + key.size();
			const size_t end = svg.find('"', val);
			if (end == std::string::npos)
				break;
			const std::string replacement = key + std::to_string(sizePx) + "\"";
			svg.replace(start, end + 1 - start, replacement);
			pos = start + replacement.size();
			// Only rewrite the first svg root attrs (avoid touching nested uses).
			break;
		}
	};

	replaceAttr("width");
	replaceAttr("height");
}

bool CreateStreamOnMemory(const std::string &bytes, IStream **outStream)
{
	*outStream = nullptr;
	HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
	if (!mem)
		return false;
	void *ptr = GlobalLock(mem);
	if (!ptr) {
		GlobalFree(mem);
		return false;
	}
	memcpy(ptr, bytes.data(), bytes.size());
	GlobalUnlock(mem);

	IStream *stream = nullptr;
	HRESULT hr = CreateStreamOnHGlobal(mem, TRUE, &stream);
	if (FAILED(hr) || !stream) {
		GlobalFree(mem);
		return false;
	}
	*outStream = stream;
	return true;
}

bool HasVisiblePixels(const std::vector<uint8_t> &bgra, UINT sizePx, UINT stride)
{
	size_t opaque = 0;
	for (UINT y = 0; y < sizePx; y++) {
		for (UINT x = 0; x < sizePx; x++) {
			const size_t o = static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4;
			if (bgra[o + 3] > 8)
				opaque++;
		}
	}
	return opaque > 4;
}

bool RasterizeWithWicTarget(IWICImagingFactory *wic, ID2D1Factory *d2d, IStream *stream, UINT sizePx,
			    std::vector<uint8_t> &outBgra, UINT &outStride)
{
	IWICBitmap *wicBmp = nullptr;
	HRESULT hr = wic->CreateBitmap(sizePx, sizePx, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &wicBmp);
	if (FAILED(hr) || !wicBmp) {
		obs_log(LOG_WARNING, "SVG: CreateBitmap failed: 0x%08x", hr);
		return false;
	}

	D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
		D2D1_RENDER_TARGET_TYPE_DEFAULT,
		D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
	ID2D1RenderTarget *rt = nullptr;
	hr = d2d->CreateWicBitmapRenderTarget(wicBmp, rtProps, &rt);
	if (FAILED(hr) || !rt) {
		obs_log(LOG_WARNING, "SVG: CreateWicBitmapRenderTarget failed: 0x%08x", hr);
		wicBmp->Release();
		return false;
	}

	ID2D1DeviceContext5 *dc5 = nullptr;
	hr = rt->QueryInterface(IID_PPV_ARGS(&dc5));
	if (FAILED(hr) || !dc5) {
		obs_log(LOG_WARNING, "SVG: ID2D1DeviceContext5 QI (WIC RT) failed: 0x%08x", hr);
		rt->Release();
		wicBmp->Release();
		return false;
	}

	ID2D1SvgDocument *svgDoc = nullptr;
	hr = dc5->CreateSvgDocument(stream, D2D1::SizeF((float)sizePx, (float)sizePx), &svgDoc);
	if (FAILED(hr) || !svgDoc) {
		obs_log(LOG_WARNING, "SVG: CreateSvgDocument failed: 0x%08x", hr);
		dc5->Release();
		rt->Release();
		wicBmp->Release();
		return false;
	}

	rt->BeginDraw();
	rt->SetTransform(D2D1::Matrix3x2F::Identity());
	rt->Clear(D2D1::ColorF(0, 0, 0, 0));
	dc5->DrawSvgDocument(svgDoc);
	hr = rt->EndDraw();
	svgDoc->Release();
	dc5->Release();
	rt->Release();
	if (FAILED(hr)) {
		obs_log(LOG_WARNING, "SVG: EndDraw failed: 0x%08x", hr);
		wicBmp->Release();
		return false;
	}

	outStride = sizePx * 4;
	outBgra.assign(static_cast<size_t>(outStride) * static_cast<size_t>(sizePx), 0);
	WICRect rect = {0, 0, static_cast<INT>(sizePx), static_cast<INT>(sizePx)};
	hr = wicBmp->CopyPixels(&rect, outStride, static_cast<UINT>(outBgra.size()), outBgra.data());
	wicBmp->Release();
	if (FAILED(hr)) {
		obs_log(LOG_WARNING, "SVG: WIC CopyPixels failed: 0x%08x", hr);
		outBgra.clear();
		return false;
	}
	return true;
}

bool RasterizeWithD3dTarget(ID2D1Factory *d2d, IStream *stream, UINT sizePx, std::vector<uint8_t> &outBgra,
			    UINT &outStride)
{
	ID2D1Factory1 *factory1 = nullptr;
	HRESULT hr = d2d->QueryInterface(IID_PPV_ARGS(&factory1));
	if (FAILED(hr) || !factory1) {
		obs_log(LOG_WARNING, "SVG: ID2D1Factory1 QI failed: 0x%08x", hr);
		return false;
	}

	D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
	ID3D11Device *d3dDevice = nullptr;
	ID3D11DeviceContext *d3dContext = nullptr;
	UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
			       &d3dDevice, &featureLevel, &d3dContext);
	if (FAILED(hr)) {
		hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
				       &d3dDevice, &featureLevel, &d3dContext);
	}
	if (FAILED(hr) || !d3dDevice) {
		obs_log(LOG_WARNING, "SVG: D3D11CreateDevice failed: 0x%08x", hr);
		factory1->Release();
		return false;
	}

	IDXGIDevice *dxgiDevice = nullptr;
	hr = d3dDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
	if (FAILED(hr) || !dxgiDevice) {
		obs_log(LOG_WARNING, "SVG: IDXGIDevice QI failed: 0x%08x", hr);
		if (d3dContext)
			d3dContext->Release();
		d3dDevice->Release();
		factory1->Release();
		return false;
	}

	ID2D1Device *d2dDevice = nullptr;
	hr = factory1->CreateDevice(dxgiDevice, &d2dDevice);
	dxgiDevice->Release();
	factory1->Release();
	if (FAILED(hr) || !d2dDevice) {
		obs_log(LOG_WARNING, "SVG: CreateDevice failed: 0x%08x", hr);
		if (d3dContext)
			d3dContext->Release();
		d3dDevice->Release();
		return false;
	}

	ID2D1DeviceContext *dc = nullptr;
	hr = d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
	if (FAILED(hr) || !dc) {
		obs_log(LOG_WARNING, "SVG: CreateDeviceContext failed: 0x%08x", hr);
		d2dDevice->Release();
		if (d3dContext)
			d3dContext->Release();
		d3dDevice->Release();
		return false;
	}

	ID2D1DeviceContext5 *dc5 = nullptr;
	hr = dc->QueryInterface(IID_PPV_ARGS(&dc5));
	if (FAILED(hr) || !dc5) {
		obs_log(LOG_WARNING, "SVG: ID2D1DeviceContext5 QI (D3D) failed: 0x%08x", hr);
		dc->Release();
		d2dDevice->Release();
		if (d3dContext)
			d3dContext->Release();
		d3dDevice->Release();
		return false;
	}

	float dpiX = 96.0f, dpiY = 96.0f;
	dc->GetDpi(&dpiX, &dpiY);
	const D2D1_SIZE_U px = D2D1::SizeU(sizePx, sizePx);
	D2D1_BITMAP_PROPERTIES1 targetProps = D2D1::BitmapProperties1(
		D2D1_BITMAP_OPTIONS_TARGET,
		D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpiX, dpiY);
	ID2D1Bitmap1 *targetBmp = nullptr;
	hr = dc5->CreateBitmap(px, nullptr, 0, &targetProps, &targetBmp);
	if (FAILED(hr) || !targetBmp) {
		obs_log(LOG_WARNING, "SVG: CreateBitmap(TARGET) failed: 0x%08x", hr);
		dc5->Release();
		dc->Release();
		d2dDevice->Release();
		if (d3dContext)
			d3dContext->Release();
		d3dDevice->Release();
		return false;
	}

	ID2D1SvgDocument *svgDoc = nullptr;
	hr = dc5->CreateSvgDocument(stream, D2D1::SizeF((float)sizePx, (float)sizePx), &svgDoc);
	if (FAILED(hr) || !svgDoc) {
		obs_log(LOG_WARNING, "SVG: CreateSvgDocument (D3D) failed: 0x%08x", hr);
		targetBmp->Release();
		dc5->Release();
		dc->Release();
		d2dDevice->Release();
		if (d3dContext)
			d3dContext->Release();
		d3dDevice->Release();
		return false;
	}

	dc5->SetTarget(targetBmp);
	dc5->BeginDraw();
	dc5->Clear(D2D1::ColorF(0, 0, 0, 0));
	dc5->DrawSvgDocument(svgDoc);
	hr = dc5->EndDraw();
	svgDoc->Release();
	dc5->SetTarget(nullptr);
	if (FAILED(hr)) {
		obs_log(LOG_WARNING, "SVG: D3D EndDraw failed: 0x%08x", hr);
		targetBmp->Release();
		dc5->Release();
		dc->Release();
		d2dDevice->Release();
		if (d3dContext)
			d3dContext->Release();
		d3dDevice->Release();
		return false;
	}

	D2D1_BITMAP_PROPERTIES1 cpuProps = D2D1::BitmapProperties1(
		D2D1_BITMAP_OPTIONS_CANNOT_DRAW | D2D1_BITMAP_OPTIONS_CPU_READ,
		D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpiX, dpiY);
	ID2D1Bitmap1 *cpuBmp = nullptr;
	hr = dc->CreateBitmap(px, nullptr, 0, &cpuProps, &cpuBmp);
	bool ok = false;
	if (SUCCEEDED(hr) && cpuBmp) {
		hr = cpuBmp->CopyFromBitmap(nullptr, targetBmp, nullptr);
		if (SUCCEEDED(hr)) {
			D2D1_MAPPED_RECT mapped = {};
			hr = cpuBmp->Map(D2D1_MAP_OPTIONS_READ, &mapped);
			if (SUCCEEDED(hr) && mapped.bits) {
				outStride = sizePx * 4;
				outBgra.resize(static_cast<size_t>(outStride) * static_cast<size_t>(sizePx));
				for (UINT y = 0; y < sizePx; y++) {
					memcpy(outBgra.data() + static_cast<size_t>(y) * outStride,
					       mapped.bits + static_cast<size_t>(y) * mapped.pitch,
					       static_cast<size_t>(sizePx) * 4);
				}
				ok = true;
				cpuBmp->Unmap();
			} else {
				obs_log(LOG_WARNING, "SVG: Map failed: 0x%08x", hr);
			}
		} else {
			obs_log(LOG_WARNING, "SVG: CopyFromBitmap failed: 0x%08x", hr);
		}
		cpuBmp->Release();
	} else {
		obs_log(LOG_WARNING, "SVG: CreateBitmap(CPU_READ) failed: 0x%08x", hr);
	}

	targetBmp->Release();
	dc5->Release();
	dc->Release();
	d2dDevice->Release();
	if (d3dContext)
		d3dContext->Release();
	d3dDevice->Release();
	return ok;
}

} // namespace

bool RasterizeFile(IWICImagingFactory *wic, ID2D1Factory *d2d, const std::wstring &path, UINT sizePx,
		   std::vector<uint8_t> &outBgra, UINT &outW, UINT &outH, UINT &outStride)
{
	outBgra.clear();
	outW = 0;
	outH = 0;
	outStride = 0;
	if (!wic || !d2d || path.empty() || sizePx == 0)
		return false;

	std::string markup;
	if (!ReadFileUtf8(path, markup)) {
		obs_log(LOG_WARNING, "SVG: failed to read file");
		return false;
	}
	NormalizeSvgMarkup(markup, sizePx);

	IStream *stream = nullptr;
	if (!CreateStreamOnMemory(markup, &stream) || !stream) {
		obs_log(LOG_WARNING, "SVG: create memory stream failed");
		return false;
	}

	bool ok = RasterizeWithWicTarget(wic, d2d, stream, sizePx, outBgra, outStride);
	if (!ok || !HasVisiblePixels(outBgra, sizePx, outStride)) {
		LARGE_INTEGER zero = {};
		stream->Seek(zero, STREAM_SEEK_SET, nullptr);
		ok = RasterizeWithD3dTarget(d2d, stream, sizePx, outBgra, outStride);
	}
	stream->Release();

	if (!ok || !HasVisiblePixels(outBgra, sizePx, outStride)) {
		obs_log(LOG_WARNING, "SVG: raster produced empty image");
		outBgra.clear();
		outStride = 0;
		return false;
	}

	outW = sizePx;
	outH = sizePx;
	return true;
}

} // namespace overlay::svg

#endif
