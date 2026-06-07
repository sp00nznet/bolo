# Runbook — where to pick up

## ★★★ MILESTONE (2026-06-07): heap init fixed — the recomp reaches GRAPHICS RENDERING

The QB string-heap GC infinite loop is **fixed**, and the recomp now runs all the way
into the game's **graphics line-drawing** code (it is trying to draw the screen).

**Root cause that was blocking everything:** the QB runtime reaches its init / handler
/ heap-registration routines through INDIRECT calls via DGROUP routine tables (the
handler walker `res_01AE54`'s `call far ds:[di]`, the init dispatcher `res_0146DF`'s
`call word ds:[si]`). Many of those targets land in the MIDDLE of other functions
(e.g. the init-table patcher `res_01B72E` = 1B72:000E) and were never discovered, so
the dispatchers hit no-op stubs. Result: the init table stayed unbound (all `0x58DE`
ret-stubs), the string heap was never initialized (head `ds:[0x4846]`=`0xFFFF`), and
the GC `res_017B81` looped forever.

**The fix:** collect EVERY call target from the Unicorn ground truth and force them as
function starts with their ground-truth segment.
- `uni_original.py` records `{linear_addr: cs}` for all post-entry call targets ->
  `tools/calltgts.json` (regenerate: `UNI_HEAPTRACE=0x1 UNI_NMAX=30000000 python
  tools/uni_original.py`; needs faithful-enough INT 21h to run — see caveat below).
- `lift_bolo.py` loads `tools/calltgts.json` and forces each as a start (segbase=cs).
- Functions 947 -> 1110. The heap GC wedge is GONE.

