"""Resource groups: the XBE holds NAME_start / NAME_end strings and a sorted
   (crc32(name), entry index) table; a group is the entries [start, end)."""
import re, struct, zlib, sys, collections, json
sys.path.insert(0, __file__.rsplit('/', 1)[0])
from rbb import Rbb
def groups(xbe_path):
    d = open(xbe_path, 'rb').read()
    names = sorted(set(m.group(1).decode() for m in re.finditer(rb'([A-Za-z0-9_]+)_start\0', d)))
    # locate the table by one known crc
    key = struct.pack('<I', zlib.crc32(b'KURENAI_start') & 0xffffffff)
    o = d.find(key)
    lo = o
    while struct.unpack_from('<I', d, lo - 8)[0] < struct.unpack_from('<I', d, lo)[0]: lo -= 8
    tab = {}
    p = lo
    while True:
        c, v = struct.unpack_from('<II', d, p)
        if p > lo and c <= struct.unpack_from('<I', d, p - 8)[0]: break
        tab[c] = v; p += 8
    out = {}
    for n in names:
        s = tab.get(zlib.crc32((n + '_start').encode()) & 0xffffffff)
        e = tab.get(zlib.crc32((n + '_end').encode()) & 0xffffffff)
        if s is not None and e is not None: out[n] = (s, e)
    return out, len(tab)
if __name__ == '__main__':
    g, nt = groups(sys.argv[1]); r = Rbb(sys.argv[2])
    print(len(g), 'groups of', nt, 'table entries;', r.n, 'archive entries')
    for n, (s, e) in sorted(g.items(), key=lambda x: x[1]):
        c = collections.Counter(t[:3] for t in r.types[s:e])
        print('%-28s %5d..%5d %s' % (n, s, e, ' '.join('%s:%d' % kv for kv in c.most_common(8))))
