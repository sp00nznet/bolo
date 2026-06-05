/* Bolo Adventures III - static recomp entry point.
 *
 * Loads the decompressed BOLO3 load image into the recomp16 flat memory at
 * linear base 0 (so the image's unrelocated segment values map 1:1 to image
 * offsets, matching the lifted call graph), seeds the CPU registers from the
 * PKLITE register footer, and dispatches to the program entry.
 *
 * STATUS (Phase 3): segment-aware discovery lifts 932 functions (the whole
 * in-image call graph, including the QuickBASIC/CRT startup at CS:IP=2011:0010
 * / image 0x20120 dispatched below). Only 14 out-of-image far targets remain
 * stubbed. Bring-up now runs the startup until it reaches an unhandled x87 FPU
 * op or an indirect-dispatch site (both still emitted as comments) -- those are
 * the next gaps to close. See docs/PLAN.md Phase 3.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "recomp/gen/bolo_recomp.h"

/* program entry, from the PKLITE footer */
#define ENTRY_CS 0x2011
#define ENTRY_IP 0x0010
#define ENTRY_SS 0x2348
#define ENTRY_SP 0x0080

typedef struct { unsigned long addr; void (*fn)(CPU*); } dispatch_t;
extern const dispatch_t g_dispatch[];
extern const int g_dispatch_count;

static void (*lookup(unsigned long addr))(CPU*)
{
    for (int i = 0; i < g_dispatch_count; i++)
        if (g_dispatch[i].addr == addr) return g_dispatch[i].fn;
    return NULL;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "work/BOLO3_image.bin";
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open image: %s\n", path); return 1; }

    static CPU cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.mem = calloc(MEM_SIZE, 1);
    if (!cpu.mem) { fprintf(stderr, "out of memory\n"); return 1; }

    /* load image at linear base 0 */
    long n = fread(cpu.mem, 1, MEM_SIZE, f);
    fclose(f);
    fprintf(stderr, "loaded %ld bytes at linear 0\n", n);

    cpu.cs = ENTRY_CS; cpu.ip = ENTRY_IP;
    cpu.ss = ENTRY_SS; cpu.sp = ENTRY_SP;
    cpu.ds = 0; cpu.es = 0;

    unsigned long entry = (unsigned long)ENTRY_CS * 16 + ENTRY_IP;
    void (*fn)(CPU*) = lookup(entry);
    if (!fn) {
        fprintf(stderr,
            "entry 0x%05lX not in lifted set yet (QB/CRT startup unlifted).\n"
            "Phase 3 next: lift the startup + runtime call targets.\n", entry);
        return 2;
    }
    fn(&cpu);
    return 0;
}
