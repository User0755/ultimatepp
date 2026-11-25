#ifndef _plugin_wic_wic_h_
#define _plugin_wic_wic_h_

#include <Draw/Draw.h>

namespace Upp {

#ifdef PLATFORM_WIN32

// Windows Imaging Component (WIC) based raster support.
//
// Purpose: allow Windows builds to load and store images through the
// operating system imaging stack instead of libpng / libjpeg-turbo /
// libtiff, removing those vcpkg dependencies. Covers PNG, JPEG, TIFF,
// BMP and GIF. Non-Windows builds are unaffected: this package compiles
// to nothing there.
//
// One decode class serves every container WIC understands. Create()
// verifies the container and refuses the ones we do not manage (ICO and
// other exotic containers), so their traditional plugins keep precedence.

class WICRaster : public StreamRaster {
public:
	class Data;
	One<Data> data;

public:
	virtual bool    Create();
	virtual Size    GetSize();
	virtual Info    GetInfo();
	virtual Line    GetLine(int line);
	virtual int     GetPaletteCount();
	virtual const RGBA *GetPalette();
	virtual const RasterFormat *GetFormat();

	WICRaster();
	virtual ~WICRaster();
};

typedef WICRaster WICPNGRaster;
typedef WICRaster WICJPGRaster;
typedef WICRaster WICTIFRaster;
typedef WICRaster WICBMPRaster;
typedef WICRaster WICGIFRaster;

// Generic WIC stream encoder, configured with the container format GUID.
// Scanlines arrive from RasterEncoder as standard U++ RGBA (b, g, r, a byte
// order on Windows), which is byte-identical to WIC 32bppBGRA. WIC performs
// any format conversion the container needs (e.g. JPEG drops alpha,
// GIF quantizes).
class WICStreamEncoder : public StreamRasterEncoder {
public:
	class Data;
	One<Data> data;

	GUID   container;
	double quality;   // 0..100, used for JPEG
	int    bpp;       // advisory, TIFF bits-per-pixel

	WICStreamEncoder(const GUID& container_);
	virtual ~WICStreamEncoder();

	WICStreamEncoder& Quality(int q) { quality = q; return *this; }

	virtual int  GetPaletteCount();
	virtual void Start(Size sz);
	virtual void WriteLineRaw(const byte *data);
};

// Concrete encoders are declared here and defined in wicupp.cpp so that
// the WIC GUIDs stay local to this package (no windowscodecs.lib / uuid.lib
// needed by includers).

class WICPNGEncoder : public WICStreamEncoder {
public:
	WICPNGEncoder();
};

class WICJPGEncoder : public WICStreamEncoder {
public:
	WICJPGEncoder(int q = 50);
};

class WICTIFEncoder : public WICStreamEncoder {
public:
	WICTIFEncoder(int bpp = 24);
};

class WICBMPEncoder : public WICStreamEncoder {
public:
	WICBMPEncoder();
};

class WICGIFEncoder : public WICStreamEncoder {
public:
	WICGIFEncoder();
};

bool IsWICJPG(StreamRaster *s);

// Diagnostic: human-readable trace of the last encoder run(s):
// "<container>:<stage>=0x<hresult>;..." Accumulated across attempts.
// Empty when everything succeeded.
String WICDiag();

#else
// Windows-only package. Nothing to expose on other platforms; the header
// must still be includable so that dependent packages can be built
// unconditionally.
#endif

}

#endif
