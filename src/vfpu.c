/* psprecomp — the VFPU. See include/psprecomp/vfpu.h. */

#include "psprecomp/vfpu.h"
#include "psprecomp/recomp_rt.h"

#include <math.h>

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

/* The matrix ops do not apply prefixes. A pending one makes them trap rather
 * than compute a number that ignores it; WTF never prefixes a matrix op. */
static int take_prefixes(uint32_t addr, const char *what) {
    if (!psp_vfpu_prefix_pending()) return 1;
    memset(g_prefix_set, 0, sizeof g_prefix_set);
    psp_vfpu_unimplemented(addr, what);
    return 0;
}

/* Every vector op consumes the prefixes, whether or not they were set. */
static void end_op(void) { memset(g_prefix_set, 0, sizeof g_prefix_set); }

/* ---- register addressing ------------------------------------------------- */

/* All four lanes from a register's first lane on, in its direction. A prefix
 * swizzle can name lanes past the operand's size, so a pair or a single still
 * has four. */
static void lanes4(uint32_t vreg, int size, int out[4]) {
    const int mtx = (vreg >> 2) & 7;
    const int col = vreg & 3;
    int transpose = (vreg >> 5) & 1, row;

    switch (size) {
    case 1:  row = (vreg >> 5) & 3; transpose = 0; break;
    case 3:  row = (vreg >> 6) & 1;                break;
    default: row = (vreg >> 5) & 2;                break;
    }
    for (int i = 0; i < 4; i++) {
        /* Transposed access walks columns instead of rows -- the same storage
         * seen the other way round, which is what makes a matrix transpose
         * free on this hardware. */
        const int step = (row + i) & 3;
        out[i] = transpose ? mtx * 4 + step * 32 + col
                           : mtx * 4 + col  * 32 + step;
    }
}

int psp_vfpu_regs(uint32_t vreg, int size, int out[4]) {
    int l[4];
    const int len = size >= 1 && size <= 4 ? size : 4;
    lanes4(vreg, len, l);
    for (int i = 0; i < len; i++) out[i] = l[i];
    return len;
}

/* ---- prefixes -------------------------------------------------------------
 *
 * vpfxs / vpfxt rewrite each source lane i (bits for lane i):
 *   swizzle  1..0 + 2i   which lane of the register to read
 *   abs      8 + i       take its absolute value
 *   const    12 + i      ignore the register: a constant, indexed by
 *                        swizzle | abs << 2, from PFX_CONST
 *   negate   16 + i
 * vpfxd post-processes each destination lane:
 *   saturate 1..0 + 2i   1 clamps to [0, 1], 3 to [-1, 1]
 *   mask     8 + i       do not write the lane at all
 *
 * WTF uses the source forms to broadcast a scalar (`vpfxt [x,x,x,x]` before a
 * clamp's vmin/vmax), to build box corners (`vpfxs [-w,-w,-w,w]` on vmov),
 * and to compare against |x| with a constant 0 lane. */
static const float PFX_CONST[8] = {
    0.0f, 1.0f, 2.0f, 0.5f, 3.0f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f
};

static float sat0(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static float sat1(float v) { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); }

/* Read an operand through prefix `which` (0 = s, 1 = t). */
static void read_src(int which, uint32_t vreg, int size, float out[4]) {
    int l[4];
    lanes4(vreg, size, l);
    if (!g_prefix_set[which]) {
        for (int i = 0; i < size; i++) out[i] = psp_cpu.v[l[i]];
        return;
    }
    const uint32_t p = g_prefix[which];
    for (int i = 0; i < size; i++) {
        const int sw = (int)(p >> (2 * i)) & 3;
        const int ab = (int)(p >> (8 + i)) & 1;
        const int k  = (int)(p >> (12 + i)) & 1;
        float x = k ? PFX_CONST[sw | (ab << 2)] : psp_cpu.v[l[sw]];
        if (!k && ab) x = fabsf(x);
        if ((p >> (16 + i)) & 1) x = -x;
        out[i] = x;
    }
}

/* Write a result through vpfxd. Called only after every source is read, since
 * the destination may alias a source. */
