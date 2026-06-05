# Phase 2 — Classification

> Separate the known QuickBASIC runtime from the actual Bolo game logic, so we
> only reverse-engineer what's genuinely custom.

Method: in the *unrelocated* decompressed image, far-call segment words are
already image-base-relative, so every `9A off off seg seg` resolves directly to
an image offset. Tallying all 2,180 far-call sites gives a call graph without
needing relocations applied.

## The split

| Bucket | Calls | Share | Distinct entry points |
|--------|------:|------:|----------------------:|
| **QuickBASIC runtime** (segment `0x1183`, base `0x11830`) | 1,783 | **81%** | 77 |
| **Game logic + other** (≈13 segments) | 397 | 18% | ~70 |

One segment swallows four out of five calls — that's the statically-linked
**BCOM45 QuickBASIC 4.5 runtime**. The real reverse-engineering target is the
**~70 game routines** that call into it. (Compare Gunman Chronicles: 78% SDK,
~13% custom. Same shape.)

### Game-logic code segments (RE targets)

| Segment base | Calls out | Routines | Likely role (to confirm) |
|-------------|----------:|---------:|--------------------------|
| `0x00EE00` | 99 | 10 | hot — main loop / move engine? |
| `0x00F9B0` | 56 | 13 | game module |
| `0x00E110` | 55 | 1 | tight inner routine (animation?) |
| `0x010E10` | 37 | 3 | |
| `0x010F50` | 35 | 10 | game module |
| `0x019B40` | 33 | 13 | game module |
| `0x00F8A0` | 25 | 3 | |
| `0x011300` | 21 | 7 | |
| `0x00D1C0` | 13 | 1 | |
| smaller: `0x00DA80`, `0x00C300`, `0x00ED90`, `0x003750`, `0x000E90`, `0x01A890` | 2 each | | |

## The shim TODO list (interrupt surface)

Counts are from a raw `CD nn` scan (singletons are mostly false positives in
data; the high-count entries are real):

| INT | Count | Purpose | Shim target |
|----:|------:|---------|-------------|
| `21h` | 147 | DOS file I/O, memory, exit | host file API / runtime |
| `10h` | 20 | Video BIOS — mode set (`SCREEN 9`), palette | SDL2 framebuffer |
| `1Ah` | 5 | System timer ticks | host clock |
| `16h` | 4 | Keyboard BIOS | SDL2 input |
| `33h` | 4 | Mouse driver | SDL2 mouse (optional) |
| `13h` | 2 | Disk (rare) | host file API |
| `34h`–`3Dh` | 25/18/13/9/5/5… | **MS floating-point emulator** | native `float`/`double` |

Because the QB runtime mediates almost everything, the practical shim list is
"implement the ~77 runtime entry points we actually hit" — most of which fan out
to this small INT set. The FP-emulator traps (`INT 34h–3Dh`) mean we'll map the
emulator entry points to native floating point during the lift.

## DGROUP string constant pool

The game's string literals live in a contiguous QuickBASIC descriptor table.
Each record is a 5-byte descriptor + inline data:

```
0x1D1E4:  c5 09 00 de 0e  "BOLO3.OV0"     ; len=0x09, DS-offset=0x0EDE
0x1D1F2:  f4 09 00 ec 0e  "BOLO3.OV1"     ; offsets step +0x0E per record
          f4 09 00 fa 0e  "BOLO3.OV2"
          f4 09 00 08 0f  "BOLO3.OV3"
          f4 09 00 16 0f  "BOLO3.OV4"
          f4 09 00 24 0f  "BOLO3.JFT"
          f4 17 00 32 0f  "*  S H A R E W A R E  *"   ; len=0x17
          f4 14 00 4e 0f  "REGISTE..."               ; len=0x14
```

So `[lo] [len] 00 [data_off_lo] [data_off_hi]`. The loader code references each
asset by its DS-relative offset — these offsets are the anchors for finding the
file-I/O and graphics-load routines in the next pass.

## Anchors found (routines that issue the key INTs)

Mapping each `INT` site to its enclosing far-call target:

| Routine | Issues | Read as |
|--------|--------|---------|
| `0x152C3` | INT 21h ×31 | QB runtime **DOS dispatch core** (file open/read/write) |
| `0x170EA`, `0x17862` | INT 21h + INT 10h | runtime file+screen routines |
| `0x120C2`, `0x18893` | INT 10h ×5 each | QB **graphics** BIOS wrappers (mode/palette/blit) |
| `0x1A8B4`, `0x14A7C`, `0x14C3C` | INT 21h | more file-I/O runtime |
| `0x10C59` | INT 16h ×2 **and** INT 33h ×2 | the **input-poll** routine (keyboard + mouse) |
| `0x00F810`, `0x00F94C` | INT 10h | game-side graphics helpers (below the runtime segment) |

These are the first runtime/HAL entry points to name and shim.

## Runtime entry points named so far (by usage in the lifted C)

The lift is now legible enough to name QB runtime routines from how the game calls
them. First identifications:

| Runtime routine | Address | Role |
|----------------|---------|------|
| `res_013CC2` | `1183:2492` | **QB string assign** `LET s$=...` — called as `(src_descriptor, dest_var_off)`; it's the hottest routine (337 calls). |
| `res_0140EA` | `1183:28BA` | string/var housekeeping (paired after assigns) |
| `res_01133A` | `1130:003A` | string helper (game-segment thunk) |
| `res_0152C3` | runtime | DOS dispatch core (INT 21h ×31) — file open/read/write |
| `res_0120C2` / `res_018893` | runtime | EGA graphics BIOS wrappers (INT 10h) |
| `res_00F1EE` | game | `PUT`-style EGA blitter (array → `A000`); see FORMATS.md |
| `res_010C59` | game | keyboard+mouse input poll (INT 16h/33h) |

Example of how readable the lifted game logic now is — assigning a filename to a
BASIC string variable before opening it:
```c
cpu->ax = 0xEE8;  push16(cpu, cpu->ax);   /* push &"BOLO3.OV1" descriptor */
cpu->ax = 0x6A;   push16(cpu, cpu->ax);   /* push dest var slot          */
res_013CC2(cpu);                          /* B$ string-assign            */
```

## Next

1. **Name the 77 runtime entry points** by behavior — which wrap `INT 10h`
   (graphics: `SCREEN`/`PSET`/`LINE`/`PUT`/`GET`/`PALETTE`), `INT 21h` (`OPEN`/
   `GET`/`PUT`/`BLOAD`), strings, arrays, error handling. This is the runtime
   header for the lift/shim.
2. **Find the game's `main`** — trace from the startup (`CS:IP=2011:0010`) to the
   first user-module call, then the puzzle loader (references the `0x0EDE…`
   asset offsets) and the move/animation loop (hot segment `0x00EE00`).
3. Feed both back into FORMATS (the `PUT`/`GET` graphics format) and PLAN Phase 3.
