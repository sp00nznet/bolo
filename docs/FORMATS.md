# Data Format Reverse-Engineering Notes

The files next to `BOLO3.EXE` carry `.OV*` extensions but are **data, not code
overlays**. This is the working log of reversing them. Everything here is
provisional until cross-referenced against the loader code in the unpacked exe.

> **Big hint (Phase 1):** the game is compiled **QuickBASIC 4.5**. So the graphics
> blobs are very likely **`GET`/`PUT` sprite arrays** (and/or `BSAVE` images) for
> **`SCREEN 9`** (EGA 640×350×16). A QB `GET` array begins with a small header
> encoding the sprite's bit-width and pixel-height; the planar `00 AA …` bodies in
> OV0/OV1 are consistent with 4-plane EGA `PUT` data. The `.JFT` is plausibly a
> bitmap **font** table (the `GETFONT` routine tag supports this) and/or the puzzle
> grids. Reverse against the QB runtime's `B$PUT`/`B$GET`/`B$BLOAD` call sites.

## `BOLO3.OV0` / `BOLO3.OV1` — EGA graphics (sprites/tiles)

First bytes:
```
OV0: 0A 05 01 01 00 00 00 00 7F 02 63 00 2C 01 2C 01  3B 3E 3E 00 00 AA 00 AA ...
OV1: 0A 05 01 01 00 00 00 00 7F 02 5D 01 2C 01 2C 01  00 00 00 00 00 AA 00 AA ...
```
Header (16 bytes), as decoded words:
```
OV0: 050A 0101 0000 0000 | 027F 0063 012C 012C   (then pixel data)
OV1: 050A 0101 0000 0000 | 027F 015D 012C 012C
```
**Verified:** word4 = `0x027F` = **639** and OV1 word5 = `0x015D` = **349** → these
are `xmax,ymax` for **`SCREEN 9` (EGA 640×350×16, 4 bitplanes)**. OV0's word5 =
`0x63` = 99 → a 640×100 band. `0x050A 0101` is a signature/version; `012C 012C` =
300,300 (a default coord pair?).

**Compression confirmed, codec not yet:** a full 640×350 SCREEN 9 planar image is
640/8·350·4 = **112,000 bytes**, but OV1 is only **42,104** (~2.66:1), so the body
is *compressed*, not raw planes. Tested and ruled out:
- raw 4-plane planar (row-interleaved and plane-sequential) → renders as noise;
- simple `(count,value)` / `(value,count)` RLE → expands to ~200 KB, not 112 KB.

So it's a **custom Soleau/QB codec** (likely a plane-aware or marker-based RLE).
OV2 starts with a `0xFD` run — possibly a QB `BSAVE` image (magic `0xFD`) for the
title screen, a different container than OV0/OV1.

**Rendering pipeline (traced in the lifted code):** the EGA-touching functions are
mapped — game-side blitters at `0xEDC3, 0xEE3F, 0xF1EE, 0xF227, 0xF308, 0xF4BD,
0xF610, 0xF773` (these set `ES=0xA000` and drive the `0x3CE` graphics-controller
plane registers) plus the QB graphics runtime around `0x112xx / 0x124xx / 0x153xx`.
`res_00F1EE` is a `PUT`-style blitter: it computes the screen offset
`y * rowbytes(es:[0x4A]) + x + screenbase(ds:[0x3B9E])`, loads a far pointer to a
source array via `les si,[0x10C]`, sets `ES=0xA000`, and copies. So the flow is:

```
BOLO3.OVx  --loader/decoder-->  QB sprite array (planar)  --PUT (res_00F1EE)-->  EGA A000
```

Because the blitter consumes a *planar array* but the OVx file is smaller than the
planar bitmap, the **custom decompression happens in the loader** that fills the
array (OVx is not a raw QB `GET`/`PUT` array). 

**Next:** read the loader (opens `BOLO3.OV1`, parses the 16-byte header, expands
into the array `les si,[0x10C]` points at) to recover the codec. Easiest validated
once a build runs (dump the array post-load and diff against a re-encode); until
then, the decode harness lives in this file's git history. The PUT geometry above
(`rowbytes`, `screenbase`, plane order via `0x3CE`) already fixes how to render the
array once decoded.

## `BOLO3.OV2` — image / level art

```
FD FD FD FD FD FD E4 E3 E3 E3 E4 E4 E4 E3 E3 F5 ...
```
Dense byte stream, values clustered around 0xE3–0xFD. Looks like an 8bpp (VGA
mode 13h) image or packed tile indices rather than planar EGA. **TODO:** confirm
against loader; could be the title/background screen.

## `BOLO3.OV3` — text screens

ASCII, human-readable:
```
"   FN:C1        BOLO ADVENTURES III  :  REGISTRA[TION] ..."
```
Looks like formatted menu/registration/help text, possibly with `FN:` field codes
for layout or color. **TODO:** decode the `FN:` markup.

## `BOLO3.OV4` — small text/obfuscated blob (156 B)

```
"BOLO ADVENTURES III\r\n" then scrambled bytes (F8 FF F6 94 FE F0 FB ...)
```
Tiny. Possibly a high-score table, registration record, or an XOR/ADD-obfuscated
string blob. **TODO:** check for a simple cipher (running XOR / byte add) once the
loader is found.

## `BOLO3.JFT` — structured table (font and/or puzzles)

```
0C 00 01 00 FE 00 00 00 10 00 00 00 00 00 A8 00 ...
01 00 00 00 ...
```
Header smells like record metadata: `0C 00` (12?), `01 00` (1?), `FE 00` (254?),
`10 00` (16 — a cell/glyph size?), `A8 00` (168?). `.JFT` may stand for a font
table ("...Font Table") — 16-pixel glyphs would fit a `10 00` field — and/or hold
the 15 puzzle grid definitions. **TODO:** determine record size and count; a
font would have ~96–256 fixed-size records, the puzzles ~15 grid blobs.

---

### Method (per philosophy §8)
1. Hex dump, find magic/structure (done above, provisionally).
2. Find the loader in the unpacked binary via the filename strings.
3. Write a standalone parser/dumper into `tools/`.
4. Validate by round-trip (parse → render → compare to a screenshot of the
   original running under an emulator for reference only).
