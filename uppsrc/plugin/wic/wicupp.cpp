#include "wic.h"
#include "wicifd.h"

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

// Success-path / informational traces are gated behind the WICDIAG_VERBOSE
// compile-time macro: in production builds WICDiag() only ever records
// failures (WICTRACE), so it stays empty when everything succeeded. Test
// builds may define WICDIAG_VERBOSE to also capture the flow traces
// (trydecode, pf=, bytes=, idx=, qm_hit/qm_miss, metamiss, ...).
#ifdef WICDIAG_VERBOSE
#define WICINFO(x) sWICDiag() << (x)
#else
#define WICINFO(x)
#endif

// Build stamp: lets tests verify they are running against a freshly
// compiled plugin (Blitz/object caching otherwise hides stale binaries).
const char *WICBuildStamp()
{
	return __DATE__ " " __TIME__;
}

// Pending container attribution: the encoder Start() records the container
// name here; it is emitted only if a failure trace actually fires. This
// keeps failure traces attributable ("png: createencoder=0x...") without
// leaving a stray "png: " in the diag string on success.
static String sPendingContainer;

#define WICTRACE(stage, hr) \
	do { \
		sWICDiag() << sPendingContainer << sWICStage(stage, hr) << "; "; \
		sPendingContainer.Clear(); \
	} while(0)

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
	ONCELOCK {
		w.Init();
	}
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
// Metadata helper - reads an integer property through WIC's query reader.
// Missing properties are a normal situation (e.g. no graphic control
// extension in a GIF), in which case the default is returned.

static int sWICMetaInt(IWICMetadataQueryReader *q, const wchar_t *expr, int defval)
{
	PROPVARIANT v;
	PropVariantInit(&v);
	HRESULT hr = q->GetMetadataByName(expr, &v);
	if(FAILED(hr)) {
		WICINFO(String() << "metamiss(" << WString(expr).ToString() << "); ");
		return defval;
	}
	int r = defval;
	switch(v.vt) {
	case VT_UI1: r = v.bVal; break;
	case VT_UI2: r = v.uiVal; break;
	case VT_UI4: r = v.ulVal; break;
	case VT_I2:  r = v.iVal; break;
	case VT_I4:  r = v.lVal; break;
	}
	PropVariantClear(&v);
	return r;
}

// Sanity limit for decompressed size: geo imaging may handle very large
// rasters, so allow up to 4 GB of decoded RGBA data (1 gigapixel); anything
// larger is rejected before any big allocation happens.
static const int64 WIC_MAX_DECODE_BYTES = INT64_C(4) * 1024 * 1024 * 1024;

////////////////////////////////////////////////////////////////////////////////////////////////////
// GeoTIFF tag numbers (unregistered private IFD tags).
static const word WIC_TIF_ModelPixelScale   = 33550;
static const word WIC_TIF_IntergraphMatrix  = 33920;
static const word WIC_TIF_ModelTiepoint      = 33922;
static const word WIC_TIF_ModelTransform     = 34264;
static const word WIC_TIF_GeoKeyDirectory    = 34735;
static const word WIC_TIF_GeoDoubleParams   = 34736;
static const word WIC_TIF_GeoAsciiParams    = 34737;
static const word WIC_TIF_GDALMetadata      = 42112;
static const word WIC_TIF_GDALNoData        = 42113;
static const word WIC_TIF_RPCCoefficient    = 50844;

// GeoTIFF geokey directory entry constants (same numbering as plugin/tif).
static const int  WIC_GK_RasterTypeGeoKey    = 1025;
static const int  WIC_GK_GeographicTypeGeoKey = 2048;
static const int  WIC_GK_ProjectedCSTypeGeoKey = 3072;

////////////////////////////////////////////////////////////////////////////////////////////////////
// WICRaster::Data - multi-frame WIC decoder; serves the active page's rows
// as 32bppBGRA (= U++ RGBA layout on Windows).

class WICRaster::Data {
public:
	struct PageInfo {
		Rect  rect;      // placement of the frame (GIF subimages), full frame otherwise
		int   delay;     // delay in 1/100 sec (GIF)
		int   disposal;  // disposal method (GIF)
	};

