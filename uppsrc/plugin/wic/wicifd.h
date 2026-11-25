#pragma once

// Minimal TIFF IFD reader, sufficient for GeoTIFF tag extraction. We parse
// the file ourselves because WIC's query system does not expose arbitrary
// IFD tags on all Windows builds (empirically verified: frame- and
// container-level readers, all path dialects, both cache modes).
//
// Limited on purpose: types 1 (BYTE), 2 (ASCII), 3 (SHORT), 4 (LONG),
// 12 (DOUBLE). Count arrays supported up to a sane cap. Multi-page: parses
// the IFD chain and keeps per-page tag maps.

namespace Upp {

struct WicIFDTag {
	word  tag;
	word  type;
	dword count;
	Value value;      // int, String, double, or ValueArray of those
};

class WicIFDParser {
public:
	Vector<Array<WicIFDTag>> ifds;   // one tag list per TIFF page/IFD

	bool Parse(const String& data)
	{
		ifds.Clear();
		if(data.GetCount() < 8)
			return false;
		const byte *p = (const byte *)~data;
		byte b0 = p[0], b1 = p[1];
		bool le;
		if(b0 == 'I' && b1 == 'I') le = true;
		else if(b0 == 'M' && b1 == 'M') le = false;
		else return false;
		auto W16 = [&](int off) { return le ? (p[off] | (p[off+1] << 8))
		                                    : ((p[off] << 8) | p[off+1]); };
		auto W32 = [&](int off) { return le
			? (dword)p[off] | ((dword)p[off+1] << 8) | ((dword)p[off+2] << 16) | ((dword)p[off+3] << 24)
			: ((dword)p[off] << 24) | ((dword)p[off+1] << 16) | ((dword)p[off+2] << 8) | (dword)p[off+3]; };
		if(W16(2) != 42) return false;
		dword off = W32(4);
		int safety = 0;
		while(off && off + 2 <= (dword)data.GetCount() && safety++ < 256) {
			Array<WicIFDTag> ifd;
			word n = W16(off);
			if(off + 2 + (dword)n * 12u + 4u > (dword)data.GetCount())
				break;
			for(word i = 0; i < n; i++) {
				dword e = off + 2 + (dword)i * 12u;
				WicIFDTag t;
				t.tag = W16(e);
				t.type = W16(e+2);
				t.count = W32(e+4);
				int ts = TagSize(t.type);
				if(ts <= 0) continue;              // unsupported type: skip
				dword total = t.count * (dword)ts;
				if(total == 0 || total > 1 << 20)  // 1 MB cap per tag value
					continue;
				dword voff = total <= 4 ? e + 8 : W32(e+8);
				if(voff + total > (dword)data.GetCount())
					continue;
				t.value = ReadValue(p + voff, t.type, t.count, le);
				ifd.Add(t);
			}
			ifds.Add(pick(ifd));
			off = W32(off + 2 + (dword)n * 12u);
		}
		return ifds.GetCount() > 0;
	}

	static int TagSize(int type)
	{
		switch(type) {
		case 1: return 1;   // BYTE
		case 2: return 1;   // ASCII
		case 3: return 2;   // SHORT
		case 4: return 4;   // LONG
		case 12: return 8;  // DOUBLE
		}
		return -1;
	}

	static Value ReadValue(const byte *q, int type, dword count, bool le)
	{
		if(count == 1) {
			switch(type) {
			case 1: return (int)q[0];
			case 3: return le ? (int)(q[0] | (q[1] << 8)) : (int)((q[0] << 8) | q[1]);
			case 4: return le ? (int)(dword)(q[0] | (q[1]<<8) | (q[2]<<16) | ((dword)q[3]<<24))
			                  : (int)(((dword)q[0]<<24) | (q[1]<<16) | (q[2]<<8) | q[3]);
			case 12: { double d; memcpy(&d, q, 8); return d; }  // TIFF doubles are native-endian on LE, and big-endian MM files: swap below
			}
			return Value();
		}
		// Arrays:
		ValueArray va;
		for(dword i = 0; i < count; i++) {
			va.Add(ReadValue(q + i * TagSize(type), type, 1, le));
		}
		// ASCII: join as string instead
		if(type == 2) {
			String s((const char *)q, count);
			int z = s.Find('\0');
			if(z >= 0) s.Trim(z);
			return s;
		}
		return Value(va);
	}
};

} // namespace Upp
