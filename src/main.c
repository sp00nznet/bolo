/* Bolo Adventures III - static recomp entry point.
 *
 * The QuickBASIC self-relocating/decompressing startup (PKLITE stub + QB
 * __astart) is hard to run as static C, so we BYPASS it: tools/uni_original.py
 * runs the original packed EXE through PKLITE + the QB startup in an emulator and
 * snapshots the fully-decompressed/relocated memory + CPU state at the real
 * program entry. We load that snapshot here and dispatch straight to the entry,
 * so the lifted functions run on correct memory.
 *
 *   snapshot.bin       : full 1 MB+ image at runtime linear positions
 *   snapshot_regs.h    : post-startup CPU registers + entry + load base
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "cpu.h"
#include "recomp/gen/bolo_recomp.h"
#include "recomp/gen/snapshot_regs.h"

extern unsigned long g_load_base;
extern int g_trace;
extern long g_dispatch_calls, g_dispatch_misses;
extern unsigned long g_ring[32];
extern int g_ring_pos;

static void on_crash(int sig)
{
    fprintf(stderr, "\n*** CRASH (sig %d) after %ld dispatches. "
            "Last dispatched image offsets:\n", sig, g_dispatch_calls);
    for (int i = 0; i < 16; i++) {
        int idx = (g_ring_pos - 1 - i) & 31;
        if (g_ring[idx]) fprintf(stderr, "    res_%06lX\n", g_ring[idx]);
    }
    _Exit(139);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "work/snapshot.bin";
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open snapshot: %s\n", path); return 1; }

    static CPU cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.mem = calloc(MEM_SIZE, 1);
    if (!cpu.mem) { fprintf(stderr, "out of memory\n"); return 1; }

    long n = fread(cpu.mem, 1, MEM_SIZE, f);
    fclose(f);
    fprintf(stderr, "loaded snapshot %ld bytes\n", n);

    signal(SIGSEGV, on_crash);
    setvbuf(stderr, NULL, _IONBF, 0);   /* unbuffered so traces survive a crash */
    g_load_base = 0;     /* snapshot-lift keys functions by raw runtime linear */
    g_trace = getenv("BOLO_TRACE") ? 1 : 0;

    cpu.ax = SNAP_AX; cpu.bx = SNAP_BX; cpu.cx = SNAP_CX; cpu.dx = SNAP_DX;
    cpu.si = SNAP_SI; cpu.di = SNAP_DI; cpu.bp = SNAP_BP; cpu.sp = SNAP_SP;
    cpu.cs = SNAP_CS; cpu.ds = SNAP_DS; cpu.es = SNAP_ES; cpu.ss = SNAP_SS;
    cpu.ip = SNAP_IP;

    /* initialize the recomp16 DOS/BIOS runtime (sets g_dos, IVT, HAL, BIOS data
     * area). Without this, INT 21h handlers deref a NULL g_dos. */
    static DosState dos;
    dos_init(&dos, &cpu, "original");

    fprintf(stderr, "dispatching real entry %04X:%04X (image 0x%05lX)\n",
            SNAP_ENTRY_SEG, SNAP_ENTRY_OFF,
            (unsigned long)SNAP_ENTRY_SEG * 16 + SNAP_ENTRY_OFF - SNAP_LOAD_BASE);

    recomp_dispatch(&cpu, SNAP_ENTRY_SEG, SNAP_ENTRY_OFF);

    fprintf(stderr, "entry returned. dispatch: %ld calls, %ld misses, halted=%d\n",
            g_dispatch_calls, g_dispatch_misses, cpu.halted);
    return 0;
}
