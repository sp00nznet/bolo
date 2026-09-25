/* Minimal EGA (SCREEN 9, 640x350x16, 4 planes) emulation for the Bolo recomp.
 *
 * recomp16's video.c only models VGA mode 13h (linear 8bpp). Bolo uses EGA
 * planar memory at A000h with plane selection via the Sequencer map-mask
 * (port 3C4/3C5 reg 2) and the Graphics Controller (3CE/3CF: set/reset,
 * enable-set/reset, data rotate, read-map, mode, bit-mask). We implement write
 * modes 0 and 2 (what QuickBASIC's SCREEN 9 runtime uses) over 4 plane buffers,
 * with the EGA latch behavior on reads.
 */
#include "ega.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define PLANE_SZ 0x10000
static uint8_t plane[4][PLANE_SZ];
long g_ega_writes = 0;
static uint8_t latch[4];

static uint8_t seq_idx, gc_idx, crtc_idx, attr_idx, attr_flip;
static uint16_t crtc_start;        /* CRTC regs 0C/0D: displayed page offset */
/* EGA palette registers: 6-bit rgbRGB, BIOS defaults for 16 colours */
static uint8_t pal[16] = { 0x00,0x01,0x02,0x03,0x04,0x05,0x14,0x07,
                           0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F };
static uint8_t map_mask = 0x0F;   /* Seq reg 2          */
static uint8_t set_reset = 0;     /* GC reg 0           */
static uint8_t enable_sr = 0;     /* GC reg 1           */
static uint8_t rotate = 0;        /* GC reg 3 (rot+op)  */
static uint8_t read_map = 0;      /* GC reg 4           */
static uint8_t gc_mode = 0;       /* GC reg 5           */
static uint8_t bit_mask = 0xFF;   /* GC reg 8           */
static uint8_t color_cmp = 0;     /* GC reg 2           */
static uint8_t color_dc = 0x0F;   /* GC reg 7 (don't care mask) */

void ega_set_mode(void)
{
    memset(plane, 0, sizeof plane);
    map_mask = 0x0F; bit_mask = 0xFF; set_reset = enable_sr = rotate = 0;
    read_map = 0; gc_mode = 0;
}

long g_ega_portw = 0;
void ega_port_write(uint16_t port, uint8_t v)
{
    g_ega_portw++;
    switch (port) {
    case 0x3C4: seq_idx = v; break;
    case 0x3C5: if (seq_idx == 2) map_mask = v & 0x0F; break;
    case 0x3CE: gc_idx = v; break;
    case 0x3D4: crtc_idx = v; break;
    case 0x3D5:
        if (crtc_idx == 0x0C) crtc_start = (uint16_t)((crtc_start & 0x00FF) | (v << 8));
        if (crtc_idx == 0x0D) crtc_start = (uint16_t)((crtc_start & 0xFF00) | v);
        break;
    case 0x3C0:                     /* attribute controller: index/data flip-flop */
        if (!attr_flip) attr_idx = v & 0x1F;
        else if (attr_idx < 16) pal[attr_idx] = v & 0x3F;
        attr_flip ^= 1;
        break;
    case 0x3CF:
        switch (gc_idx) {
        case 0: set_reset = v & 0x0F; break;
        case 1: enable_sr = v & 0x0F; break;
        case 3: rotate = v; break;
        case 4: read_map = v & 3; break;
        case 5: gc_mode = v; break;
        case 2: color_cmp = v & 0x0F; break;
        case 7: color_dc = v & 0x0F; break;
        case 8: bit_mask = v; break;
        }
        break;
    }
}

uint8_t ega_port_read(uint16_t port) { (void)port; return 0; }

void ega_attr_reset(void) { attr_flip = 0; }     /* a 3DA read resets the flip-flop */
void ega_set_palette(int reg, uint8_t v) { if (reg >= 0 && reg < 16) pal[reg] = v & 0x3F; }