static void write_dst(uint32_t vreg, int size, const float in[4]) {
    int l[4];
    lanes4(vreg, size, l);
    const uint32_t p = g_prefix_set[2] ? g_prefix[2] : 0;
    for (int i = 0; i < size; i++) {
        if ((p >> (8 + i)) & 1) continue;
        float x = in[i];
        switch ((p >> (2 * i)) & 3) {
        case 1: x = sat0(x); break;
        case 3: x = sat1(x); break;
        default: break;
        }
        psp_cpu.v[l[i]] = x;
    }
}

/* ---- moves between the integer and vector files --------------------------
 *
 * mfv/mtv name a single lane with the low 7 bits; with bit 7 set they are
 * mfvc/mtvc and name a control register instead: 0-2 the prefixes, 3 the
 * condition codes that vcmp writes (WTF reads them this way after vcmp). */
uint32_t psp_mfv(uint32_t reg) {
    if (reg & 0x80) {
        switch (reg & 0x7F) {
        case 0: case 1: case 2: return g_prefix[reg & 3];
        case 3:  return psp_cpu.vfpu_cc;
        default: return 0;
        }
    }
    int l[4];
    lanes4(reg & 0x7F, 1, l);
    return psp_f32_to_bits(psp_cpu.v[l[0]]);
}

void psp_mtv(uint32_t reg, uint32_t bits) {
    if (reg & 0x80) {
        switch (reg & 0x7F) {
        case 0: case 1: case 2: psp_vfpu_set_prefix((int)(reg & 3), bits & 0xFFFFFu); break;
        case 3:  psp_cpu.vfpu_cc = bits & 0x3F; break;
        default: break;
        }
        return;
    }
    int l[4];
    lanes4(reg & 0x7F, 1, l);
    psp_cpu.v[l[0]] = psp_bits_to_f32(bits);
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
        float s[4], t[4], out[4];                                            \
        read_src(0, vs, size, s);                                            \
        read_src(1, vt, size, t);                                            \
        for (int i = 0; i < size; i++) {                                     \
            const float a = s[i], b = t[i];                                  \
            out[i] = (expr);                                                 \
        }                                                                    \
        write_dst(vd, size, out);                                            \
        end_op();                                                            \
    }

BINOP(vadd, a + b)
BINOP(vsub, a - b)
BINOP(vmul, a * b)
BINOP(vdiv, a / b)
BINOP(vmin, a < b ? a : b)
BINOP(vmax, a > b ? a : b)
BINOP(vsge, a >= b ? 1.0f : 0.0f)
BINOP(vslt, a < b ? 1.0f : 0.0f)
BINOP(vscmp, a < b ? -1.0f : (a > b ? 1.0f : 0.0f))

/* Dot product: sums all lanes into a single destination lane. */
void psp_vdot(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    float s[4], t[4], out[4] = { 0 };
    read_src(0, vs, size, s);
    read_src(1, vt, size, t);
    for (int i = 0; i < size; i++) out[0] += s[i] * t[i];
    write_dst(vd, 1, out);
    end_op();
}

/* Homogeneous dot: the last lane of vs counts as 1, so a point (x, y, z, 1)
 * dotted with a plane (a, b, c, d) needs no fourth component stored. */
void psp_vhdp(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    float s[4], t[4], out[4] = { 0 };
    read_src(0, vs, size, s);
    read_src(1, vt, size, t);
    for (int i = 0; i < size - 1; i++) out[0] += s[i] * t[i];
    out[0] += t[size - 1];
    write_dst(vd, 1, out);
    end_op();
}

/* Scale: every lane of vs multiplied by the scalar in vt. */
void psp_vscl(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    float s[4], t[4], out[4];
    read_src(0, vs, size, s);
    read_src(1, vt, 1, t);
    for (int i = 0; i < size; i++) out[i] = s[i] * t[0];
    write_dst(vd, size, out);
    end_op();
}

/* vcrs.t is half a cross product, (s.y t.z, s.z t.x, s.x t.y); a game
 * subtracts the other half itself. vcrsp.t is the whole one, and at quad size
 * the same encoding is vqmul, the quaternion product. */
void psp_vcrs(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    float s[4], t[4], out[4];
    (void)size;
    read_src(0, vs, 3, s);
    read_src(1, vt, 3, t);
    out[0] = s[1] * t[2];
    out[1] = s[2] * t[0];
    out[2] = s[0] * t[1];
    write_dst(vd, 3, out);
    end_op();
}

