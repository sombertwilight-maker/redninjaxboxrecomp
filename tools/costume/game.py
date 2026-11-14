"""Read a player mesh out of res.rbb: skeleton, bone palette, vertices, faces."""
import struct, sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'rbb'))
import rbb


def _u(d, o):  return struct.unpack_from('<I', d, o)[0]
def _s(d, o):  return struct.unpack_from('<h', d, o)[0]
def _f(d, o):  return struct.unpack_from('<f', d, o)[0]
def _m4(d, o): return [[_f(d, o + 16 * r + 4 * c) for c in range(4)] for r in range(4)]


class Player:
    """Entry 202: Kurenai's body. 74 bytes a vertex, three bone influences."""
    NODE0, NODE_STRIDE = 0x140, 0xA0
    REC0, REC_STRIDE = 0x2050, 0x110
    VERT0, NVERT, STRIDE = 0x5020, 1789, 74

    def __init__(self, path, entry=202):
        self.d = rbb.Rbb(path).get(entry)
        self.nodes = []          # (name, local bind, inverse bind)
        d = self.d
        o = self.NODE0
        while o + self.NODE_STRIDE <= len(d):
            nm = d[o:o + 0x20].split(b'\0')[0].decode('latin1', 'replace')
            if not nm or not (nm[0].isalpha() or nm[0] == '_'):
                break
            self.nodes.append((nm, _m4(d, o + 0x20), _m4(d, o + 0x60)))
            o += self.NODE_STRIDE
        self.slot_node = {}      # shader slot (negative) -> node index
        o = self.REC0
        while o + self.REC_STRIDE <= len(d):
            ty = _u(d, o + 4)
            if ty > 7:
                break
            if ty == 3:
                sl = _u(d, o + 8)
                self.slot_node[sl - (1 << 32) if sl >= (1 << 31) else sl] = _u(d, o + 0xC)
            o += self.REC_STRIDE

    def vertex(self, v):
        """(influences, uv) where each influence is (node, weight, weighted xyz)."""
        d, o = self.d, self.VERT0 + v * self.STRIDE
        infl = []
        for i, so in enumerate((0x44, 0x46, 0x48)):
            w = _f(d, o + i * 16 + 12)
            if w <= 0.001:
                continue
            node = self.slot_node.get(_s(d, o + so))
            infl.append((node, w, (_f(d, o + i * 16), _f(d, o + i * 16 + 4), _f(d, o + i * 16 + 8))))
        return infl, (_f(d, o + 0x3C), _f(d, o + 0x40))

    def raw_vertex(self, v):
        o = self.VERT0 + v * self.STRIDE
        return self.d[o:o + self.STRIDE]

    def faces(self, first=0x27104, last=0x29240):
        """Triangles from the recorded ARRAY_ELEMENT16 strip."""
        d, p, idx = self.d, first, []
        while p < last - 4:
            w = _u(d, p)
            cnt, meth = (w >> 18) & 0x7FF, w & 0x1FFF
            if cnt == 0:
                p += 4
                continue
            if meth == 0x1800:
                for k in range(cnt):
                    v = _u(d, p + 4 + 4 * k)
                    idx.append(v & 0xFFFF)
                    idx.append((v >> 16) & 0xFFFF)
            p += 4 + 4 * cnt
        tris = []
        for i in range(len(idx) - 2):
            a, b, c = idx[i], idx[i + 1], idx[i + 2]
            if 0xFFFF in (a, b, c) or a == b or b == c or a == c:
                continue
            tris.append((a, b, c) if i % 2 == 0 else (b, a, c))
        return tris

    def bind_pose(self):
        """node index -> its bind transform in model space (inverse of the stored inverse)."""
        import mat
        out = {}
        for i, (nm, loc, inv) in enumerate(self.nodes):
            out[i] = mat.inverse(inv)
        return out
