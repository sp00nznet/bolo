/* Computed-transfer dispatcher for the Bolo recomp.
 *
 * The lifter emits recomp_dispatch(cpu, seg, off) for retf-trampolines and
 * indirect call/jmp. We map the runtime far address seg:off back to an image
 * offset and binary-search the generated dispatch table (g_dispatch in
 * recomp_dispatch.c) for the lifted C function.
 *
 * Mapping: linear = seg*16 + off; image_off = linear - g_load_base.
 * g_load_base is where image offset 0 lives in the flat memory (set by main.c).
 * A relocation delta can be folded in later once the startup's self-move is
 * understood; for now unresolved targets are traced so we can discover it.
 */
#include <stdio.h>
#include <stdint.h>
#include "cpu.h"

typedef struct { unsigned long addr; void (*fn)(CPU*); } dispatch_t;
extern const dispatch_t g_dispatch[];
extern const int g_dispatch_count;

unsigned long g_load_base = 0;      /* linear address of image offset 0 */
int g_trace = 0;                    /* set by BOLO_TRACE env in main */
long g_dispatch_calls = 0;
long g_dispatch_misses = 0;
static int g_depth = 0;             /* runaway guard */

/* ring buffer of recent dispatch targets (for crash diagnosis) */
unsigned long g_ring[32];
int g_ring_pos = 0;

static void (*lookup(unsigned long image_off))(CPU*)
{
    int lo = 0, hi = g_dispatch_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        unsigned long a = g_dispatch[mid].addr;
        if (a == image_off) return g_dispatch[mid].fn;
        if (a < image_off) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

void recomp_dispatch(CPU *cpu, uint16_t seg, uint16_t off)
{
    unsigned long linear = (unsigned long)seg * 16 + off;
    unsigned long image_off = linear - g_load_base;
    void (*fn)(CPU*) = lookup(image_off);
    /* TODO: the QB startup self-relocates (copies the program to a new segment
     * and retf's into the copy), so post-relocation targets need a relocation
     * delta applied here. See docs/NEXT.md "self-relocating startup". */
    g_dispatch_calls++;
    if (!fn) {
        g_dispatch_misses++;
        if (g_trace && g_dispatch_misses <= 40)
            fprintf(stderr, "[icall] miss %04X:%04X -> image 0x%05lX "
                    "(no lifted fn)\n", seg, off, image_off);
        return;                      /* unresolved -> treat as plain return */
    }
    if (g_depth > 4096) {            /* runaway guard */
        if (g_trace) fprintf(stderr, "[icall] depth cap hit\n");
        return;
    }
    if (g_trace && g_dispatch_calls <= 60)
        fprintf(stderr, "[icall] %04X:%04X -> image 0x%05lX\n", seg, off, image_off);
    g_ring[g_ring_pos++ & 31] = image_off;
    g_depth++;
    fn(cpu);
    g_depth--;
}
