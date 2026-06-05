# Runbook — where to pick up

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
2. **Computed control flow + self-relocation.** The startup physically moves the
   program (`rep movsb`) and `retf`-trampolines to the *moved copy* — but our
   lifted functions are static C at original image offsets, so the moved copy has
   no code to run. Two viable fixes:
   - **Dispatch with relocation delta:** add `recomp_dispatch(cpu,seg,off)` (binary-
     search `g_dispatch`, map `seg*16+off - reloc_base` → image offset) and emit it
     for indirect `call`/`jmp` and for `retf`-when-the-target-resolves. The
     relocation is a constant paragraph shift, so the delta is a single constant.
   - **Bypass the relocation:** skip the self-move and jump straight to the post-
     relocation QB entry (the runtime init + user main), mapped back to its original
     image offset. Often the pragmatic choice for self-relocating CRT startups.

These are coupled: fix the 0x100 load base + PSP seeding first (quick, in
`main.c`), then make `retf`/indirect transfers dispatch — each change is now
testable in the build/run loop (`scripts/build.sh` + run).

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
