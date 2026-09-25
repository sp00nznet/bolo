/*
 * cpu.c - CPU state management for Civilization static recompilation
 *
 * Part of the Civ Recomp project (sp00nznet/civ)
 */

#include "recomp/cpu.h"
#include <stdlib.h>
#include <stdio.h>

int cpu_alloc_mem(CPU *cpu)
{
    cpu->mem = (uint8_t *)calloc(1, MEM_SIZE);
    if (!cpu->mem) {
        fprintf(stderr, "Error: failed to allocate %d bytes for CPU memory\n", MEM_SIZE);
        return -1;
    }
    return 0;
}

void cpu_free(CPU *cpu)
{
    if (cpu->mem) {
        free(cpu->mem);
        cpu->mem = NULL;
    }
}

int cpu_load(CPU *cpu, const char *path, uint16_t seg, uint16_t off)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open '%s'\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint32_t addr = seg_off(seg, off);
    if (addr + size > MEM_SIZE) {
        fprintf(stderr, "Error: binary too large for memory (addr=0x%X, size=%ld)\n",
                addr, size);
        fclose(f);
        return -1;
    }

    size_t read = fread(cpu->mem + addr, 1, size, f);
    fclose(f);

    if ((long)read != size) {
        fprintf(stderr, "Error: short read (%zu of %ld bytes)\n", read, size);
        return -1;
    }

    printf("Loaded %ld bytes at %04X:%04X (flat 0x%06X)\n", size, seg, off, addr);
    return 0;
}

/* Hardware raises INT 0 here and the DOS handler aborts the program. The game
 * never divides by zero on purpose, so reaching this means a lifted operand is
 * wrong -- say which op and keep going with AX/DX untouched, which is more
 * debuggable than dying. */
void recomp_div0(const char *what)
{
    static int seen = 0;
    if (seen++ < 16)
        fprintf(stderr, "[div0] %s: divide by zero (quotient left unchanged)\n", what);
}

/* ─── x87 (see cpu.h) ─── */
#include <math.h>

X87 g_x87 = { {0}, 0, 0, 0x037F };

static double f80_to_double(const uint8_t *b)
{
    uint64_t man = 0;
    for (int i = 7; i >= 0; i--) man = (man << 8) | b[i];
    int exp = (b[8] | (b[9] << 8)) & 0x7FFF, neg = b[9] & 0x80;
    double v;
    if (exp == 0 && man == 0) v = 0.0;
    else if (exp == 0x7FFF) v = (man << 1) ? NAN : INFINITY;
    else v = ldexp((double)man, exp - 16383 - 63);
    return neg ? -v : v;
}

static void double_to_f80(double v, uint8_t *b)
{
    int neg = signbit(v) != 0, exp = 0;
    uint64_t man = 0;
    v = fabs(v);
    if (isnan(v)) { exp = 0x7FFF; man = 0xC000000000000000ull; }
    else if (isinf(v)) { exp = 0x7FFF; man = 0x8000000000000000ull; }
    else if (v != 0.0) {
        int e; double m = frexp(v, &e);           /* v = m * 2^e, m in [0.5,1) */
        man = (uint64_t)ldexp(m, 64);             /* explicit integer bit at 63 */
        exp = e - 1 + 16383;
    }
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(man >> (8 * i));
    b[8] = (uint8_t)exp; b[9] = (uint8_t)((exp >> 8) | (neg ? 0x80 : 0));
}

double x87_rd(CPU *cpu, uint16_t seg, uint16_t off, int kind)
{
    uint8_t b[10];
    int n = kind < 0 ? -kind : kind;
    for (int i = 0; i < n; i++) b[i] = mem_read8(cpu, seg, (uint16_t)(off + i));
    switch (kind) {
    case 4:  { float f; memcpy(&f, b, 4); return f; }
    case 8:  { double d; memcpy(&d, b, 8); return d; }
    case 10: return f80_to_double(b);
    case -2: { int16_t i; memcpy(&i, b, 2); return i; }
    case -4: { int32_t i; memcpy(&i, b, 4); return i; }
    case -8: { int64_t i; memcpy(&i, b, 8); return (double)i; }
    }
    return 0.0;
}

/* integer store: round per the control word; out of range stores the
 * "integer indefinite" (most negative value), as the FPU does */
