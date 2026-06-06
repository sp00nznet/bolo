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
# Forced function starts: trampoline continuation points the static call graph
# can't see. (Empty for now; the QB self-relocating startup needs reloc-aware
# dispatch rather than forced starts -- see docs/NEXT.md.)
FORCE_STARTS = set()


def load_funcs():
    funcs = []
    for line in open(TOML):
        m = FUNC_RE.search(line)
        if m:
            funcs.append((m.group(1), int(m.group(2), 0), int(m.group(3), 0),
                          m.group(5) == "true"))
    return funcs


def scan_far_targets(image):
    """Whole-image scan for far transfers; returns {target_abs: segbase}.

    `9A` = CALL far ptr16:16, `EA` = JMP far ptr16:16. The target is
    seg*16+off, an absolute image offset (image based at 0), and `seg` is the
    segment that code runs under -- exactly the segment base we need to resolve
    that function's *near* calls. (A few hits land in data; bogus ones get
    filtered out later because they don't decode/aren't reached.)
    """
    N = len(image)
    seg_of = {}
    for i in range(N - 4):
        if image[i] in (0x9A, 0xEA):
            off = image[i + 1] | (image[i + 2] << 8)
            seg = image[i + 3] | (image[i + 4] << 8)
            t = seg * 16 + off
            if 0 <= t < N:
                seg_of.setdefault(t, seg)
    return seg_of


def near_call_target(ins, func_start, segbase):
    """Correct absolute target of a near `call rel16`, segment-aware.

    A near call keeps CS, so the target lives in the caller's segment. Recover
    the real rel from the raw bytes (the decoder masks disp to 16 bits, losing
    the sign for backward calls) and wrap within the 64 KB segment:
        target = segbase*16 + ((Aoff + pos_after + rel) & 0xFFFF)
    where Aoff is the caller's offset within its segment.
    """
    raw = ins.raw
    if not raw or raw[0] != 0xE8 or len(raw) < 3:
        return None
    rel = int.from_bytes(raw[1:3], "little", signed=True)
    pos_after = ins.address + len(raw)          # blob-relative (relative to func_start)
    aoff = func_start - segbase * 16
    seg_rel = (aoff + pos_after + rel) & 0xFFFF
    return segbase * 16 + seg_rel


def segbase_for(addr, far_sorted):
    """Segment base for an address: the nearest far-target segbase at or below it
    (functions in a segment are contiguous and sprinkled with far entries)."""
    import bisect
    i = bisect.bisect_right([a for a, _ in far_sorted], addr) - 1
    if i >= 0:
        return far_sorted[i][1]
    return addr >> 4 & 0xF000        # last-resort fallback


def discover(image, detected, far_seg):
    """Segment-aware function discovery (far + near closure).

    Seeds with far-call targets (each carrying its exact segbase), the entry,
    and the analyzer's detected functions; then follows near calls using each
    function's segment base, assigning newly-found near targets the caller's
    segbase (same segment). Returns {start: segbase}.
    """
    from decode16 import OpType
    N = len(image)
    segbase = dict(far_seg)
    segbase[ENTRY] = 0x2011
    far_sorted = sorted(far_seg.items())
    for _, s, _, _ in detected:                 # detected funcs: infer segbase
        segbase.setdefault(s, segbase_for(s, far_sorted))

    changed = True
    while changed:
        changed = False
        starts = sorted(segbase)
        for i, s in enumerate(starts):
            sb = segbase[s]
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
                    if 0 <= t < N and t not in segbase:
                        segbase[t] = op.far_seg
                        changed = True
                elif ins.mnemonic == "call" and op and op.type == OpType.REL16:
                    t = near_call_target(ins, s, sb)
                    if t is not None and 0 <= t < N and t not in segbase:
                        segbase[t] = sb        # same segment as caller
                        changed = True
    return segbase


def main():
    os.makedirs(OUT, exist_ok=True)
    image = open(IMAGE, "rb").read()
    N = len(image)
    detected = load_funcs()
    far_of = {start: far for name, start, end, far in detected}

    from decode16 import OpType
    # segment-aware discovery -> {start: segbase}; recompute boundaries
    far_seg = scan_far_targets(image)
    segbase = discover(image, detected, far_seg)
    for fs in FORCE_STARTS:                 # trampoline continuations
        segbase.setdefault(fs, segbase_for(fs, sorted(far_seg.items())))
    starts = sorted(segbase)
    n_detected = len(detected)
    funcs = []
    for i, s in enumerate(starts):
        end = starts[i + 1] if i + 1 < len(starts) else N
        end = min(end, s + MAXLEN, N)
        funcs.append((f"res_{s:06X}", s, end, far_of.get(s, True)))

    known = {start: name for name, start, end, far in funcs}
    defined = set(known.values())
    start_set = set(starts)

    lifter = Lifter(hdr_size=0, known_funcs=known)
    lifter.dispatch = True          # emit recomp_dispatch() for retf/indirect transfers
    bodies = []
    referenced = set()
    n_ok = n_fail = n_insns = n_unhandled = 0
    near_total = near_hit = 0           # near-call resolution quality metric

    for name, start, end, far in funcs:
        try:
            insns = Decoder(image[start:end], base_offset=start).decode_all()
            sb = segbase[start]
            # rewrite near-call disps to the segment-correct target so the lifter
            # (target = func_start + disp) resolves them to the right function
            for ins in insns:
                op = ins.op1
                if ins.mnemonic == "call" and op and op.type == OpType.REL16:
                    t = near_call_target(ins, start, sb)
                    if t is not None:
                        op.disp = t - start
                        near_total += 1
                        if t in start_set:
                            near_hit += 1
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
                '#include "cpu.h"\n#include "dos_compat.h"\n\n'
                "/* computed-transfer dispatcher (retf trampolines, indirect "
                "call/jmp) */\nvoid recomp_dispatch(CPU *cpu, uint16_t seg, "
                "uint16_t off);\n\n")
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

    near_pct = (100 * near_hit // near_total) if near_total else 0
    print(f"functions:   {len(funcs)}  (was {n_detected} detected; "
          f"+{len(funcs) - n_detected} via segment-aware closure)")
    print(f"lifted ok {n_ok}, failed {n_fail}")
    print(f"instructions:{n_insns}")
    print(f"chunks:      {len(chunks)}  ({CHUNK} funcs each) in {OUT}")
    print(f"near calls:  {near_total}  resolved to a real start: {near_hit} "
          f"({near_pct}%)")
    print(f"referenced:  {len(referenced)}  undefined->stubbed {len(stubs)}")
    print(f"soft gaps:   {n_unhandled} (UNHANDLED / needs-dispatch markers)")


if __name__ == "__main__":
    main()
