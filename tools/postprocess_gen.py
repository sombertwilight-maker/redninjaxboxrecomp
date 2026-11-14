#!/usr/bin/env python3
"""Post-process the generated C for Red Ninja: End of Honor.

Run after `tools.recomp` has written src/recomp/gen/*.c:

    python tools/postprocess_gen.py src/recomp/gen

Bring-up probes (rn_probe in src/main.c prints them to the log):
  1  resource file header: decrypted date stamp does not match  (0x0016926E)
  2  resource reader: stream object could not be created        (0x0016947C)
  3  malloc returned NULL                                        (0x002033F7)
  4  level loader callback: reader reported an error event       (0x0004DB37)
  5  level loader callback: a resource handler refused its item  (0x0004DB3D)
  6  the "problem with the disc" screen was entered              (0x00166B50)

Idempotent: a file already processed is left as it is.
"""
import os
import sys

MARK = "/* rn-postprocess */"
PROBE = ("    { extern void rn_probe(int, uint32_t, uint32_t, uint32_t); "
         "rn_probe(%d, %s, esi, edi); } " + MARK + "\n")

# (function, exact line to instrument, probe id, register holding `this`)
SITES = [
    # CreateFiber: the common exit, eax = the new fiber (or 0)
    ("sub_00212D58", "loc_00212DDF: ;\n", 0, None),
    ("sub_00168F60", "    MEM16(ebp + 0x20) = 0xF;\n", 1, "ebp"),
    ("sub_00169410", "    MEM16(edi + 0x20) = 0xF;\n", 2, "edi"),
]

