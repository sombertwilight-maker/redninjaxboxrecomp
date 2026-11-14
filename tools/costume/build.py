"""Convert a skinned FBX into a costume the port can draw in Kurenai's place.

The output is in the game's own vertex layout (see docs/mesh-format.md), so
the game's vertex program skins it with the game's own bone matrices: three
influences, each a position premultiplied by its weight, a packed normal, a
texture coordinate, and three bone slots.

Her head is not converted. The head vertices of the game's own mesh are
copied across untouched, which keeps her face, her texture and her eyelid
rig exactly as they were, and the imported body is bound to the same
skeleton below the neck.
"""
import sys, os, struct, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fbx, model, game, mat, bones
from PIL import Image

FBX = '../../bin/mods/Kurenaioutfit.fbx'
RBB = '../../bin/game/media/res.rbb'
TEXDIR = os.path.expanduser('~/mnt/Ayane_DOA2U_C10')
OUT = '../../bin/mods'

# Kurenai keeps her own head, so the model's head parts are left out.
SKIP = ('5_Face', '5_Eyes', '5_Mouth', '7_EyeLashes', '7_Hair', '5_Headband')
HEAD_NODES = {44, 45}          # bb_head, bf_mabuta
ATLAS = 2048


def pack_normal(n):
    x, y, z = n
    d = math.sqrt(x * x + y * y + z * z) or 1.0
    x, y, z = x / d, y / d, z / d
    xi = max(-1023, min(1023, int(round(x * 1023)))) & 0x7FF
    yi = max(-1023, min(1023, int(round(y * 1023)))) & 0x7FF
    zi = max(-511, min(511, int(round(z * 511)))) & 0x3FF
    return xi | (yi << 11) | (zi << 22)


def main():
    scene = fbx.Scene(FBX)
    meshes = [m for m in model.meshes(scene) if not m.name.startswith(SKIP)]
    player = game.Player(RBB)
    gbind = player.bind_pose()
    gidx = {nm: i for i, (nm, l, v) in enumerate(player.nodes)}

    # The game keeps a vertex in the local space of each bone that moves it
    # -- the inverse bind is baked into the data and the palette holds the
    # bone's world transform -- so an imported vertex is simply carried into
    # that bone's bind frame.
    fbind = {}
    for m in model.meshes(scene):
        fbind.update(m.bind)
    slot_of = {}
    for sl, node in player.slot_node.items():
        slot_of[node] = sl

    def correction(m, xname):
        """Geometry space to the game bone's local frame.

        The model's vertices are in metres and Z up; the hundredfold and the
        turn to Y up live in the cluster matrices, so the walk is: into the
        model bone's frame (Transform), out to the bind world (TransformLink),
        then into the game bone's frame."""
        g = bones.game_bone_for(xname)
        if not g or g not in gidx:
            return None
        node = gidx[g]
        if node not in slot_of or xname not in m.geo or xname not in m.bind:
            return None
        def m4(a):
            return [[a[r * 4 + c] for c in range(4)] for r in range(4)]
        w = mat.mul(m4(m.geo[xname]), m4(m.bind[xname]))
        return mat.mul(w, mat.inverse(gbind[node])), slot_of[node]

    # One atlas for the lot: the game binds a single texture per draw.
    used = {}
    for m in meshes:
        used.setdefault(texture_of(scene, m.name), []).append(m.name)
    used.pop(None, None)
    atlas, rects = pack_atlas(sorted(used))
    atlas.save(os.path.join(OUT, 'costume5.png'))

    verts, tris, seen = [], [], {}
    for m in meshes:
        rect = rects.get(texture_of(scene, m.name), (0, 0, 1, 1))
        # vertex -> [(game slot, weight, correction)], best three
        vw = {}
        for bone, d in m.weights.items():
            c = correction(m, bone)
            if not c:
                continue
            for v, w in d.items():
                vw.setdefault(v, []).append((w, c[1], c[0]))
        for tri in m.tris:
            out = []
            for vi, k in tri:
                n = m.normal_of(k) or (0.0, 1.0, 0.0)
                uv = m.uv_of(k) or (0.0, 0.0)
                key = (m.name, vi,
                       round(n[0], 3), round(n[1], 3), round(n[2], 3),
                       round(uv[0], 5), round(uv[1], 5))
                if key not in seen:
                    seen[key] = len(verts)
                    verts.append(build_vertex(m, vi, k, vw.get(vi, []), rect))
                out.append(seen[key])
            if len(set(out)) == 3:
                tris.append(out)

    body = len(verts)
    head_map = {}
    for v in range(player.NVERT):
        infl, _ = player.vertex(v)
        if any(n in HEAD_NODES for n, w, p in infl):
            head_map[v] = len(verts)
            verts.append(player.raw_vertex(v))
    for a, b, c in player.faces():
        if a in head_map and b in head_map and c in head_map:
            tris.append([head_map[a], head_map[b], head_map[c]])

    write(os.path.join(OUT, 'costume5.msh'), verts, tris)
    print('body %d verts, head %d verts, %d triangles' % (body, len(verts) - body, len(tris)))
    print('atlas %dx%d, %d textures' % (atlas.size[0], atlas.size[1], len(rects)))


