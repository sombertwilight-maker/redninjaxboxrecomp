"""Pull a skinned model out of an FBX scene into plain Python lists."""
import fbx as fbxlib


def _layer(geo, kind, data_name, index_name):
    """A layer element, resolved to one value per polygon-vertex."""
    n = geo.first(kind)
    if not n:
        return None, None
    data = n.first(data_name)
    idx = n.first(index_name)
    mapping = n.first('MappingInformationType')
    ref = n.first('ReferenceInformationType')
    return ((data.props[0] if data else None),
            (idx.props[0] if idx else None),
            (mapping.props[0] if mapping else ''),
            (ref.props[0] if ref else ''))


class Mesh:
    pass


def meshes(scene):
    out = []
    for geo in scene.root.first('Objects').kids:
        if geo.name != 'Geometry':
            continue
        m = Mesh()
        m.name = fbxlib.name_of(geo)
        m.pos = geo.first('Vertices').props[0]            # x,y,z triples
        pvi = geo.first('PolygonVertexIndex').props[0]
        m.nverts = len(m.pos) // 3

        nd, ni, nmap, nref = _layer(geo, 'LayerElementNormal', 'Normals', 'NormalsIndex')
        ud, ui, umap, uref = _layer(geo, 'LayerElementUV', 'UV', 'UVIndex')

        # Triangulate, carrying each corner's normal and uv with it.
        tris, poly = [], []
        for k, raw in enumerate(pvi):
            v = ~raw if raw < 0 else raw
            poly.append((v, k))
            if raw < 0:
                for t in range(1, len(poly) - 1):
                    tris.append((poly[0], poly[t], poly[t + 1]))
                poly = []
        m.tris = tris

        def corner(dat, idx, ref, k, n):
            if dat is None:
                return None
            j = (idx[k] if (idx and 'IndexToDirect' in ref) else k)
            return dat[j * n:j * n + n]
        m.normal_of = lambda k, _d=nd, _i=ni, _r=nref: corner(_d, _i, _r, k, 3)
        m.uv_of = lambda k, _d=ud, _i=ui, _r=uref: corner(_d, _i, _r, k, 2)

        # Skin: bone name -> {vertex: weight}, and the bone's bind transform.
        m.weights = {}
        m.bind = {}        # bone -> TransformLink: bone local -> bind world
        m.geo = {}         # bone -> Transform: geometry -> that bone's local
        for skin in scene.kids_named(geo.props[0], 'Deformer'):
            for cl in scene.kids_named(skin.props[0], 'Deformer'):
                bone = None
                for ch in scene.children.get(cl.props[0], ()):
                    o = scene.by_id.get(ch)
                    if o is not None and o.name == 'Model':
                        bone = fbxlib.name_of(o)
                if not bone:
                    continue
                tl = cl.first('TransformLink')
                if tl:
                    m.bind[bone] = tl.props[0]
                tf = cl.first('Transform')
                if tf:
                    m.geo[bone] = tf.props[0]
                ix = cl.first('Indexes')
                wt = cl.first('Weights')
                if not ix or not wt:
                    continue
                d = m.weights.setdefault(bone, {})
                for v, w in zip(ix.props[0], wt.props[0]):
                    if w > 0.0:
                        d[v] = d.get(v, 0.0) + w
        out.append(m)
    return out
