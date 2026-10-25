/* ftoltest: checks cmspeed's fast_ftol replacement against the game's own __ftol.
 *
 * Usage (under Wine): wine ftoltest.exe <path to cm0102.exe> [iterations]
 * Copies the original __ftol (0x945640, 39 bytes) out of the exe and the replacement from
 * src/ftol_fast.h into executable memory, then runs both on the same x87 inputs under every caller
 * rounding mode and precision control: random 80-bit bit patterns (including denormals, NaNs,
 * infinities and out-of-range values), random doubles and floats, and edge cases. Compares the
 * 64-bit result, that the caller's control word is preserved, and that ST0 was popped.
 * Exit status 0 = no differences. A deliberately broken replacement is run first as a control.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FTOL_RVA  0x545640
#define FTOL_LEN  39

#include "ftol_fast.h"

typedef unsigned long long u64;

struct result { u64 value; unsigned short cw; unsigned short sw_top; };

/* Load an 80-bit value, call fn (cdecl, value in ST0), record EDX:EAX, the control word and TOP. */
static struct result run(void *fn, const unsigned char *x80, unsigned short cw)
{
    struct result r;
    unsigned lo, hi;
    unsigned short cw_after, sw;
    __asm__ volatile(
        "fninit\n\t"
        "fldcw %[cw]\n\t"
        "fldt %[x]\n\t"
        "call *%[fn]\n\t"
        "fnstcw %[cwa]\n\t"
        "fnstsw %[sw]\n\t"
        : "=a"(lo), "=d"(hi), [cwa] "=m"(cw_after), [sw] "=m"(sw)
        : [fn] "r"(fn), [x] "m"(*(const unsigned char(*)[10])x80), [cw] "m"(cw)
        : "ecx", "memory", "cc");
    r.value = (u64)hi << 32 | lo;
    r.cw = cw_after;
    r.sw_top = (sw >> 11) & 7;
    return r;
}

static u64 rng_state = 0x9e3779b97f4a7c15ull;
static u64 rnd(void)
{
    rng_state ^= rng_state << 13, rng_state ^= rng_state >> 7, rng_state ^= rng_state << 17;
    return rng_state;
}

static void to80_double(double d, unsigned char *x80)
{
    long double ld = d;
    memcpy(x80, &ld, 10);
}

static void gen(unsigned char *x80, u64 i)
{
    static const double edge[] = {0.0, -0.0, 0.5, -0.5, 0.999999999, -0.999999999, 1.0, -1.0, 1.5, -1.5,
                                  2147483647.0, 2147483648.0, -2147483648.0, -2147483649.0,
                                  4294967295.5, 9.2233720368547748e18, -9.2233720368547758e18,
                                  9.2233720368547758e18, 1e300, -1e300, 1e-320};
    unsigned kind = i % 4;
    if (i < sizeof edge / sizeof *edge) {
        to80_double(edge[i], x80);
    } else if (kind == 0) { /* any 80-bit pattern */
        u64 m = rnd();
        unsigned short se = (unsigned short)rnd();
        memcpy(x80, &m, 8), memcpy(x80 + 8, &se, 2);
    } else if (kind == 1) { /* in-range values, the common case: |x| < 2^(0..63) */
        u64 m = rnd() | 0x8000000000000000ull;
        unsigned short se = (unsigned short)(16383 + rnd() % 64) | (rnd() & 1) << 15;
        memcpy(x80, &m, 8), memcpy(x80 + 8, &se, 2);
    } else if (kind == 2) { /* any double */
        u64 b = rnd();
        double d;
        memcpy(&d, &b, 8);
        to80_double(d, x80);
    } else { /* any float */
        unsigned b = (unsigned)rnd();
        float f;
        memcpy(&f, &b, 4);
        to80_double(f, x80);
    }
}

static void *exec_copy(const void *code, size_t len)
{
    void *p = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(p, code, len);
    return p;
}

static long compare(void *orig, void *repl, u64 n, int quiet)
{
    static const unsigned short cws[] = {0x027f, 0x037f, 0x007f, 0x0e7f, 0x047f, 0x087f, 0x0c7f, 0x0f7f, 0x003f};
    long bad = 0;
    for (u64 i = 0; i < n; i++) {
        unsigned char x[10];
        gen(x, i);
        unsigned short cw = cws[i % (sizeof cws / sizeof *cws)];
        struct result a = run(orig, x, cw), b = run(repl, x, cw);
        if (a.value != b.value || a.cw != b.cw || a.sw_top != b.sw_top) {
            if (!quiet && bad < 10)
                printf("diff: x=%02x%02x:%02x%02x%02x%02x%02x%02x%02x%02x cw=%04x orig=%016llx/%04x/%u "
                       "repl=%016llx/%04x/%u\n", x[9], x[8], x[7], x[6], x[5], x[4], x[3], x[2], x[1], x[0], cw,
                       a.value, a.cw, a.sw_top, b.value, b.cw, b.sw_top);
            bad++;
        }
    }
    return bad;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: ftoltest <cm0102.exe> [iterations]\n");
        return 2;
    }
    u64 n = argc > 2 ? strtoull(argv[2], NULL, 0) : 100000000ull;
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    /* .text's file offset equals its RVA in cm0102.exe (0x1000). */
    unsigned char orig_code[FTOL_LEN];
    fseek(f, FTOL_RVA, SEEK_SET);
    if (fread(orig_code, 1, FTOL_LEN, f) != FTOL_LEN || memcmp(orig_code, ftol_orig, FTOL_LEN)) {
        fprintf(stderr, "%s: __ftol bytes differ from cm0102.exe 3.9.68\n", argv[1]);
        return 2;
    }
    fclose(f);
    void *orig = exec_copy(orig_code, FTOL_LEN);
    void *repl = exec_copy(ftol_fast, sizeof ftol_fast);

    /* Control: rounding to nearest instead of truncating must be caught. */
    static const unsigned char broken[] = {0x83, 0xec, 0x08, 0xdf, 0x3c, 0x24, 0x8b, 0x04, 0x24,
                                           0x8b, 0x54, 0x24, 0x04, 0x83, 0xc4, 0x08, 0xc3};
    long ctrl = compare(orig, exec_copy(broken, sizeof broken), 100000, 1);
    printf("control (fistp, round-to-nearest): %ld/100000 differ%s\n", ctrl, ctrl ? "" : " -- TEST IS BROKEN");
    if (!ctrl)
        return 1;

    long bad = compare(orig, repl, n, 0);
    printf("fast_ftol: %ld/%llu inputs differ\n", bad, n);
    return bad != 0;
}
