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
#include "recomp/dos_compat.h"

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
int  recomp_dispatch(CPU *cpu, uint16_t seg, uint16_t off);   /* fwd decl; 1 = dispatched */
extern uint32_t dos_get_vector(unsigned n);
extern void dos_tick(CPU *cpu);
static int g_in_timer = 0;
long g_timer_fires = 0;

/* Periodically fire the game's installed timer ISR (INT 1Ch / IRQ0 INT 8) so the
 * frame-timing wait loops advance. IRET pops a FLAGS/CS/IP frame (lift_bolo sets
 * iret_frame), so push one like the CPU would; SP is restored afterwards in case
 * the ISR chained somewhere that never IRETs. */
static void fire_isr(CPU *c, uint32_t v)
{
    uint16_t sp = c->sp;
    push16(c, (uint16_t)(c->flags | 0x0002)); push16(c, c->cs); push16(c, 0xFFFF);
    recomp_dispatch(c, (uint16_t)(v >> 16), (uint16_t)v);
    c->sp = sp;
}
/* IRQ0 at the PC's real 18.2 Hz, by wall clock. Checked from function entry and
 * from every loop back-edge (RECOMP_TICK), so a spin loop waiting on the tick
 * count still sees it move. Held off while IF is clear, as the PIC would. */
#include <windows.h>
static void maybe_fire_timer(void)
{
    static ULONGLONG next;
    if (g_host_pump) g_host_pump();
    if (g_in_timer || !g_dbg_cpu || !(g_dbg_cpu->flags & FLAG_IF) || g_deterministic > 0) return;
    ULONGLONG now = GetTickCount64();
    if (!next) next = now;
    if (now < next) return;
    next = (now - next > 500) ? now + 55 : next + 55;   /* after a stall, don't burst */
    g_in_timer = 1;
    dos_tick(g_dbg_cpu);
    uint32_t v = dos_get_vector(0x1C);
    if (v) { g_timer_fires++; fire_isr(g_dbg_cpu, v); }
    v = dos_get_vector(0x08);
    if (v) fire_isr(g_dbg_cpu, v);
    g_in_timer = 0;
}
void recomp_tick_now(void) { maybe_fire_timer(); }

/* An 8087-emulator INT (34h-3Dh), done as the CPU does it: push FLAGS, CS and
 * the real IP just past `CD nn`, clear IF, enter the handler the program
 * installed. QB's emulator reads the operand bytes at CS:IP and IRETs; the
 * lifted code then carries on past the whole instruction. */
void x87_emu_int(CPU *c, uint8_t n, uint16_t cs, uint16_t ip)
{
    uint32_t v = dos_get_vector(n);
    if (!v) { static int w; if (!w++) fprintf(stderr, "[x87] INT %02Xh has no handler\n", n); return; }
    push16(c, (uint16_t)(c->flags | 0x0002)); push16(c, cs); push16(c, ip);
    /* (IF left alone: the handler starts with sti anyway) */
    recomp_dispatch(c, (uint16_t)(v >> 16), (uint16_t)v);
}
void recomp_tick(void)
{
    static unsigned n;
    if ((++n & 1023) == 0) maybe_fire_timer();
}