void psp_vcrsp(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    float s[4], t[4], out[4];
    if (size == 4) {
        read_src(0, vs, 4, s);
        read_src(1, vt, 4, t);
        out[0] = s[0] * t[3] + s[1] * t[2] - s[2] * t[1] + s[3] * t[0];
        out[1] = -s[0] * t[2] + s[1] * t[3] + s[2] * t[0] + s[3] * t[1];
        out[2] = s[0] * t[1] - s[1] * t[0] + s[2] * t[3] + s[3] * t[2];
        out[3] = -s[0] * t[0] - s[1] * t[1] - s[2] * t[2] + s[3] * t[3];
        write_dst(vd, 4, out);
    } else {
        read_src(0, vs, 3, s);
        read_src(1, vt, 3, t);
        out[0] = s[1] * t[2] - s[2] * t[1];
        out[1] = s[2] * t[0] - s[0] * t[2];
        out[2] = s[0] * t[1] - s[1] * t[0];
        write_dst(vd, 3, out);
    }
    end_op();
}

/* 2x2 determinant of the pairs vs and vt, into one lane. */
void psp_vdet(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    float s[4], t[4], out[4] = { 0 };
    (void)size;
    read_src(0, vs, 2, s);
    read_src(1, vt, 2, t);
    out[0] = s[0] * t[1] - s[1] * t[0];
    write_dst(vd, 1, out);
    end_op();
}

/* ---- unary element-wise ops (VFPU4) -------------------------------------- */

/* Round half to even, without depending on the host's rounding mode. */
static double round_even(double v) {
    double r = floor(v);
    const double frac = v - r;                   /* exact for |v| < 2^52 */
    if (frac > 0.5 || (frac == 0.5 && fmod(r, 2.0) != 0.0)) r += 1.0;
    return r;
}

/* The conversions, as measured on a PSP-3000 (pspautotests cpu/vfpu/convert
 * and convert_scaled). vf2in/vf2iz/vf2iu/vf2id round x * 2^scale to nearest
 * (ties to even), toward zero, up, and down, then saturate to int32; NaN of
 * either sign gives 0x7FFFFFFF. A plain C cast would be undefined for all
 * three of those edges. */
static uint32_t f2i(float x, int scale, int op) {
    if (x != x) return 0x7FFFFFFFu;
    const double v = ldexp((double)x, scale);    /* exact: float * 2^31 fits a double */
    double r;
    switch (op) {
    case PSP_VU_F2IN: r = round_even(v); break;
    case PSP_VU_F2IU: r = ceil(v);       break;
    case PSP_VU_F2ID: r = floor(v);      break;
    default:          r = trunc(v);      break;   /* PSP_VU_F2IZ */
    }
    if (r >= 2147483648.0)  return 0x7FFFFFFFu;
    if (r <= -2147483648.0) return 0x80000000u;
    return (uint32_t)(int32_t)r;                 /* an in-range integer: exact */
}

/* vi2f: int * 2^-scale, rounded to float once. */
static float i2f(uint32_t bits, int scale) {
    return (float)ldexp((double)(int32_t)bits, -scale);
}

