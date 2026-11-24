#include "wic.h"

#ifdef PLATFORM_WIN32

#include <objidl.h>
// Defining the WIC GUIDs in this compilation unit: avoids any dependency on
// windowscodecs.lib / uuid.lib; everything is invoked through COM vtables,
// only ole32 (CoCreateInstance, CreateStreamOnHGlobal) is linked.
#include <initguid.h>
#include <wincodec.h>

NAMESPACE_UPP

////////////////////////////////////////////////////////////////////////////////////////////////////
// WICGuard - lazy COM / WIC factory bootstrap

class WICGuard {
	bool cominit;

public:
	IWICImagingFactory *factory;

	bool Init()
	{
		if(factory)
			return true;
		// Bring up COM if the host has not already done so. S_FALSE and
		// RPC_E_CHANGED_MODE mean COM is already available.
		HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
		cominit = hr == S_OK;
		if(FAILED(hr) && hr != RPC_E_CHANGED_MODE && hr != S_FALSE)
			return false;
		return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
		                                  IID_IWICImagingFactory, (void **)&factory));
	}

	IWICImagingFactory *operator~() { return factory; }

	WICGuard()                          { cominit = false; factory = NULL; }
	~WICGuard()
	{
		if(factory)
			factory->Release();
		if(cominit)
			CoUninitialize();
	}
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// WICRaster::Data - decodes the whole stream to 32bppBGRA (= U++ RGBA layout on Windows)

class WICRaster::Data {
public:
	Size          size;
	Buffer<RGBA>  pix;
	RasterFormat  fmt;
	bool          alpha;

	bool Create(Stream& stream);
};

bool WICRaster::Data::Create(Stream& stream)
{
	// Load the entire stream. WIC consumes an IStream; raster streams handed
	// to StreamRaster are seekable and image files are small in practice.
	StringBuffer data;
	{
		Stream& s = stream;
		int64 sz = s.GetSize();
		if(sz >= 0 && sz < ((int64)1 << 31)) {
			s.Seek(0);
			data.SetCount((int)sz);
			int64 done = 0;
			while(done < sz) {
				int n = s.Get(~data + (int)done, (int)min<int64>(sz - done, 1 << 20));
				if(n <= 0)
					break;
				done += n;
			}
			data.SetCount((int)done);
		}
		else { // size unknown: read to end the slow way
			for(;;) {
				int c = s.GetChar();
				if(c < 0)
					break;
				data.Cat(c);
			}
		}
	}

	if(data.GetCount() == 0)
		return false;

	WICGuard w;
	if(!w.Init())
		return false;

	// Wrap the memory as an IStream (we keep ownership of the HGLOBAL)
	IStream *pis = NULL;
	HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)data.GetCount());
	if(!hg)
		return false;
	{
		void *pv = GlobalLock(hg);
		if(!pv) {
			GlobalFree(hg);
			return false;
		}
		memcpy(pv, ~data, (size_t)data.GetCount());
		GlobalUnlock(hg);
	}
	HRESULT hr = CreateStreamOnHGlobal(hg, FALSE, &pis);
	if(FAILED(hr)) {
		GlobalFree(hg);
		return false;
	}

	bool ok = false;

	IWICBitmapDecoder *dec = NULL;
	hr = ~w->CreateDecoderFromStream(pis, NULL, WICDecodeMetadataCacheOnDemand, &dec);
	if(SUCCEEDED(hr)) {
		// Restrict to the containers we serve - others (ICO and exotic
		// containers) stay with their traditional plugins.
		GUID container;
		if(SUCCEEDED(dec->GetContainerFormat(&container)) &&
		   (IsEqualGUID(container, GUID_ContainerFormatPng) ||
		    IsEqualGUID(container, GUID_ContainerFormatJpeg) ||
		    IsEqualGUID(container, GUID_ContainerFormatTiff) ||
		    IsEqualGUID(container, GUID_ContainerFormatBmp) ||
		    IsEqualGUID(container, GUID_ContainerFormatGif))) {
			IWICBitmapFrameDecode *frame = NULL;
			if(SUCCEEDED(dec->GetFrame(0, &frame))) {
				UINT cw = 0, ch = 0;
				if(SUCCEEDED(frame->GetSize(&cw, &ch)) && cw > 0 && ch > 0 &&
				   cw <= 0xFFFF && ch <= 0xFFFF) {
					size = Size(cw, ch);

					IWICFormatConverter *conv = NULL;
					IWICBitmapSource *src = frame;
					if(SUCCEEDED(~w->CreateFormatConverter(&conv))) {
						if(SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
						                              WICBitmapDitherTypeNone, NULL, 0,
						                              WICBitmapPaletteTypeCustom)))
							src = conv;
						else
							conv->Release();
					}

					pix.Alloc(size.cx * size.cy);
					hr = src->CopyPixels(NULL, size.cx * 4, (UINT)size.cx * size.cy * 4,
					                     (BYTE *)~pix);
					ok = SUCCEEDED(hr);

					if(ok) {
						alpha = false;
						for(const RGBA *p = ~pix, *e = p + size.cx * size.cy; p < e; p++)
							if(p->a != 255) {
								alpha = true;
								break;
							}
					}
					fmt.SetRGBA();

					if(src == conv)
						conv->Release();
				}
				frame->Release();
			}
		}
		dec->Release();
	}
	pis->Release();
	GlobalFree(hg);

	if(!ok)
		pix.Clear();

	return ok;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// WICRaster

