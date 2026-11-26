#pragma once

// Minimal TIFF IFD reader, sufficient for GeoTIFF tag extraction. We parse
// the file ourselves because WIC's query system does not expose arbitrary
// IFD tags on all Windows builds (empirically verified: frame- and
// container-level readers, all path dialects, both cache modes).
//
// Limited on purpose: types 1 (BYTE), 2 (ASCII), 3 (SHORT), 4 (LONG),
// 12 (DOUBLE). Count arrays supported up to a sane cap. Multi-page: parses
// the IFD chain and keeps per-page tag maps. Per-tag payload capped at 1 MB;
// IFD chain capped at 256 pages.

namespace Upp {

struct WicIFDTag {
	word  tag;
	word  type;
	dword count;
	Value value;      // int, double, String, or ValueArray of those
};

class WicIFDParser {
	const byte *p = NULL;
	int    len = 0;
	bool   le = true;

	word  W16(int off) const  { return le ? word(p[off] | (p[off + 1] << 8))
	                                     : word((p[off] << 8) | p[off + 1]); }
	dword W32(int off) const  { return le
		? dword(p[off]) | (dword(p[off + 1]) << 8) | (dword(p[off + 2]) << 16) | (dword(p[off + 3]) << 24)
		: (dword(p[off]) << 24) | (dword(p[off + 1]) << 16) | (dword(p[off + 2]) << 8) | dword(p[off + 3]); }

	double W64(int off) const
	{
		byte buf[8];
		memcpy(buf, p + off, 8);
		if(!le) {
			byte t;
			t = buf[0]; buf[0] = buf[7]; buf[7] = t;
			t = buf[1]; buf[1] = buf[6]; buf[6] = t;
			t = buf[2]; buf[2] = buf[5]; buf[5] = t;
			t = buf[3]; buf[3] = buf[4]; buf[4] = t;
		}
		double d;
		memcpy(&d, buf, 8);
		return d;
	}

	static int TypeSize(int type)
	{
		switch(type) {
		case 1:  case 2: return 1;   // BYTE, ASCII
		case 3: return 2;           // SHORT
		case 4: return 4;           // LONG
		case 12: return 8;          // DOUBLE
		}
		return -1;
	}

	Value ReadScalar(int off, int type) const
	{
		switch(type) {
		case 1:  return (int)p[off];
		case 2:  return (int)p[off];
		case 3:  return (int)W16(off);
		case 4:  return (int)W32(off);
		case 12: return W64(off);
		}
		return Value();
	}

public:
	Vector<Array<WicIFDTag>> ifds;   // one tag list per TIFF page/IFD

	bool Parse(const String& data)
	{
		ifds.Clear();
		p = (const byte *)~data;
		len = data.GetCount();
		if(len < 8)
			return false;
		if(p[0] == 'I' && p[1] == 'I')
			le = true;
		else if(p[0] == 'M' && p[1] == 'M')
			le = false;
		else
			return false;
		if(W16(2) != 42)
			return false;
		int off = (int)W32(4);
		int guard = 0;
		while(off && off + 2 <= len && guard++ < 256) {
			int n = W16(off);
			int eend = off + 2 + n * 12 + 4;
			if(eend > len)
				break;
			Array<WicIFDTag> ifd;
			for(int i = 0; i < n; i++) {
				int e = off + 2 + i * 12;
				WicIFDTag t;
				t.tag   = W16(e);
				t.type  = W16(e + 2);
				t.count = W32(e + 4);
				int ts = TypeSize(t.type);
				if(ts < 1)
					continue;
				int64 total = (int64)t.count * ts;
				if(total < 1 || total > (1 << 20))
					continue;
				int voff = total <= 4 ? e + 8 : (int)W32(e + 8);
				if(voff < 0 || voff + total > len)
					continue;
				if(t.type == 2) { // ASCII: single string
					String s((const char *)p + voff, (int)t.count);
					int z = s.Find('\0');
					if(z >= 0)
						s.Trim(z);
					t.value = s;
				}
				else if(t.count == 1) {
					t.value = ReadScalar(voff, t.type);
				}
				else {
					ValueArray va;
					for(dword i2 = 0; i2 < t.count; i2++)
						va.Add(ReadScalar(voff + int(i2) * ts, t.type));
					t.value = Value(va);
				}
				ifd.Add(t);
			}
			ifds.Add(pick(ifd));
			off = (int)W32(off + 2 + n * 12);
		}
		return ifds.GetCount() > 0;
	}

	const WicIFDTag *Find(int ifdi, int tag) const
	{
		if(ifdi < 0 || ifdi >= ifds.GetCount())
			return NULL;
		for(const WicIFDTag& t : ifds[ifdi])
			if(t.tag == tag)
				return &t;
		return NULL;
	}
};

} // namespace Upp