void psp_vunary(int op, uint32_t vd, uint32_t vs, int size) {
    float s[4], out[4];
    read_src(0, vs, size, s);

    for (int i = 0; i < size; i++) {
        const float a = s[i];
        float r;
        switch (op) {
        case PSP_VU_MOV:  r = a;            break;
        case PSP_VU_ABS:  r = a < 0 ? -a : a; break;
        case PSP_VU_NEG:  r = -a;           break;
        case PSP_VU_ZERO: r = 0.0f;         break;
        case PSP_VU_ONE:  r = 1.0f;         break;
        case PSP_VU_RCP:  r = 1.0f / a;     break;
        case PSP_VU_NRCP: r = -1.0f / a;    break;
        case PSP_VU_RSQ:  r = 1.0f / psp_fsqrt(a); break;
        case PSP_VU_SQRT: r = psp_fsqrt(a); break;
        /* The PSP's trig takes its argument in *quarter turns*: vsin(x) is
         * sin(x * pi/2), not sin(x). Treating it as radians gives a result
         * that is smooth, plausible, and wrong -- rotations end up at the
         * wrong angle rather than visibly broken. */
        case PSP_VU_SIN:  r = sinf(a * 1.5707963267948966f);  break;
        case PSP_VU_COS:  r = cosf(a * 1.5707963267948966f);  break;
        case PSP_VU_NSIN: r = -sinf(a * 1.5707963267948966f); break;
        case PSP_VU_ASIN: r = asinf(a) * 0.6366197723675814f; break;  /* 2/pi */
        case PSP_VU_EXP2: r = powf(2.0f, a);   break;
        case PSP_VU_REXP2:r = 1.0f / powf(2.0f, a); break;
        case PSP_VU_LOG2: r = logf(a) * 1.4426950408889634f; break;   /* 1/ln2 */
        case PSP_VU_SAT0: r = sat0(a);      break;
        case PSP_VU_SAT1: r = sat1(a);      break;
        case PSP_VU_OCP:  r = 1.0f - a;     break;
        case PSP_VU_SGN:  r = a > 0 ? 1.0f : (a < 0 ? -1.0f : 0.0f); break;
        /* Conversions move between the float and integer *interpretations* of
         * a vector register; the bits are reinterpreted, not just cast. */
        case PSP_VU_F2IZ: r = psp_bits_to_f32(f2i(a, 0, op)); break;
        case PSP_VU_I2F:  r = i2f(psp_f32_to_bits(a), 0);     break;
        default:          psp_vfpu_unimplemented(psp_cpu.pc, "vunary"); end_op(); return;
        }
        out[i] = r;
    }
    write_dst(vd, size, out);
    end_op();
}

void psp_vcvt(int op, uint32_t vd, uint32_t vs, int size, int scale) {
    float s[4], out[4];
    read_src(0, vs, size, s);
    for (int i = 0; i < size; i++) {
        const float a = s[i];
        switch (op) {
        case PSP_VU_F2IN: case PSP_VU_F2IZ: case PSP_VU_F2IU: case PSP_VU_F2ID:
                          out[i] = psp_bits_to_f32(f2i(a, scale, op)); break;
        case PSP_VU_I2F:  out[i] = i2f(psp_f32_to_bits(a), scale);    break;
        default:          psp_vfpu_unimplemented(psp_cpu.pc, "vcvt"); end_op(); return;
        }
    }
    write_dst(vd, size, out);
    end_op();
}

/* Packing and unpacking integer lanes (VFPU7). These work on the integer bit
 * patterns the vf2i conversions leave in the registers, and change the lane
 * count: vi2uc.q packs four lanes into the bytes of one, vs2i.p unpacks the
 * halfwords of two lanes into four. The narrowing forms keep the top bits of
 * each value, and the unsigned ones clamp negatives to zero.
 *
 * WTF's colour helper at 0x318D8 is `vsat0; vscl 255*2^23; vf2iz; vi2uc; mfv`:
 * floats in [0, 1] to a packed 8888 colour. Unimplemented, it returned whatever
 * $v0 already held, and every menu glyph was drawn in a "colour" that was the
 * display-list pointer, alpha 9 -- below the alpha test, so no text. */
void psp_vpack(int op, uint32_t vd, uint32_t vs, int size) {
    int sl[4], dl[4];
    uint32_t in[4], out[4] = { 0 };
    int nout;
    lanes4(vs, size, sl);
    for (int i = 0; i < size; i++) in[i] = psp_f32_to_bits(psp_cpu.v[sl[i]]);

    switch (op) {
    case PSP_VP_I2UC: case PSP_VP_I2C:            /* quad -> single */
        for (int i = 0; i < 4; i++) {
            const int32_t v = (int32_t)in[i];
            const uint32_t b = op == PSP_VP_I2UC ? (v < 0 ? 0u : ((uint32_t)v >> 23) & 0xFF)
                                                 : ((uint32_t)v >> 24) & 0xFF;
            out[0] |= b << (8 * i);
        }
        nout = 1;
        break;
    case PSP_VP_I2US: case PSP_VP_I2S:            /* pairs of lanes -> one */
        nout = size / 2;
        for (int i = 0; i < nout * 2; i++) {
            const int32_t v = (int32_t)in[i];
            const uint32_t h = op == PSP_VP_I2US ? (v < 0 ? 0u : ((uint32_t)v >> 15) & 0xFFFF)
                                                 : ((uint32_t)v >> 16) & 0xFFFF;
            out[i / 2] |= h << (16 * (i & 1));
        }
        break;
    case PSP_VP_UC2I: case PSP_VP_C2I:            /* single -> quad */
        for (int i = 0; i < 4; i++) {
            const uint32_t b = (in[0] >> (8 * i)) & 0xFF;
            out[i] = op == PSP_VP_UC2I ? (b * 0x01010101u) >> 1 : b << 24;
        }
        nout = 4;
        break;
    default:                                      /* US2I, S2I: one lane -> two */
        nout = size * 2;
        if (nout > 4) nout = 4;
        for (int i = 0; i < nout; i++) {
            const uint32_t h = (in[i / 2] >> (16 * (i & 1))) & 0xFFFF;
            out[i] = op == PSP_VP_US2I ? h << 15 : h << 16;
        }
        break;
    }
    if (nout < 1) nout = 1;
    lanes4(vd, nout, dl);
    const uint32_t mask = g_prefix_set[2] ? g_prefix[2] : 0;
    for (int i = 0; i < nout; i++)
        if (!((mask >> (8 + i)) & 1)) psp_cpu.v[dl[i]] = psp_bits_to_f32(out[i]);
    end_op();
}