void *g_miss_site = 0;          /* C return addr of the last dispatch call site */
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
    if (g_trace && (g_enter_n < 400000 || g_enter_n % 500 == 0)) {
        if (g_dbg_cpu && g_enter_n < 400000) {
            CPU *c = g_dbg_cpu;
            fprintf(stderr, "E %06lX ax=%04X bx=%04X cx=%04X dx=%04X si=%04X "
                    "di=%04X bp=%04X ds=%04X es=%04X sp=%04X\n", addr, c->ax, c->bx,
                    c->cx, c->dx, c->si, c->di, c->bp, c->ds, c->es, c->sp);
        } else {
            fprintf(stderr, "E %06lX\n", addr);
        }
    }
    g_enter_n++;
    g_last_enter = addr;
    g_enter_ring[g_enter_ring_pos++ & 63] = addr;
    if (getenv("BOLO_HEAP") && g_dbg_cpu) {
        CPU *c = g_dbg_cpu;
        if (addr == 0x013BBF) {     /* line drawer: dump endpoints + clip box */
            static int n=0;
            if (n++ < 4) {
                uint32_t b=(uint32_t)c->ds*16;
                fprintf(stderr, "[draw] res_013BBF enter#%ld ax=%04X bx=%04X cx=%04X dx=%04X | "
                    "clipbox[47EB..47F1]=%04X %04X %04X %04X\n", g_enter_n, c->ax,c->bx,c->cx,c->dx,
                    c->mem[b+0x47EB]|(c->mem[b+0x47EC]<<8), c->mem[b+0x47ED]|(c->mem[b+0x47EE]<<8),
                    c->mem[b+0x47EF]|(c->mem[b+0x47F0]<<8), c->mem[b+0x47F1]|(c->mem[b+0x47F2]<<8));
            }
        }
        else if (addr == 0x019AD2) { /* clip outcode: watch the point converge */
            static int n=0;
            if (n++ < 24)
                fprintf(stderr, "[clip] res_019AD2 #%d cx=%04X dx=%04X\n", n, c->cx, c->dx);
        }
        else if (addr == 0x0146DF)  /* QB init-routine dispatcher */
            fprintf(stderr, "[heap] init-dispatch res_0146DF enter#%ld ds=%04X si=%04X cnt=%04X DF=%d\n",
                    g_enter_n, c->ds, c->si, c->mem[(uint32_t)c->ds*16+c->si]|(c->mem[(uint32_t)c->ds*16+c->si+1]<<8),
                    (c->flags & FLAG_DF) ? 1 : 0);
        else if (addr == 0x0173FA)  /* block B: string-space bounds setup */
            fprintf(stderr, "[heap] block-B res_0173FA enter#%ld ds=%04X\n", g_enter_n, c->ds);
        else if (addr == 0x017E40)  /* string allocator (sets head=si) */
            fprintf(stderr, "[heap] alloc res_017E40 enter#%ld si=%04X head=%04X\n", g_enter_n,
                    c->si, c->mem[(uint32_t)c->ds*16+0x4846]|(c->mem[(uint32_t)c->ds*16+0x4847]<<8));
        else if (addr == 0x017E82)  /* sets head=di */
            fprintf(stderr, "[heap] res_017E82 enter#%ld di=%04X cx=%04X\n", g_enter_n, c->di, c->cx);
        else if (addr == 0x017D00)  /* heap walker (loops if head bad) */
            fprintf(stderr, "[heap] walker res_017D00 enter#%ld si=%04X head=%04X\n", g_enter_n,
                    c->si, c->mem[(uint32_t)c->ds*16+0x4846]|(c->mem[(uint32_t)c->ds*16+0x4847]<<8));
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
    recomp_tick();
    /* BOLO_STOPAT=N: dump state and stop after N function entries (debug;
     * this used to be hard-wired to 3,000,000 and ended every long run) */
    static long stop_at = -1;
    if (stop_at < 0) { const char *e = getenv("BOLO_STOPAT"); stop_at = e ? atol(e) : 0; }
    if (stop_at && g_enter_n == stop_at) {
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

int recomp_dispatch(CPU *cpu, uint16_t seg, uint16_t off)
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
        if (g_trace && g_dispatch_misses <= 200)
            fprintf(stderr, "[icall] miss %04X:%04X -> image 0x%05lX "
                    "(no lifted fn) from %06lX cx=%04X si=%04X sp=%04X\n",
                    seg, off, image_off, g_last_enter,
                    g_dbg_cpu ? g_dbg_cpu->cx : 0,
                    g_dbg_cpu ? g_dbg_cpu->si : 0,
                    g_dbg_cpu ? g_dbg_cpu->sp : 0);
        /* A miss storm means some loop is walking a table off its end and
         * calling data. g_last_enter pins the last function that ran, not the
         * loop -- the enter ring does. */
        if (g_dispatch_misses == 30) {
            fprintf(stderr, "*** miss storm; call site %p (addr2line it); "
                    "enter ring (newest first):\n", g_miss_site);
            for (int i = 0; i < 24; i++) {
                int idx = (g_enter_ring_pos - 1 - i) & 63;
                if (g_enter_ring[idx])
                    fprintf(stderr, "    res_%06lX\n", g_enter_ring[idx]);
            }
        }
        return 0;                    /* unresolved -> treat as plain return */
    }
    if (g_depth > 4096) {            /* runaway guard */
        if (g_trace) fprintf(stderr, "[icall] depth cap hit\n");
        return 0;
    }
    if (g_trace && g_dispatch_calls <= 60)
        fprintf(stderr, "[icall] %04X:%04X -> image 0x%05lX\n", seg, off, image_off);
    g_ring[g_ring_pos++ & 31] = image_off;
    g_depth++;
    fn(cpu);
    g_depth--;
    return 1;
}

/* An indirect FAR call site pushes cs + a 0xFFFF sentinel offset and expects
 * the callee's retf to pop them. If nothing runs at the target, nobody pops --
 * so unwind the 4-byte frame here, or the stack drifts by 4 on every miss and
 * the caller returns into rubbish.
 *
 * An offset of 0 is not a miss, it is an EMPTY SLOT. The QB runtime's routine
 * tables start zeroed and the real code skips a zero entry rather than calling
 * it; dispatching it anyway walks into whatever sits at the segment base. Same
 * reasoning the lifter uses to ignore a popped return offset of 0 -- a real
 * target points *after* a call, so it is never 0. */
void dispatch_far(CPU *cpu, uint16_t seg, uint16_t off)
{
    g_miss_site = __builtin_return_address(0);
    if (!off && !seg) { cpu->sp += 4; return; }
    if (!recomp_dispatch(cpu, seg, off)) cpu->sp += 4;
}


/* The near form pushes only the 2-byte return offset, and its table slot holds
 * the offset alone -- so a zero offset is the empty slot regardless of seg. */
void dispatch_near(CPU *cpu, uint16_t seg, uint16_t off)
{
    g_miss_site = __builtin_return_address(0);
    if (!off) { cpu->sp += 2; return; }
    if (!recomp_dispatch(cpu, seg, off)) cpu->sp += 2;
}
