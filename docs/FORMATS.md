# Data Format Reverse-Engineering Notes

The files next to `BOLO3.EXE` carry `.OV*` extensions but are **data, not code
overlays**. This is the working log of reversing them. Everything here is
provisional until cross-referenced against the loader code in the unpacked exe.

## `BOLO3.OV0` / `BOLO3.OV1` — EGA graphics (sprites/tiles)

First bytes:
```
OV0: 0A 05 01 01 00 00 00 00 7F 02 63 00 2C 01 2C 01  3B 3E 3E 00 00 AA 00 AA ...
OV1: 0A 05 01 01 00 00 00 00 7F 02 5D 01 2C 01 2C 01  00 00 00 00 00 AA 00 AA ...
```
- Shared 16-byte-ish header (`0A 05 01 01`, then a `7F 02` field, then `2C 01 2C 01`
  = 0x012C = 300 twice — likely width/height or a coordinate pair).
- Body is dominated by the planar EGA pattern `00 AA 00 AA AA AA ...` — classic
  4-plane 16-color EGA bitmap / color data. OV1 is the big one (42 KB) → probably
  the main sprite/tile sheet; OV0 (19 KB) a secondary set.
- **TODO:** find the loader (cross-ref the `"BOLO3.OV0"` / `"BOLO3.OV1"` strings in
  the unpacked exe) to confirm dimensions, plane order, and whether it's raw EGA
  planes or a simple RLE.

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
