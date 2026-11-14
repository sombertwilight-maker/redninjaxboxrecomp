# The game's mesh resources (QMX0)

What has been read out of `res.rbb` so far, written down so it does not have
to be worked out twice. Everything here came from reading the archive and the
pushbuffers the game submits; nothing is guessed.

## Shape of an entry

A mesh entry is a header, a table of per-node records, the vertex data, and a
**precompiled NV2A pushbuffer** that draws it. The pushbuffer is a verbatim
recording: the addresses in it are the build tool's, and every one of the
3,785 vertex-array offsets in the archive is a page-aligned placeholder.

* Node names: at `0x140`, one every `0xA0` bytes. Node 0 is `base`, then
  `root`, `bb_hanger`, `skin`, the IK helpers, then the skeleton proper.
* Relocation records: `0x110` bytes each, in a table of their own.
  * `+0x00` offset into the pushbuffer that this record patches
  * `+0x04` type (see below)
  * `+0x08` an index whose meaning depends on the type
  * `+0x0C` a value whose meaning depends on the type

### Record types

The engine walks this table every frame (`sub_00178170`) and writes the
results into a D3DFixup, which patches the pushbuffer before it runs.

| Type | What it writes |
|---|---|
| 2 | The 16 vertex array offsets. `+0x08` is the stream, `+0x0C` the stride, and `+0x10` onwards is one 16-byte descriptor per attribute: offset within the vertex, then the NV2A format word. **One-shot**: after writing it once the engine rewrites the record's type to 4, which is a no-op, because the patch is meant to stay in the pushbuffer. |
| 3 | One bone matrix. `+0x08` is the bone's slot as the shader sees it (signed, slot − 96); `+0x0C` is the node index. |
| 6, 7 | Material and transform constants, rewritten every frame. |
| 4 | Nothing; what a type 2 becomes once it has been used. |

## Skinning

The player's vertex program is a three-influence skin with the weights folded
into the data:

```
arl a0.x = v12.x              ; bone index 0
R2  = dp4(v0, c[a0+96..98])   ; position, already multiplied by its weight
arl a0.x = v12.y
R4  = dp4(v1, c[a0+96..98])
arl a0.x = v12.z
R10 = dp4(v2, c[a0+96..98])
R0  = R2 + R4 + R10
```

| Attribute | Meaning |
|---|---|
| `v0` `v1` `v2` | position premultiplied by each influence's weight, 4 floats each |
| `v4` `v5` `v6` | normal, likewise, one packed dword each |
| `v8` | texture coordinates, 2 floats |
| `v12` | the three bone indices, 3 shorts, each (slot − 96) |

Kurenai's body (entry 202, first draw) is 74 bytes a vertex and uses 30 bones.

## The epilogue

These programs apply the viewport themselves and output screen coordinates,
so `oPos.xy` is already in pixels and must not be divided by `w` again:

```
rcq R1.x  = 1 / R12.w
mul o0.xyz = R12 * c[190]        ; viewport scale
mad o0.xyz = R12 * R1.x + c[191] ; divide, then offset
```

Most programs keep the viewport in `c[58]`/`c[59]`; the wire's keeps it in
`c[190]`/`c[191]`.
