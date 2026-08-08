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
#include <cstring>
#include <string>
#include <vector>

namespace overlay::svg {
namespace {

template<typename T> struct ComRef {
	T *p = nullptr;

	ComRef() = default;
	~ComRef() { reset(); }

	ComRef(const ComRef &) = delete;
	ComRef &operator=(const ComRef &) = delete;

	void reset(T *next = nullptr)
	{
		if (p)
			p->Release();
		p = next;
	}

	T **out()
	{
		reset();
		return &p;
	}

	T *get() const { return p; }
	T *operator->() const { return p; }
	explicit operator bool() const { return p != nullptr; }
};

bool HasExtension(const std::wstring &path, const wchar_t *ext)
{
	const size_t n = wcslen(ext);
	return path.size() >= n && _wcsicmp(path.c_str() + path.size() - n, ext) == 0;
}

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

// Locate the root <svg ...> opening tag (case-insensitive tag name).
size_t FindSvgOpenTag(const std::string &svg)
{
	for (size_t i = 0; i + 4 <= svg.size(); i++) {
		if (svg[i] != '<')
			continue;
		const char c1 = svg[i + 1] | 0x20;
		const char c2 = svg[i + 2] | 0x20;
		const char c3 = svg[i + 3] | 0x20;
		if (c1 != 's' || c2 != 'v' || c3 != 'g')
			continue;
		if (i + 4 >= svg.size())
			return i;
		const char next = svg[i + 4];
		if (next == ' ' || next == '\t' || next == '\n' || next == '\r' || next == '/' || next == '>')
			return i;
	}
	return std::string::npos;
}

// D2D viewport is sizePx×sizePx. Absolute width/height like "800px" make viewBox
// content render off-canvas. Keep viewBox, force width/height on the root <svg> only.
void NormalizeSvgMarkup(std::string &svg, UINT sizePx)
{
	const size_t open = FindSvgOpenTag(svg);
	if (open == std::string::npos)
		return;
	const size_t gt = svg.find('>', open);
	if (gt == std::string::npos || gt <= open)
		return;

	std::string tag = svg.substr(open, gt - open);

	auto replaceAttr = [&](const char *name) {
		const std::string sizeStr = std::to_string(sizePx);
		for (const char quote : {'"', '\''}) {
			const std::string key = std::string(name) + "=" + quote;
			const size_t pos = tag.find(key);
			if (pos == std::string::npos)
				continue;
			const size_t val = pos + key.size();
			const size_t end = tag.find(quote, val);
			if (end == std::string::npos)
				return;
			const std::string replacement = key + sizeStr + quote;
			tag.replace(pos, end + 1 - pos, replacement);
			return;
		}
	};

	replaceAttr("width");
	replaceAttr("height");
	svg.replace(open, gt - open, tag);
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
			if (bgra[o + 3] > 8) {
				opaque++;
				if (opaque > 4)
					return true;
			}
		}
	}
	return false;
}

bool RasterizeWithWicTarget(IWICImagingFactory *wic, ID2D1Factory *d2d, IStream *stream, UINT sizePx,
			    std::vector<uint8_t> &outBgra, UINT &outStride)
{
	ComRef<IWICBitmap> wicBmp;
	HRESULT hr = wic->CreateBitmap(sizePx, sizePx, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad,
				       wicBmp.out());
	if (FAILED(hr) || !wicBmp)
		return false;

	D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
		D2D1_RENDER_TARGET_TYPE_DEFAULT,
		D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
	ComRef<ID2D1RenderTarget> rt;
	hr = d2d->CreateWicBitmapRenderTarget(wicBmp.get(), rtProps, rt.out());
	if (FAILED(hr) || !rt)
		return false;

	ComRef<ID2D1DeviceContext5> dc5;
	hr = rt->QueryInterface(IID_PPV_ARGS(dc5.out()));
	if (FAILED(hr) || !dc5)
		return false;

	ComRef<ID2D1SvgDocument> svgDoc;
	hr = dc5->CreateSvgDocument(stream, D2D1::SizeF(static_cast<float>(sizePx), static_cast<float>(sizePx)),
				    svgDoc.out());
	if (FAILED(hr) || !svgDoc)
		return false;

	rt->BeginDraw();
	rt->SetTransform(D2D1::Matrix3x2F::Identity());
	rt->Clear(D2D1::ColorF(0, 0, 0, 0));
	dc5->DrawSvgDocument(svgDoc.get());
	hr = rt->EndDraw();
	if (FAILED(hr))
		return false;

	outStride = sizePx * 4;
	outBgra.assign(static_cast<size_t>(outStride) * static_cast<size_t>(sizePx), 0);
	WICRect rect = {0, 0, static_cast<INT>(sizePx), static_cast<INT>(sizePx)};
	hr = wicBmp->CopyPixels(&rect, outStride, static_cast<UINT>(outBgra.size()), outBgra.data());
	if (FAILED(hr)) {
		outBgra.clear();
		return false;
	}
	return true;
}