def texture_of(scene, geo_name):
    for g in scene.root.first('Objects').kids:
        if g.name != 'Geometry' or fbx.name_of(g) != geo_name:
            continue
        for p in scene.parents.get(g.props[0], []):
            mo = scene.by_id.get(p)
            if mo is None or mo.name != 'Model':
                continue
            for mid in scene.children.get(mo.props[0], []):
                o = scene.by_id.get(mid)
                if o is None or o.name != 'Material':
                    continue
                for tid in scene.children.get(o.props[0], []):
                    t = scene.by_id.get(tid)
                    if t is not None and t.name == 'Texture':
                        rf = t.first('RelativeFilename')
                        if rf:
                            return rf.props[0].split('\\')[-1]
    return None


def pack_atlas(names):
    """Shelf packing, biggest first; every texture keeps its own pixels."""
    imgs = {}
    for n in names:
        p = os.path.join(TEXDIR, n)
        if os.path.exists(p):
            imgs[n] = Image.open(p).convert('RGBA')
    order = sorted(imgs, key=lambda n: -imgs[n].size[1])
    at = Image.new('RGBA', (ATLAS, ATLAS), (0, 0, 0, 255))
    rects, x, y, shelf = {}, 0, 0, 0
    for n in order:
        im = imgs[n]
        w, h = im.size
        if w > ATLAS:
            im = im.resize((ATLAS, max(1, h * ATLAS // w))); w, h = im.size
        if x + w > ATLAS:
            x, y, shelf = 0, y + shelf, 0
        if y + h > ATLAS:
            break
        at.paste(im, (x, y))
        rects[n] = (x / ATLAS, y / ATLAS, w / ATLAS, h / ATLAS)
        x += w
        shelf = max(shelf, h)
    return at, rects


def build_vertex(m, vi, k, infl, rect):
    pos = m.pos[vi * 3:vi * 3 + 3]
    nrm = m.normal_of(k) or (0.0, 1.0, 0.0)
    uv = m.uv_of(k) or (0.0, 0.0)
    infl = sorted(infl, key=lambda t: -t[0])[:3]
    tot = sum(w for w, s, c in infl) or 1.0
    out = bytearray(74)
    slots = [-93, -93, -93]
    for i, (w, slot, corr) in enumerate(infl):
        w /= tot
        p = mat.xform(corr, pos)
        n = mat.xform3(corr, nrm)
        struct.pack_into('<4f', out, i * 16, p[0] * w, p[1] * w, p[2] * w, w)
        struct.pack_into('<I', out, 0x30 + i * 4, pack_normal((n[0] * w, n[1] * w, n[2] * w)))
        slots[i] = slot
    ox, oy, sw, sh = rect
    struct.pack_into('<2f', out, 0x3C, ox + uv[0] * sw, oy + (1.0 - uv[1]) * sh)
    struct.pack_into('<3h', out, 0x44, *slots)
    return bytes(out)


def write(path, verts, tris):
    with open(path, 'wb') as f:
        f.write(b'RNMS')
        f.write(struct.pack('<III', 1, len(verts), len(tris)))
        for v in verts:
            f.write(v)
        for t in tris:
            f.write(struct.pack('<3H', *t))


main()