**Current wedge (next frontier): Cohen-Sutherland line-clip non-convergence.**
`res_013BBF` (QB `LINE`/draw, dispatches plot primitives via `ds:[0x46AB..0x46C7]`)
runs the clip loop `res_013AAF`/`res_013B0C`/`res_019AD2`/`res_013B42` which never
converges (wedges ~enter#112383; no EGA writes yet — wedged before the actual plot).
Two contributing leads:
1. **Mid-function jcc targets still miss.** e.g. `res_013B42`'s `je`->`0x13B88`
   (a `jmp 0x147C6`) dispatch-MISSES because `0x13B88` is mid-`res_013B66` and not a
   function start, so the clip multiply helper can return wrong -> clip diverges.
   A broad "split at every cross-function branch target" pass was tried and REVERTED:
   it turns intra-function loops that span a new split point into tail-RECURSION
   (stack blow-up; wedged at 74 enters). Mid-function entries need a real mechanism
   (true mid-function dispatch, or a careful loop-aware split), not a blanket split.
2. The clip math (`res_013B0C` fixed-point muldiv via `res_013B42`) may have a flag/
   arithmetic lifting bug, or the line coords feeding `res_013BBF` are off.

**CAVEAT on the oracle:** the harness now wedges in its OWN run (incomplete INT 21h),
so our recomp runs AHEAD of it — the differential trace can't validate the graphics
code. To debug further, either (a) make `uni_original.py` faithful enough (more INT
21h/10h/16h) to reach the graphics code, or (b) debug the clip math directly
(instrument `res_013BBF` line coords + `ds:[0x46xx]` plot pointers + the clip loop's
exit condition). Diagnostics ready: `BOLO_HEAP`, `BOLO_WATCHDOG`, `BOLO_WATCH`.

## ▶ SESSION UPDATE (2026-06-06): control-flow lifter fixes + dispatch-table discovery

The recomp now executes **~286K instructions** (was wedging at ~4K) before hitting
the current frontier. Found and fixed a chain of real lifter bugs by differential
tracing against the Unicorn ground truth (`uni_original.py --enter-trace`, now with
per-enter register dumps) plus new runtime diagnostics.

**Lifter fixes (in the SHARED toolkit `D:/recomp/pc/tools/tools/lift/lift16.py`):**
1. **Out-of-function `jcc`/`jmp`/`loop*`/`jcxz` were dropped to comments.** They are
   tail-jumps to other functions; now emit a real tail-call (`fn(cpu); return;`) or
   `recomp_dispatch(...)` fallback. New helper `_tail_jump(abs)`. This was THE bug
   behind the `res_013AAF`/`res_013B42` infinite clip/line-draw loops.
2. **Fallthrough off the end of a function** (last insn not a terminator) now emits a
   tail-call into the next function (QB runtime shares tails by falling through).
3. **`mov <sreg>, *` write** emitted `SEG_xxxx = ...` (assigning a `#define`); now
   writes `cpu-><sreg>`.
4. **Dispatch-table discovery (`tools/lift_bolo.py` `discover()`):** QB runtime uses
   `call/jmp word cs:[reg+disp]` jump tables whose entries point INTO the middle of
   shared blocks (e.g. table[0]=`0x003A`→`0x1B8CA`, mid-`res_01B8B4`). Added a scan
   that registers each table entry as a function start (skipping null/`0x00` slots so
   we don't turn embedded data like the `MSEM87…` signature into a bogus function).
   Functions 947 → 1084.

**New diagnostics (all env-gated, off by default):**
- `BOLO_WATCHDOG=<sec>` — watchdog thread dumps the recent call-path (enter-ring) on
  hang. Catches intra-function `goto` loops that never call `recomp_enter`.
- `BOLO_WATCH=0x<lin>` (+ `BOLO_WATCH_FF`) — memory write watchpoint; prints/stops on
  writes to a linear address (used to trace heap corruption).
- `recomp_enter` now dumps registers for the first 200 enters under `BOLO_TRACE`, to
  diff against the harness's per-enter register dump.

**CURRENT FRONTIER — QB string-heap GC infinite loop (`res_017B81`/`res_017D33`).**
After ~286K instructions the recomp wedges walking the QB string heap:
`res_017B81` walks blocks via `si -= ds:[si-3]` until `ds:[si]==4`, but the heap head
`ds:[0x4846]` (ds=`0x1E49`) is `0xFFFF` and the heap region is all `0xFF`, so `[si-3]`
=`0xFFFF` makes `si += 1` and it scans forever (and the GC itself then writes `0xFFFF`
into the heap, compounding it). **The harness/truth never enters this GC** (it reaches
asset-loading), so we diverge *into* it.
- The heap head is set to `0xFFFF` at **enter#57** by `mov word ds:[0x4846], si/di`
  (three sites in `recomp_0012.c`); `si`/`di` was `0xFFFF` there — i.e. an earlier QB
  string-heap **allocator** returned `0xFFFF` ("out of space"/uninitialized). Trace
  *that* allocator next (why it yields `0xFFFF` instead of a real heap pointer).
- CAVEAT on the trace diff: the Unicorn harness **stubs INT 21h** (e.g. AH=44h IOCTL
  returns without setting DX), so recomp vs harness register traces legitimately
  diverge at the first such INT 21h (≈enter#23, `res_01AE54`). Past that point the
  harness is NOT faithful ground truth — to extend the diff, make the harness
  implement the relevant INT 21h calls like `dos_compat.c` does. The heap-GC loop is
  a *separate* real bug (no DOS would spin forever in string GC).

### Heap allocator trace — sharper diagnosis (BOLO_HEAP beacon)

Traced it (`BOLO_HEAP=1` logs entries of the heap init/GC; uni `UNI_HEAPTRACE=`):
- **The heap INITIALIZER `res_017D7C` runs ZERO times** in our recomp. It writes the
  head `ds:[0x4846]`, the bound copies `[0x4848]/[0x484A]`, **and the `0x4` terminator
  `ds:[si]=4`** that the GC walks until — none of that happens.
- The GC `res_017B81` runs at **enter#70** with `head=0xFFFF` already (uninitialized),
  so it scans `0xFF` forever (and then writes `0xFFFF` itself, compounding it).
- `res_017D7C` is called only from **block E at `0x17429` inside the MERGED function
  `res_0173CA`**. `res_0173CA` is several distinct QB-runtime routines glued together:
  block A (entry) `ret`s at `0x173F9`; blocks B(`0x173FA` sets string-space bounds
  `[0x45B4]/[0x45B6]`), C(`0x1741A`, has a direct far `jmp 1283:2084` rendered as a
  bare COMMENT — another lifter gap), D(`0x17422`), **E(`0x17429`, the heap init)**,
  F(`0x17439`) follow AFTER the ret, reachable only by EXTERNAL calls/jumps to those
  mid-function addresses. They are not function starts, nothing references `0x17429`,
  so far-calls to them resolve to no-op stubs → **the heap is never initialized.**
- **And the harness/truth never runs the GC at all** in this window (it reaches asset
  loading). So our recomp is doing a **premature string allocation** (`res_017E40` →
  GC) that truth doesn't — i.e. an EARLIER behavioral divergence sends us into the
  allocator before the heap is set up.

### Two concrete paths forward (both substantial)
1. **Faithful DOS oracle.** The Unicorn harness STUBS INT 21h (AH=44h etc.), so the
   recomp-vs-truth differential trace stops being trustworthy at ~enter#23 — exactly
   where the premature-alloc divergence likely originates. Port `dos_compat.c`'s INT
   21h semantics into `uni_original.py`'s `hook_intr` so the trace stays faithful past
   INT 21h, then re-diff to find the FIRST real divergence.
2. **Basic-block-level entries for merged runtime routines.** `res_0173CA` (and likely
   others) merge multiple QB-runtime entry points; mid-function entries (`0x173FA`,
   `0x17422`, `0x17429`, …) are unreachable. Either split discovered functions at
   `ret`-bounded blocks that are referenced elsewhere, or make `recomp_dispatch` able
   to enter a function at an arbitrary offset. Also: fix the remaining lifter gap —
   direct far `jmp seg:off` (op1.type==FAR under mnemonic `jmp`) is dropped to a
   comment (see `res_0173CA` block C `jmp 1283:2084`); emit a tail-dispatch like the
   far-`call` path.

Diagnostics added this session for the above: `BOLO_HEAP` (heap init/GC beacons in
recomp_enter) and `UNI_HEAPTRACE=<addr,addr>` (uni prints caller/stack when IP hits
given addresses).

### ▶▶ DECISIVE heap finding (next-session start here)

Traced both sides exhaustively:
- **Truth NEVER writes the heap head `ds:[0x4846]` (=img 0x22CD6) and NEVER runs the
  string GC `res_017B81`** in its whole run (`UNI_WATCH=0x22CD6 UNI_HEAPTRACE=...`).
  The head stays at its snapshot value (0). The QB heap initializer `res_017D7C`
  also never runs in truth.
- **Our recomp writes head=`0xFFFF` and loops in the GC.** The write comes from the
  string ALLOCATOR `res_017E40` (`ds:[0x4846]=si` with si=garbage), which is reached
  via this call path (captured with the GC beacon): `res_0144E2 → res_0174FC →
  res_014871 → res_014883 → res_0148B4 → res_017B7E → … → res_017D33 → res_017B81`.
  The allocator's free-block pointer `si` is non-zero (garbage) where truth's would be
  0 (empty heap → grow path, no GC) — because an upstream heap WALK (`res_017D00`,
  same `si -= ds:[si-3]` pattern) returns garbage on the never-initialized heap.
- The QB init dispatcher `res_0146DF` (`call word ds:[si]` over a DGROUP table) DOES
  run now (enter#26, 7 entries) — but the GC wedge happens during the FIRST init
  routine (`0x1e65→res_014695`), BEFORE the bounds-setup routine (block B, table
  entry #1) runs.

**Conclusion: the divergence is UPSTREAM** — our recomp performs string allocations
(or a heap walk yielding garbage) that truth doesn't at this point. Both start from
the identical snapshot heap (head=0, zeros), so something between the entry and the
allocator diverges. Pinning it requires register/memory-faithful comparison past the
first INT 21h, which the harness currently stubs.

**THE next step (path #1): make `uni_original.py`'s `hook_intr` implement INT 21h like
`src/recomp/dos_compat.c`** (AH=44h IOCTL→DX, 30h, 25h/35h, 48/49/4A, 3D/3F/3E/40,
62h, etc.) so the recomp-vs-truth enter+register diff stays faithful from the entry
through the heap divergence. Then re-run the differential trace (both already emit
`E <addr> <regs>`), find the FIRST true register/branch divergence, and fix that
lifter/runtime bug. The heap GC loop is the *symptom*; the real bug is whatever first
makes a register/branch differ after the snapshot entry.

## ✅✅ THE NATIVE RECOMP BOOTS AND RUNS THE GAME

`build/bolo.exe` loads the snapshot and executes the real QuickBASIC game:
**600+ functions of the QB runtime + game init run with no crash**, and it settles
into the game's input polling loop (`013B42/013B0C/019AD2`) — i.e. it has booted and
is waiting for keyboard/timer input. Getting here took: snapshot lift + the
`dos_init()` runtime init (sets `g_dos`/IVT/HAL/BIOS area) + two systematic lifter
fixes (call-far/jmp-far dispatch, `_CODE_SEG` cs-relative). Build:
`bash scripts/build.sh` then relink with `-Wl,--stack,0x8000000`.

### EGA video HAL implemented (`src/recomp/ega.c`) — runs, not yet drawing

Built a from-scratch EGA SCREEN 9 (640×350×16, 4-plane) HAL: plane memory with
the Sequencer map-mask (3C4/3C5) + Graphics Controller (3CE/3CF: set/reset,
enable-SR, rotate, read-map, mode, bit-mask), write modes 0/2 with EGA latches,
`ega_set_mode`, and a planes→PNG dump (`tools/decode_ega.py`). Wired: A000 reads/
writes route through it (cpu.h), EGA ports route via `port_out8`, and INT 10h now
answers EGA detect (AH=12h/1Ah) + EGA mode-set (AL=0Dh/0Eh/10h/0Fh).

**Current state:** the recomp runs 3M+ functions but draws **nothing** yet — 0 A000
writes AND 0 B800 (text) writes at the input loop. So it's stuck in a *pre-render*
path. Leading hypotheses (debug next):
- The first thing after init is `INT 21h AH=3Fh` on stdin (handle 0). This is likely
  a console/redirection check; recomp16's stdin read either blocks (no input) or, if
  fed, makes the game think input is redirected — both wrong. Implement proper
  console semantics for AH=3Fh/AH=0Bh on stdin so the game proceeds to draw.
- Then menu input is probably via **INT 16h** (BIOS keyboard), which needs keys
  pushed into recomp16's keyboard HAL (it currently returns no-key without an SDL
  `poll_events`). Push keystrokes into the buffer so the menu advances.
- Re-run the differential trace (recomp vs harness) once input is handled to catch
  any remaining lifter divergence before the draw.
The EGA HAL + screen-dump are ready to reveal the frame the moment the game draws.

### Remaining for a *visible/playable* build (well-scoped Phase 4 I/O)
1. **EGA planar video HAL.** `recomp16/hal/video.c` only does VGA mode-13h (linear
   8bpp); Bolo uses `SCREEN 9` (EGA 640×350, 4 planes via ports 0x3C4/0x3CE).
   Add plane-mask emulation + a 4-plane framebuffer, present as RGBA to SDL2.
   (To verify offline: dump `cpu.mem[0xA0000..]` after init and decode the planes
   to PNG.)
2. **Feed input + timer events.** The game polls INT 16h (keyboard) / INT 1Ah
   (timer); wire the SDL2 HAL event pump (recomp16 has a `poll_events` callback in
   DosState) so key presses reach the game and it advances past the title/menu.
3. **Asset file loads** already work via INT 21h AH=3D (game_dir="original").
Then: title → menu → select puzzle → move Mr. Bolo. The hard parts (boot, lift,
runtime, dispatch) are done; this is standard I/O-shim wiring.

## (historical) MECHANICAL INTEGRATION — native recomp runs real game code

The recomp now boots from the post-startup snapshot and executes the real
decompressed QB program:
- `tools/uni_original.py` runs PKLITE + QB startup and dumps `work/snapshot.bin`
  + `src/recomp/gen/snapshot_regs.h` at the real entry `1AB4:0944`.
- `tools/lift_bolo.py` (snapshot mode) lifts from that snapshot: **947 functions,
  420,268 instructions, near-call resolution 100%, ZERO stubs.**
- `src/main.c` loads the snapshot 1:1, seeds regs, `g_load_base=0`, dispatches the
  entry. `res_01B484` is verified-correct QB C-runtime startup (DOS version check,
  stack setup) → calls the QB runtime (`res_014670` @1283) + game modules.
- Build: `bash scripts/build.sh` then relink with `-Wl,--stack,0x8000000`.

### Differential debug vs harness — WORKING, two systematic lifter bugs fixed

Method: both the recomp and `uni_original.py --enter-trace` emit a function-entry
sequence (`E <imageoff>`); diff to find the first divergence. The recomp injects
`recomp_enter()` at each function top (under BOLO_TRACE); main.c has a SIGSEGV
handler + dispatch ring buffer + unbuffered stderr.

Fixed so far (each advanced the boot):
1. **`call far`/`jmp far` indirect (FF /3, FF /5)** were emitted as UNHANDLED no-ops
   — decode16 names them `"call far"`/`"jmp far"`, not `"call"`/`"jmp"`. Added
   dispatch handlers in lift16.py.
2. **cs-relative accesses used stale `cpu->cs`** (not maintained across far calls).
   Wired the lifter's `_CODE_SEG`: cs-relative reads + near-indirect dispatch now
   use the function's constant segment (`SEG_xxxx`), and each function sets
   `cpu->cs` at entry. (lift_bolo.py sets `_CODE_SEG` per function + emits the
   `SEG_` defines.)

Result: recomp now matches the harness **exactly through ~17 functions** — through
the QB C-runtime entry and into the runtime's interrupt-vector install. Crash moved
to `res_01ACDE` (the INT 21h AH=0x30/0x35/0x25 vector-install loop). Likely next
causes: an INT 21h handler returning values that differ from the harness, or
another unhandled instruction form. Continue the same diff loop.

### Current debug frontier (iterative)
It runs the entry + into the QB runtime, then **crashes (SIGSEGV)** after 3 missed
indirect calls to `1AB4:0000` (cs:0, uninitialized). A SIGSEGV handler in main.c
prints the dispatch ring buffer. KEY: the **harness runs this same code fine**
end-to-end (to EGA init), so `1AB4:0000` is a *symptom of earlier divergence* in
the recomp, not the real target. Next steps, in order:
1. **Differential debug against the harness (ground truth).** Both
   `uni_original.py` (real CPU) and the recomp execute the same code from the same
   snapshot. Add a per-function entry trace to the lifter (emit `trace(addr)` at
   each function top under `g_trace`) and compare the recomp's function-call
   sequence to the harness's CS:IP trace to find the FIRST divergence — that's the
   lifter/runtime bug.
2. **Lifter gap: direct far `jmp seg:off` (tail calls)** are emitted as comments
   (e.g. `res_01B484` ends with `jmp 1AB4:0010`). Add dispatch-or-tailcall handling
   like the far-`call` path.
3. Wire INT 10h (EGA→SDL2), INT 16h (keyboard), INT 21h (asset file loads) via the
   recomp16 HAL so it progresses to rendering.



Snapshot of the project state and the exact next moves, ordered by value. The
foundation (Phases 0–2 + the bulk of Phase 3) is done; what remains is closing
lift gaps and getting a build running.

## Regenerate everything from scratch (one path)

```bash
# 1. unpack the original (writes work/BOLO3_image.bin + work/BOLO3_unpacked.exe)
python tools/unpklite.py original/BOLO3.EXE work/BOLO3_image.bin --mz work/BOLO3_unpacked.exe
# 2. function analysis -> work/bolo3.toml
python D:/recomp/pc/tools/tools/disasm/analyze.py work/BOLO3_unpacked.exe -symbols work/bolo3.toml
# 3. lift to C -> src/recomp/gen/   (segment-aware; 932 funcs, 99% near calls)
python tools/lift_bolo.py
```

## Current state

- **932 functions / 56,821 instructions / ~70K lines of C** lifted, no exceptions.
- Call graph essentially fully resolved: near calls 99% to real starts, only **14
  stubs** (all out-of-image BIOS/absolute far targets — fine to leave).
- Program entry `res_020120` is lifted and in the dispatch table.
- **IT BUILDS AND RUNS.** `bash scripts/build.sh` → `build/bolo.exe` (mingw64
  gcc + SDL2). It loads the image, calls the entry, and executes real lifted
  startup code (segment setup + `rep movsb` self-relocation) before returning.

## The active frontier (Phase 5 debugging) — two coupled problems

Traced from the entry `res_020120` (which runs cleanly today):
```
mov ax,es ; add ax,0x10     ; ax = load-module seg = PSP+0x10
push cs ; pop ds            ; DS = CS  (so ds:[..] reads CODE-segment data)
mov ds:[4],ax ; add ax,ds:[0xC] ; ds:[0xC] is real image data -> end-of-prog seg
mov cx,ds:[6] ; ... rep movsb (std, downward)   ; self-relocate the program
push ax ; push 0x34 ; retf  ; computed far jump to relocated_seg:0x0034
```

1. **Load base / PSP seeding.** The original load module starts at PSP:0x100 (the
   decompressor wrote output at `DI=0x100`). So load the image at linear **0x100**
   (not 0), set `ES=DS=PSP` (e.g. segment 0) and `ax`/regs so `PSP+0x10` is the
   load-module segment, set `CS = 0x10 + 0x2011`, `SS = 0x10 + 0x2348`, `SP=0x80`,
   and fill a minimal PSP (top-of-memory paras at `PSP:[0x02]`). Then the startup's
   `es+0x10` and `ds:[0xC]/[6]` reads line up and the relocation math is sane.
   (NOTE: an earlier draft wrongly said "no PSP -> garbage"; because `DS=CS`, the
   `ds:[..]` reads are code-segment data and are valid — the real miss is the
   0x100 load base + `ES=PSP` seeding.)
   **DONE:** `main.c` now loads at linear 0x100, seeds a PSP at segment 0, and sets
   `CS=0x10+0x2011`, `SS=0x10+0x2348`. `g_load_base=0x100` tells the dispatcher
   image offset 0 == linear 0x100.

2. **Computed control flow — dispatch DONE; self-relocation is the open blocker.**
   `recomp_dispatch()` (src/icall.c) is wired: the lifter now emits it for indirect
   `call`/`jmp` and `retf` (opt-in `Lifter.dispatch`; civ unaffected). The entry
   dispatches correctly. The wall is the QB self-relocating startup. Exact disasm
   (CS image base 0x20110; header `[4]` scratch, `[6]`=0xC600 move count,
   `[C]`=0x1B22 program paragraphs):
   ```
   CS:0010 inc dx ; mov bp,ax ; mov ax,es ; add ax,0x10   ; ax = load module seg
   CS:0018 push cs ; pop ds                                 ; DS = CS (header reads)
   CS:001a mov [4],ax ; add ax,[0xC] ; mov es,ax           ; ES = top = LM + 0x1B22
   CS:0023 mov cx,[6] ; mov di,cx ; dec di ; mov si,di ; std
   CS:002d rep movsb                                        ; copy program to `top`
   CS:002f push ax ; mov ax,0x34 ; push ax ; retf           ; jump to top:0x0034
   ```
   The `retf` jumps into the *relocated copy* at `top:0x34`. At runtime we observe
   the miss `1B32:0034 -> image 0x1B254` (top=0x1B32). Our lifted functions are
   static C at original offsets, so the copy has no code. Two ways through:
   - **Relocation-aware dispatch:** track the self-move (record that segment `top`
     aliases the original load-module segment, delta = orig − top paragraphs) and in
     `recomp_dispatch` map a relocated `seg:off` back to the original image offset.
     The program relocates a small number of times, so a short alias table suffices.
   - **Bypass the startup:** skip the self-relocation and call the post-relocation
     QB entry (runtime init → user `main`) directly with the environment hand-set.
     Usually the pragmatic choice for self-relocating CRT startups.
   Either is now iterable in the build/run loop (`scripts/build.sh`; run with
   `BOLO_TRACE=1` to watch dispatch).

### Full QB startup analysis (disassembled CS:0x10–0xB0)

The entry is a 3-phase QuickBASIC/BCOM loader:
- **Phase 1 (CS:0x10–0x34): self-move + trampoline.** Computes `top` from a code-
  segment header (`[6]`=move count 0xC600, `[C]`=0x1B22 paras), `rep movsb`-copies
  the segment to `top`, then `push top; push 0x34; retf` into the copy.
- **Phase 2 (CS:0x35–0xA0): LZ/RLE decompressor.** Reads a control byte into `dl`,
  a count into `cx`, then `and al,0xFE; cmp al,0xB0` → `rep stosb` (run fill) or
  `cmp al,0xB2` → `rep movsb` (copy). Classic byte-LZ.
- **Phase 3 (CS:0xA2+): relocation apply.** `mov si,0x132; push cs; pop ds;
  mov bx,[4]; lodsw …` walks a relocation list and patches segment words.
- Then it calls the QB runtime init and the user `main`.

**KEY INSIGHT that reframes path 1 vs bypass:** the image is *already fully
unpacked* — PKLITE produced the final bytes, and the DGROUP data is readable
verbatim in `work/BOLO3_image.bin` (the filename table at 0x1D1E9, all strings,
clean function bodies). So phases 1–3 (move/decompress/relocate the program into
its run-time position) are **redundant for the recomp**: our lifted functions are
static C and the data is already in place. Trying to run phases 1–3 faithfully
fights an approximated DOS environment (the self-move trampoline currently lands
on a copied `retf` and the stack region is BSS/zero, a dead end).

**The handoff (fully disassembled).** Phase 3's relocation loop (CS:0xAE–0xE0)
walks a segment-grouped reloc table and ends with:
```
CS:00E2 mov ax,bx          ; bx = load base ([4])
CS:00E4 mov di,[8]         ; init SP   = 0x0012
CS:00E8 mov si,[0xA] ; add si,ax        ; init SS  = 0x2608 + base
CS:00F2 sub ax,0x10 ; mov ds,ax ; mov es,ax   ; DS=ES = PSP
CS:00FC cli ; mov ss,si ; mov sp,di ; sti
CS:0104 ljmp cs:[0]        ; FAR JUMP through the header pointer = real entry
```
Header (CS:0): `[0..3]` far entry ptr, `[4]` base, `[6]` move count, `[8]` SP,
`[0xA]` SS, `[0xC]` size, then the **relocation table at CS:0x132 (image 0x20242)**.

**Open puzzle / the real prerequisite.** The header entry pointer reads
`0xB409:0x44FF` → image 0xB858F, far beyond the 0x213D7 image. So it is a
*pre-relocation* value; resolving it (and every far pointer the game loads from
data) requires **applying the QB relocation table** with the correct base. That is
the deferred Phase 1d (reconstruct & apply relocations) — and it turns out to be
the actual gate to booting, not env tweaks or a naive bypass.

**Concrete next step:** implement the reloc walker (mirror CS:0xAE–0xE0) over the
table at image 0x20242 to (a) resolve the real entry pointer and (b) decide the
base so segment words land in our image-relative dispatch space (likely base 0 so
they stay image offsets). Then dispatch to the resolved entry. The reloc format is
decoded above; src/icall.c + the build/run/trace loop are ready.

### Focused-run finding: the reloc table is PACKED (only exists post phase-2)

A static walk of the table at 0x20242 fails because that region is still
compressed. Entropy of `work/BOLO3_image.bin`:

| region | entropy | meaning |
|--------|--------:|---------|
| code @0xF000 | 6.52 | real 8086 code (lifted fine) |
| strings @0x1D000 | 5.62 | readable text |
| **tail @0x20242** | **7.30** | **compressed** (the "reloc table" area) |
| **end @0x21000** | **7.21** | **compressed** |

The header entry pointer `CS:[0] = 0xB409:0x44FF` → image 0xB858F is *far beyond*
the 136 KB image: it's a pre-decompression placeholder. So the **relocation table
and the real entry pointer only materialize after the startup's phase-2 byte-LZ
decompressor runs.** A standalone reloc walker therefore cannot work.

### Corrected path to boot: emulate the startup loader (phases 1–3)

Write a small faithful emulator of the startup (the exact disasm is in this file:
self-move → byte-LZ decompress → relocation apply → `ljmp cs:[0]`) operating on a
DOS-style memory image (load module + zeroed BSS for `min_alloc`). Run it once,
offline, to produce: (a) the fully-decompressed/relocated memory, (b) the resolved
real entry CS:IP. Then the recomp loads that final memory and `recomp_dispatch`es
to the resolved entry (map it to its image offset). Instruction set to support is
small: mov/add/sub/cmp/and/or/not/shr/shl, push/pop, lodsw/stosw, rep movsb/stosb,
repe scasb, jcxz/loop/jmp/jcc, std/cld/cli/sti, retf, ljmp.

Phase-2 decompressor (CS:0x35–0xA0) to transcribe: find 0xFF via `repe scasb`,
then loop: `dl=ctrl byte; cx=count word; al=dl & 0xFE`; `al==0xB0` → `rep stosb`
(run-fill of one literal byte); `al==0xB2` → `rep movsb` (copy); `dl & 1` ends the
stream. This expands the packed tail into the reloc table + final DGROUP + the real
entry pointer.

This (Phase 1d, now correctly understood) is the single remaining gate to booting.
Everything else — lift, dispatch, runtime, build — is in place.

### Startup emulator built (`tools/emulate_startup.py`) + the wraparound finding

A focused 8086 interpreter (enough opcodes for the startup) now runs the loader on
a DOS-style image. It correctly executes phase 1, and **pinpointed the real
mechanism**: the self-move `rep movsb` (CX=0xC600 bytes, `std`/downward, src=CS,
dst=`top`=LM+0x1B22) only works when `top` is **above** CS, but `top` is always
0x4EF paragraphs **below** CS — *unless the program is loaded high enough that the
CS segment wraps past 0x10000*. Verified:

| load module seg | CS = LM+0x2011 | top = LM+0x1B22 | move-up (correct)? |
|---|---|---|---|
| 0x0010 | 0x2021 | 0x1B32 | no (corrupts) |
| 0x1000 | 0x3011 | 0x2B22 | no |
| **0xE000** | **0x0011** (wrapped) | **0xFB22** | **yes** |

So Bolo uses the classic **QuickBASIC high-memory load with 8086 segment
wraparound**: CS wraps around the 1 MB boundary. My flat low-load (LM=0x10) is why
the move corrupted its own `push;retf` trampoline (CS:0x2F became `00 00`).

**Next iteration on the emulator:** (1) give it a wraparound-faithful memory model
(linear address `& 0xFFFFF`, and mirror the 64 KB overflow region so seg:off near
1 MB wraps to 0); (2) load the image at the high LM DOS would actually use (compute
from the MZ `min_alloc` / a 640 KB top), so CS wraps correctly; (3) run phases 1–3
and read the resolved entry from the final `ljmp cs:[0]`. The interpreter already
supports the needed opcodes; only the memory model + load segment need fixing.

**HONEST CAVEAT (don't chase the wraparound naively).** `LM=0xE000` is not a real
DOS load for a 136 KB program — it would straddle the 1 MB boundary. Yet under
*every* realistic low/normal load, the `std` self-move with CX=0xC600 (> the
0x4EF0 src/dst gap) overwrites its own `push;retf` continuation (CS:0x2F–0x34)
before reaching it, which cannot be how the real (working) program behaves. So
there is still a genuine misunderstanding of the QB BCOM startup or of the image
layout — likely candidates: (a) the entry CS/footer or PKLITE relocations need
applying first so the segment math differs; (b) the move count/`[C]`/`[6]` header
fields mean something other than a flat byte-count/program-size; (c) the move is
into a region that does not overlap because the program is loaded with a different
base than assumed. **Recommended:** find QuickBASIC 4.5 / BC.EXE compiled-EXE
startup documentation (or compare against a known QB4.5 EXE whose startup is
documented) before more emulation — the emulator is correct; the *model of the
startup's intent* is what's incomplete.

### ★★ BOOT CRACKED — the game runs end-to-end in the harness (`tools/uni_original.py`)

Running the *original packed* `BOLO3.EXE` through the Unicorn harness boots Bolo
end-to-end: **PKLITE stub** decompresses + relocates → **QB startup** (CS=0x2111)
self-moves (now correctly, move-up) + decompresses + relocates → **`ljmp cs:[0]`
resolves the real entry `1AB4:0944`** → the program executes the **QB runtime**
(CS=0x1283) and then the **game's EGA/VGA initialization** (INT 10h AH=0x12 EGA/VGA
detect, 0x0F get-mode, 0x1B functionality, 0x11 char-gen, 0x03/0x05). That is
exactly Bolo's "Requires EGA/VGA" startup — it boots and reaches video setup.

**Key numbers (use these to make the recomp boot):**
- **Real program entry = image offset `0x1A484`** (runtime `1AB4:0944`, linear
  0x1B484, minus the `0x1000` load base). Add it as a forced lifted-function start
  (`res_01A484`); the lifter currently splits it between 0x1A31E and 0x1A538.
- **Load-base mapping confirmed:** `image_offset = runtime_linear − 0x1000`
  (runtime seg 0x1283 == classified runtime image seg 0x1183 + 0x100). So the recomp
  should set `g_load_base = 0x1000` and place the image at linear 0x1000... OR keep
  it image-relative and subtract 0x100 from runtime segments in dispatch.
- The QB startup MUST run first (it decompresses DGROUP + applies relocations).

**Recomp integration (clear path to a running native binary):**
1. Use `uni_original.py` to run PKLITE+QB-startup, then **dump the fully-processed
   memory** (the program region) at the moment control reaches `1AB4:0944`.
2. Load that dumped memory as the recomp's `cpu.mem` (it has decompressed DGROUP +
   applied relocations), set `g_load_base` per the mapping above.
3. Add `0x1A484` as a forced start and `recomp_dispatch` to it from `main.c`.
4. The lifted functions then execute on correct memory; wire the INT 10h/16h/21h
   shims (recomp16 HAL) so EGA output + keyboard + asset file loads work.
Alternatively, extract PKLITE's reloc list by diffing harness memory pre/post and
bake a fully-relocated static image — but dumping the post-startup snapshot is
simplest and is what the harness already produces.

### ★ ROOT CAUSE FOUND (confirmed in the Unicorn harness): missing PKLITE relocations

The startup self-move corrupts only because **`unpklite.py` never applied PKLITE's
relocations.** The startup reads `[C]` (CS:0x0C) and computes `top = LM + [C]`; the
`std` move is correct only when `top > CS` (=LM+0x2011), i.e. when `[C] > 0x2011`.
Our unpacked image has the *un-relocated* `[C]=0x1B22 < 0x2011`, so the move goes the
wrong way and clobbers its own `push;retf`.

**Proven empirically:** patching `[C]` to ≈ program size (0x2140) in the harness makes
`top`(0x2160) > CS(0x2031); CS:0x2F survives intact (`50 b8`); and execution **takes
the retf trampoline to a new segment (CS=0x8544)** — phase 1 completes. So `[C]` (and
every other segment word) is a PKLITE relocation target: at real DOS load PKLITE adds
the load segment (~0x1000+), pushing `[C]` above 0x2011.

**THE FIX (now well-defined and validatable):**
1. Reconstruct & apply PKLITE's relocation table in `unpklite.py` (the deferred Phase
   1d). For large+extra mode the relocations are applied during/after decompression;
   parse them and add a chosen load-segment to every listed word. (A naive trailing
   table doesn't hold enough entries, so the reloc info is in the large-mode stream
   structure — see the PKLITE format refs in git history.)
2. Pick a non-zero load segment (real DOS uses ~0x1000+) so relocated `[C] > 0x2011`.
3. Run `uni_startup.py` (load at that segment, relocations applied) → the startup will
   move-up correctly, decompress (phase 2), relocate (phase 3), and `ljmp cs:[0]` to
   the real entry. Read that entry; feed it to the recomp's `recomp_dispatch`.
4. The recomp must then either use the relocated image + matching load base, or keep
   image-relative dispatch and resolve the (now-known) real entry.

This single missing pass — PKLITE relocation reconstruction — gates the entire boot.
Everything else (lift, dispatch, runtime, build, emulator) is in place and verified.

### Automated emulation harness (`tools/uni_startup.py`) — built & exhaustively run

Rigged up a **fully scriptable Unicorn-Engine harness** (pip `unicorn`, no DOSBox
GUI): loads the image into real-mode memory, seeds the DOS entry state, hooks
INT/code/mem, runs, and reads regs/memory at every instruction. Ground-truth
findings (this is the real CPU behavior, not hand-analysis):

- **Low/normal load (LM small):** the `rep movsb` self-move is a downward
  overlapping copy that **zeroes its own `push;retf`** (CS:0x2F → `00 00`);
  execution falls into an `add [bx+si],al` zero-sled and wanders (~521 K insns).
- **High/wraparound load (LM=0xE000):** the move becomes "up" (top=0xFB22 > CS=0x0011
  wrapped), but `top:0xC5FF` exceeds 1 MB and **wraps back over low memory**, again
  corrupting the startup. Execution reaches the QB runtime segment **0x1183** (~509 K
  insns) but runs on corrupted/zero bytes and faults at `1183:E6D2` on `00 00`.

So under **every** load model the self-move corrupts — and the `retf` keeps landing
on a *copy of itself* (`top:0x34` = copied `retf`). That can't be the real behavior,
so a precondition is still wrong.

**Prime hypothesis now: PKLITE's own relocations were never applied.** `unpklite.py`
stops at the LZ terminator and does not reconstruct/apply PKLITE's relocation table,
so the unpacked image's segment words (and possibly the startup header fields the
self-move reads) are still load-relative / unfixed. Applying them may change `[C]`/
`[6]`/the move geometry so it no longer overlaps. **Do this next:** parse PKLITE's
trailing reloc data (large+extra "long mode": `count` u16 groups, `+0xFFF` seg step;
or inline via the `0xFE` segment-separators in the LZ stream) and apply the fixups,
then re-run `uni_startup.py` (it's ready to validate). If that still corrupts,
observe the original `BOLO3.EXE` in DOSBox-X `debug` (not installed here) to read the
real load segment + the move's true src/dst — one observation settles it.

### QB4.5 startup investigation (results)

Dug into it: there is **no public assembly-level spec** of the QB4.5 compiled-EXE
startup — only the binary `BCOM45.LIB` (whose `__astart`/`B$START` module *is* the
exact code already in our image, so disassembling it adds nothing). Re-verified
facts: the entry **is** at image 0x20120 (CS=0x2011, startup at the image end;
image 0x120 is game code, not the startup — so the footer parse is correct).

The contradiction, stated cleanly: the `std` self-move is non-corrupting only when
`top` (=LM+0x1B22) is **above** CS (=LM+0x2011), i.e. only when CS **wraps past
0x10000** — which needs the program loaded at segment ≈0xE000. A 136 KB program
can't load that high in 640 KB. So under any normal DOS load the move stomps its
own `push;retf`. Since the real game works, a premise is still wrong, and it's not
resolvable from first principles or the public web.

**Two concrete ways to settle it (pick one):**
1. **Observe the original in an emulator with a debugger** (DOSBox-X `debug`, or
   PCjs). Break at the EXE entry, single-step the self-move, and read the *actual*
   ES/DS/CX/SI/DI and the post-move CS:IP. That directly reveals the real load
   segment + the move's true source/dest, settling the wraparound question. (No
   DOSBox is installed in this environment — this needs one.)
2. **Apply PKLITE relocations first.** We deferred reconstructing PKLITE's own
   reloc table; the unpacked image's segment words may still be load-relative in a
   way that changes the startup's arithmetic. Reconstruct + apply them (or unpack
   with a reference tool like `deark`/`mz-explode` that also emits relocations) and
   re-check CS/[C]/the move. This is cheap to try and may be the missing premise.

The startup emulator (`tools/emulate_startup.py`) is ready to validate whichever
answer these produce.

## Next moves (in order)

### 1. Get a first build  *(needs MSVC + SDL2 — not present in the dev env used so far)*
```bash
cmake -B build -G "Visual Studio 17 2022" -A x64 -DSDL2_DIR=<path>
cmake --build build
```
Expect link/compile errors first; work them down. The generated gaps below are
emitted as C *comments*, so they don't block compilation — they make the program
functionally incomplete, surfacing as no-ops at runtime.

### 2. Close the two lift gaps (the "hard parts")
- **x87 FPU (~174 `/* FPU: esc */`).** `lift16.py` doesn't translate x87. Either
  add an FPU stack model to `recomp16` (like `lift32`'s `_st[8]` + fp_push/pop) and
  teach the lifter the esc opcodes, or map the **INT 34h–3Dh emulator** entry points
  to native `double`. Bolo's float use is light (coords/timing), so a small subset
  likely suffices.
- **Indirect dispatch (~160 `/* needs dispatch */`).** `call/jmp [reg/mem]`. Add a
  `recomp_icall(cpu, far_addr)` that binary-searches `g_dispatch` (already
  generated in `recomp_dispatch.c`) and teach the lifter to emit it. This is where
  QB's runtime dispatch and any vtable-like tables get wired.

### 3. Relocations (deferred Phase 1d)
Reconstruct the large-model reloc table (see FORMATS/unpklite notes) and apply, so
segment arithmetic at runtime is correct. Needed once the program executes, not to
compile. The register footer (entry CS:IP/SS:SP) is already recovered.

### 4. Asset codec RE (parallel, compiler-independent)
`.OVx`/`.JFT` are the game DATA (see FORMATS.md). OV0/OV1 are 640×350 SCREEN 9 EGA,
custom-compressed (~2.7:1). Trace the loader (the routine that opens `BOLO3.OV1`
and writes EGA memory) to recover the codec, then render sprites to PNG to verify.
This unlocks "see Mr. Bolo" independently of the code build.

### 5. Bring-up milestone
Boot → main menu → select puzzle 1 → arrow-key a move → laser/box/water rules
behave → reach the red stairs. Debug against the recomp16 trace/crash handlers.

## Key facts to remember
- Image loads at **linear base 0**, so unrelocated segment values = image offsets.
- Entry **CS:IP 2011:0010**, **SS:SP 2348:0080**.
- Language: **compiled QuickBASIC 4.5** (BCOM45 static). Runtime segment **0x1183**.
- Shim surface: INT 21h/10h/16h/33h/1Ah + INT 34h–3Dh FP emulator.
