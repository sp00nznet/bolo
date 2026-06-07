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
#include "hal/input.h"

/* scripted keystrokes fed when the game polls for input (so it advances past
 * prompts/menus and draws). One key pushed per poll. */
static const unsigned char g_keys[] = " \r \rS1\r \r ";
static int g_key_i = 0;
static void feed_keys(void *ctx, void *ds, const void *cpu)
{
    (void)ctx; (void)cpu;
    DosState *d = (DosState *)ds;
    unsigned char c = g_keys[g_key_i];
    if (c) g_key_i++; else c = ' ';      /* after the script, keep tapping space */
    keyboard_push(&d->keyboard, 0x39, c);
}

extern unsigned long g_load_base;
extern int g_trace;
extern long g_dispatch_calls, g_dispatch_misses;
extern unsigned long g_ring[32];
extern int g_ring_pos;
extern long g_enter_n;
extern unsigned long g_last_enter;
extern unsigned long g_enter_ring[64];
extern int g_enter_ring_pos;

#include <windows.h>
/* Watchdog: if the game wedges in an intra-function goto-loop (no recomp_enter,
 * so the enter-count dump never fires), this thread dumps the recent call path
 * after a timeout so we can find the looping function. Enable with BOLO_WATCHDOG. */
static DWORD WINAPI watchdog(LPVOID arg)
{
    DWORD secs = (DWORD)(size_t)arg;
    Sleep(secs * 1000);
    fprintf(stderr, "\n*** WATCHDOG fired after %lu s: %ld enters, "
            "last func res_%06lX\n", (unsigned long)secs, g_enter_n, g_last_enter);
    fprintf(stderr, "recent call path (newest first):\n");
    for (int i = 0; i < 24; i++) {
        int idx = (g_enter_ring_pos - 1 - i) & 63;
        if (g_enter_ring[idx]) fprintf(stderr, "    res_%06lX\n", g_enter_ring[idx]);
    }
    extern CPU *g_dbg_cpu;
    if (g_dbg_cpu) {
        CPU *c = g_dbg_cpu;
        fprintf(stderr, "regs ds=%04X es=%04X ss=%04X si=%04X di=%04X ax=%04X bx=%04X\n",
                c->ds, c->es, c->ss, c->si, c->di, c->ax, c->bx);
        unsigned long dbase = (unsigned long)c->ds * 16;
        fprintf(stderr, "ds:[0x4846]=%04X (heap head)\n",
                c->mem[dbase+0x4846] | (c->mem[dbase+0x4847]<<8));
        unsigned long sp = dbase + c->si;
        fprintf(stderr, "heap @ds:si-8 .. si+8: ");
        for (long o = -8; o <= 8; o++)
            fprintf(stderr, "%02X ", c->mem[sp+o]);
        fprintf(stderr, "\n  byte ds:[si]=%02X  word ds:[si-3]=%04X (block size)\n",
                c->mem[sp], c->mem[sp-3] | (c->mem[sp-2]<<8));
    }
    _Exit(42);
}

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
    dos.poll_events = feed_keys;               /* feed scripted keystrokes */
    extern CPU *g_dbg_cpu; g_dbg_cpu = &cpu;   /* for debug screen dumps */

    if (getenv("BOLO_WATCH")) {
        extern uint32_t g_watch_addr;
        g_watch_addr = (uint32_t)strtoul(getenv("BOLO_WATCH"), NULL, 0);
        fprintf(stderr, "watching writes to linear 0x%05lX\n", (unsigned long)g_watch_addr);
    }

    if (getenv("BOLO_WATCHDOG")) {
        DWORD secs = (DWORD)atoi(getenv("BOLO_WATCHDOG"));
        if (!secs) secs = 10;
        CreateThread(NULL, 0, watchdog, (LPVOID)(size_t)secs, 0, NULL);
    }

    fprintf(stderr, "dispatching real entry %04X:%04X (image 0x%05lX)\n",
            SNAP_ENTRY_SEG, SNAP_ENTRY_OFF,
            (unsigned long)SNAP_ENTRY_SEG * 16 + SNAP_ENTRY_OFF - SNAP_LOAD_BASE);

    recomp_dispatch(&cpu, SNAP_ENTRY_SEG, SNAP_ENTRY_OFF);

    fprintf(stderr, "entry returned. dispatch: %ld calls, %ld misses, halted=%d\n",
            g_dispatch_calls, g_dispatch_misses, cpu.halted);
    return 0;
}
