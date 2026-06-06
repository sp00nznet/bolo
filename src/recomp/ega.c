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

static uint8_t seq_idx, gc_idx;
static uint8_t map_mask = 0x0F;   /* Seq reg 2          */
static uint8_t set_reset = 0;     /* GC reg 0           */
static uint8_t enable_sr = 0;     /* GC reg 1           */
static uint8_t rotate = 0;        /* GC reg 3 (rot+op)  */
static uint8_t read_map = 0;      /* GC reg 4           */
static uint8_t gc_mode = 0;       /* GC reg 5           */
static uint8_t bit_mask = 0xFF;   /* GC reg 8           */

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
    case 0x3CF:
        switch (gc_idx) {
        case 0: set_reset = v & 0x0F; break;
        case 1: enable_sr = v & 0x0F; break;
        case 3: rotate = v; break;
        case 4: read_map = v & 3; break;
        case 5: gc_mode = v; break;
        case 8: bit_mask = v; break;
        }
        break;
    }
}

uint8_t ega_port_read(uint16_t port) { (void)port; return 0; }

uint8_t ega_read8(uint32_t off)
{
    if (off >= PLANE_SZ) return 0;
    for (int p = 0; p < 4; p++) latch[p] = plane[p][off];
    return plane[read_map][off];
}

void ega_write8(uint32_t off, uint8_t value)
{
    if (off >= PLANE_SZ) return;
    g_ega_writes++;
    /* DEBUG: once the game has drawn a screenful of EGA graphics, snapshot it */
    if (g_ega_writes == 30000) { ega_dump("work/ega_planes.bin"); _Exit(0); }
    int mode = gc_mode & 3;
    uint8_t data = value;
    int rot = rotate & 7;
    if (rot) data = (uint8_t)((data >> rot) | (data << (8 - rot)));
    for (int p = 0; p < 4; p++) {
        if (!(map_mask & (1 << p))) continue;
        uint8_t d;
        if (mode == 2) {
            d = (value & (1 << p)) ? 0xFF : 0x00;   /* color in low nibble */
        } else {                                    /* mode 0 */
            if (enable_sr & (1 << p)) d = (set_reset & (1 << p)) ? 0xFF : 0x00;
            else d = data;
        }
        plane[p][off] = (uint8_t)((d & bit_mask) | (latch[p] & ~bit_mask));
    }
}

/* dump the 4 planes (640x350) for offline decode to PNG */
void ega_dump(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    for (int p = 0; p < 4; p++) fwrite(plane[p], 1, 640 / 8 * 350, f);
    fclose(f);
    extern long g_ega_portw;
    fprintf(stderr, "[EGA] %ld mem-writes, %ld port-writes -> %s\n",
            g_ega_writes, g_ega_portw, path);
}
