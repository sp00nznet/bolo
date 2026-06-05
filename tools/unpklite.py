#!/usr/bin/env python3
"""
unpklite.py -- static decompressor for PKLITE-compressed DOS MZ executables.

Written for the Bolo Adventures III recomp (BOLO3.EXE is PKLITE 1.15,
large + extra model), but aims to be a reusable pcrecomp toolkit addition --
the toolkit had no DOS-exe unpacker before this.

Algorithm references:
  - OpenTESArena pklite_specification.md (Dozayon)  -- the LZ + XOR core
  - jsummers/deark modules/pklite.c                 -- exact Huffman tables,
                                                       large-model relocs
  - moddingwiki.shikadi.net/wiki/PKLite             -- header / data-start

PKLITE in one paragraph: an LZSS variant. A 16-bit little-endian "tag" word
feeds a control bitstream (LSB first). Control bit 0 => copy one literal byte
straight from the stream; control bit 1 => a back-reference whose length and
offset-high-byte are Huffman-coded in the bitstream and whose offset-low-byte
is a raw stream byte. In "extra" mode each literal byte is XOR-encrypted with
the number of bits still unread in the current tag word.
"""
import struct
import sys

# --- Huffman tables (from deark modules/pklite.c) ---------------------------
# Each entry packs (code_length << 12) | code_bits. The array index is the
# decoded symbol. Codes are read MSB-first from the LSB-first bitstream
# (i.e. accumulate code = (code << 1) | bit).
MATCHLEN_LG = [
    0x2003, 0x3000, 0x4002, 0x4003, 0x4004, 0x500a, 0x500b, 0x500c,
    0x601a, 0x601b, 0x703a, 0x703b, 0x703c, 0x807a, 0x807b, 0x807c,
    0x90fa, 0x90fb, 0x90fc, 0x90fd, 0x90fe, 0x90ff, 0x601c, 0x2002,
]
OFFSET_HI = [
    0x1001, 0x4000, 0x4001, 0x5004, 0x5005, 0x5006, 0x5007, 0x6010,
    0x6011, 0x6012, 0x6013, 0x6014, 0x6015, 0x6016, 0x702e, 0x702f,
    0x7030, 0x7031, 0x7032, 0x7033, 0x7034, 0x7035, 0x7036, 0x7037,
    0x7038, 0x7039, 0x703a, 0x703b, 0x703c, 0x703d, 0x703e, 0x703f,
]

LONG_ML_SYM = 22   # symbol that introduces a raw length byte / control codes
ML2_SYM = 23       # symbol meaning "match length 2, offset high byte = 0"


def build_decoder(table):
    """(length, code) -> symbol lookup."""
    d = {}
    for sym, packed in enumerate(table):
        length = packed >> 12
        code = packed & 0x0FFF
        d[(length, code)] = sym
    return d


class BitStream:
    """Mirrors the PKLITE decompressor's bit engine exactly.

    BP holds a 16-bit tag word; bits are consumed LSB-first via `shr bp,1`.
    DX counts the bits left in the word, starting at 16. The instant a consume
    drops the counter to 0 the word is reloaded and the counter reset to 16
    (the asm does `mov dl,0x10` inside the same bit read). The literal XOR key
    is this live counter, so its timing must match to the bit.
    """
    def __init__(self, data, pos):
        self.data = data
        self.pos = pos
        # preload the first tag word; counter starts full (=16)
        self.buf = data[pos] | (data[pos + 1] << 8)
        self.pos += 2
        self.cnt = 16
        self.refills = 1

    def bit(self):
        b = self.buf & 1
        self.buf >>= 1
        self.cnt -= 1
        if self.cnt == 0:
            self.buf = self.data[self.pos] | (self.data[self.pos + 1] << 8)
            self.pos += 2
            self.cnt = 16
            self.refills += 1
        return b

    def byte(self):
        v = self.data[self.pos]
        self.pos += 1
        return v

    def decode_sym(self, decoder, maxlen=12):
        code = 0
        for length in range(1, maxlen + 1):
            code = (code << 1) | self.bit()
            sym = decoder.get((length, code))
            if sym is not None:
                return sym
        raise ValueError(f"bad Huffman code near pos {self.pos:#x}")


def decompress(data, comp_start, xor_mode="cnt", offset_xor=0, max_out=1 << 20):
    ml = build_decoder(MATCHLEN_LG)
    of = build_decoder(OFFSET_HI)
    bs = BitStream(data, comp_start)
    out = bytearray()
    n_lit = n_match = 0
    stop = "eof-overrun"

    while len(out) < max_out and bs.pos < len(data) + 2:
        if bs.bit() == 0:
            # literal
            b = bs.byte()
            if xor_mode == "cnt":
                b ^= (bs.cnt & 0xFF)
            elif xor_mode == "cnt1":
                b ^= ((bs.cnt + 1) & 0xFF)
            elif xor_mode == "inv":
                b ^= ((16 - bs.cnt) & 0xFF)
            # "none" -> no xor
            out.append(b & 0xFF)
            n_lit += 1
            continue

        sym = bs.decode_sym(ml)
        offs_hi_known = None
        if sym < LONG_ML_SYM:
            matchlen = sym + 3
        elif sym == ML2_SYM:
            matchlen = 2
            offs_hi_known = 0
        else:  # LONG_ML_SYM (22) -- raw length byte / control
            b = bs.byte()
            if b == 0xFF:
                stop = "terminator(0xFF)"
                break
            if b == 0xFE:
                # segment separator -- keep going
                continue
            if b == 0xFD:
                stop = "uncompressed-area(0xFD) NOT IMPLEMENTED"
                break
            matchlen = b + 25

        if offs_hi_known is None:
            offs_hi = bs.decode_sym(of)
        else:
            offs_hi = offs_hi_known
        offs_lo = bs.byte() ^ (offset_xor & 0xFF)
        dist = (offs_hi << 8) | offs_lo
        if dist == 0 or dist > len(out):
            stop = f"bad-dist({dist}) at out={len(out):#x}"
            break
        src = len(out) - dist
        for _ in range(matchlen):
            out.append(out[src])
            src += 1
        n_match += 1

    return out, dict(stop=stop, n_lit=n_lit, n_match=n_match,
                     refills=bs.refills, end_pos=bs.pos, out_len=len(out))