	IWICBitmapDecoder *dec;
	IStream           *pis;     // decoder reads pixel data lazily from this
	                             // stream - it must outlive dec (paging)
	HGLOBAL            hg;      // backing store of pis, freed with it

	Data() { dec = NULL; pis = NULL; hg = NULL; page_count = 0; page_index = 0; alpha = false; md_loaded = false; ifd_parsed = false; }
	int                page_count;
	int                page_index;
	Array<PageInfo>    pages;
	Size               size;
	Buffer<RGBA>       pix;
	RasterFormat       fmt;
	bool               alpha;

	bool Create(Stream& stream);
	bool DecodeFrame(int page);
	
	bool WicGeoLookup(const String& id, Value& out) const;
	bool WicRawTagLookup(const String& id, Value& out) const;

	VectorMap<String, Value> md;      // cached per-page metadata
	bool                     md_loaded;
	bool                     ifd_parsed;
	WicIFDParser             ifd;      // parsed TIFF IFD chain (empty for non-TIFF)
	String                   raw;      // TIFF: whole-file bytes, for our own IFD parser

	Value QueryMeta(const String& fullpath);   // direct WIC query, used for
	                                           // tags the enumerator can't list
	bool LoadFrameMeta(int page);     // fills 'md' from the frame query reader
	
	~Data()
	{
		if(dec)
			dec->Release();
		if(pis)
			pis->Release();
		if(hg)
			GlobalFree(hg);
	}
};

bool WICRaster::Data::Create(Stream& stream)
{
	String data = LoadStream(stream);

	if(data.IsEmpty()) {
		WICTRACE("decode_empty", E_ABORT);
		return false;
	}

	WICGuard& w = WIC();
	if(!w.factory) {
		WICTRACE("decode_no_factory", E_FAIL);
		return false;
	}
	WICINFO(String() << "trydecode(" << data.GetCount() << "); ");

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
	// Store the stream lifetime in the members: the WIC decoder reads
	// pixel data lazily on SeekPage, so the backing stream must outlive
	// dec. Both are released in ~Data. (Locals deliberately reused for
	// creation; no shadowing resets here.)
	this->pis = pis;
	this->hg = hg;
	dec = NULL;
	HRESULT dhr = w->CreateDecoderFromStream(pis, NULL, WICDecodeMetadataCacheOnDemand, &dec);
	if(FAILED(dhr))
		WICTRACE("createdecoder", dhr);
	if(SUCCEEDED(dhr)) {
		GUID container;
		HRESULT chr = dec->GetContainerFormat(&container);
		if(FAILED(chr))
			WICTRACE("containerformat", chr);
		if(SUCCEEDED(chr) &&
		   (IsEqualGUID(container, GUID_ContainerFormatPng) ||
		    IsEqualGUID(container, GUID_ContainerFormatJpeg) ||
		    IsEqualGUID(container, GUID_ContainerFormatTiff) ||
		    IsEqualGUID(container, GUID_ContainerFormatBmp) ||
		    IsEqualGUID(container, GUID_ContainerFormatGif))) {
		        
	        if(IsEqualGUID(container, GUID_ContainerFormatTiff)) {
				raw = data;   // keep the bytes for our own IFD parser (metadata)
				ifd_parsed = ifd.Parse(raw);
			}
	        
			UINT fc = 0;
			HRESULT ghr = dec->GetFrameCount(&fc);
			if(FAILED(ghr))
				WICTRACE("getframecount", ghr);
			if(fc == 0)
				WICTRACE("nopages", E_FAIL);
			if(SUCCEEDED(ghr) && fc > 0 && fc <= 0xFFFF) {
				page_count = (int)fc;

				bool isgif = IsEqualGUID(container, GUID_ContainerFormatGif);

				for(UINT i = 0; i < fc; i++) {
					PageInfo& pg = pages.Add();
					pg.rect = Rect(0, 0, 0, 0);
					pg.delay = 0;
					pg.disposal = 0;

					IWICBitmapFrameDecode *frame = NULL;
					HRESULT fhr = dec->GetFrame(i, &frame);
					if(FAILED(fhr)) {
						WICTRACE("enumframe", fhr);
						pages.Drop();
						continue;
					}
					UINT cw = 0, ch = 0;
					if(SUCCEEDED(frame->GetSize(&cw, &ch)) && cw > 0 && ch > 0 &&
					   cw <= 0xFFFF && ch <= 0xFFFF) {
						int x = 0, y = 0;

						IWICMetadataQueryReader *q = NULL;
						HRESULT qhr = frame->GetMetadataQueryReader(&q);
						if(FAILED(qhr))
							WICTRACE("metareader", qhr);
						if(SUCCEEDED(qhr)) {
							// GIF frames are subimages placed at Left/Top with
							// per-frame timing in the graphic control extension.
							// Properties may legitimately be missing - defaults
							// are harmless.
							if(isgif) {
								x = sWICMetaInt(q, L"/imgdesc/Left", 0);
								y = sWICMetaInt(q, L"/imgdesc/Top", 0);
								pg.delay = sWICMetaInt(q, L"/grctlext/Delay",
									sWICMetaInt(q, L"/imgdesc/grctlext/Delay", 0));
								pg.disposal = sWICMetaInt(q, L"/grctlext/Disposal",
									sWICMetaInt(q, L"/imgdesc/grctlext/DisposalMethod", 0));
							}
							q->Release();
						}
						pg.rect = Rect(x, y, x + (int)cw, y + (int)ch);
					}
					frame->Release();
				}

				if(pages.GetCount())
					ok = DecodeFrame(0); // decode the first page eagerly
			}
		}
	}
	// pis and hg are intentionally NOT released here: the WIC decoder reads
	// pixel data lazily on SeekPage, so the stream must outlive dec.
	// They are released in ~Data.

	if(!ok) {
		pix.Clear();
		pages.Clear();
		page_count = 0;
		if(dec) {
			dec->Release();
			dec = NULL;
		}
	}

	return ok;
}

