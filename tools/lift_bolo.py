#!/usr/bin/env python3
"""
lift_bolo.py -- drive the pcrecomp 16-bit lifter over the unpacked Bolo3 image.

Decodes every function in the analyzer's symbol table and lifts it to C against
the recomp16 CPU model. Because we load the image at linear base 0, the image's
(unrelocated) segment values map 1:1 to image offsets, so far calls resolve to
the same offset space the symbol table uses -- we pass hdr_size=0 so the lifter's
far-call resolver computes seg*16+off with no fixup.

Outputs into src/recomp/gen/:
    recomp_NNNN.c     lifted functions (chunked)
    bolo_recomp.h     CPU include + forward declarations
    recomp_stubs.c    empty bodies for referenced-but-undetected call targets
    recomp_dispatch.c addr -> function-pointer table (for indirect calls later)
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = r"D:/recomp/pc/tools"
sys.path.insert(0, os.path.join(TOOLS, "tools", "disasm"))
sys.path.insert(0, os.path.join(TOOLS, "tools", "lift"))
from decode16 import Decoder            # noqa: E402
from lift16 import Lifter               # noqa: E402

IMAGE = os.path.join(ROOT, "work", "BOLO3_image.bin")
TOML = os.path.join(ROOT, "work", "bolo3.toml")
OUT = os.path.join(ROOT, "src", "recomp", "gen")
CHUNK = 40

FUNC_RE = re.compile(
    r"(res_[0-9A-Fa-f]+)\s*=\s*\{ start = (0x[0-9A-Fa-f]+), "
    r"end = (0x[0-9A-Fa-f]+), size = (\d+), far = (true|false)")


ENTRY = 0x20120          # CS:IP 2011:0010 from the PKLITE footer
MAXLEN = 0x2000          # cap a region scan so we don't run deep into data


def load_funcs():
    funcs = []
    for line in open(TOML):
        m = FUNC_RE.search(line)
        if m:
            funcs.append((m.group(1), int(m.group(2), 0), int(m.group(3), 0),
                          m.group(5) == "true"))
    return funcs


def discover(image, seeds):
    """Far-call-closure function discovery.

    The analyzer's MSC-prologue heuristic only finds 182 functions, but the
    binary is QuickBASIC: nearly all cross-function/runtime transfers are FAR
    calls, whose target `far_seg*16+off` is an absolute image offset (image is
    based at 0). Iterating that closure pulls the QB runtime and the rest of the
    game in from the image instead of stubbing them. (Near calls are intra-
    segment and can't be resolved to an absolute offset without segment context,
    so we don't use them for discovery -- intra-function flow is handled by the
    lifter's labels anyway.)
    """
    from decode16 import OpType
    N = len(image)
    known = set(seeds)
    changed = True
    while changed:
        changed = False
        starts = sorted(known)
        for i, s in enumerate(starts):
            nxt = starts[i + 1] if i + 1 < len(starts) else N
            end = min(s + MAXLEN, nxt, N)
            if end <= s:
                continue
            try:
                insns = Decoder(image[s:end], base_offset=s).decode_all()
            except Exception:
                continue
            for ins in insns:
                op = ins.op1
                if ins.mnemonic in ("call", "jmp") and op and op.type == OpType.FAR:
                    t = op.far_seg * 16 + op.disp
                    if 0 <= t < N and t not in known:
                        known.add(t)
                        changed = True
    return known


def main():
    os.makedirs(OUT, exist_ok=True)
    image = open(IMAGE, "rb").read()
    N = len(image)
    detected = load_funcs()
    far_of = {start: far for name, start, end, far in detected}

    # expand via far-call closure, then recompute boundaries (end = next start)
    seeds = {start for _, start, _, _ in detected} | {ENTRY}
    starts = sorted(discover(image, seeds))
    n_detected = len(detected)
    funcs = []
    for i, s in enumerate(starts):
        end = starts[i + 1] if i + 1 < len(starts) else N
        end = min(end, s + MAXLEN, N)
        funcs.append((f"res_{s:06X}", s, end, far_of.get(s, True)))

    known = {start: name for name, start, end, far in funcs}
    defined = set(known.values())

    lifter = Lifter(hdr_size=0, known_funcs=known)
    bodies = []
    referenced = set()
    n_ok = n_fail = n_insns = n_unhandled = 0

    for name, start, end, far in funcs:
        try:
            insns = Decoder(image[start:end], base_offset=start).decode_all()
            c = lifter.lift_function(name, insns, start, is_far=far)
            n_insns += len(insns)
            n_unhandled += c.count("UNHANDLED") + c.count("needs dispatch")
            referenced |= lifter.func_calls | lifter.ovl_calls
            bodies.append(c)
            n_ok += 1
        except Exception as e:                       # keep going; stub it
            bodies.append(f"void {name}(CPU *cpu) {{ /* LIFT FAILED: {e} */ }}")
            n_fail += 1

    # referenced-but-undefined call targets -> stubs
    stubs = sorted(n for n in referenced if n not in defined)

    # write chunked bodies
    chunks = [bodies[i:i + CHUNK] for i in range(0, len(bodies), CHUNK)]
    for i, ch in enumerate(chunks):
        with open(os.path.join(OUT, f"recomp_{i:04d}.c"), "w") as f:
            f.write('#include "bolo_recomp.h"\n\n')
            f.write("\n\n".join(ch))
            f.write("\n")

    # header: CPU + forward decls
    with open(os.path.join(OUT, "bolo_recomp.h"), "w") as f:
        f.write("/* generated by tools/lift_bolo.py -- do not edit */\n"
                "#ifndef BOLO_RECOMP_H\n#define BOLO_RECOMP_H\n"
                '#include "cpu.h"\n#include "dos_compat.h"\n\n')
        for n in sorted(defined) + stubs:
            f.write(f"void {n}(CPU *cpu);\n")
        f.write("\n#endif\n")

    # stubs file
    with open(os.path.join(OUT, "recomp_stubs.c"), "w") as f:
        f.write('#include "bolo_recomp.h"\n\n'
                "/* Referenced call targets not detected as functions by the\n"
                "   analyzer (mostly QB-runtime entry points and indirect/far\n"
                "   targets). Stubbed so the project links; fill in as we name\n"
                "   the runtime. */\n\n")
        for n in stubs:
            f.write(f"void {n}(CPU *cpu) {{ /* TODO: {n} */ }}\n")

    # dispatch table (addr -> fn) for future indirect-call resolution
    with open(os.path.join(OUT, "recomp_dispatch.c"), "w") as f:
        f.write('#include "bolo_recomp.h"\n\n'
                "typedef struct { unsigned long addr; void (*fn)(CPU*); } "
                "dispatch_t;\n\nconst dispatch_t g_dispatch[] = {\n")
        for name, start, end, far in sorted(funcs, key=lambda x: x[1]):
            f.write(f"    {{ 0x{start:06X}UL, {name} }},\n")
        f.write("};\nconst int g_dispatch_count = "
                f"{len(funcs)};\n")

    print(f"functions:   {len(funcs)}  (was {n_detected} detected; "
          f"+{len(funcs) - n_detected} via far-call closure)")
    print(f"lifted ok {n_ok}, failed {n_fail}")
    print(f"instructions:{n_insns}")
    print(f"chunks:      {len(chunks)}  ({CHUNK} funcs each) in {OUT}")
    print(f"referenced:  {len(referenced)}  undefined->stubbed {len(stubs)}")
    print(f"soft gaps:   {n_unhandled} (UNHANDLED / needs-dispatch markers)")


if __name__ == "__main__":
    main()
