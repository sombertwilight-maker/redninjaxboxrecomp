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
