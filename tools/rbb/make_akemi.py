"""Akemi as the player: the patched copy of her mesh the game loads in place
   of Kurenai's (F6 > Costume > Akemi).  Run once, from the project folder:

       python tools/rbb/make_akemi.py bin/game/media/res.rbb bin/mods/akemi_mesh.bin

   Her mesh (CL01, entry 2221) lacks a node the player code looks up by name,
   bb_hanger -- the point Kurenai is placed by while she works a lever or
   hangs from something.  Without it the lookup fails and she is thrown
   across the map.  A mesh's nodes are 160-byte records (name, local 4x4,
   a second 4x4) after a table of parent indices at +0x64; the node count is
   the 16-bit word at +0x46.  One of her nodes that nothing looks up by name,
   the pole-vector helper pv_hand_l, is renamed bb_hanger and given the
   local matrix Kurenai's has; like Kurenai's it hangs off `base`.  The
   output is the patched mesh, zlib-packed as the archive's entries are."""
import struct, sys, zlib
sys.path.insert(0, __file__.replace('\\', '/').rsplit('/', 1)[0])
from rbb import Rbb

def nodes(d):
    n = struct.unpack_from('<H', d, 0x46)[0]
    first = 0x64 + n * 4
    first = (first + 31) & ~31 if False else first
    # the records follow the parent table, padded to 32 bytes
    while d[first:first + 4] != b'base': first += 4
    return n, first

def main(rbb_path, out_path):
    r = Rbb(rbb_path)
    k = r.get(202); a = bytearray(r.get(2221))
    kn, kf = nodes(k); an, af = nodes(a)
    kname = [k[kf + i*160: kf + i*160 + 32].split(b'\0')[0].decode() for i in range(kn)]
    aname = [bytes(a[af + i*160: af + i*160 + 32]).split(b'\0')[0].decode() for i in range(an)]
    if 'bb_hanger' in aname:
        raise SystemExit('already has bb_hanger')
    src = kname.index('bb_hanger'); dst = aname.index('pv_hand_l')
    kpar = struct.unpack_from('<%di' % kn, k, 0x64); apar = struct.unpack_from('<%di' % an, a, 0x64)
    assert kname[kpar[src]] == 'base' and aname[apar[dst]] == 'base', (kpar[src], apar[dst])
    a[af + dst*160: af + dst*160 + 160] = k[kf + src*160: kf + src*160 + 160]
    import os
    os.makedirs(os.path.dirname(out_path) or '.', exist_ok=True)
    open(out_path, 'wb').write(zlib.compress(bytes(a), 9))
    print('node %d pv_hand_l -> bb_hanger; %d bytes -> %s' % (dst, len(a), out_path))

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