/* vrot: a row of a rotation matrix from one angle, in quarter turns. The
 * immediate places cos in lane imm[1:0] and sin in lane imm[3:2], negated if
 * imm[4]; other lanes are 0. If both name the same lane, cos goes there and
 * every other lane gets sin. WTF builds its rotation matrices this way: vmidt,
 * two vrot rows, vmmul. */
void psp_vrot(uint32_t vd, uint32_t vs, uint32_t imm, int size) {
    float a[4], out[4];
    read_src(0, vs, 1, a);
    const float ang = a[0] * 1.5707963267948966f;
    float sn = sinf(ang);
    const float cs = cosf(ang);
    if (imm & 0x10) sn = -sn;
    const int ci = (int)(imm & 3), si = (int)((imm >> 2) & 3);
    for (int i = 0; i < size; i++)
        out[i] = i == ci ? cs : (si == ci || i == si) ? sn : 0.0f;
    write_dst(vd, size, out);
    end_op();
}

/* ---- matrix ops without a multiply --------------------------------------- */

/* A matrix register names `size` consecutive columns, each a vector of `size`
 * lanes. Writing one column at a time through the same addressing the vector
 * ops use keeps the two consistent -- which matters because a game builds a
 * matrix with these and then transforms with vtfm. */
static void matrix_cols(uint32_t vd, int size, int cols[4][4]) {
    /* Build a fresh vector register per column: matrix, column index, row 0.
     * An earlier version modified bits 6:5 of the incoming register, which
     * varies the *row* field and so walked rows while claiming to walk
     * columns -- every matrix op addressed the wrong elements. */
    const uint32_t mtx       = (vd >> 2) & 7;
    const uint32_t transpose = (vd >> 5) & 1;
    for (int c = 0; c < size; c++) {
        uint32_t vreg = (mtx << 2) | (uint32_t)c | (transpose << 5);
        psp_vfpu_regs(vreg, size, cols[c]);
    }
}

/* Read a whole matrix out into [col][row] order. */
static void matrix_read(uint32_t v, int size, float m[4][4]) {
    int cols[4][4];
    matrix_cols(v, size, cols);
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) m[c][r] = psp_cpu.v[cols[c][r]];
}

static void matrix_write(uint32_t v, int size, const float m[4][4]) {
    int cols[4][4];
    matrix_cols(v, size, cols);
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) psp_cpu.v[cols[c][r]] = m[c][r];
}

void psp_vmidt(uint32_t vd, int size) {
    if (!take_prefixes(psp_cpu.pc, "vmidt")) return;
    int cols[4][4];
    matrix_cols(vd, size, cols);
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++)
            psp_cpu.v[cols[c][r]] = (c == r) ? 1.0f : 0.0f;
}

/* vidt -- an identity *vector*: all zeroes but for a single 1.0.
 *
 * The register names a column (or, transposed, a row) of a matrix, and vidt
 * writes that column of the identity: the 1 goes in the lane equal to the
 * register's column index, vd & 3 (vd & 1 for a pair). `vidt.q C730` is
 * (0, 0, 0, 1).
 *
 * WTF's VFPU setup at 0x315BC does exactly that and then relies on it: s733
 * (= 1.0) is the "1" in its quaternion-to-matrix routine's 1 - 2(y^2 + z^2),
 * and C730 is the w = 1 its frustum corners are built from. Bits 6-7, used
 * before, gave (1, 0, 0, 0), so every animated node came out with a zero 3x3
 * and the whole instructions screen collapsed to a point. */