def find_start(data, lo=0x60, hi=0x1200):
    """Locate the compressed-data start by structural validation.

    The LZ control/Huffman structure is independent of the literal XOR, so the
    true start is the offset whose stream decodes cleanly to the 0xFF
    terminator and yields the largest image. (The header byte-0x4E heuristic
    does not hold for the v1.15 large+extra stub used by Bolo3, so we scan.)
    """
    best = None
    for start in range(lo, hi):
        try:
            out, st = decompress(data, start, xor_mode="none", max_out=1 << 20)
        except Exception:
            continue
        if st["stop"].startswith("terminator") and (best is None or st["out_len"] > best[1]):
            best = (start, st["out_len"])
    return best[0] if best else None


def build_mz(image, ss, sp, cs, ip, minalloc=0x0FD8):
    """Wrap a decompressed load module in a fresh MZ header.

    NOTE: relocations are NOT yet reconstructed (large-model PKLITE normalizes
    them; see docs/FORMATS.md). This is sufficient for disassembly/analysis,
    which scans the resident image directly. The lifter will need the reloc
    table to distinguish segment-reference words from data -- TODO.
    """
    img = bytearray(image)
    while len(img) % 16:
        img.append(0)
    hdr = bytearray(0x20)
    total = len(hdr) + len(img)
    pages = (total + 511) // 512
    cblp = total - (pages - 1) * 512
    struct.pack_into("<2sHHHHHHHHHHHHH", hdr, 0, b"MZ", cblp, pages, 0, 2,
                     minalloc, 0xFFFF, ss, sp, 0, ip, cs, 0x1C, 0)
    return bytes(hdr) + bytes(img)


def main():
    if len(sys.argv) < 2:
        print("usage: unpklite.py <packed.exe> [out_image.bin] "
              "[--start 0xNNN] [--xor cnt|cnt1|inv|none] [--offxor 0xNN] "
              "[--mz out.exe]")
        sys.exit(1)
    path = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith("-") else None
    data = open(path, "rb").read()

    pg_header = struct.unpack_from("<H", data, 8)[0]
    stub = pg_header * 16
    v0, v1 = data[0x1C], data[0x1D]
    extra = bool(v1 & 0x10)
    large = bool(v1 & 0x20)

    if "--start" in sys.argv:
        comp = int(sys.argv[sys.argv.index("--start") + 1], 0)
    else:
        comp = find_start(data)
        if comp is None:
            print("error: could not locate compressed data start")
            sys.exit(2)
    xor_mode = sys.argv[sys.argv.index("--xor") + 1] if "--xor" in sys.argv else ("cnt" if extra else "none")
    offxor = int(sys.argv[sys.argv.index("--offxor") + 1], 0) if "--offxor" in sys.argv else 0

    print(f"PKLITE  v1.{v0:02d}  extra={extra} large={large}")
    print(f"stub @ {stub:#x}   compressed @ {comp:#x}   file_len {len(data):#x}")
    print(f"xor_mode={xor_mode} offset_xor={offxor:#x}")

    out, stats = decompress(data, comp, xor_mode=xor_mode, offset_xor=offxor)
    print("stats:", stats)

    # footer (last 8 bytes): SS, SP, CS, IP -- the real entry registers
    ss, sp, cs, ip = struct.unpack("<4H", data[-8:])
    print(f"footer  CS:IP={cs:#06x}:{ip:#06x}  SS:SP={ss:#06x}:{sp:#06x}")

    # quick sanity: look for known plaintext / code prologues
    for needle in (b"BOLO3.OV0", b"Soleau", b"Mr. Bolo", b"\x55\x8b\xec"):
        idx = out.find(needle)
        print(f"  find {needle!r:24}: {'@'+hex(idx) if idx>=0 else 'not found'}")

    if out_path:
        open(out_path, "wb").write(out)
        print(f"wrote image: {len(out)} bytes -> {out_path}")
    if "--mz" in sys.argv:
        mz_path = sys.argv[sys.argv.index("--mz") + 1]
        mz = build_mz(out, ss, sp, cs, ip)
        open(mz_path, "wb").write(mz)
        print(f"wrote MZ:    {len(mz)} bytes -> {mz_path}")


if __name__ == "__main__":
    main()