bool WICRaster::Data::DecodeFrame(int page)
{
	WICGuard& w = WIC();
	if(!w.factory || !dec || page < 0 || page >= page_count)
		return false;

	IWICBitmapFrameDecode *frame = NULL;
	HRESULT hr = dec->GetFrame(page, &frame);
	if(FAILED(hr)) {
		WICTRACE("getframe", hr);
		return false;
	}

	bool ok = false;

	UINT cw = 0, ch = 0;
	HRESULT shr = frame->GetSize(&cw, &ch);
	if(FAILED(shr))
		WICTRACE("framesize", shr);
	if(SUCCEEDED(shr) && cw > 0 && ch > 0 &&
	   cw <= 0xFFFF && ch <= 0xFFFF &&
	   (int64)cw * (int64)ch * 4 <= WIC_MAX_DECODE_BYTES) {
		size = Size(cw, ch);
		page_index = page;

		GUID fpf = GUID_WICPixelFormatDontCare;
		HRESULT ffhr = frame->GetPixelFormat(&fpf);
		if(FAILED(ffhr))
			WICTRACE("framepixfmt", ffhr);
		int bpp = 0;
		if(SUCCEEDED(ffhr)) {
			if(IsEqualGUID(fpf, GUID_WICPixelFormat8bppIndexed))      bpp = 8;
			else if(IsEqualGUID(fpf, GUID_WICPixelFormat4bppIndexed)) bpp = 4;
			else if(IsEqualGUID(fpf, GUID_WICPixelFormat2bppIndexed)) bpp = 2;
			else if(IsEqualGUID(fpf, GUID_WICPixelFormat1bppIndexed)) bpp = 1;
		}

		if(bpp) {
			// Indexed source: decode the index plane and map through the
			// frame palette ourselves. The IWICFormatConverter path yields
			// S_OK with all-black output for indexed GIF frames; manual
			// mapping is deterministic and also exposes the raw indices
			// for diagnostics.
			IWICPalette *spal = NULL;
			if(SUCCEEDED(w->CreatePalette(&spal))) {
				HRESULT phr = frame->CopyPalette(spal);
				if(SUCCEEDED(phr)) {
					UINT nc = 0;
					spal->GetColorCount(&nc);
					Buffer<WICColor> cols(max<int>((int)nc, 1));
					UINT got = 0;
					if(nc)
						spal->GetColors(nc, ~cols, &got);
					int stride = (size.cx * bpp + 7) / 8;
					int total = stride * size.cy;
					Buffer<byte> raw(max(total, 1));
					hr = frame->CopyPixels(NULL, stride, total, ~raw);
					ok = SUCCEEDED(hr);
					if(FAILED(hr))
						WICTRACE("rawcopypixels", hr);
				if(ok) {
					WICINFO(String() << "idx[" << (int)raw[0] << ".." << (int)raw[total - 1]
					               << "] " << bpp << "bpp/" << (int)got << "pal; ");
						pix.Alloc(size.cx * size.cy);
						int mask = (1 << bpp) - 1;
						for(int y = 0; y < size.cy; y++) {
							const byte *srow = ~raw + y * stride;
							RGBA *drow = ~pix + y * size.cx;
							for(int x = 0; x < size.cx; x++) {
								int shift = 8 - bpp - ((x * bpp) & 7);
								int idx = (srow[(x * bpp) >> 3] >> shift) & mask;
								dword c = idx < (int)got ? cols[idx] : 0xFF000000;
								drow[x].r = byte((c >> 16) & 0xFF);
								drow[x].g = byte((c >> 8) & 0xFF);
								drow[x].b = byte(c & 0xFF);
								drow[x].a = byte((c >> 24) & 0xFF);
							}
						}
					}
				}
				else
					WICTRACE("framepal", phr);
				spal->Release();
			}
			else
				WICTRACE("nopalfac", E_FAIL);
			fmt.SetRGBA();
		}
		else {
			// Non-indexed source: converter path (as before, no palette
			// involved - the earlier palette passing is irrelevant for a
			// non-indexed destination).
			IWICFormatConverter *conv = NULL;
			IWICBitmapSource *src = frame;
			if(SUCCEEDED(w->CreateFormatConverter(&conv))) {
				HRESULT chr = conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
				                              WICBitmapDitherTypeNone, NULL,
				                              0, WICBitmapPaletteTypeCustom);
				if(SUCCEEDED(chr))
					src = conv;
				else {
					WICTRACE("convinit", chr);
					conv->Release();
				}
			}
			else
				WICTRACE("noconv", E_FAIL);

			pix.Alloc(size.cx * size.cy);
			memset(~pix, 0, sizeof(RGBA) * size.cx * size.cy); // zero-fill: distinguishes 'copy failed' from 'decoded black'
			hr = src->CopyPixels(NULL, size.cx * 4, (UINT)size.cx * size.cy * 4,
			                     (BYTE *)~pix);
			ok = SUCCEEDED(hr);
			if(FAILED(hr))
				WICTRACE("copypixels", hr);

			fmt.SetRGBA();

			if(src == conv)
				conv->Release();
		}

		if(ok) {
			alpha = false;
			for(const RGBA *p = ~pix, *e = p + size.cx * size.cy; p < e; p++)
				if(p->a != 255) {
					alpha = true;
					break;
				}
		}
	}

	frame->Release();

	if(!ok)
		pix.Clear();

	return ok;
}