bool RasterizeWithD3dTarget(ID2D1Factory *d2d, IStream *stream, UINT sizePx, std::vector<uint8_t> &outBgra,
			    UINT &outStride)
{
	ComRef<ID2D1Factory1> factory1;
	HRESULT hr = d2d->QueryInterface(IID_PPV_ARGS(factory1.out()));
	if (FAILED(hr) || !factory1)
		return false;

	D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
	ComRef<ID3D11Device> d3dDevice;
	UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
			       d3dDevice.out(), &featureLevel, nullptr);
	if (FAILED(hr)) {
		hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
				       d3dDevice.out(), &featureLevel, nullptr);
	}
	if (FAILED(hr) || !d3dDevice)
		return false;

	ComRef<IDXGIDevice> dxgiDevice;
	hr = d3dDevice->QueryInterface(IID_PPV_ARGS(dxgiDevice.out()));
	if (FAILED(hr) || !dxgiDevice)
		return false;

	ComRef<ID2D1Device> d2dDevice;
	hr = factory1->CreateDevice(dxgiDevice.get(), d2dDevice.out());
	if (FAILED(hr) || !d2dDevice)
		return false;

	ComRef<ID2D1DeviceContext> dc;
	hr = d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, dc.out());
	if (FAILED(hr) || !dc)
		return false;

	ComRef<ID2D1DeviceContext5> dc5;
	hr = dc->QueryInterface(IID_PPV_ARGS(dc5.out()));
	if (FAILED(hr) || !dc5)
		return false;

	float dpiX = 96.0f, dpiY = 96.0f;
	dc->GetDpi(&dpiX, &dpiY);
	const D2D1_SIZE_U px = D2D1::SizeU(sizePx, sizePx);
	D2D1_BITMAP_PROPERTIES1 targetProps = D2D1::BitmapProperties1(
		D2D1_BITMAP_OPTIONS_TARGET,
		D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpiX, dpiY);
	ComRef<ID2D1Bitmap1> targetBmp;
	hr = dc5->CreateBitmap(px, nullptr, 0, &targetProps, targetBmp.out());
	if (FAILED(hr) || !targetBmp)
		return false;

	ComRef<ID2D1SvgDocument> svgDoc;
	hr = dc5->CreateSvgDocument(stream, D2D1::SizeF(static_cast<float>(sizePx), static_cast<float>(sizePx)),
				    svgDoc.out());
	if (FAILED(hr) || !svgDoc)
		return false;

	dc5->SetTarget(targetBmp.get());
	dc5->BeginDraw();
	dc5->Clear(D2D1::ColorF(0, 0, 0, 0));
	dc5->DrawSvgDocument(svgDoc.get());
	hr = dc5->EndDraw();
	dc5->SetTarget(nullptr);
	if (FAILED(hr))
		return false;

	D2D1_BITMAP_PROPERTIES1 cpuProps = D2D1::BitmapProperties1(
		D2D1_BITMAP_OPTIONS_CANNOT_DRAW | D2D1_BITMAP_OPTIONS_CPU_READ,
		D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpiX, dpiY);
	ComRef<ID2D1Bitmap1> cpuBmp;
	hr = dc->CreateBitmap(px, nullptr, 0, &cpuProps, cpuBmp.out());
	if (FAILED(hr) || !cpuBmp)
		return false;

	hr = cpuBmp->CopyFromBitmap(nullptr, targetBmp.get(), nullptr);
	if (FAILED(hr))
		return false;

	D2D1_MAPPED_RECT mapped = {};
	hr = cpuBmp->Map(D2D1_MAP_OPTIONS_READ, &mapped);
	if (FAILED(hr) || !mapped.bits)
		return false;

	outStride = sizePx * 4;
	outBgra.resize(static_cast<size_t>(outStride) * static_cast<size_t>(sizePx));
	for (UINT y = 0; y < sizePx; y++) {
		memcpy(outBgra.data() + static_cast<size_t>(y) * outStride,
		       mapped.bits + static_cast<size_t>(y) * mapped.pitch, static_cast<size_t>(sizePx) * 4);
	}
	cpuBmp->Unmap();
	return true;
}

} // namespace

bool IsSvgPath(const std::wstring &path)
{
	return HasExtension(path, L".svg");
}

bool RasterizeFile(IWICImagingFactory *wic, ID2D1Factory *d2d, const std::wstring &path, UINT sizePx,
		   std::vector<uint8_t> &outBgra, UINT &outW, UINT &outH, UINT &outStride)
{
	outBgra.clear();
	outW = 0;
	outH = 0;
	outStride = 0;
	if (!wic || !d2d || path.empty() || sizePx == 0)
		return false;

	const char *failReason = nullptr;

	std::string markup;
	if (!ReadFileUtf8(path, markup)) {
		failReason = "failed to read file";
	} else {
		NormalizeSvgMarkup(markup, sizePx);

		ComRef<IStream> stream;
		if (!CreateStreamOnMemory(markup, stream.out()) || !stream) {
			failReason = "create memory stream failed";
		} else {
			bool ok = RasterizeWithWicTarget(wic, d2d, stream.get(), sizePx, outBgra, outStride);
			if (!ok || !HasVisiblePixels(outBgra, sizePx, outStride)) {
				LARGE_INTEGER zero = {};
				stream->Seek(zero, STREAM_SEEK_SET, nullptr);
				ok = RasterizeWithD3dTarget(d2d, stream.get(), sizePx, outBgra, outStride);
			}

			if (!ok || !HasVisiblePixels(outBgra, sizePx, outStride)) {
				failReason = "raster produced empty image";
				outBgra.clear();
				outStride = 0;
			} else {
				outW = sizePx;
				outH = sizePx;
				return true;
			}
		}
	}

	if (failReason)
		obs_log(LOG_WARNING, "SVG: %s", failReason);
	return false;
}

} // namespace overlay::svg

#endif
