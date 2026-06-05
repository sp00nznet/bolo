# The Plan — Static Recomp of Bolo Adventures III

Following the pcrecomp universal pipeline (`tools/docs/PHILOSOPHY.md`), specialized
for a small single-binary 16-bit DOS target. The closest precedent is the **civ**
project, which proved this exact toolchain on 1991-era MSC 16-bit code.

## Strategy

Bolo3 is small enough that we pursue a **two-track** approach that converges:

1. **Mechanical lift (correctness track).** Decompress → decode → analyze → lift
   the 8086 code to C against the `recomp16` DOS/VGA runtime. Goal: a correct,
   running baseline that behaves exactly like the original, ugly C and all. This
   is "static recomp" in the literal sense and is the toolkit's bread and butter.

2. **Format/asset RE (clarity track).** In parallel, reverse the data formats
   (the `.OVx` EGA graphics, `.JFT` table, the 15 puzzle layouts) into clean
   loaders. These are small and self-contained, and understanding them makes the
   lifted code legible ("ah, *this* function blits a tile from OV1").

The mechanical track guarantees we can stand the whole thing up. The format track
lets us progressively replace ugly lifted blobs with clean reimplementations
where it's worth it — and is what eventually enables modern extras (scaling,
save states, new puzzles).

## Phases & milestones

### Phase 0 — Reconnaissance ✅
See `RECON.md`. Single 80 KB PKLITE'd 16-bit MZ; `.OVx`/`.JFT` are data.

### Phase 1 — Unpack & disassemble  ✅
- [x] **1a. PKLITE decompression** (`tools/unpklite.py`). Static unpacker — wrote
      a byte-exact decompressor for the v1.15 **large+extra** variant by emulating
      the self-decrypting stub. Auto-locates the stream (file 0x31E) → 136,151-byte
      image. Verified by strings + `55 8B EC` prologues. Emits `work/BOLO3_image.bin`
      and a rebuilt MZ `work/BOLO3_unpacked.exe`. Entry CS:IP=2011:0010.
- [x] **1b. Decode** resident code (capstone 16-bit; integrated via analyze).
- [x] **1c. Analyze** → **182 functions, 35,076 insns, 833 strings**; symbol table
      `work/bolo3.toml`.
- [x] **Compiler identified: Microsoft QuickBASIC 4.5 (BCOM45 statically linked)** —
      from the QB runtime error-string table. (See RECON.md.)
- [ ] **1d. Relocations.** Reconstruct the large-model reloc table + apply, so the
      MZ is fully runnable and the lifter can tell segment-reference words from
      data. (Deferred — not needed for analysis; see FORMATS.md / unpklite TODO.)

### Phase 2 — Classification  ✅ (see CLASSIFY.md)
- [x] **Fingerprinted the QuickBASIC runtime via the call graph.** 81% of far
      calls (1,783) land in one segment (`0x1183`) across 77 entry points = the
      BCOM45 runtime. Game logic is the other 18% (397 calls, ~70 routines in ~13
      segments). The RE target is just **~70 game routines**.
- [x] Mapped the **interrupt surface / shim list**: INT 21h (file), INT 10h
      (`SCREEN 9` graphics), INT 16h (kbd), INT 33h (mouse), INT 1Ah (timer), and
      the **INT 34h–3Dh MS floating-point emulator**.
- [x] Found first **anchors**: DOS dispatch `0x152C3`, graphics `0x120C2/0x18893`,
      input-poll `0x10C59`; plus the DGROUP string-descriptor table format.
- [ ] **2-next.** Name the 77 runtime entry points by behavior; trace startup →
      game `main` → puzzle loader (asset-offset refs) → move loop (hot seg
      `0x00EE00`). Feeds Phase 3 + FORMATS.

### Phase 3 — Lifting  🔨 in progress
- [x] Wrote the driver `tools/lift_bolo.py` (decode16 + lift16, far calls resolved
      via base-0 image layout so `seg*16+off` = image offset).
- [x] Lifted to C in `src/recomp/gen/` (chunked + forward-decl header + dispatch
      table). **No lift exceptions.**
- [x] Wired the `recomp16` runtime into `src/runtime/`, wrote `src/main.c`
      (loads image at linear 0, seeds regs from the footer) and `CMakeLists.txt`.
- [ ] **Gaps to close** (categorized from the generated comments):
  - **174 x87 FPU escapes** — translate the float math (Bolo uses floats); also the
    `INT 34h–3Dh` emulator entry points map here.
  - **160 indirect-dispatch sites** (`call/jmp [reg/mem]`) — wire through the
    generated `recomp_dispatch.c` table.
  - 24 far/indirect jmp/call, minor `into`/`grp4`.
- [x] **Expanded the function set via far-call closure** (`discover()` in
      lift_bolo.py): 182 → **225 functions**, and confirmed the QB runtime *is*
      lifted (DOS dispatch `0x152C3`, graphics `0x120C2`, hot helper `0x13CC2` all
      land in defined functions). Only **10 far-call targets remain unresolved**.
- [x] **Segment-aware near-call resolution — done.** Implemented `scan_far_targets`
      + `near_call_target` + segment-aware `discover()`: each function gets a
      segment base (exact from far-call `seg` values, inferred for the rest), near
      calls are resolved as `segbase*16 + ((aoff + pos + rel) & 0xFFFF)` with the
      true rel recovered from the raw bytes, and near-call disps are rewritten so
      the lifter resolves them correctly. **Result: 182 → 932 functions, 56,821
      instructions, near-call resolution 0% → 99% (1466/1467 hit a real start),
      stubs 566 → 14.** The program entry `res_020120` is now lifted and in the
      dispatch table. The 14 remaining stubs are all out-of-image far targets
      (BIOS/absolute/runtime-allocated segments) — correctly left as stubs.
- [ ] **Relocations** (deferred 1d) — reconstruct so segment-arithmetic is correct
      at runtime (needed once we execute, not to compile).

### Phase 4 — Shimming
- [ ] EGA/VGA output → SDL2 framebuffer (16-color planar → RGBA).
- [ ] Keyboard (arrows + command keys) → SDL2 events.
- [ ] DOS file I/O for the asset files → host fopen.
- [ ] PC-speaker sound → SDL2 audio (optional, gated behind the in-game toggle).

### Phase 5 — Build & debug
- [ ] CMake project (from template), 32-bit host build hosting the 16-bit memory
      model, fixed base, large stack.
- [ ] Boot → main menu → select puzzle → move Mr. Bolo → solve room 1.
- [ ] Debug loop against the runtime's trace/crash handlers.

### Phase 6 — Ship & extend
- [ ] Native Windows build, no DOSBox.
- [ ] Stretch: integer-scaled window, save/restore, level editor, new puzzle packs.

## Definition of done (MVP)

Mr. Bolo loads the first puzzle from the original assets, responds to the arrow
keys, the laser/box/water rules behave identically to the original, and reaching
the red stairs advances the room — running as a native binary with no emulator.

## Risks / unknowns

- **PKLITE exactness.** A decompressor must be byte-exact or the code is garbage.
  Mitigation: validate against known strings + MZ structure; cross-check the
  reconstructed entry point disassembles to a sane CRT startup.
- **Self-modifying / timing-sensitive code.** Possible in the animation/sound
  loop; the recomp16 runtime already models the PIT.
- **Data format opacity.** The EGA blit format and `.JFT` table need RE, but they
  are tiny and we have the rendering code in the binary to cross-reference.
