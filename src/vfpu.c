/* psprecomp — the VFPU. See include/psprecomp/vfpu.h. */

#include "psprecomp/vfpu.h"

#include <stdio.h>
#include <string.h>

static uint32_t g_prefix[3];      /* vpfxs, vpfxt, vpfxd */
static int      g_prefix_set[3];
static uint64_t g_traps;

void psp_vfpu_reset(void) {
    memset(g_prefix, 0, sizeof g_prefix);
    memset(g_prefix_set, 0, sizeof g_prefix_set);
    g_traps = 0;
}

void psp_vfpu_set_prefix(int which, uint32_t value) {
    if (which < 0 || which > 2) return;
    g_prefix[which] = value;
    g_prefix_set[which] = 1;
}

int psp_vfpu_prefix_pending(void) {
    return g_prefix_set[0] || g_prefix_set[1] || g_prefix_set[2];
}

uint64_t psp_vfpu_trap_count(void) { return g_traps; }

void psp_vfpu_unimplemented(uint32_t addr, const char *what) {
    /* First few only: a VFPU-heavy inner loop would otherwise produce
     * megabytes of identical lines and hide everything else. */
    if (g_traps < 16)
        fprintf(stderr, "psprecomp: VFPU %s at 0x%08X not implemented\n", what, addr);
    else if (g_traps == 16)
        fprintf(stderr, "psprecomp: (further VFPU traps suppressed)\n");
    g_traps++;
}

/* Consume the pending prefixes. Returns 1 if it is safe to compute, 0 if a
 * prefix was pending -- in which case the caller must trap rather than produce
 * a number that ignores it. */
static int take_prefixes(uint32_t addr, const char *what) {
    if (!psp_vfpu_prefix_pending()) return 1;
    memset(g_prefix_set, 0, sizeof g_prefix_set);
    psp_vfpu_unimplemented(addr, what);
    return 0;
}

/* ---- register addressing ------------------------------------------------- */

int psp_vfpu_regs(uint32_t vreg, int size, int out[4]) {
    const int mtx       = (vreg >> 2) & 7;
    const int col       = vreg & 3;
    int transpose       = (vreg >> 5) & 1;
    int row = 0, len = 1;

    switch (size) {
    case 1: row = (vreg >> 5) & 3; transpose = 0; len = 1; break;
    case 2: row = (vreg >> 5) & 2;                len = 2; break;
    case 3: row = (vreg >> 6) & 1;                len = 3; break;
    default:row = (vreg >> 5) & 2;                len = 4; break;
    }

    for (int i = 0; i < len; i++) {
        /* Transposed access walks columns instead of rows -- the same storage
         * seen the other way round, which is what makes a matrix transpose
         * free on this hardware. */
        const int step = (row + i) & 3;
        out[i] = transpose ? mtx * 4 + step * 32 + col
                           : mtx * 4 + col  * 32 + step;
    }
    return len;
}

/* ---- load / store -------------------------------------------------------- */

void psp_lv_s(uint32_t vt, uint32_t addr) {
    int r[4];
    psp_vfpu_regs(vt, 1, r);
    psp_cpu.v[r[0]] = psp_read_f32(addr & ~3u);
}

void psp_sv_s(uint32_t vt, uint32_t addr) {
    int r[4];
    psp_vfpu_regs(vt, 1, r);
    psp_write_f32(addr & ~3u, psp_cpu.v[r[0]]);
}

void psp_lv_q(uint32_t vt, uint32_t addr) {
    int r[4];
    psp_vfpu_regs(vt, 4, r);
    addr &= ~15u;                       /* quad access is 16-byte aligned */
    for (int i = 0; i < 4; i++)
        psp_cpu.v[r[i]] = psp_read_f32(addr + (uint32_t)i * 4);
}

void psp_sv_q(uint32_t vt, uint32_t addr) {
    int r[4];
    psp_vfpu_regs(vt, 4, r);
    addr &= ~15u;
    for (int i = 0; i < 4; i++)
        psp_write_f32(addr + (uint32_t)i * 4, psp_cpu.v[r[i]]);
}

