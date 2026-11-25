#include "wic.h"

#ifdef PLATFORM_WIN32

// Windows headers, with the CY fence mirroring Core/App.cpp: on MSC, Core.h
// has renamed the SDK's CY type to win32_CY_ while including objidl.h and
// undef'ed the macro afterwards - so the rename must be re-armed around
// wincodec.h, whose ocidl.h references CY again. On mingw, Core.h included
// windows.h unfenced, objidl.h already declared the real CY typedef, and
// App.cpp's pattern is to #undef the macro before such headers - so mingw
// needs no fence at all.
// initguid.h keeps the WIC GUIDs local to this translation unit: no
// windowscodecs.lib / uuid.lib linkage needed.
#ifdef COMPILER_MSC
#define CY win32_CY_
#endif
#include <windows.h>
#include <ole2.h>
#include <objidl.h>
#include <initguid.h>
#include <wincodec.h>
#ifdef COMPILER_MSC
#undef CY
#endif

#include <stdlib.h>
#include <string.h>

// Some mingw winerror.h variants lack this STORAGE facility error code.
#ifndef STG_E_INVALIDSEEK
#define STG_E_INVALIDSEEK ((HRESULT)0x80030026L)
#endif

NAMESPACE_UPP

////////////////////////////////////////////////////////////////////////////////////////////////////
// Diagnostics - stage/HRESULT trace of the last encoder runs, exposed via
// WICDiag().

static String& sWICDiag()
{
	static String d;
	return d;
}

static String sWICStage(const char *stage, HRESULT hr)
{
	String s = stage;
	s << "=0x" << FormatIntHex((dword)((int)hr & 0xffffffff));
	return s;
}

String WICDiag()
{
	return sWICDiag();
}

#define WICTRACE(stage, hr)  sWICDiag() << sWICStage(stage, hr) << "; "

static const char *sWICContainerName(const GUID& g)
{
	if(IsEqualGUID(g, GUID_ContainerFormatPng))  return "png";
	if(IsEqualGUID(g, GUID_ContainerFormatJpeg)) return "jpg";
	if(IsEqualGUID(g, GUID_ContainerFormatTiff)) return "tif";
	if(IsEqualGUID(g, GUID_ContainerFormatBmp))  return "bmp";
	if(IsEqualGUID(g, GUID_ContainerFormatGif)) return "gif";
	return "???";
}

static const char *sWICPFName(const GUID& g)
{
	if(IsEqualGUID(g, GUID_WICPixelFormat32bppBGRA)) return "32bppBGRA";
	if(IsEqualGUID(g, GUID_WICPixelFormat32bppBGR))  return "32bppBGR";
	if(IsEqualGUID(g, GUID_WICPixelFormat24bppBGR))  return "24bppBGR";
	if(IsEqualGUID(g, GUID_WICPixelFormat8bppIndexed)) return "8bppIndexed";
	return "other";
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// WICGuard - process-lifetime COM / WIC factory bootstrap

class WICGuard {
	bool cominit;

public:
	IWICImagingFactory *factory;

	void Init()
	{
		if(factory)
			return;
		HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
		cominit = hr == S_OK;
		if(FAILED(hr) && hr != RPC_E_CHANGED_MODE && hr != S_FALSE)
			return;
		// mingw headers DEFINE_GUID the IIDs (localized by initguid.h), while
		// the MSVC SDK defines IID_* externs in uuid.lib - use __uuidof there.
		CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
#ifdef _MSC_VER
		                 __uuidof(IWICImagingFactory),
#else
		                 IID_IWICImagingFactory,
#endif
		                 (void **)&factory);
	}

	IWICImagingFactory *operator->()        { return factory; }

	WICGuard()                              { cominit = false; factory = NULL; }
	~WICGuard()
	{
		if(factory)
			factory->Release();
		if(cominit)
			CoUninitialize();
	}
};

