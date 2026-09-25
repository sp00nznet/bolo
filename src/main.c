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
#include <SDL2/SDL.h>

/* BOLO_KEYS="..." feeds scripted keystrokes, one per input poll, for headless
 * runs; after the script it keeps tapping space (uni_original.py's UNI_KEYS
 * does the same, so traces stay comparable). `^U ^D ^L ^R` are the arrow keys,
 * `~` waits 150 ms of wall clock before the next key, a final `$` stops the
 * space filler. Without BOLO_KEYS input
 * is the keyboard, through the SDL window. */
static const char *g_keys;
static unsigned g_key_wait;
static void feed_keys(void *ctx, void *ds, const void *cpu)
{
    (void)ctx; (void)cpu;
    DosState *d = (DosState *)ds;
    static unsigned skip;                          /* replay mode: `~` = 20 polls */
    if (skip) { skip--; return; }
    if (g_key_wait && SDL_GetTicks() < g_key_wait) return;
    g_key_wait = 0;
    unsigned char c = (unsigned char)*g_keys;
    if (getenv("BOLO_KEYTRACE")) fprintf(stderr, "[key] t=%u c=%02X\n", (unsigned)SDL_GetTicks(), c);
    if (c == '$') return;                          /* end of script, no filler */
    if (c == '~') {
        g_keys++;
        if (g_deterministic > 0) skip = 19; else g_key_wait = SDL_GetTicks() + 150;
        return;
    }
    if (c == '^' && g_keys[1]) {                   /* arrows: scancode, ASCII 0 */
        const char *m = strchr("UDLR", g_keys[1]);
        static const uint8_t sc[] = { 0x48, 0x50, 0x4B, 0x4D };
        g_keys += 2;
        if (m) keyboard_push(&d->keyboard, sc[m - "UDLR"], 0);
        return;
    }
    if (c) g_keys++; else c = ' ';
    keyboard_push(&d->keyboard, c == '\r' ? 0x1C : 0x39, c);   /* Enter needs its scancode */
}

/* --- host window: 640x350 EGA, keyboard --- */
#include <SDL2/SDL.h>
#include "recomp/ega.h"
static SDL_Renderer *g_ren;
static SDL_Texture *g_tex;
static DosState *g_dosp;

static uint8_t sdl_to_scancode(SDL_Scancode sc)
{
    static const struct { SDL_Scancode s; uint8_t d; } m[] = {
        {SDL_SCANCODE_ESCAPE,0x01},{SDL_SCANCODE_1,0x02},{SDL_SCANCODE_2,0x03},
        {SDL_SCANCODE_3,0x04},{SDL_SCANCODE_4,0x05},{SDL_SCANCODE_5,0x06},
        {SDL_SCANCODE_6,0x07},{SDL_SCANCODE_7,0x08},{SDL_SCANCODE_8,0x09},
        {SDL_SCANCODE_9,0x0A},{SDL_SCANCODE_0,0x0B},{SDL_SCANCODE_BACKSPACE,0x0E},
        {SDL_SCANCODE_TAB,0x0F},{SDL_SCANCODE_Q,0x10},{SDL_SCANCODE_W,0x11},
        {SDL_SCANCODE_E,0x12},{SDL_SCANCODE_R,0x13},{SDL_SCANCODE_T,0x14},
        {SDL_SCANCODE_Y,0x15},{SDL_SCANCODE_U,0x16},{SDL_SCANCODE_I,0x17},
        {SDL_SCANCODE_O,0x18},{SDL_SCANCODE_P,0x19},{SDL_SCANCODE_RETURN,0x1C},
        {SDL_SCANCODE_A,0x1E},{SDL_SCANCODE_S,0x1F},{SDL_SCANCODE_D,0x20},
        {SDL_SCANCODE_F,0x21},{SDL_SCANCODE_G,0x22},{SDL_SCANCODE_H,0x23},
        {SDL_SCANCODE_J,0x24},{SDL_SCANCODE_K,0x25},{SDL_SCANCODE_L,0x26},
        {SDL_SCANCODE_Z,0x2C},{SDL_SCANCODE_X,0x2D},{SDL_SCANCODE_C,0x2E},
        {SDL_SCANCODE_V,0x2F},{SDL_SCANCODE_B,0x30},{SDL_SCANCODE_N,0x31},
        {SDL_SCANCODE_M,0x32},{SDL_SCANCODE_SPACE,0x39},{SDL_SCANCODE_F1,0x3B},
        {SDL_SCANCODE_F2,0x3C},{SDL_SCANCODE_F3,0x3D},{SDL_SCANCODE_F4,0x3E},
        {SDL_SCANCODE_F5,0x3F},{SDL_SCANCODE_F6,0x40},{SDL_SCANCODE_F7,0x41},
        {SDL_SCANCODE_F8,0x42},{SDL_SCANCODE_F9,0x43},{SDL_SCANCODE_F10,0x44},
        {SDL_SCANCODE_UP,0x48},{SDL_SCANCODE_LEFT,0x4B},{SDL_SCANCODE_RIGHT,0x4D},
        {SDL_SCANCODE_DOWN,0x50},
    };
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) if (m[i].s == sc) return m[i].d;
    return 0;
}