# (function, label, statement appended after the label)
AFTER_LABEL = [
    ("sub_00212DE4", "loc_00212DE4: ;\n",
     "    { extern void rn_fiber_deleted(uint32_t); rn_fiber_deleted(MEM32(esp + 4)); } "
     + MARK + "\n"),
    ("sub_002033EC", "loc_002033F7: ;\n",
     "    if (!eax) { extern void rn_probe(int, uint32_t, uint32_t, uint32_t); "
     "rn_probe(3, MEM32(esp + 4), esi, edi); } " + MARK + "\n"),
    ("sub_00166B50", "loc_00166B50: ;\n",
     "    { extern void rn_probe(int, uint32_t, uint32_t, uint32_t); "
     "rn_probe(6, 0, esi, edi); } " + MARK + "\n"),
    ("sub_0004CDC0", "loc_0004DB37: ;\n",
     "    { extern void rn_probe(int, uint32_t, uint32_t, uint32_t); "
     "rn_probe(4, ebp, esi, edi); } " + MARK + "\n"),
    # Camera update: the block at 0x000C0F69 steers the orbit yaw towards the
    # way the player is facing whenever she is moving and the right stick is
    # idle. rn_cam_no_follow (RN_CAM_FOLLOW=0, the default) skips it, so the
    # camera turns only when it is told to.
    ("sub_000C0500", "loc_000C0F55: ;\n",
     "    { extern int rn_cam_no_follow; if (rn_cam_no_follow) goto loc_000C1069; } "
     + MARK + "\n"),
    # Same routine, 0x000C10E0: xmm2 is how far the yaw implied by where the
    # camera now sits relative to the player has drifted from the stored yaw
    # (the camera lags as she moves), and the title folds a share of it back
    # in:  yaw += clamp(k * drift) + stick. That is what turns the view when
    # she runs sideways. Dropped, the stored yaw is the only authority and
    # the camera orbits rigidly.
    ("sub_000C0500", "loc_000C10E0: ;\n",
     "    { extern int rn_cam_no_follow; if (rn_cam_no_follow) xmm2.f[0] = 0.0f; } "
     + MARK + "\n"),
    # Jump input (sub_000B4110). At 0x000B419E a jump press has been seen and
    # ebx says what kind it can be: 1 from the ground, 0 in the air (nothing
    # happens). rn_dj_try turns one mid-air press per jump into a ground-style
    # jump, and at 0x000B4D9B, where the action for that jump has been chosen
    # (0x415 standing, 0x4C9 running, 0x4CA the somersault the title reserves
    # for her top running speed), rn_dj_take makes it the somersault.
    ("sub_000B4110", "loc_000B4110: ;\n",
     "    { extern void rn_dj_dbg(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t); "
     "rn_dj_dbg(MEM32(MEM32(ecx + 0x10) + 0xC4), MEM32(ecx + 0x234), MEM32(ecx + 0x238), eax, "
     "MEM32(MEM32(0x69CBD8)), MEM32(esp + 8)); } " + MARK + "\n"),
    ("sub_000B4110", "loc_000B419E: ;\n",
     "    { extern int rn_dj_try(uint32_t); "
     "if (!ebx && rn_dj_try(MEM32(MEM32(edi + 0x10) + 0xC4))) ebx = 1; } " + MARK + "\n"),
    ("sub_000B4110", "loc_000B4D9B: ;\n",
     "    { extern int rn_dj_take(void); if (rn_dj_take()) esi = 0x4CA; } " + MARK + "\n"),
    # Run update (sub_000E2E40). Every path reaches 0x000E54D5 with the speed
    # factor in [edi + 0x1278]: 1.0 is the jog, and the title lets it creep to
    # 1.03 over several seconds of running one way, then accelerates it to 1.6,
    # the fast run. rn_run_speed replaces that build-up with a toggle.
    ("sub_000E2E40", "loc_000E54D5: ;\n",
     "    { extern float rn_run_speed(float); "
     "MEMF(edi + 0x1278) = rn_run_speed(MEMF(edi + 0x1278)); } " + MARK + "\n"),
    # Same routine: the title only looks at the crouch button in the standing
    # case (0x000E497D), so she has to stop first. From the run update, a held
    # crouch button now enters that case's crouch start (0x000E49B4: set the
    # crouch flag, blend, action 0x46A) directly. eax and esi are what that
    # code expects: the actor flags and the current action.
    ("sub_000E2E40", "loc_000E54D5: ;\n",
     "    { extern int rn_run_crouch(void); "
     "if (rn_run_crouch() && (MEM32(MEM32(0x69CC1C)) & MEM32(ebp + 8))) { "
     "eax = MEM32(edi + 0x234); esi = MEM32(MEM32(edi + 0x10) + 0xC4); goto loc_000E49B4; } } "
     + MARK + "\n"),
    # Stage loader (sub_0004E280), the player's resource set. The stage number
    # (0x69AF94, in eax) picks one of four: KURENAI below stage 0x1E and
    # KURENAI_DARK, the black suit, from there on; in eight stages the "BS"
    # pair instead, split the same way. rn_costume_stage hands the two tests
    # a stage from the wanted half (F6 menu, Costume) and leaves the choice
    # between the plain and the BS pair, made before them, alone.
    ("sub_0004E280", "loc_0004EF52: ;\n",
     "    { extern uint32_t rn_costume_stage(uint32_t); eax = rn_costume_stage(eax); } " + MARK + "\n"),
    ("sub_0004E280", "loc_0004EF96: ;\n",
     "    { extern uint32_t rn_costume_stage(uint32_t); eax = rn_costume_stage(eax); } " + MARK + "\n"),
    # The wire effect's six materials (wire_ef_ha01..03, wire_ef_hun01..03),
    # looked up by name when the player object is built: RN_WIRE_LOG=1 says
    # which were found, to tell "the effect never starts" from "it starts
    # and draws nothing".
    # D3DDevice_RunPushBuffer (sub_0022A960), on entry: arg1 = the pushbuffer
    # descriptor, arg2 = the D3DFixup. The engine writes a mesh's vertex array
    # offsets into the fixup exactly once and then marks the record done, so
    # they have to be applied now, while the arena still holds them -- the
    # executor's walk reaches the record a frame or more later, after D3D has
    # recycled the arena. Without this the wire trail draws from the archive's
    # placeholder addresses and never appears.
    ("sub_0022A960", "loc_0022A960: ;\n",
     "    { extern void xbox_Nv2aFixupVertexArrays(uint32_t, uint32_t); "
     "xbox_Nv2aFixupVertexArrays(MEM32(esp + 4), MEM32(esp + 8)); } " + MARK + "\n"),
    # The one-shot vertex-array patch: case 2 of the per-frame fixup builder
    # has just handed D3D a submesh's array offsets, and is about to mark the
    # record done (type 4). ebx = the record, esi = the pushbuffer descriptor.
    ("sub_00178170", "loc_00178259: ;\n",
     "    { extern void rn_vtxfix(uint32_t, uint32_t); rn_vtxfix(ebx, esi); } " + MARK + "\n"),
    ("sub_00072A90", "loc_00072B11: ;\n",
     "    { extern void rn_wire_log(uint32_t); rn_wire_log(ebp); } " + MARK + "\n"),
    # The wire effect's life, to find where the razor wire goes missing
    # (RN_WIRE_LOG=1): started, updated, drawn, stopped.
    ("sub_00072B20", "loc_00072B20: ;\n",
     "    { extern void rn_wire_evt(int, uint32_t); rn_wire_evt(1, eax); } " + MARK + "\n"),
    ("sub_00072BE0", "loc_00072BF5: ;\n",
     "    { extern void rn_wire_evt(int, uint32_t); rn_wire_evt(2, esi); } " + MARK + "\n"),
    ("sub_00072BE0", "loc_00072CDA: ;\n",
     "    { extern void rn_wire_evt(int, uint32_t); rn_wire_evt(3, edi); } " + MARK + "\n"),
    ("sub_00072BE0", "loc_00072CE1: ;\n",
     "    { extern void rn_wire_evt(int, uint32_t); rn_wire_evt(4, edi); } " + MARK + "\n"),
    ("sub_00072D00", "loc_00072D00: ;\n",
     "    { extern void rn_wire_evt(int, uint32_t); rn_wire_evt(5, eax); } " + MARK + "\n"),
    # The wire throw's own state machine (sub_000C4230): thrown, hit, and
    # the timer running out, so a throw that starts but never connects can
    # be told from one that never starts.
    ("sub_000C4230", "loc_000C5726: ;\n",
     "    { extern void rn_wire_evt(int, uint32_t); rn_wire_evt(6, edi); } " + MARK + "\n"),
    ("sub_000C4230", "loc_000C5814: ;\n",
     "    { extern void rn_wire_evt(int, uint32_t); rn_wire_evt(7, edi); } " + MARK + "\n"),
    ("sub_000C4230", "loc_000C519C: ;\n",
     "    { extern void rn_wire_evt(int, uint32_t); rn_wire_evt(8, edi); } " + MARK + "\n"),
    # Diagnostics (RN_ACT_LOG=1): every action change request and every call
    # of the routine only the somersault jump was seen to use.
    ("sub_000A93E0", "loc_000A93E0: ;\n",
     "    { extern void rn_act_log(int, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t); "
     "rn_act_log(0, eax, ecx, MEM32(esp + 4), MEM32(esp + 8), MEM32(esp)); } " + MARK + "\n"),
    ("sub_000B2C60", "loc_000B2C60: ;\n",
     "    { extern void rn_act_log(int, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t); "
     "rn_act_log(1, esi, ecx, MEM32(esp + 4), 0, MEM32(esp)); } " + MARK + "\n"),
    # Player control: at 0x000A7E6C xmm4/xmm3 hold the right stick (x, y)
    # after the title's own dead zone, just before
    #   pitch += y * dt * k_pitch;  yaw += x * dt * k_yaw
    # (camera + 0xC00 / + 0xC04). rn_cam_shape (src/main.c) flips, scales,
    # curves and eases them there, so only the camera is affected.
    ("sub_000A6580", "loc_000A7E6C: ;\n",
     "    { extern void rn_cam_shape(float *, float *); "
     "rn_cam_shape(&xmm4.f[0], &xmm3.f[0]); } " + MARK + "\n"),
    ("sub_0004CDC0", "loc_0004DB3D: ;\n",
     "    if (MEM32(esp + 0x20) != 1) { extern void rn_probe(int, uint32_t, uint32_t, uint32_t); "
     "rn_probe(5, ebp, esi, edi); } " + MARK + "\n"),
]