// PROPVARIANT -> U++ Value conversion shared by the metadata readers.
static Value sPropToValue(const PROPVARIANT& v)
{
	switch(v.vt) {
	case VT_I1:  return (int)v.iVal;
	case VT_I2:  return (int)v.iVal;
	case VT_UI1: case VT_UI2: return (int)v.uiVal;
	case VT_I4:  return (int)v.lVal;
	case VT_UI4: return (int)v.ulVal;
	case VT_R4:  return (double)v.fltVal;
	case VT_R8:  return (double)v.dblVal;
	case VT_BOOL: return v.boolVal ? 1 : 0;
	case VT_LPSTR:  return String(v.pszVal);
	case VT_LPWSTR: return WString(v.pwszVal).ToString();
	case VT_VECTOR | VT_I1:
	case VT_VECTOR | VT_UI1:
	case VT_VECTOR | VT_I2:
	case VT_VECTOR | VT_UI2:
	case VT_VECTOR | VT_I4:
	case VT_VECTOR | VT_UI4:
	case VT_VECTOR | VT_R4:
	case VT_VECTOR | VT_R8:
	case VT_VECTOR | VT_LPSTR:
	case VT_VECTOR | VT_LPWSTR: break;
	default: return Value();
	}
	// Vector payloads (union field name follows the element type):
	Vector<Value> va;
	ULONG n = v.cadbl.cElems;   // cElems layout is identical for all CA* unions
	switch(v.vt & ~VT_VECTOR) {
	case VT_I1:  for(ULONG i = 0; i < n; i++) va.Add((int)v.cai.pElems[i]); break;
	case VT_UI1: for(ULONG i = 0; i < n; i++) va.Add((int)v.caub.pElems[i]); break;
	case VT_I2:  for(ULONG i = 0; i < n; i++) va.Add((int)v.cai.pElems[i]); break;
	case VT_UI2: for(ULONG i = 0; i < n; i++) va.Add((int)v.caui.pElems[i]); break;
	case VT_I4:  for(ULONG i = 0; i < n; i++) va.Add((int)v.cal.pElems[i]); break;
	case VT_UI4: for(ULONG i = 0; i < n; i++) va.Add((int)v.caul.pElems[i]); break;
	case VT_R4:  for(ULONG i = 0; i < n; i++) va.Add((double)v.caflt.pElems[i]); break;
	case VT_R8:  for(ULONG i = 0; i < n; i++) va.Add(v.cadbl.pElems[i]); break;
	case VT_LPSTR:  for(ULONG i = 0; i < n; i++) va.Add(String(v.calpstr.pElems[i])); break;
	case VT_LPWSTR: for(ULONG i = 0; i < n; i++) va.Add(WString(v.calpwstr.pElems[i]).ToString()); break;
	}
	return Value(va);
}

