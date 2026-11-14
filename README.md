# Red Ninja: End of Honor — static recompilation

A native Windows build of the Xbox game, made with
[xboxrecomp](https://github.com/sp00nznet/xboxrecomp) 0.17.0 plus the runtime
patches in `toolkit/toolkit-patches.diff`. The Xbox executable is translated to
C ahead of time; no emulator runs underneath.

**No game data is included.** On first run `RedNinja.exe` asks for your own disc
image of the USA/Europe release (title ID 56550027), checks it, and unpacks it
into `bin\game\` (about 2 GB). The recompiled C is generated from your own copy
too — see `regen.bat` — which is why `src/recomp/gen/` is not in this
repository.

## State

The game is playable start to finish at 60 fps. Rendering, sound, controller,
saves and movies all work.

Known gaps are listed at the end.

## What this port adds

Quality-of-life and modding work layered on top of the original game:

* **60 fps** with the game's own timing left intact (the step and rate the
  title reads are rewritten rather than the frame loop).
* **Camera**: invertible axes, separate horizontal and vertical speed, a
  response curve, smoothing, and the auto-follow turned off.
* **Movement**: a double jump, and L3 to toggle jog/run instead of holding.
* **F6 menu**: resolution, antialiasing, widescreen, texture dump/replace, a
  ReShade-style effects tab (bloom, contrast, saturation, warmth, sharpen,
  vignette, gamma), control rebinding, and a controls legend. Pad-driven.
* **Live texture replacement**: drop a PNG named after a dumped texture's hash
  into `bin\textures\replacement\` and it swaps in while the game runs.
* **Costume swaps**, including playing as Akemi: her mesh and texture stand in
  for Kurenai's, with her skeleton's rest pose corrected and a `bb_hanger` node
  patched in so levers and hanging work.

## Layout

| Path | What it is |
|---|---|
| `src/` | The port's own code: entry point, fibers, ISO setup, and the game mods in `main.c` |
| `tools/postprocess_gen.py` | Re-applies the port's hooks to freshly generated code |
| `tools/rbb/` | Reader for the game's `res.rbb` archive, plus the Akemi mesh patcher |
| `toolkit/toolkit-patches.diff` | Runtime changes against stock xboxrecomp 0.17.0 |

## Building

1. Unpack xboxrecomp 0.17.0 somewhere and apply `toolkit/toolkit-patches.diff`
   to it.
2. Run `regen.bat` to recompile your `default.xbe` into `src/recomp/gen/`
   (this also runs `tools/postprocess_gen.py`).
3. Run `build.bat`.

## Known gaps

* Akemi has no physics on her fringe, and her mesh names its materials
  differently from Kurenai's, so material effects on the player body do nothing.
* Cutscene character models are not swapped with the player costume.
* The thrown hook never registers a hit.
