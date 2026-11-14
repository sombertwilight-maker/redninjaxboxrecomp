"""Red Ninja res.rbb reader: siff container, zlib-compressed entries.
   chunks: RSRC, RIDX (offset, size|0x80000000 per entry), TYPE (4cc per entry),
   EXIX (unpacked size per entry), GAP0, RBB0 (data)."""
import struct, zlib
class Rbb:
    def __init__(self, path):
        self.f = f = open(path, 'rb')
        o = 8; self.ch = {}
        import os
        size = os.path.getsize(path)
        while o < size:
            f.seek(o); h = f.read(8)
            if len(h) < 8: break
            tag, sz = h[:4], struct.unpack('<I', h[4:])[0]
            self.ch[tag] = (o + 8, sz); o += 8 + sz
        o, sz = self.ch[b'RIDX']; f.seek(o); idx = f.read(sz)
        self.n = sz // 8
        self.ents = [struct.unpack_from('<II', idx, i * 8) for i in range(self.n)]
        o, sz = self.ch[b'TYPE']; f.seek(o); t = f.read(sz)
        self.types = [t[i*4:i*4+4].decode('latin1') for i in range(self.n)]
        o, sz = self.ch[b'EXIX']; f.seek(o); t = f.read(sz)
        self.usize = [struct.unpack_from('<I', t, i*4)[0] for i in range(self.n)]
        self.base = self.ch[b'RBB0'][0]
    def raw(self, i):
        off, sz = self.ents[i]
        self.f.seek(self.base + off); return self.f.read(sz & 0x7fffffff), sz >> 31
    def get(self, i):
        d, c = self.raw(i)
        return zlib.decompress(d) if c else d