// Acquire a metadata query reader; frame-level first, container-level as
// fallback (the WIC TIFF handler keeps the IFD chain at container scope,
// while GIF/EXIF live at frame scope).
static IWICMetadataQueryReader *sQueryReader(IWICBitmapDecoder *dec,
                                             IWICBitmapFrameDecode *frame, int page)
{
	IWICMetadataQueryReader *q = NULL;
	if(frame && SUCCEEDED(frame->GetMetadataQueryReader(&q)))
		return q;
	if(dec && page >= 0 && SUCCEEDED(dec->GetMetadataQueryReader(&q)))
		return q;
	return NULL;
}

bool WICRaster::Data::LoadFrameMeta(int page)
{
	md.Clear();
	if(!dec || page < 0 || page >= page_count)
		return false;
	IWICBitmapFrameDecode *frame = NULL;
	if(FAILED(dec->GetFrame(page, &frame)))
		return false;
	IWICMetadataQueryReader *q = sQueryReader(dec, frame, page);
	frame->Release();
	if(!q)
		return false;
	IEnumString *paths = NULL;
	if(SUCCEEDED(q->GetEnumerator(&paths))) {
		LPOLESTR p = NULL;
		while(paths->Next(1, &p, NULL) == S_OK) {
			String path = WString(p).ToString();
			CoTaskMemFree(p);
			String full = path;
			if(!full.StartsWith("/"))
				full = "/" + full;
			PROPVARIANT v;
			PropVariantInit(&v);
			if(SUCCEEDED(q->GetMetadataByName((LPCWSTR)~full.ToWString(), &v))) {
				Value val = sPropToValue(v);
				PropVariantClear(&v);
				String key = path;
				if(key.StartsWith("ifd/"))
					key = key.Mid(4);
				md.GetAdd(key) = val;
			}
			else
				PropVariantClear(&v);
		}
		paths->Release();
	}
	q->Release();
	return md.GetCount() > 0;
}

