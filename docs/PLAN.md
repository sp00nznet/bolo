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

### Phase 1 — Unpack & disassemble  ⬅ current
- [ ] **1a. PKLITE decompression** (`tools/unpklite.py`). Static unpacker → clean
      `work/BOLO3_unpacked.exe` (real MZ, real entry point, relocations applied).
      Verify: readable strings (filenames `BOLO3.OV0`, menu text), sane MZ header,
      entry lands on a real prologue.
- [ ] **1b. Decode** resident code with `decode16.py --resident`.
- [ ] **1c. Analyze** function boundaries / call graph / strings with `analyze.py`,
      export a symbol table (`work/bolo3.toml`).
- [ ] Identify the compiler/runtime (MSC vs Turbo C) from prologues & CRT strings
      to know which functions are library boilerplate we can shortcut.

### Phase 2 — Classification
- [ ] Separate C runtime / library code from Bolo game logic.
- [ ] Map the interrupt/BIOS surface actually used (INT 10h video, INT 16h kbd,
      INT 21h DOS file I/O, INT 33h mouse?, PIT/PC-speaker for sound). This is the
      exact shim TODO list — likely small for a game this size.
- [ ] Name the obvious entry points: main menu, puzzle loader, move/animation
      engine, collision/laser logic, renderer, file I/O for `.OVx`/`.JFT`.

### Phase 3 — Lifting
- [ ] `lift16.py` over the symbol table → C in `src/recomp/gen/`.
- [ ] Wire the `recomp16` runtime (CPU state struct, INT handlers, VGA framebuffer,
      keyboard) from `pc/tools/runtime/recomp16/`.

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
