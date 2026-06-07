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
#include <stdlib.h>
#include "recomp/ega.h"

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

CPU *g_dbg_cpu = NULL;
long g_enter_n = 0;
void recomp_dispatch(CPU *cpu, uint16_t seg, uint16_t off);   /* fwd decl */
extern uint32_t dos_get_vector(unsigned n);
extern void dos_tick(CPU *cpu);
static int g_in_timer = 0;
long g_timer_fires = 0;

/* Periodically fire the game's installed timer ISR (INT 1Ch / IRQ0 INT 8) so the
 * frame-timing wait loops advance. The lifter renders IRET as a bare return, so
 * we just dispatch to the installed vector. */
static void maybe_fire_timer(void)
{
    if (g_in_timer || !g_dbg_cpu) return;
    if (g_enter_n % 150 != 0) return;
    g_in_timer = 1;
    dos_tick(g_dbg_cpu);
    uint32_t v = dos_get_vector(0x1C);
    if (v) { g_timer_fires++; recomp_dispatch(g_dbg_cpu, (uint16_t)(v >> 16), (uint16_t)v); }
    v = dos_get_vector(0x08);
    if (v) recomp_dispatch(g_dbg_cpu, (uint16_t)(v >> 16), (uint16_t)v);
    g_in_timer = 0;
}

unsigned long g_last_enter = 0;          /* most recent function entered */
unsigned long g_enter_ring[64];          /* recent enter history (call path) */
int g_enter_ring_pos = 0;

uint32_t g_watch_addr = 0;               /* linear addr to trace writes (0=off) */
static long g_watch_hits = 0;
void mem_watch_hit(uint32_t a, uint16_t val, int width)
{
    if (g_watch_hits++ < 40)
        fprintf(stderr, "[watch] write%d 0x%05lX = 0x%0*X  by res_%06lX (enter#%ld)\n",
                width, (unsigned long)a, width == 8 ? 2 : 4, val,
                g_last_enter, g_enter_n);
    /* BOLO_WATCH_FF: stop and dump the call path the first time the watched word
     * is written 0xFFFF (the heap-corruption value) so we can find the culprit. */
    if (getenv("BOLO_WATCH_FF") && (val & 0xFF) == 0xFF && width == 16) {
        fprintf(stderr, "\n*** heap-corrupt write 0x%05lX=0x%04X enter#%ld; call path:\n",
                (unsigned long)a, val, g_enter_n);
        for (int i = 0; i < 24; i++) {
            int idx = (g_enter_ring_pos - 1 - i) & 63;
            if (g_enter_ring[idx]) fprintf(stderr, "    res_%06lX\n", g_enter_ring[idx]);
        }
        _Exit(43);
    }
}

void recomp_enter(unsigned long addr)
{
    if (g_trace && (g_enter_n < 4000 || g_enter_n % 500 == 0)) {
        if (g_dbg_cpu && g_enter_n < 200) {
            CPU *c = g_dbg_cpu;
            fprintf(stderr, "E %06lX ax=%04X bx=%04X cx=%04X dx=%04X si=%04X "
                    "di=%04X bp=%04X ds=%04X es=%04X\n", addr, c->ax, c->bx,
                    c->cx, c->dx, c->si, c->di, c->bp, c->ds, c->es);
        } else {
            fprintf(stderr, "E %06lX\n", addr);
        }
    }
    g_enter_n++;
    g_last_enter = addr;
    g_enter_ring[g_enter_ring_pos++ & 63] = addr;
    if (getenv("BOLO_HEAP") && g_dbg_cpu) {
        CPU *c = g_dbg_cpu;
        if (addr == 0x0146DF)       /* QB init-routine dispatcher */
            fprintf(stderr, "[heap] init-dispatch res_0146DF enter#%ld ds=%04X si=%04X cnt=%04X DF=%d\n",
                    g_enter_n, c->ds, c->si, c->mem[(uint32_t)c->ds*16+c->si]|(c->mem[(uint32_t)c->ds*16+c->si+1]<<8),
                    (c->flags & FLAG_DF) ? 1 : 0);
        else if (addr == 0x0173FA)  /* block B: string-space bounds setup */
            fprintf(stderr, "[heap] block-B res_0173FA enter#%ld ds=%04X\n", g_enter_n, c->ds);
        else if (addr == 0x017D7C)  /* heap initializer */
            fprintf(stderr, "[heap] INIT res_017D7C enter#%ld ds=%04X cx=%04X\n",
                    g_enter_n, c->ds, c->cx);
        else if (addr == 0x017B81) { /* string GC */
            static int gc_logged = 0;
            if (!gc_logged++) {
                fprintf(stderr, "[heap] GC res_017B81 enter#%ld ds=%04X head[0x4846]=%04X; "
                        "call path (newest first):\n", g_enter_n, c->ds,
                        c->mem[(uint32_t)c->ds*16+0x4846] | (c->mem[(uint32_t)c->ds*16+0x4847]<<8));
                for (int i = 0; i < 30; i++) {
                    int idx = (g_enter_ring_pos - 1 - i) & 63;
                    if (g_enter_ring[idx]) fprintf(stderr, "    res_%06lX\n", g_enter_ring[idx]);
                }
            }
        }
    }
    maybe_fire_timer();
    if (g_enter_n == 3000000) {
        ega_dump("work/ega_planes.bin");
        if (g_dbg_cpu) {                        /* also dump text screen (B800) */
            FILE *t = fopen("work/text.bin", "wb");
            if (t) { fwrite(&g_dbg_cpu->mem[0xB8000], 1, 4000, t); fclose(t); }
        }
        fprintf(stderr, "[stats] dispatch calls=%ld misses=%ld\n",
                g_dispatch_calls, g_dispatch_misses);
        fprintf(stderr, "[timer] fires=%ld  ivt1C=%08X ivt08=%08X ivt09=%08X\n",
                g_timer_fires, dos_get_vector(0x1C), dos_get_vector(0x08),
                dos_get_vector(0x09));
        fprintf(stderr, "[loop] last dispatched image offsets:\n");
        for (int i = 0; i < 12; i++) {
            int idx = (g_ring_pos - 1 - i) & 31;
            if (g_ring[idx]) fprintf(stderr, "    res_%06lX\n", g_ring[idx]);
        }
        if (g_dbg_cpu)
            fprintf(stderr, "[regs] ds=%04X es=%04X cs=%04X\n",
                    g_dbg_cpu->ds, g_dbg_cpu->es, g_dbg_cpu->cs);
        _Exit(0);
    }
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
