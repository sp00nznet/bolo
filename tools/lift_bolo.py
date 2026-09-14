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
def _pcrecomp_home():
    """Where the pcrecomp toolkit lives.

    PCRECOMP_HOME if it is set, otherwise a sibling checkout next to this one --
    which is how the recomp projects are laid out. Never a hardcoded absolute
    path: this file carried one for months, it named a drive the toolkit had
    since moved off, and the relift silently kept using a three-month-old
    lifter. Same resolver as dinopark/tools/lift_dinopark.py.
    """
    env = os.environ.get("PCRECOMP_HOME")
    cands = [env] if env else []
    cands += [os.path.join(os.path.dirname(ROOT), "tools"),
              os.path.join(ROOT, "..", "pcrecomp")]
    for c in cands:
        if c and os.path.isdir(os.path.join(c, "tools", "disasm")):
            return os.path.abspath(c)
    raise SystemExit(
        "cannot find the pcrecomp toolkit.\n"
        "Set PCRECOMP_HOME to your checkout of\n"
        "  https://github.com/sp00nznet/pcrecomp\n"
        "(looked in: %s)" % ", ".join(str(c) for c in cands))


TOOLS = _pcrecomp_home()
sys.path.insert(0, os.path.join(TOOLS, "tools", "disasm"))
sys.path.insert(0, os.path.join(TOOLS, "tools", "lift"))
import decode16                         # noqa: E402
from decode16 import Decoder            # noqa: E402

# We decode a SLICE of the snapshot per function (base_offset = the function's
# linear start), so `pos` is a linear address, not a segment offset. Wrapping a
# relative branch target to 16 bits is right only for the latter: here a
# backwards jump produces a small negative target, and masking turns it into
# 0xFFxx, which the lifter then adds to the function's linear start and lands
# 0x10000 too high -- on a real function that happens to exist up there, so it
# dispatches somewhere plausible and wrong. 184 sites in this image, including
# res_014871's `jmp -0xB4` becoming a jump to 0x247BD.
decode16.WRAP_NEAR_TARGETS = False
from lift16 import Lifter               # noqa: E402
import lift16                           # noqa: E402  (to set _CODE_SEG per function)

# We lift from the post-startup SNAPSHOT (the fully PKLITE+QB-decompressed and
# relocated runtime memory captured by tools/uni_original.py), NOT the static
# pre-decompression image -- the real game/main code only exists after the QB
# startup decompresses it. Functions are keyed by snapshot LINEAR address; the
# recomp loads the snapshot 1:1 and dispatches with g_load_base=0.
IMAGE = os.path.join(ROOT, "work", "snapshot.bin")
TOML = os.path.join(ROOT, "work", "bolo3.toml")    # unused in snapshot mode
OUT = os.path.join(ROOT, "src", "recomp", "gen")
CHUNK = 40

FUNC_RE = re.compile(
    r"(res_[0-9A-Fa-f]+)\s*=\s*\{ start = (0x[0-9A-Fa-f]+), "
    r"end = (0x[0-9A-Fa-f]+), size = (\d+), far = (true|false)")


ENTRY = 0x1B484          # real program entry: runtime 1AB4:0944 -> snapshot linear
ENTRY_SEG = 0x1AB4       # caller segment for the entry (for near-call resolution)
MAXLEN = 0x2000          # cap a region scan so we don't run deep into data
# Call targets captured from the Unicorn ground truth (work/calltgts.json, a
# {linear_addr: cs} map produced by `UNI_HEAPTRACE=0x1 UNI_NMAX=... uni_original.py`).
# The QB runtime reaches many routines via INDIRECT calls through DGROUP routine
# tables (init/handler/heap registration); their targets often land in the MIDDLE of
# other discovered functions (e.g. the init-table patcher 0x1B72E, or 0x173FA inside
# the merged res_0173CA). Without forcing them as function starts the dispatcher
# misses them and the QB string-space/heap init never runs (-> string GC loops on a
# bad heap). Force each as a start with its ground-truth segment (cs).
import os as _os, json as _json
CALLTGT_SEG = {}     # {linear_addr: segbase}
# committed copy (tools/calltgts.json); regenerate via UNI_HEAPTRACE=0x1 uni_original.py
for _ctf in (_os.path.join(_os.path.dirname(__file__), "calltgts.json"),
             _os.path.join(_os.path.dirname(__file__), "..", "work", "calltgts.json")):
    if _os.path.exists(_ctf):
        CALLTGT_SEG = {int(a): int(c) for a, c in _json.load(open(_ctf)).items()}
        break
# Fallback subset (used if calltgts.json hasn't been regenerated) -- all seg 0x1283.
INIT_ROUTINES = set(CALLTGT_SEG) or {0x12EFD, 0x14695, 0x16D15, 0x16EF1,
                                     0x173FA, 0x17422, 0x19BA4}