Value WICRaster::Data::QueryMeta(const String& fullpath)
{
	if(!dec || page_index < 0 || page_index >= page_count)
		return Value();
	IWICBitmapFrameDecode *frame = NULL;
	if(FAILED(dec->GetFrame(page_index, &frame)))
		return Value();
	IWICMetadataQueryReader *q = sQueryReader(dec, frame, page_index);
	frame->Release();
	if(!q) {
		WICTRACE("qm_noreader", E_FAIL);
		return Value();
	}
	// Path variants: verbatim, no leading slash, and root-relative form
	// (drop an "ifd/" segment) - covers every layout dialect we have seen.
	Vector<String> cand;
	cand.Add(fullpath);
	if(fullpath.StartsWith("/"))
		cand.Add(fullpath.Mid(1));
	int fi = fullpath.Find("ifd/");
	if(fi >= 0) {
		String tail = fullpath.Mid(fi + 4);
		cand.Add("/" + tail);
		cand.Add(tail);
	}
	// Reader levels: try frame-level via sQueryReader (which itself already
	// falls back to container), then force container-only as the last shot.
	IWICMetadataQueryReader *qc = NULL;
	if(dec)
		dec->GetMetadataQueryReader(&qc);
	for(int pass = 0; pass < 2; pass++) {
		IWICMetadataQueryReader *r = pass == 0 ? q : qc;
		if(!r)
			continue;
		for(const String& c : cand) {
			PROPVARIANT v;
			PropVariantInit(&v);
			if(SUCCEEDED(r->GetMetadataByName((LPCWSTR)~c.ToWString(), &v))) {
				Value val = sPropToValue(v);
				PropVariantClear(&v);
				q->Release();
				if(qc)
					qc->Release();
				WICINFO(String() << "qm_hit(" << c << "); ");
				return val;
			}
			PropVariantClear(&v);
		}
	}
	WICINFO(String() << "qm_miss(" << fullpath << "); ");
	q->Release();
	if(qc)
		qc->Release();
	return Value();
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// WICRaster

// GeoTIFF friendly-key lookup, plugin/tif-compatible namespace: "epsg",
// "GK<n>" (one per geokey), "geo_matrix", "nodata". Derivation follows the
// same scheme as tifupp.cpp's TIFRaster: EPSG from geokeys 2048/3072,
// GK<n> values resolved through GeoAsciiParams / GeoDoubleParams by the
// key's location offset (raw offset semantics, matching plugin/tif).
bool WICRaster::Data::WicGeoLookup(const String& id, Value& out) const
{
	if(!ifd_parsed)
		return false;
	int pi = page_index < ifd.ifds.GetCount() ? page_index : 0;
	const WicIFDTag *gkd = ifd.Find(pi, WIC_TIF_GeoKeyDirectory);

	if(id == "epsg") {
		if(gkd && gkd->type == 3 && gkd->count >= 4 && (gkd->count % 4) == 0) {
			ValueArray kv = gkd->value.Get<ValueArray>();
			for(int i = 4; i + 3 < kv.GetCount(); i += 4) {
				int key = (int)kv[i];
				int loc = (int)kv[i + 1];
				int off = (int)kv[i + 3];
				if((key == WIC_GK_GeographicTypeGeoKey || key == WIC_GK_ProjectedCSTypeGeoKey) && loc == 0) {
					out = off;
					return true;
				}
			}
		}
		return false;
	}

	if(id == "geo_matrix") {
		const WicIFDTag *mt = ifd.Find(pi, WIC_TIF_ModelTransform);
		if(mt && mt->count == 16 && mt->type == 12) {
			out = mt->value;
			return true;
		}
		const WicIFDTag *tp = ifd.Find(pi, WIC_TIF_ModelTiepoint);
		const WicIFDTag *ps = ifd.Find(pi, WIC_TIF_ModelPixelScale);
		if(tp && ps && tp->type == 12 && ps->type == 12 && tp->count >= 6 && ps->count >= 3) {
			ValueArray tv = tp->value.Get<ValueArray>();
			ValueArray sv = ps->value.Get<ValueArray>();
			double sx = (double)sv[0], sy = (double)sv[1], sz = (double)sv[2];
			ValueArray m;
			m.Add(sx);  m.Add(0.0); m.Add(0.0); m.Add((double)tv[3] - (double)tv[0] * sx);
			m.Add(0.0); m.Add(sy);  m.Add(0.0); m.Add((double)tv[4] - (double)tv[1] * sy);
			m.Add(0.0); m.Add(0.0); m.Add(sz);  m.Add((double)tv[5] - (double)tv[2] * sz);
			m.Add(0.0); m.Add(0.0); m.Add(0.0); m.Add(1.0);
			out = Value(m);
			return true;
		}
		return false;
	}

	if(id == "nodata") {
		const WicIFDTag *nd = ifd.Find(pi, WIC_TIF_GDALNoData);
		if(nd && nd->type == 2) {
			out = nd->value;
			return true;
		}
		return false;
	}

	if(id.StartsWith("GK") && gkd && gkd->type == 3 && gkd->count >= 4 && (gkd->count % 4) == 0) {
		int want = ScanInt(id.Mid(2));
		if(want > 0) {
			ValueArray kv = gkd->value.Get<ValueArray>();
			const WicIFDTag *gd = ifd.Find(pi, WIC_TIF_GeoDoubleParams);
			const WicIFDTag *ga = ifd.Find(pi, WIC_TIF_GeoAsciiParams);
			for(int i = 4; i + 3 < kv.GetCount(); i += 4) {
				if((int)kv[i] != want)
					continue;
				int loc = (int)kv[i + 1];
				int cnt = (int)kv[i + 2];
				int off = (int)kv[i + 3];
				if(loc == 0) {                      // inline SHORT value
					out = off;
					return true;
				}
				if(ga && ga->type == 2) {           // offset into GeoAsciiParams
					String ascii = ga->value;
					if(off >= 0 && off + cnt <= ascii.GetCount()) {
						out = ascii.Mid(off, cnt);
						return true;
					}
					return false;
				}
				if(gd && gd->type == 12) {          // index into GeoDoubleParams
					ValueArray dv = gd->value.Get<ValueArray>();
					if(off < dv.GetCount()) {
						out = dv[off];
						return true;
					}
					return false;
				}
				return false;
			}
		}
		return false;
	}
	return false;
}

// Raw tag access by number: GetMetaData("33550"), GetMetaData("tiff:33550")
// or the legacy WIC-query form GetMetaData("{ushort=33550}").
bool WICRaster::Data::WicRawTagLookup(const String& id, Value& out) const
{
	if(!ifd_parsed)
		return false;
	int tagnum = -1;
	if(IsDigit(*id))
		tagnum = ScanInt(id);
	else if(id.StartsWith("tiff:"))
		tagnum = ScanInt(id.Mid(5));
	else if(id.StartsWith("{ushort="))
		tagnum = ScanInt(id.Mid(8));
	if(tagnum <= 0)
		return false;
	int pi = page_index < ifd.ifds.GetCount() ? page_index : 0;
	for(const WicIFDTag& t : ifd.ifds[pi])
		if(t.tag == tagnum) {
			out = t.value;
			return true;
		}
	return false;
}

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
		info.dots = data->size;
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

int WICRaster::GetPageCount()
{
	return data ? data->page_count : 0;
}

int WICRaster::GetActivePage() const
{
	return data ? data->page_index : 0;
}

void WICRaster::SeekPage(int page)
{
	if(data)
		data->DecodeFrame(page);
}

int WICRaster::GetPageDelay(int n)
{
	if(!data || n < 0 || n >= data->pages.GetCount())
		return 0;
	return data->pages[n].delay;
}

Rect WICRaster::GetPageRect(int n)
{
	if(!data || n < 0 || n >= data->pages.GetCount())
		return Rect(0, 0, 0, 0);
	return data->pages[n].rect;
}

int WICRaster::GetPageDisposal(int n)
{
	if(!data || n < 0 || n >= data->pages.GetCount())
		return 0;
	return data->pages[n].disposal;
}

Value WICRaster::GetMetaData(String id)
{
	if(!data)
		return Value();
	// Enumeration may legitimately yield nothing (WIC enumerates metadata
	// blocks, and a plain TIFF IFD has none) - that is NOT a failure and
	// must not block the direct-query fallback below. Only try loading
	// once per page (md stays empty; QueryMeta works from the readers).
	if(data->md.IsEmpty() && !data->md_loaded) {
		data->md_loaded = true;
		data->LoadFrameMeta(data->page_index);
	}
	// Exact match first ("{ushort=33550}" style, as EnumMetaData reports):
	int f = data->md.Find(id);
	if(f >= 0)
		return data->md[f];
	// Direct query fallback: WIC's TIFF enumerator does not list individual
	// IFD tags (LoadFrameMeta can only harvest what enumeration yields),
	// so arbitrary tags - e.g. GeoTIFF 33550 - must be queried by full
	// path. Ids starting with '{' get the /ifd/ block prefix; ids starting
	// with '/' or containing '/' are treated as explicit query paths and
	// passed through verbatim (used by tests/probes).
	if(*id == '{') {
		Value v = data->QueryMeta("/ifd/" + id);
		if(!v.IsNull())
			return v;
		// WIC misses TIFF IFD tags on every variant tested - fall through
		// to our own parser, which understands the {ushort=N} form too.
	}
	if(id.Find('/') >= 0)
		return data->QueryMeta(id);
	// GeoTIFF / TIFF native-tag access: our own IFD parse (independent of
	// WIC's query system, which does not expose these tags). Plugin/tif-
	// compatible friendly keys ("epsg", "GK<n>", "geo_matrix", "nodata")
	// plus raw tag ids ("33550", "tiff:33550", legacy "{ushort=33550}").
	{
		Value v;
		if(data->WicGeoLookup(id, v))
			return v;
		if(data->WicRawTagLookup(id, v))
			return v;
	}
	return Value();
}

void WICRaster::EnumMetaData(Vector<String>& id_list)
{
	id_list.Clear();
	if(!data)
		return;
	if(data->md.IsEmpty() && !data->md_loaded) {
		data->md_loaded = true;
		data->LoadFrameMeta(data->page_index);
	}
	for(int i = 0; i < data->md.GetCount(); i++)
		id_list.Add(data->md.GetKey(i));
	// TIFF: surface the tags our own IFD parser found (WIC enumeration
	// never lists them). Friendly geo keys first, then raw tag numbers.
	if(data->ifd_parsed && data->page_index < data->ifd.ifds.GetCount()) {
		static const char *friendly[] = { "epsg", "geo_matrix", "nodata" };
		for(const char *fk : friendly) {
			Value v;
			if(data->WicGeoLookup(fk, v))
				id_list.Add(fk);
		}
		const WicIFDTag *gkd = data->ifd.Find(data->page_index, WIC_TIF_GeoKeyDirectory);
		if(gkd && gkd->type == 3 && gkd->count >= 4 && (gkd->count % 4) == 0) {
			ValueArray kv = gkd->value.Get<ValueArray>();
			for(int i = 4; i + 3 < kv.GetCount(); i += 4)
				id_list.Add("GK" + AsString((int)kv[i]));
		}
		for(const WicIFDTag& t : data->ifd.ifds[data->page_index])
			id_list.Add("tiff:" + AsString((int)t.tag));
	}
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
	WICINFO(String() << "bytes=" << len << "; ");
	stream->Put64(out->Ptr(), len);
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

	sPendingContainer = String(sWICContainerName(container)) << ": ";

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
			WICTRACE("imagequality", hr2);
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
	WICINFO(String() << "pf=" << sWICPFName(negotiated) << ", ");
	d->ok = true;
	sPendingContainer.Clear();   // no failure: eat the container prefix
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