/* called from retrace polls, input polls and the timer tick; does real work
 * at most ~60 times a second */
/* BOLO_SHOT=N: save the displayed page to work/shot_NNN.bmp every N seconds */
static void maybe_screenshot(Uint32 now)
{
    static Uint32 every, next; static int n;
    if (!every) { const char *e = getenv("BOLO_SHOT"); every = e ? (Uint32)(atof(e) * 1000) : ~0u; next = every; }
    if (every == ~0u || now < next) return;
    next = now + every;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 640, 350, 32, SDL_PIXELFORMAT_ABGR8888);
    if (!s) return;
    ega_render((uint32_t *)s->pixels);
    char path[64]; snprintf(path, sizeof path, "work/shot_%03d.bmp", n++);
    SDL_SaveBMP(s, path);
    SDL_FreeSurface(s);
}

static void host_pump(void)
{
    static Uint32 last;
    Uint32 now = SDL_GetTicks();
    if (!g_tex || now - last < 16) return;
    last = now;
    maybe_screenshot(now);
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) _Exit(0);
        if (e.type == SDL_KEYDOWN) {
            SDL_Keycode k = e.key.keysym.sym;
            uint8_t sc = sdl_to_scancode(e.key.keysym.scancode), ascii = 0;
            if (k >= 32 && k < 127) {
                ascii = (uint8_t)k;
                if ((e.key.keysym.mod & KMOD_SHIFT) && k >= 'a' && k <= 'z') ascii -= 32;
            } else if (k == SDLK_RETURN) ascii = 13;
            else if (k == SDLK_ESCAPE) ascii = 27;
            else if (k == SDLK_BACKSPACE) ascii = 8;
            else if (k == SDLK_TAB) ascii = 9;
            if (sc || ascii) keyboard_push(&g_dosp->keyboard, sc, ascii);
        }
    }
    void *px; int pitch;
    if (SDL_LockTexture(g_tex, NULL, &px, &pitch) == 0) {
        ega_render((uint32_t *)px);            /* ABGR8888 640 wide: pitch is 2560 */
        SDL_UnlockTexture(g_tex);
    }
    SDL_RenderClear(g_ren);
    SDL_RenderCopy(g_ren, g_tex, NULL, NULL);
    SDL_RenderPresent(g_ren);
}
static void pump_hook(void *ctx, void *ds, const void *cpu) { (void)ctx; (void)ds; (void)cpu; host_pump(); }

static void host_init(DosState *dos)
{
    g_dosp = dos;
    if (SDL_Init(SDL_INIT_VIDEO) < 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return; }
    /* EGA 640x350 was shown on a 4:3 tube: stretch to 1280x960 */
    SDL_Window *w = SDL_CreateWindow("Bolo Adventures III", SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, 1280, 960, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    g_ren = w ? SDL_CreateRenderer(w, -1, 0) : NULL;
    if (!g_ren) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return; }
    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ABGR8888,
                              SDL_TEXTUREACCESS_STREAMING, 640, 350);
    g_host_pump = host_pump;
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
        fprintf(stderr, "regs ds=%04X es=%04X ss=%04X si=%04X di=%04X ax=%04X bx=%04X "
                "sp=%04X flags=%04X ticks=%04X\n",
                c->ds, c->es, c->ss, c->si, c->di, c->ax, c->bx, c->sp, c->flags,
                c->mem[0x46C] | (c->mem[0x46D] << 8));
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
    host_init(&dos);
    g_keys = getenv("BOLO_KEYS");
    dos.poll_events = g_keys ? feed_keys : pump_hook;
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