/* 640x350 page at the CRTC start address -> 0xAABBGGRR (SDL ABGR8888) */
void ega_render(uint32_t *out)
{
    uint32_t rgb[16];
    for (int i = 0; i < 16; i++) {
        uint8_t c = pal[i];     /* rgbRGB: primary bits 0-2, secondary 3-5 */
        int r = ((c >> 2) & 1) * 0xAA + ((c >> 5) & 1) * 0x55;
        int g = ((c >> 1) & 1) * 0xAA + ((c >> 4) & 1) * 0x55;
        int b = ((c >> 0) & 1) * 0xAA + ((c >> 3) & 1) * 0x55;
        rgb[i] = 0xFF000000u | (uint32_t)(b << 16) | (uint32_t)(g << 8) | (uint32_t)r;
    }
    for (int y = 0; y < 350; y++) {
        for (int xb = 0; xb < 80; xb++) {
            uint32_t o = (uint32_t)(crtc_start + y * 80 + xb) & 0xFFFF;
            uint8_t p0 = plane[0][o], p1 = plane[1][o], p2 = plane[2][o], p3 = plane[3][o];
            for (int bit = 7; bit >= 0; bit--) {
                int idx = ((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1) |
                          (((p2 >> bit) & 1) << 2) | (((p3 >> bit) & 1) << 3);
                *out++ = rgb[idx];
            }
        }
    }
}

uint8_t ega_read8(uint32_t off)
{
    if (off >= PLANE_SZ) return 0;
    for (int p = 0; p < 4; p++) latch[p] = plane[p][off];
    if (gc_mode & 0x08) {       /* read mode 1: 1 where every compared plane
                                 * matches color_cmp; with nothing compared
                                 * that is 0xFF, which the PCX loader relies on
                                 * for `and es:[di], ah` to write AH as-is */
        uint8_t r = 0xFF;
        for (int p = 0; p < 4; p++)
            if (color_dc & (1 << p))
                r &= (uint8_t)~(latch[p] ^ ((color_cmp >> p) & 1 ? 0xFF : 0x00));
        return r;
    }
    return plane[read_map][off];
}

void ega_write8(uint32_t off, uint8_t value)
{
    if (off >= PLANE_SZ) return;
    g_ega_writes++;
    /* debug: BOLO_EGADUMP=N snapshots the planes after N writes and exits */
    static long dump_at = -1;
    if (dump_at < 0) { const char *e = getenv("BOLO_EGADUMP"); dump_at = e ? atol(e) : 0; }
    if (dump_at && g_ega_writes == dump_at) { ega_dump("work/ega_planes.bin"); _Exit(0); }
    int mode = gc_mode & 3;
    static int stat = -1;
    if (stat < 0) stat = getenv("BOLO_EGASTAT") != NULL;
    extern unsigned long g_last_enter;
    if (stat) {     /* histogram: (writer, write mode, map mask, ALU op) */
        static struct { unsigned long fn; int key; long n; } h[256];
        static int nh;
        int key = (gc_mode & 3) << 8 | map_mask << 4 | ((rotate >> 3) & 3);
        int i;
        for (i = 0; i < nh && (h[i].fn != g_last_enter || h[i].key != key); i++) {}
        if (i == nh && nh < 256) { h[nh].fn = g_last_enter; h[nh].key = key; nh++; }
        if (i < 256) h[i].n++;
        if (g_ega_writes % 200000 == 0)
            for (int k = 0; k < nh; k++)
                if (h[k].n) { fprintf(stderr, "[egah] res_%06lX mode=%d map=%X op=%d: %ld\n",
                                      h[k].fn, h[k].key >> 8, (h[k].key >> 4) & 15, h[k].key & 3, h[k].n);
                              h[k].n = 0; }
    }
    if (stat && g_ega_writes % 2000000 == 0)
        fprintf(stderr, "[egaw] #%ld off=%04X v=%02X mode=%d map=%X bm=%02X esr=%X sr=%X rot=%02X\n",
                g_ega_writes, (unsigned)off, value, mode, map_mask, bit_mask, enable_sr,
                set_reset, rotate);
    uint8_t data = value;
    int rot = rotate & 7, op = (rotate >> 3) & 3;     /* GC reg 3: count, function */
    if (rot) data = (uint8_t)((data >> rot) | (data << (8 - rot)));
    for (int p = 0; p < 4; p++) {
        if (!(map_mask & (1 << p))) continue;
        if (mode == 1) { plane[p][off] = latch[p]; continue; }    /* latch copy */
        uint8_t d, mask = bit_mask;
        if (mode == 2) {
            d = (value & (1 << p)) ? 0xFF : 0x00;   /* colour in low nibble */
        } else if (mode == 3) {                     /* set/reset colour, data masks */
            d = (set_reset & (1 << p)) ? 0xFF : 0x00;
            mask &= data;
        } else {                                    /* mode 0 */
            if (enable_sr & (1 << p)) d = (set_reset & (1 << p)) ? 0xFF : 0x00;
            else d = data;
        }
        switch (op) {                               /* ALU against the latch */
        case 1: d &= latch[p]; break;
        case 2: d |= latch[p]; break;
        case 3: d ^= latch[p]; break;
        }
        plane[p][off] = (uint8_t)((d & mask) | (latch[p] & ~mask));
    }
}

/* dump the 4 planes (640x350) for offline decode to PNG */
void ega_dump(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    for (int p = 0; p < 4; p++) fwrite(plane[p], 1, PLANE_SZ, f);   /* all 64K: both pages */
    fclose(f);
    extern long g_ega_portw;
    fprintf(stderr, "[EGA] %ld mem-writes, %ld port-writes -> %s\n",
            g_ega_writes, g_ega_portw, path);
    fprintf(stderr, "[EGA] crtc start %04X, palette", crtc_start);
    for (int i = 0; i < 16; i++) fprintf(stderr, " %02X", pal[i]);
    long nz[4] = {0};
    for (int p = 0; p < 4; p++) for (long o = 0; o < PLANE_SZ; o++) nz[p] += plane[p][o] != 0;
    fprintf(stderr, "\n[EGA] nonzero bytes per plane (64K): %ld %ld %ld %ld\n",
            nz[0], nz[1], nz[2], nz[3]);
}
