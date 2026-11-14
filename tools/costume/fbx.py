"""Enough of binary FBX 7.x to read a skinned model: meshes, skin weights,
bind matrices, and which material each polygon uses."""
import struct, zlib


class Node:
    __slots__ = ('name', 'props', 'kids')

    def __init__(self, name, props, kids):
        self.name, self.props, self.kids = name, props, kids

    def find(self, name):
        return [k for k in self.kids if k.name == name]

    def first(self, name):
        k = self.find(name)
        return k[0] if k else None


def _array(d, o, kind):
    n, enc, clen = struct.unpack_from('<III', d, o); o += 12
    raw = d[o:o + clen]; o += clen
    if enc:
        raw = zlib.decompress(raw)
    fmt = {'f': 'f', 'd': 'd', 'l': 'q', 'i': 'i', 'b': 'b'}[kind]
    return list(struct.unpack('<%d%s' % (n, fmt), raw)), o


def _prop(d, o):
    t = chr(d[o]); o += 1
    if t in 'YCIFDL':
        sz = {'Y': 2, 'C': 1, 'I': 4, 'F': 4, 'D': 8, 'L': 8}[t]
        f = {'Y': '<h', 'C': '<b', 'I': '<i', 'F': '<f', 'D': '<d', 'L': '<q'}[t]
        return struct.unpack_from(f, d, o)[0], o + sz
    if t in 'fdlib':
        return _array(d, o, t)
    if t in 'SR':
        n = struct.unpack_from('<I', d, o)[0]; o += 4
        v = d[o:o + n]; o += n
        return (v.decode('utf-8', 'replace') if t == 'S' else v), o
    raise ValueError('property type %r at %d' % (t, o))


def _node(d, o, ver):
    if ver >= 7500:
        end, nprop, plen = struct.unpack_from('<QQQ', d, o); o += 24
    else:
        end, nprop, plen = struct.unpack_from('<III', d, o); o += 12
    nlen = d[o]; o += 1
    if end == 0:
        return None, o
    name = d[o:o + nlen].decode('utf-8', 'replace'); o += nlen
    props = []
    for _ in range(nprop):
        v, o = _prop(d, o)
        props.append(v)
    kids = []
    while o < end:
        k, o = _node(d, o, ver)
        if k is None:
            break
        kids.append(k)
    return Node(name, props, kids), end


def load(path):
    d = open(path, 'rb').read()
    ver = struct.unpack_from('<I', d, 23)[0]
    o, roots = 27, []
    while o < len(d) - 16:
        n, o = _node(d, o, ver)
        if n is None:
            break
        roots.append(n)
    return Node('root', [], roots)


def name_of(n):
    """An FBX object's name is 'name\\x00\\x01Class'."""
    return n.props[1].split('\x00')[0] if len(n.props) > 1 else ''


class Scene:
    def __init__(self, path):
        self.root = load(path)
        objs = self.root.first('Objects')
        self.by_id = {}
        for k in objs.kids:
            if k.props:
                self.by_id[k.props[0]] = k
        # id -> [child ids], from the OO connections
        self.children = {}
        self.parents = {}
        # OO is object-to-object; OP hangs an object off a property of
        # another (a texture on a material's DiffuseColor, say). Both are
        # parent/child here.
        for c in self.root.first('Connections').kids:
            if len(c.props) >= 3 and c.props[0] in ('OO', 'OP'):
                child, parent = c.props[1], c.props[2]
                self.children.setdefault(parent, []).append(child)
                self.parents.setdefault(child, []).append(parent)

    def kids_named(self, oid, kind):
        return [self.by_id[c] for c in self.children.get(oid, ())
                if c in self.by_id and self.by_id[c].name == kind]
