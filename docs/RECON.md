# Phase 0 — Reconnaissance

> What are we looking at, before we touch anything.

## The target

**Bolo Adventures III** — Soleau Software, 1993. Program by William Soleau.
A grid-based logic/puzzle game (Sokoban lineage): guide Mr. Bolo through 15 rooms
of lasers, crates, water, balls, boxes, holes, buttons and stairs. EGA/VGA only.
Distributed as shareware.

## File inventory

| File | Size | What it is |
|------|------|-----------|
| `BOLO3.EXE` | 79,521 B | **All game code.** 16-bit DOS MZ, **PKLITE-compressed.** |
| `BOLO3.OV0` | 19,169 B | Data — EGA graphics (header `0A 05 01 01 ... 7F 02`, planar `00 AA` palette/bitmap pattern). |
| `BOLO3.OV1` | 42,104 B | Data — EGA graphics (same header shape as OV0; largest asset file). |
| `BOLO3.OV2` | 24,931 B | Data — image/level art (byte stream `FD FD .. E4 E3`). |
| `BOLO3.OV3` |  2,104 B | Data — text screens ("FN:C1", "BOLO ADVENTURES III : REGISTRATION..."). |
| `BOLO3.OV4` |    156 B | Data — small text/obfuscated blob ("BOLO ADVENTURES III\r\n" + scrambled bytes). |
| `BOLO3.JFT` |  8,738 B | Data — structured table (header `0C 00 01 00 FE 00 ... 10 00 ... A8 00`). Font and/or the 15 puzzle definitions. |
| `BOLO3.DOC` / `BOLO_REG.DOC` / `FILE_ID.DIZ` | — | Documentation. |

### Key insight: the `.OVx` files are NOT executable overlays

Despite the `.OV0`–`.OV4` extensions (which usually denote Borland/MSC code
overlays), these are plain **data files**. Their headers are graphics/text data,
not MZ images, and there is no INT 3Fh overlay machinery referenced. Soleau
simply reused the `.OV*` naming for the game's asset blobs.

**Consequence:** *all executable code lives in the single 79 KB `BOLO3.EXE`.*
This is a small, self-contained, single-binary 16-bit DOS target — about as
tractable as a static recomp gets.

## Executable header (BOLO3.EXE)

```
MZ  bytes_last_page=0x00A1  pages=0x009C  relocs=1  hdrparas=6 (0x60)
min_alloc=0x0FD8  max_alloc=0xFFFF  SS:SP=136B:0200  CS:IP=FFF0:0100
reloc_tbl@0x52  overlay=0
```
Image size = (0x9C-1)*512 + 0xA1 = 79,521 B → equals file size, so there is **no
appended internal overlay**; the load module is the whole file.

## Compression: PKLITE

```
offset 0x1E: "PKLITE Copr. 1990-92 PKWARE Inc. All Rights Reserved"
```
The load module at file offset 0x60 is the PKLITE decompression stub:
```
B8 40 23      mov ax, 0x2340
BA 65 13      mov dx, 0x1365
...           cmp ax,[0x0002] / jb / "Not enough memory$" / int 21h, int 20h
```
This is the classic PKLITE 1.x "check memory, relocate self, decompress, jump to
real entry" stub. **The real game code is compressed and must be decompressed
before any disassembly is meaningful.** Decompression is the first hard step
(see PLAN.md → Phase 1). No DOSBox is available locally, so we unpack statically
with our own PKLITE decompressor (`tools/unpklite.py`).

## Toolchain available

- Python 3.13 + `capstone` 5.0.7 + `pefile`
- pcrecomp 16-bit pipeline: `decode16.py` → `analyze.py` → `lift16.py`
  (+ `runtime/recomp16/` DOS/VGA runtime). Closest prior project: **civ** (1991,
  16-bit DOS, MSC 5.x).
- `gh` (authed), `git`.

## Phase 1a/1b/1c results (unpacked & analyzed)

**Unpacked** with our own `tools/unpklite.py` (no DOSBox needed):
- The PKLITE stub is **self-decrypting** (rolling-XOR loop at IP 0x13E unpacks the
  real decompressor in place). We emulated that to read the true decompressor and
  nail the bit engine, then auto-locate the compressed stream (file **0x31E**).
- Decode: 41,289 literals + 17,382 matches → **136,151-byte** load image,
  terminating 10 bytes from EOF (reloc table + register footer).
- Real entry **CS:IP = 2011:0010**, **SS:SP = 2348:0080**. Large segment values
  ⇒ multi-segment large-model program (lots of far calls). Verified by readable
  strings (`BOLO3.OV0`, `Soleau`, `Mr. Bolo`) and `55 8B EC` prologues.

**Analyzed** (`analyze.py` on the rebuilt MZ): **182 functions, 35,076
instructions, 833 strings**; symbol table at `work/bolo3.toml`.

### Language: compiled Microsoft QuickBASIC 4.5  ⭐

The string table is full of the **QuickBASIC runtime error messages**
(`RETURN without GOSUB`, `Out of DATA`, `CASE ELSE expected`, `RESUME without
error`, `FIELD statement active`, `Redo from start`, `Bytes free`, …) plus
linked-routine tags like `bmGETFONT`, `blSTRIP`, `blEWINDC`. Bolo3 was written in
**QuickBASIC and compiled with BCOM45 statically linked** (self-contained, no
BRUN needed). Implications:

- A large share of the 182 functions is the **documented QB runtime** (string/
  array/file/graphics `B$…` helpers) — prime classification targets we don't have
  to reverse from scratch.
- Graphics are almost certainly BASIC **`SCREEN 9` (EGA 640×350×16)** with
  **`PUT`/`GET`** sprite arrays — which explains the `.OVx` graphics blobs.

Extra asset references found in the image: `BOLO3.SCR` (title screen?), `T1.JFT`
(per-puzzle table?), and the trio `BOLO3.OV3 BOLO3.OV4 BOLO3.JFT` loaded together.

## Closest prior art

**civ** (Sid Meier's Civilization, 1991) — same era, same format (16-bit MZ),
same pipeline (custom 16-bit decoder → analyzer → lifter → DOS compat runtime),
and it compiles. Bolo3 is *far* smaller (80 KB vs 305 KB, one binary vs 23
overlays), so it should be a substantially easier run through the same machinery.