static int64_t x87_round(double v, int bits, int *ok)
{
    switch ((g_x87.cw >> 10) & 3) {
    case 0: v = nearbyint(v); break;     /* default rounding mode is nearest-even */
    case 1: v = floor(v); break;
    case 2: v = ceil(v); break;
    case 3: v = trunc(v); break;
    }
    double lim = ldexp(1.0, bits - 1);
    *ok = !isnan(v) && v >= -lim && v < lim;
    return *ok ? (int64_t)v : 0;
}

void x87_wr(CPU *cpu, uint16_t seg, uint16_t off, int kind, double v)
{
    uint8_t b[10];
    int n = kind < 0 ? -kind : kind, ok;
    switch (kind) {
    case 4:  { float f = (float)v; memcpy(b, &f, 4); break; }
    case 8:  memcpy(b, &v, 8); break;
    case 10: double_to_f80(v, b); break;
    default: {
        int64_t i = x87_round(v, n * 8, &ok);
        if (!ok) i = -((int64_t)1 << (n * 8 - 1));
        memcpy(b, &i, n);                   /* little-endian host: low bytes first */
        break;
    }
    }
    for (int i = 0; i < n; i++) mem_write8(cpu, seg, (uint16_t)(off + i), b[i]);
}

void x87_cmp(double a, double b)
{
    uint16_t c = isnan(a) || isnan(b) ? 0x4500 : a > b ? 0 : a < b ? 0x0100 : 0x4000;
    g_x87.sw = (uint16_t)((g_x87.sw & ~0x4500) | c);   /* C3=0x4000 C2=0x0400 C0=0x0100 */
}

uint16_t x87_sw(void) { return (uint16_t)((g_x87.sw & ~0x3800) | ((g_x87.top & 7) << 11)); }

void x87_arith(int op, double *dst, double src)
{
    switch (op) {
    case 0: *dst += src; break;
    case 1: *dst *= src; break;
    case 4: *dst -= src; break;
    case 5: *dst = src - *dst; break;
    case 6: *dst /= src; break;
    case 7: *dst = src / *dst; break;
    }
}

void x87_unhandled(const char *what)
{
    static int seen;
    if (seen++ < 32) fprintf(stderr, "[x87] unhandled: %s\n", what);
}

#ifdef X87_SELFTEST
/* gcc -DX87_SELFTEST -I src -I src/recomp src/recomp/cpu.c -lm && ./a.exe */
#include <assert.h>
uint32_t g_watch_addr;
void mem_watch_hit(uint32_t a, uint16_t v, int w) { (void)a; (void)v; (void)w; }
uint8_t ega_read8(uint32_t o) { (void)o; return 0; }
void ega_write8(uint32_t o, uint8_t v) { (void)o; (void)v; }
int main(void)
{
    static CPU c; static uint8_t mem[0x20000]; c.mem = mem;
    double t[] = { 0.0, 1.0, -2.5, 3.141592653589793, 1e-300, 6.02e23 };
    for (int i = 0; i < 6; i++) {
        x87_wr(&c, 0x100, 0, 10, t[i]);
        assert(x87_rd(&c, 0x100, 0, 10) == t[i]);
    }
    /* 1.0 as 80-bit: mantissa 8000000000000000, exponent 3FFF */
    x87_wr(&c, 0x100, 0, 10, 1.0);
    assert(mem[0x1007] == 0x80 && mem[0x1008] == 0xFF && mem[0x1009] == 0x3F);
    x87_wr(&c, 0x100, 0, -2, 2.5);  assert(x87_rd(&c, 0x100, 0, -2) == 2);   /* nearest-even */
    x87_wr(&c, 0x100, 0, -2, 3.5);  assert(x87_rd(&c, 0x100, 0, -2) == 4);
    x87_wr(&c, 0x100, 0, -2, 1e9);  assert(x87_rd(&c, 0x100, 0, -2) == -32768); /* indefinite */
    x87_cmp(1.0, 2.0); assert((g_x87.sw & 0x4500) == 0x0100);
    x87_cmp(2.0, 2.0); assert((g_x87.sw & 0x4500) == 0x4000);
    puts("x87 selftest ok");
    return 0;
}
#endif