void psp_vidt(uint32_t vd, int size) {
    float out[4];
    const int one = (int)(vd & (size == 2 ? 1u : 3u));
    for (int i = 0; i < size; i++) out[i] = (i == one) ? 1.0f : 0.0f;
    write_dst(vd, size, out);
    end_op();
}

/* vcst -- load a constant from the VFPU's built-in table.
 *
 * The index is in the vs field. Values follow the hardware table; index 0 is
 * zero and anything past the end reads as zero rather than as garbage. */
void psp_vcst(uint32_t vd, uint32_t which, int size) {
    static const float K[20] = {
        0.0f,
        3.4028235e38f,          /* max float          */
        1.41421356f,            /* sqrt(2)            */
        0.70710678f,            /* sqrt(1/2)          */
        1.12837917f,            /* 2/sqrt(pi)         */
        0.63661977f,            /* 2/pi               */
        0.31830989f,            /* 1/pi               */
        0.78539816f,            /* pi/4               */
        1.57079633f,            /* pi/2               */
        3.14159265f,            /* pi                 */
        2.71828183f,            /* e                  */
        1.44269504f,            /* log2(e)            */
        0.43429448f,            /* log10(e)           */
        0.69314718f,            /* ln(2)              */
        2.30258509f,            /* ln(10)             */
        6.28318531f,            /* 2*pi               */
        0.52359878f,            /* pi/6               */
        0.30103000f,            /* log10(2)           */
        3.32192809f,            /* log2(10)           */
        0.86602540f,            /* sqrt(3)/2          */
    };

    const float k = which < 20 ? K[which] : 0.0f;
    float out[4] = { k, k, k, k };
    write_dst(vd, size, out);
    end_op();
}

void psp_vmzero(uint32_t vd, int size) {
    if (!take_prefixes(psp_cpu.pc, "vmzero")) return;
    int cols[4][4];
    matrix_cols(vd, size, cols);
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) psp_cpu.v[cols[c][r]] = 0.0f;
}

void psp_vmone(uint32_t vd, int size) {
    if (!take_prefixes(psp_cpu.pc, "vmone")) return;
    int cols[4][4];
    matrix_cols(vd, size, cols);
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) psp_cpu.v[cols[c][r]] = 1.0f;
}

void psp_vmmov(uint32_t vd, uint32_t vs, int size) {
    if (!take_prefixes(psp_cpu.pc, "vmmov")) return;
    int dc[4][4], sc[4][4];
    matrix_cols(vd, size, dc);
    matrix_cols(vs, size, sc);
    float tmp[4][4];
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) tmp[c][r] = psp_cpu.v[sc[c][r]];
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) psp_cpu.v[dc[c][r]] = tmp[c][r];
}

/* ---- matrix multiply and transform --------------------------------------- */

/* Scale every element of a matrix by a scalar. Orientation-independent, so
 * this one is unambiguous. */
void psp_vmscl(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    if (!take_prefixes(psp_cpu.pc, "vmscl")) return;
    float m[4][4];
    int t[4];
    matrix_read(vs, size, m);
    psp_vfpu_regs(vt, 1, t);
    const float k = psp_cpu.v[t[0]];
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) m[c][r] *= k;
    matrix_write(vd, size, m);
}

/* Transform a vector by a matrix: vd[r] = sum over c of M[c][r] * v[c].
 *
 * This is the column-major product a graphics pipeline wants -- the matrix
 * columns are the transformed basis vectors, so transforming (1,0,0,0) yields
 * column 0. That property is what the test pins, and it is the one that
 * distinguishes this from its transpose. */