def process(path):
    src = open(path, encoding="utf-8", errors="surrogateescape").read()
    renamed = 0
    # Fibers (src/fibers.c): SwitchToFiber is implemented by hand; the lifted
    # body is kept under another name. CreateFiber/DeleteFiber get hooks.
    old = "\nvoid sub_00212DF7(void)\n{"
    if old in src:
        src = src.replace(old, "\nvoid sub_00212DF7_lifted(void) " + MARK + "\n{")
        renamed = 1
    n = renamed
    for fn, line, pid, reg in SITES:
        head = "void %s(void)" % fn
        i = src.find("\n" + head)
        if i < 0:
            continue
        j = src.find("\nvoid sub_", i + 10)
        body = src[i:j if j > 0 else len(src)]
        probe = (PROBE % (pid, reg)) if reg else (
            "    { extern void rn_fiber_created(uint32_t); rn_fiber_created(eax); } "
            + MARK + "\n")
        if reg is None:
            if line in body and probe not in body:
                body = body.replace(line, line + probe)
                src = src[:i] + body + (src[j:] if j > 0 else "")
                n += 1
            continue
        if line in body and probe not in body:
            body = body.replace(line, probe + line)
            src = src[:i] + body + (src[j:] if j > 0 else "")
            n += 1
    for fn, label, stmt in AFTER_LABEL:
        head = "void %s(void)" % fn
        i = src.find("\n" + head)
        if i < 0:
            continue
        k = src.find(label, i)
        if k < 0:
            continue
        k += len(label)
        if src.startswith(stmt, k):
            continue
        src = src[:k] + stmt + src[k:]
        n += 1
    if n:
        open(path, "w", encoding="utf-8", errors="surrogateescape").write(src)
    return n


def main():
    gen = sys.argv[1] if len(sys.argv) > 1 else "src/recomp/gen"
    total = 0
    for name in sorted(os.listdir(gen)):
        if name.startswith("recomp_") and name.endswith(".c"):
            total += process(os.path.join(gen, name))
    print("rn-postprocess: %d site(s) instrumented" % total)


if __name__ == "__main__":
    main()