/* ---- arithmetic ---------------------------------------------------------- */

#define BINOP(name, expr)                                                    \
    void psp_##name(uint32_t vd, uint32_t vs, uint32_t vt, int size) {       \
        if (!take_prefixes(psp_cpu.pc, #name)) return;                       \
        int d[4], s[4], t[4];                                                \
        int n = psp_vfpu_regs(vd, size, d);                                  \
        psp_vfpu_regs(vs, size, s);                                          \
        psp_vfpu_regs(vt, size, t);                                          \
        /* Read every source before writing any destination: vd may alias vs \
         * or vt, and a lane-by-lane read/write would then feed results back  \
         * into later lanes. */                                              \
        float out[4];                                                        \
        for (int i = 0; i < n; i++) {                                        \
            float a = psp_cpu.v[s[i]], b = psp_cpu.v[t[i]];                  \
            out[i] = (expr);                                                 \
        }                                                                    \
        for (int i = 0; i < n; i++) psp_cpu.v[d[i]] = out[i];                \
    }

BINOP(vadd, a + b)
BINOP(vsub, a - b)
BINOP(vmul, a * b)
BINOP(vdiv, a / b)
BINOP(vmin, a < b ? a : b)
BINOP(vmax, a > b ? a : b)

/* Dot product: sums all lanes into a single destination lane. */
void psp_vdot(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    if (!take_prefixes(psp_cpu.pc, "vdot")) return;
    int d[4], s[4], t[4];
    psp_vfpu_regs(vd, 1, d);
    int n = psp_vfpu_regs(vs, size, s);
    psp_vfpu_regs(vt, size, t);

    float sum = 0.0f;
    for (int i = 0; i < n; i++) sum += psp_cpu.v[s[i]] * psp_cpu.v[t[i]];
    psp_cpu.v[d[0]] = sum;
}

/* Scale: every lane of vs multiplied by the scalar in vt. */
void psp_vscl(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    if (!take_prefixes(psp_cpu.pc, "vscl")) return;
    int d[4], s[4], t[4];
    int n = psp_vfpu_regs(vd, size, d);
    psp_vfpu_regs(vs, size, s);
    psp_vfpu_regs(vt, 1, t);

    const float k = psp_cpu.v[t[0]];
    float out[4];
    for (int i = 0; i < n; i++) out[i] = psp_cpu.v[s[i]] * k;
    for (int i = 0; i < n; i++) psp_cpu.v[d[i]] = out[i];
}

/* Compare, writing one condition bit per lane plus the any/all summary bits
 * that vcmov and the bvt/bvf branches read. */
void psp_vcmp(uint32_t cond, uint32_t vs, uint32_t vt, int size) {
    if (!take_prefixes(psp_cpu.pc, "vcmp")) return;
    int s[4], t[4];
    int n = psp_vfpu_regs(vs, size, s);
    psp_vfpu_regs(vt, size, t);

    uint32_t cc = 0;
    int all = 1, any = 0;
    for (int i = 0; i < n; i++) {
        float a = psp_cpu.v[s[i]], b = psp_cpu.v[t[i]];
        int r;
        switch (cond & 0xF) {
        case 0:  r = 0;              break;   /* FL  */
        case 1:  r = (a == b);       break;   /* EQ  */
        case 2:  r = (a <  b);       break;   /* LT  */
        case 3:  r = (a <= b);       break;   /* LE  */
        case 4:  r = 1;              break;   /* TR  */
        case 5:  r = (a != b);       break;   /* NE  */
        case 6:  r = (a >= b);       break;   /* GE  */
        case 7:  r = (a >  b);       break;   /* GT  */
        default: r = 0;              break;
        }
        if (r) { cc |= 1u << i; any = 1; } else { all = 0; }
    }
    if (any) cc |= 1u << 4;
    if (all) cc |= 1u << 5;
    psp_cpu.vfpu_cc = cc;
}