WICRaster::WICRaster()
{
}

WICRaster::~WICRaster()
{
}

bool WICRaster::Create()
{
	data = new Data;
	return data->Create(GetStream());
}

Size WICRaster::GetSize()
{
	return data ? data->size : Size(0, 0);
}

Raster::Info WICRaster::GetInfo()
{
	Info info;
	if(data) {
		info.bpp = 32;
		info.kind = data->alpha ? IMAGE_ALPHA : IMAGE_OPAQUE;
	}
	return info;
}

Raster::Line WICRaster::GetLine(int line)
{
	return Line((const RGBA *)(~data->pix + line * data->size.cx), false);
}

int WICRaster::GetPaletteCount()
{
	return 0;
}

const RGBA *WICRaster::GetPalette()
{
	return NULL;
}

const RasterFormat *WICRaster::GetFormat()
{
	return data ? &data->fmt : NULL;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// WICStreamEncoder

class WICStreamEncoder::Data {
public:
	Size                   sz;
	HGLOBAL                hg;
	IStream               *pis;
	IWICBitmapEncoder     *enc;
	IWICBitmapFrameEncode *frame;
	int                    linei;
	Buffer<byte>           buf;
	bool                   ok;
	Stream                *stream;

	void WriteLine(const byte *s);
	bool Commit();

	Data()  { hg = NULL; pis = NULL; enc = NULL; frame = NULL; linei = 0; ok = false; stream = NULL; }
	~Data()
	{
		Commit();
		if(frame)
			frame->Release();
		if(enc)
			enc->Release();
		if(pis)
			pis->Release();
		if(hg)
			GlobalFree(hg);
	}
};

void WICStreamEncoder::Data::WriteLine(const byte *s)
{
	if(!ok || !buf)
		return;
	memcpy(~buf + linei * sz.cx * 4, s, sz.cx * 4);
	linei++;
}

bool WICStreamEncoder::Data::Commit()
{
	if(!ok)
		return false;
	ok = false; // commit only once

	HRESULT  hr = S_OK;
	IWICBitmap *bitmap = NULL;
	do {
		// Build the bitmap from the buffered pixels. WIC takes over all
		// container-specific conversions at WriteSource: JPEG drops alpha,
		// GIF quantizes, BMP picks an RGB layout, TIFF may wrap.
		WICGuard w;
		if(!w.Init()) {
			hr = E_FAIL;
			break;
		}
		hr = ~w->CreateBitmapFromMemory((UINT)sz.cx, (UINT)sz.cy, GUID_WICPixelFormat32bppBGRA,
		                                sz.cx * 4, (UINT)sz.cx * sz.cy * 4, ~buf, &bitmap);
		if(FAILED(hr))
			break;

		WICRect rc;
		rc.X = 0;
		rc.Y = 0;
		rc.Width = sz.cx;
		rc.Height = sz.cy;
		hr = frame->WriteSource(bitmap, &rc);
		if(FAILED(hr))
			break;

		hr = frame->Commit();
		if(FAILED(hr))
			break;

		hr = enc->Commit();
	}
	while(false);
	if(bitmap)
		bitmap->Release();

	if(FAILED(hr))
		return false;

	// Hand the produced bytes to the U++ stream. The IStream position is the
	// number of bytes actually written; GlobalSize would only give the
	// (padded) allocation size.
	ULARGE_INTEGER u = { 0 };
	LARGE_INTEGER z = { 0 };
	if(FAILED(pis->Seek(z, STREAM_SEEK_CUR, &u)))
		return false;
	SIZE_T len = (SIZE_T)u.QuadPart;
	void *pv = GlobalLock(hg);
	if(pv) {
		stream->Put(pv, (int)len);
		GlobalUnlock(hg);
		return true;
	}
	return false;
}

WICStreamEncoder::WICStreamEncoder(const GUID& container_)
{
	container = container_;
	quality = 75;
	bpp = 24;
}

WICStreamEncoder::~WICStreamEncoder()
{
}

int WICStreamEncoder::GetPaletteCount()
{
	return 0;
}

void WICStreamEncoder::Start(Size sz_)
{
	ASSERT(sz_.cx > 0 && sz_.cy > 0);

	WICGuard w;
	Data *d = new Data;
	data = d;
	d->sz = sz_;
	d->stream = &GetStream();
	d->buf.Alloc(sz_.cx * sz_.cy * 4);

	if(!w.Init())
		return;

	d->hg = GlobalAlloc(GMEM_MOVEABLE, 1 << 20);
	if(!d->hg)
		return;
	HRESULT hr = CreateStreamOnHGlobal(d->hg, FALSE, &d->pis);
	if(FAILED(hr)) {
		GlobalFree(d->hg);
		d->hg = NULL;
		return;
	}

	if(FAILED(~w->CreateEncoderByGUID(&container, &d->enc)))
		return;

	// Frame properties must be written into the property bag BEFORE
	// frame->Initialize() - WIC freezes the frame options at Initialize.
	// Only JPEG honors ImageQuality; other containers ignore it, so we skip
	// the bag write for them. WriteSource handles pixel-format conversion
	// for every container.
	IPropertyBag2 *opts = NULL;
	if(FAILED(d->enc->CreateNewFrame(&d->frame, &opts)))
		return;

	if(IsEqualGUID(container, GUID_ContainerFormatJpeg) && opts) {
		wchar_t name[] = L"ImageQuality";
		PROPBAG2 pb[1];
		Zero(pb);
		pb[0].pstrName = name;
		pb[0].vt = VT_R4;
		VARIANT var[1];
		Zero(var);
		var[0].vt = VT_R4;
		var[0].fltVal = (float)(quality / 100.0);
		if(FAILED(opts->Write(1, pb, var))) {
			VariantClear(var);
			opts->Release();
			d->frame->Release();
			d->frame = NULL;
			return;
		}
		VariantClear(var);
	}

	if(FAILED(d->frame->Initialize(opts))) {
		if(opts)
			opts->Release();
		d->frame->Release();
		d->frame = NULL;
		return;
	}
	if(opts)
		opts->Release();

	if(FAILED(d->frame->SetSize(sz_.cx, sz_.cy)))
		return;

	GUID pf = GUID_WICPixelFormat32bppBGRA;
	if(FAILED(d->frame->SetPixelFormat(&pf)))
		return;

	d->ok = true;
}

void WICStreamEncoder::WriteLineRaw(const byte *s)
{
	if(data)
		data->WriteLine(s);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Concrete encoders (GUIDs stay local to this package)

WICPNGEncoder::WICPNGEncoder()                     : WICStreamEncoder(GUID_ContainerFormatPng)  {}
WICJPGEncoder::WICJPGEncoder(int q)                : WICStreamEncoder(GUID_ContainerFormatJpeg) { quality = q; }
WICTIFEncoder::WICTIFEncoder(int bpp_)             : WICStreamEncoder(GUID_ContainerFormatTiff) { bpp = bpp_; }
WICBMPEncoder::WICBMPEncoder()                     : WICStreamEncoder(GUID_ContainerFormatBmp)  {}
WICGIFEncoder::WICGIFEncoder()                     : WICStreamEncoder(GUID_ContainerFormatGif)  {}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Registration

bool IsWICJPG(StreamRaster *s)
{
	Stream& str = s->GetStream();
	int64 pos = str.GetPos();
	int a = str.GetChar();
	int b = str.GetChar();
	str.Seek(pos);
	return a == 0xFF && b == 0xD8;
}

INITIALIZER(WICRaster) {
	StreamRaster::Register<WICRaster>();
	SetIsJPGFn(IsWICJPG);
}

END_UPP_NAMESPACE

#endif // PLATFORM_WIN32
