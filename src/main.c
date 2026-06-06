/* Bolo Adventures III - static recomp entry point.
 *
 * Sets up a DOS-like load image in the flat recomp16 memory and dispatches to
 * the program entry. Layout mirrors how DOS loads an EXE:
 *
 *   linear 0x000 : PSP (segment 0)
 *   linear 0x100 : load module (segment 0x10) == image offset 0
 *
 * The decompressor wrote the load module at PSP:0x100, so image offset 0 lives
 * at linear 0x100 and the load-module segment is 0x10. The PKLITE footer's
 * CS/SS are load-module-relative, so absolute CS = 0x10 + 0x2011, etc.
 * g_load_base tells the dispatcher that image offset 0 == linear 0x100.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "recomp/gen/bolo_recomp.h"

extern unsigned long g_load_base;
extern int g_trace;
extern long g_dispatch_calls, g_dispatch_misses;

#define LOAD_BASE 0x100          /* image offset 0 lives here (PSP:0x100)     */
#define LM_SEG    0x10           /* load-module segment (PSP at segment 0)    */
#define ENTRY_CS  (LM_SEG + 0x2011)
#define ENTRY_IP  0x0010
#define ENTRY_SS  (LM_SEG + 0x2348)
#define ENTRY_SP  0x0080

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "work/BOLO3_image.bin";
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open image: %s\n", path); return 1; }

    static CPU cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.mem = calloc(MEM_SIZE, 1);
    if (!cpu.mem) { fprintf(stderr, "out of memory\n"); return 1; }

    long n = fread(cpu.mem + LOAD_BASE, 1, MEM_SIZE - LOAD_BASE, f);
    fclose(f);

    /* minimal PSP at segment 0 */
    cpu.mem[0] = 0xCD; cpu.mem[1] = 0x20;            /* INT 20h terminate     */
    cpu.mem[2] = 0x00; cpu.mem[3] = 0xA0;            /* top of memory = 0xA000 */

    g_load_base = LOAD_BASE;
    g_trace = getenv("BOLO_TRACE") ? 1 : 0;

    cpu.cs = ENTRY_CS; cpu.ip = ENTRY_IP;
    cpu.ss = ENTRY_SS; cpu.sp = ENTRY_SP;
    cpu.ds = 0; cpu.es = 0; cpu.ax = 0;

    fprintf(stderr, "loaded %ld bytes at linear 0x%X; entry %04X:%04X\n",
            n, LOAD_BASE, ENTRY_CS, ENTRY_IP);

    recomp_dispatch(&cpu, ENTRY_CS, ENTRY_IP);

    fprintf(stderr, "entry returned. dispatch: %ld calls, %ld misses, halted=%d\n",
            g_dispatch_calls, g_dispatch_misses, cpu.halted);
    return 0;
}