static WICGuard& WIC()
{
	static WICGuard w;
	w.Init();
	return w;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// WICMemStream - minimal writable in-memory IStream for encoder output.
//
// Some WIC encoders (notably BMP) do not write sequentially: they emit the
// pixel data first and seek back to patch the header, leaving the stream
// position somewhere inside the file. The produced length therefore cannot
// be taken from the position; this stream tracks the high-water mark of all
// bytes written (honoring SetSize), which is the true file length for every
// encoder behavior.

class WICMemStream : public IStream {
	byte  *ptr;
	int64  cap;
	int64  size;
	int64  pos;
	LONG   refs;

	bool Grow(int64 need)
	{
		if(need <= cap)
			return true;
		int64 nc = cap ? cap * 2 : 65536;
		while(nc < need)
			nc *= 2;
		byte *p = (byte *)realloc(ptr, (size_t)nc);
		if(!p)
			return false;
		ptr = p;
		cap = nc;
		return true;
	}

public:
	WICMemStream()                  { ptr = NULL; cap = size = pos = 0; refs = 1; }
	~WICMemStream()                 { free(ptr); }

	const byte *Ptr() const         { return ptr; }
	int64       Size() const       { return size; }

	// IUnknown
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv)
	{
		if(!ppv)
			return E_POINTER;
		if(IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IStream) ||
		   IsEqualGUID(riid, IID_ISequentialStream)) {
			*ppv = (IStream *)this;
			AddRef();
			return S_OK;
		}
		*ppv = NULL;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef()          { return (ULONG)++refs; }
	ULONG STDMETHODCALLTYPE Release()         { LONG r = --refs; if(!r) delete this; return (ULONG)r; }

	// ISequentialStream
	HRESULT STDMETHODCALLTYPE Read(void *pv, ULONG cb, ULONG *pcbRead)
	{
		if(!pv)
			return E_POINTER;
		int64 avail = size - pos;
		if(avail < 0)
			avail = 0;
		int64 n = min<int64>(avail, (int64)cb);
		if(n > 0)
			memcpy(pv, ptr + pos, (size_t)n);
		pos += n;
		if(pcbRead)
			*pcbRead = (ULONG)n;
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE Write(const void *pv, ULONG cb, ULONG *pcbWritten)
	{
		if(cb && !pv)
			return E_POINTER;
		if(cb) {
			if(!Grow(pos + (int64)cb))
				return STG_E_MEDIUMFULL;
			memcpy(ptr + pos, pv, cb);
			pos += cb;
			if(pos > size)
				size = pos;
		}
		if(pcbWritten)
			*pcbWritten = cb;
		return S_OK;
	}

	// IStream
	HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin, ULARGE_INTEGER *plibNewPosition)
	{
		int64 npos = pos;
		switch(dwOrigin) {
		case STREAM_SEEK_SET: npos = dlibMove.QuadPart; break;
		case STREAM_SEEK_CUR: npos = pos + dlibMove.QuadPart; break;
		case STREAM_SEEK_END: npos = size + dlibMove.QuadPart; break;
		default: return STG_E_INVALIDFUNCTION;
		}
		if(npos < 0)
			return STG_E_INVALIDSEEK;
		pos = npos;
		if(plibNewPosition)
			plibNewPosition->QuadPart = (ULONGLONG)pos;
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER libNewSize)
	{
		int64 ns = (int64)libNewSize.QuadPart;
		if(ns < 0)
			return E_FAIL;
		if(ns > size) {
			if(!Grow(ns))
				return STG_E_MEDIUMFULL;
			memset(ptr + size, 0, (size_t)(ns - size));
		}
		size = ns;
		if(pos > size)
			pos = size;
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE CopyTo(IStream *, ULARGE_INTEGER, ULARGE_INTEGER *, ULARGE_INTEGER *)
	{
		return E_NOTIMPL;
	}
	HRESULT STDMETHODCALLTYPE Commit(DWORD)        { return S_OK; }
	HRESULT STDMETHODCALLTYPE Revert()              { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD)
	{
		return E_NOTIMPL;
	}
	HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD)
	{
		return E_NOTIMPL;
	}
	HRESULT STDMETHODCALLTYPE Stat(STATSTG *pstatstg, DWORD)
	{
		if(!pstatstg)
			return E_POINTER;
		memset(pstatstg, 0, sizeof(STATSTG));
		pstatstg->type = STGTY_STREAM;
		pstatstg->cbSize.QuadPart = (ULONGLONG)size;
		pstatstg->grfMode = STGM_READWRITE;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE Clone(IStream **)     { return E_NOTIMPL; }
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
	String data = LoadStream(stream);

	if(data.IsEmpty())
		return false;

	WICGuard& w = WIC();
	if(!w.factory)
		return false;

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
	hr = w->CreateDecoderFromStream(pis, NULL, WICDecodeMetadataCacheOnDemand, &dec);
	if(SUCCEEDED(hr)) {
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
					if(SUCCEEDED(w->CreateFormatConverter(&conv))) {
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
//
// Commits to the stream the moment the LAST scanline arrives (the U++ encoder
// protocol has no finalize hook and SaveFile's stream dies right after Save).

class WICStreamEncoder::Data {
public:
	Size                   sz;
	WICMemStream          *out;
	IWICBitmapEncoder     *enc;
	IWICBitmapFrameEncode *frame;
	bool                   needspal;
	int                    linei;
	Buffer<byte>           buf;
	bool                   ok;
	Stream                *stream;

	void WriteLine(const byte *s);
	bool Commit();

	Data()  { out = NULL; enc = NULL; frame = NULL; needspal = false; linei = 0; ok = false; stream = NULL; }
	~Data()
	{
		Commit();
		if(frame)
			frame->Release();
		if(enc)
			enc->Release();
		if(out)
			out->Release();
	}
};

void WICStreamEncoder::Data::WriteLine(const byte *s)
{
	if(!ok || !buf)
		return;
	memcpy(~buf + linei * sz.cx * 4, s, sz.cx * 4);
	linei++;
	if(linei == sz.cy)
		Commit(); // last row: flush while the U++ stream is still alive
}

bool WICStreamEncoder::Data::Commit()
{
	if(!ok)
		return false;
	ok = false; // commit only once

	if(linei != sz.cy) {      // abandoned / incomplete image
		WICTRACE("abandoned", E_ABORT);
		return false;
	}

	WICGuard& w = WIC();
	if(!w.factory) {
		WICTRACE("commit_factory", E_FAIL);
		return false;
	}

	HRESULT      hr = S_OK;
	IWICBitmap  *bitmap = NULL;
	IWICPalette *pal = NULL;
	do {
		hr = w->CreateBitmapFromMemory((UINT)sz.cx, (UINT)sz.cy, GUID_WICPixelFormat32bppBGRA,
		                               sz.cx * 4, (UINT)sz.cx * sz.cy * 4, ~buf, &bitmap);
		if(FAILED(hr)) {
			WICTRACE("createbitmap", hr);
			break;
		}

		// GIF encodes 8bppIndexed and needs the palette upfront.
		if(needspal) {
			hr = w->CreatePalette(&pal);
			if(FAILED(hr)) {
				WICTRACE("createpalette", hr);
				break;
			}
			hr = pal->InitializeFromBitmap(bitmap, 256, TRUE);
			if(FAILED(hr)) {
				WICTRACE("palinit", hr);
				break;
			}
			hr = frame->SetPalette(pal);
			if(FAILED(hr)) {
				WICTRACE("setpalette", hr);
				break;
			}
		}

		// WriteSource converts from the buffered BGRA to whatever pixel
		// format was negotiated with the frame.
		WICRect rc;
		rc.X = 0;
		rc.Y = 0;
		rc.Width = sz.cx;
		rc.Height = sz.cy;
		hr = frame->WriteSource(bitmap, &rc);
		if(FAILED(hr)) {
			WICTRACE("writesource", hr);
			break;
		}

		hr = frame->Commit();
		if(FAILED(hr)) {
			WICTRACE("framecommit", hr);
			break;
		}

		hr = enc->Commit();
		if(FAILED(hr)) {
			WICTRACE("encodercommit", hr);
			break;
		}
	}
	while(false);
	if(pal)
		pal->Release();
	if(bitmap)
		bitmap->Release();

	if(FAILED(hr))
		return false;

	// The true output length is the high-water mark tracked by WICMemStream:
	// the BMP encoder (and others that patch headers by seeking back) leaves
	// the stream position inside the file, so position cannot be used here.
	int64 len = out->Size();
	if(len <= 0) {
		WICTRACE("empty", E_FAIL);
		return false;
	}
	sWICDiag() << "bytes=" << len << "; ";
	stream->Put(out->Ptr(), (int)len);
	return true;
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

	sWICDiag() << sWICContainerName(container) << ": ";

	WICGuard& w = WIC();
	if(!w.factory) {
		WICTRACE("factory", E_FAIL);
		return;
	}

	Data *d = new Data;
	data = d;
	d->sz = sz_;
	d->stream = &GetStream();
	d->buf.Alloc(sz_.cx * sz_.cy * 4);

	d->out = new WICMemStream();

	HRESULT hr = w->CreateEncoder(container, &GUID_VendorMicrosoftBuiltIn, &d->enc);
	if(FAILED(hr)) {
		WICTRACE("createencoder", hr);
		return;
	}

	// Bind the encoder to the output stream.
	hr = d->enc->Initialize(d->out, WICBitmapEncoderNoCache);
	if(FAILED(hr)) {
		WICTRACE("encinitialize", hr);
		return;
	}

	// Pixel format candidates, in preference order. The BMP encoder has no
	// BGRA support (it would nominally accept it but write a header-only
	// file), so BGR comes first for BMP.
	static const GUID pf_png[] = { GUID_WICPixelFormat32bppBGRA };
	static const GUID pf_jpg[] = { GUID_WICPixelFormat32bppBGRA, GUID_WICPixelFormat24bppBGR };
	static const GUID pf_tif[] = { GUID_WICPixelFormat32bppBGRA, GUID_WICPixelFormat24bppBGR };
	static const GUID pf_bmp[] = { GUID_WICPixelFormat24bppBGR, GUID_WICPixelFormat32bppBGR };
	static const GUID pf_gif[] = { GUID_WICPixelFormat8bppIndexed };

	const GUID *pf = pf_png;
	int         n = 1;
	if(IsEqualGUID(container, GUID_ContainerFormatJpeg))      { pf = pf_jpg; n = 2; }
	else if(IsEqualGUID(container, GUID_ContainerFormatTiff)) { pf = pf_tif; n = 2; }
	else if(IsEqualGUID(container, GUID_ContainerFormatBmp))  { pf = pf_bmp; n = 2; }
	else if(IsEqualGUID(container, GUID_ContainerFormatGif))  { pf = pf_gif; n = 1; d->needspal = true; }

	// Frame properties must be written into the property bag BEFORE
	// frame->Initialize(). Only JPEG honors ImageQuality.
	IPropertyBag2 *opts = NULL;
	hr = d->enc->CreateNewFrame(&d->frame, &opts);
	if(FAILED(hr)) {
		WICTRACE("createnewframe", hr);
		return;
	}

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
		HRESULT hr2 = opts->Write(1, pb, var);
		if(FAILED(hr2))
			WICTRACE("imagewrite", hr2);
		VariantClear(var);
	}

	hr = d->frame->Initialize(opts);
	if(FAILED(hr)) {
		if(opts)
			opts->Release();
		d->frame->Release();
		d->frame = NULL;
		WICTRACE("frameinitialize", hr);
		return;
	}
	if(opts)
		opts->Release();

	hr = d->frame->SetSize(sz_.cx, sz_.cy);
	if(FAILED(hr)) {
		WICTRACE("setsize", hr);
		return;
	}

	bool fmtok = false;
	HRESULT hr2 = E_FAIL;
	GUID    negotiated = pf[0];
	for(int i = 0; i < n && !fmtok; i++) {
		GUID fx = pf[i];   // SetPixelFormat writes back the actually chosen format
		hr2 = d->frame->SetPixelFormat(&fx);
		fmtok = SUCCEEDED(hr2);
		if(fmtok)
			negotiated = fx;
		else
			WICTRACE("setpixelformat", hr2);
	}
	if(!fmtok)
		return;

	// Record the negotiated format so a bogus container/format combination
	// cannot hide behind silent success.
	sWICDiag() << "pf=" << sWICPFName(negotiated) << ", ";
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
	int a = str.Get();
	int b = str.Get();
	str.Seek(pos);
	return a == 0xFF && b == 0xD8;
}

INITIALIZER(WICRaster) {
	StreamRaster::Register<WICRaster>();
	SetIsJPGFn(IsWICJPG);
}

END_UPP_NAMESPACE

#endif // PLATFORM_WIN32