void psp_vtfm(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    if (!take_prefixes(psp_cpu.pc, "vtfm")) return;
    float m[4][4];
    int d[4], t[4];
    matrix_read(vs, size, m);
    psp_vfpu_regs(vt, size, t);
    psp_vfpu_regs(vd, size, d);

    float in[4], out[4];
    for (int i = 0; i < size; i++) in[i] = psp_cpu.v[t[i]];
    for (int r = 0; r < size; r++) {
        float sum = 0.0f;
        for (int c = 0; c < size; c++) sum += m[c][r] * in[c];
        out[r] = sum;
    }
    /* vd may be one of the sources, so write only after the whole result is
     * computed. */
    for (int r = 0; r < size; r++) psp_cpu.v[d[r]] = out[r];
}

/* Matrix product: Md = Ms^T * Mt. The first operand is used transposed, which
 * is why code writes `vmmul.q M000, E100, M200` (E = the transposed view) to
 * get M100 * M200.
 *
 * Verified against a game rather than assumed. WTF's matrix multiply at
 * 0x0002F494 is exactly that idiom, and with it the projection matrix reaches
 * the GE column-major, with its translation in elements 12-14, as the GE
 * expects. Without the transpose it arrived transposed, and the first
 * transformed vertex landed at (-83408, -47266). */
void psp_vmmul(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    if (!take_prefixes(psp_cpu.pc, "vmmul")) return;
    float a[4][4], b[4][4], out[4][4];
    matrix_read(vs, size, a);
    matrix_read(vt, size, b);

    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) {
            float sum = 0.0f;
            for (int k = 0; k < size; k++) sum += a[r][k] * b[c][k];
            out[c][r] = sum;
        }
    matrix_write(vd, size, out);
}

/* Compare, writing one condition bit per lane plus the any/all summary bits
 * that vcmov and the bvt/bvf branches read. Conditions 8-15 test vs alone:
 * zero, NaN, infinity, and NaN-or-infinity, then the same four negated. */
void psp_vcmp(uint32_t cond, uint32_t vs, uint32_t vt, int size) {
    float s[4], t[4];
    read_src(0, vs, size, s);
    read_src(1, vt, size, t);

    uint32_t cc = 0;
    int all = 1, any = 0;
    for (int i = 0; i < size; i++) {
        const float a = s[i], b = t[i];
        const int nan = a != a, inf = isinf(a) != 0;
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
        case 8:  r = (a == 0.0f);    break;   /* EZ  */
        case 9:  r = nan;            break;   /* EN  */
        case 10: r = inf;            break;   /* EI  */
        case 11: r = nan || inf;     break;   /* ES  */
        case 12: r = (a != 0.0f);    break;   /* NZ  */
        case 13: r = !nan;           break;   /* NN  */
        case 14: r = !inf;           break;   /* NI  */
        default: r = !(nan || inf);  break;   /* NS  */
        }
        if (r) { cc |= 1u << i; any = 1; } else { all = 0; }
    }
    if (any) cc |= 1u << 4;
    if (all) cc |= 1u << 5;
    psp_cpu.vfpu_cc = cc;
    end_op();
}

/* vcmovt / vcmovf -- move rs into rd, lane by lane, when the condition says so.
 *
 * The encoding is oooooooooooo c rrri sssssss j ddddddd: rcond at bit 19 picks
 * true (0) or false (1), cc_sel at bits 18..16 picks which VFPU_CC bit is
 * tested, and the two size bits are the usual non-adjacent pair. cc_sel 6 is
 * the useful special case: each lane tests its own bit, which is how a vcmp
 * result is turned into a per-lane select. 7 is unspecified, so it is treated
 * as "bit 7", which reads as zero. */
void psp_vcmov(uint32_t vd, uint32_t vs, int size, uint32_t cc_sel, int is_true) {
    if (!take_prefixes(psp_cpu.pc, is_true ? "vcmovt" : "vcmovf")) return;
    float s[4];
    int d[4];
    read_src(0, vs, size, s);
    psp_vfpu_regs(vd, size, d);
    for (int i = 0; i < size; i++) {
        const int bit = (cc_sel == 6) ? i : (int)cc_sel;
        const int cond = (int)((psp_cpu.vfpu_cc >> bit) & 1u);
        if (is_true ? cond : !cond) psp_cpu.v[d[i]] = s[i];
    }
    end_op();
}

/* viim / vfim -- write a single lane from an immediate encoded in the
 * instruction. No prefixes apply: there is no source operand to rewrite. */
void psp_vimm(uint32_t vd, float value) {
    int d[4];
    psp_vfpu_regs(vd, 1, d);
    psp_cpu.v[d[0]] = value;
}