FORCE_STARTS = {ENTRY} | INIT_ROUTINES   # seed discovery from the real entry


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
    segbase[ENTRY] = ENTRY_SEG
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
                elif (ins.mnemonic == "jmp" and op
                      and op.type in (OpType.REL8, OpType.REL16)):
                    # A near jmp that LEAVES this function is a tail call, and its
                    # target is a function start nothing else names -- MSC/QB thunk
                    # tables are built entirely out of them (res_014871 is three
                    # such thunks). With WRAP_NEAR_TARGETS off, op.disp is the
                    # target relative to the slice we handed the decoder -- so it
                    # is SIGNED, and the linear address is s + disp. Reading it as
                    # an address instead registers every forward jump displacement
                    # as a function at linear 0x20-ish.
                    #
                    # Only targets strictly outside the scanned range qualify. An
                    # intra-function jump is a loop or an if, and registering it as
                    # a start is the "split at every branch target" refinement that
                    # was tried and reverted below -- it turns a loop spanning the
                    # split into tail-recursion and blows the C stack.
                    t = s + op.disp
                    if 0 <= t < N and not (s <= t < end) and t not in segbase:
                        segbase[t] = sb        # near jmp keeps CS
                        changed = True
                elif (ins.mnemonic in ("call", "jmp") and op and op.type == OpType.MEM
                      and ins.seg_override == "cs"):
                    # cs-relative indirect dispatch: `call/jmp word cs:[reg+disp]`.
                    # This is a jump table at cs:disp whose word entries are routine
                    # offsets within this segment -- often pointing into the *middle*
                    # of an enclosing block (shared QB-runtime handlers). Register
                    # each entry as a function start so the dispatcher can reach it.
                    tbl = sb * 16 + (op.disp & 0xFFFF)
                    for k in range(128):                # generous upper bound
                        e = tbl + k * 2
                        if e + 2 > N:
                            break
                        w = image[e] | (image[e + 1] << 8)
                        if w >= 0x8000:                 # implausible offset -> table end
                            break
                        if w == 0:                      # null/padding slot, not a target
                            continue
                        t = sb * 16 + w
                        if not (0 <= t < N):
                            break
                        # Skip targets whose first byte is an obvious non-opcode
                        # (0x00) -- avoids registering data (e.g. embedded strings)
                        # as spurious functions that the fallthrough fix walks into.
                        if image[t] == 0x00:
                            continue
                        if t not in segbase:
                            segbase[t] = sb
                            changed = True
    # NOTE: a broad "split at every cross-function branch target" refinement was
    # tried and reverted -- it converts intra-function loops that SPAN a new split
    # point into cross-function tail-recursion (stack blow-up). Mid-function jcc/jmp
    # targets must be handled another way (e.g. true mid-function entry support).
    return segbase


def main():
    os.makedirs(OUT, exist_ok=True)
    image = open(IMAGE, "rb").read()
    N = len(image)
    detected = []                 # snapshot mode: discover purely from far/near closure
    far_of = {}

    from decode16 import OpType
    # segment-aware discovery -> {start: segbase}; recompute boundaries
    far_seg = scan_far_targets(image)
    segbase = discover(image, detected, far_seg)
    for fs in FORCE_STARTS:                 # trampoline continuations
        segbase.setdefault(fs, segbase_for(fs, sorted(far_seg.items())))
    for fs in INIT_ROUTINES:               # force ground-truth segment per target
        segbase[fs] = CALLTGT_SEG.get(fs, 0x1283)
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
    segs_used = set()                   # code segments (for SEG_xxxx #defines)

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
            segs_used.add(sb)
            lift16._CODE_SEG = f"{sb:04X}"      # cs-relative reads use this constant
            c = lifter.lift_function(name, insns, start, is_far=far)
            # inject entry trace + set cpu->cs to this function's segment (for
            # `push cs`/`mov ax,cs` and to keep cs consistent after far calls)
            c = c.replace("(CPU *cpu)\n{",
                          f"(CPU *cpu)\n{{\n    recomp_enter(0x{start:06X}UL); "
                          f"cpu->cs = SEG_{sb:04X};", 1)
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

    # write chunked bodies (remove stale chunks first so a smaller run can't leave
    # orphaned recomp_NNNN.c referencing functions that no longer exist)
    import glob as _glob
    for _old in _glob.glob(os.path.join(OUT, "recomp_[0-9][0-9][0-9][0-9].c")):
        os.remove(_old)
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
                "call/jmp). dispatch_far additionally unwinds the 4-byte far "
                "frame the call site pushed when nothing is lifted at the "
                "target. */\n"
                "int recomp_dispatch(CPU *cpu, uint16_t seg, uint16_t off);\n"
                "void dispatch_far(CPU *cpu, uint16_t seg, uint16_t off);\n"
                "void dispatch_near(CPU *cpu, uint16_t seg, uint16_t off);\n"
                "void recomp_enter(unsigned long addr);\n\n")
        for s in sorted(segs_used):
            f.write(f"#define SEG_{s:04X} 0x{s:04X}\n")
        f.write("\n")
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
