/* The C emitter. See emit.h. */

#include "emit.h"
#include "decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Generated register names: r_a0, r_sp, ... The generated header #defines each
 * onto psp_cpu.r[N]. Naming them beats indexing because the emitted code then
 * reads like the assembly it came from. */
static const char *const RN[32] = {
    "r_zero","r_at","r_v0","r_v1","r_a0","r_a1","r_a2","r_a3",
    "r_t0","r_t1","r_t2","r_t3","r_t4","r_t5","r_t6","r_t7",
    "r_s0","r_s1","r_s2","r_s3","r_s4","r_s5","r_s6","r_s7",
    "r_t8","r_t9","r_k0","r_k1","r_gp","r_sp","r_fp","r_ra"
};

typedef struct {
    FILE *out;
    const a_analysis *an;
    const a_func *func;
    uint8_t *is_label;     /* per word, within the current function */
    uint8_t *is_slot;      /* per word: consumed as a delay slot */
} ectx;

/* ---- helpers ------------------------------------------------------------- */

static uint32_t widx(const a_analysis *an, uint32_t addr) {
    return (addr - an->base) >> 2;
}

static int owned_by(const a_analysis *an, uint32_t addr, uint32_t owner) {
    if (addr < an->base || addr >= an->base + an->size) return 0;
    return an->owner[widx(an, addr)] == owner;
}

static uint32_t fetch(const a_analysis *an, uint32_t addr) {
    const uint8_t *p = an->code + (addr - an->base);
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int is_import(const a_analysis *an, uint32_t addr) {
    return an->stub_size && addr >= an->stub_addr &&
           addr < an->stub_addr + an->stub_size;
}

/* Is `addr` a discovered function entry? Binary search over the sorted list. */
static int is_function(const a_analysis *an, uint32_t addr) {
    int lo = 0, hi = an->nfuncs - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (an->funcs[mid].addr == addr) return 1;
        if (an->funcs[mid].addr < addr) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

/* Emit the address + disassembly comment that precedes every statement. */
static void comment(ectx *c, const a_insn *in) {
    char text[128];
    a_format(in, text, sizeof text);
    fprintf(c->out, "    /* %08X  %s */\n", in->addr, text);
}

/* Emit a call to whatever lives at a static target. */
static void emit_static_call(ectx *c, uint32_t target) {
    if (is_import(c->an, target)) {
        fprintf(c->out, "    psp_import_%08X();\n", target);
    } else if (is_function(c->an, target)) {
        fprintf(c->out, "    psp_func_%08X();\n", target);
    } else {
        /* Discovery did not reach it. Going through the dispatch table means
         * the failure is named at run time instead of failing to link. */
        fprintf(c->out, "    psp_dispatch(0x%08Xu);  /* not discovered */\n", target);
    }
}

/* ---- one non-control-flow instruction ------------------------------------ */

/* `ind` is the indentation, so a delay slot emitted inside an `if` body lines
 * up. Returns nothing: unhandled opcodes emit a trap rather than nothing, so
 * a gap is loud at run time instead of silently doing the wrong thing. */
static void emit_simple(ectx *c, const a_insn *in, const char *ind) {
    FILE *f = c->out;
    const char *rd = RN[in->rd], *rs = RN[in->rs], *rt = RN[in->rt];

    /* Writes to $zero are discarded by the hardware. Emitting them would
     * clobber a register the rest of the code assumes is always zero. */
    #define DEST_ZERO(reg) ((reg) == 0)

    switch (in->op) {
    case A_NOP:
        fprintf(f, "%s;\n", ind);
        return;

    /* --- ALU, register --- */
    case A_ADD: case A_ADDU:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = %s + %s;\n", ind, rd, rs, rt); return;
    case A_SUB: case A_SUBU:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = %s - %s;\n", ind, rd, rs, rt); return;
    case A_AND:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = %s & %s;\n", ind, rd, rs, rt); return;
    case A_OR:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = %s | %s;\n", ind, rd, rs, rt); return;
    case A_XOR:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = %s ^ %s;\n", ind, rd, rs, rt); return;
    case A_NOR:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = ~(%s | %s);\n", ind, rd, rs, rt); return;
    case A_SLT:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_slt(%s, %s);\n", ind, rd, rs, rt); return;
    case A_SLTU:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_sltu(%s, %s);\n", ind, rd, rs, rt); return;
    case A_MAX:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_max(%s, %s);\n", ind, rd, rs, rt); return;
    case A_MIN:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_min(%s, %s);\n", ind, rd, rs, rt); return;
    case A_MOVZ:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%sif (%s == 0) %s = %s;\n", ind, rt, rd, rs); return;
    case A_MOVN:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%sif (%s != 0) %s = %s;\n", ind, rt, rd, rs); return;

    /* --- ALU, immediate --- */
    case A_ADDI: case A_ADDIU:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = %s + %d;\n", ind, rt, rs, in->imm); return;
    case A_SLTI:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = psp_slt(%s, (uint32_t)%d);\n", ind, rt, rs, in->imm); return;
    case A_SLTIU:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = psp_sltu(%s, (uint32_t)%d);\n", ind, rt, rs, in->imm); return;
    case A_ANDI:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = %s & 0x%Xu;\n", ind, rt, rs, (unsigned)in->imm); return;
    case A_ORI:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = %s | 0x%Xu;\n", ind, rt, rs, (unsigned)in->imm); return;
    case A_XORI:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = %s ^ 0x%Xu;\n", ind, rt, rs, (unsigned)in->imm); return;
    case A_LUI:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = 0x%08Xu;\n", ind, rt, (unsigned)in->imm << 16); return;

    /* --- shifts. The helpers exist because C leaves shift-by->=32 undefined
       while MIPS masks the amount to five bits. --- */
    case A_SLL:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_sll(%s, %u);\n", ind, rd, rt, in->sa); return;
    case A_SRL:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_srl(%s, %u);\n", ind, rd, rt, in->sa); return;
    case A_SRA:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_sra(%s, %u);\n", ind, rd, rt, in->sa); return;
    case A_ROTR:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_rotr(%s, %u);\n", ind, rd, rt, in->sa); return;
    case A_SLLV:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_sll(%s, %s);\n", ind, rd, rt, rs); return;
    case A_SRLV:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_srl(%s, %s);\n", ind, rd, rt, rs); return;
    case A_SRAV:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_sra(%s, %s);\n", ind, rd, rt, rs); return;
    case A_ROTRV:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_rotr(%s, %s);\n", ind, rd, rt, rs); return;

    /* --- bit manipulation --- */
    case A_CLZ:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_clz(%s);\n", ind, rd, rs); return;
    case A_CLO:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_clo(%s);\n", ind, rd, rs); return;
    case A_SEB:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_seb(%s);\n", ind, rd, rt); return;
    case A_SEH:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_seh(%s);\n", ind, rd, rt); return;
    case A_WSBH:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_wsbh(%s);\n", ind, rd, rt); return;
    case A_WSBW:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_wsbw(%s);\n", ind, rd, rt); return;
    case A_BITREV:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_bitrev(%s);\n", ind, rd, rt); return;
    case A_EXT: {
        if (DEST_ZERO(in->rt)) break;
        unsigned pos = in->sa, size = in->rd + 1u;
        fprintf(f, "%s%s = psp_ext(%s, %u, %u);\n", ind, rt, rs, pos, size); return;
    }
    case A_INS: {
        if (DEST_ZERO(in->rt)) break;
        unsigned pos = in->sa, size = in->rd - in->sa + 1u;
        fprintf(f, "%s%s = psp_ins(%s, %s, %u, %u);\n", ind, rt, rt, rs, pos, size); return;
    }

    /* --- multiply / divide. These write HI/LO, never a GPR. --- */
    case A_MULT:  fprintf(f, "%spsp_mult(%s, %s);\n",  ind, rs, rt); return;
    case A_MULTU: fprintf(f, "%spsp_multu(%s, %s);\n", ind, rs, rt); return;
    case A_DIV:   fprintf(f, "%spsp_div(%s, %s);\n",   ind, rs, rt); return;
    case A_DIVU:  fprintf(f, "%spsp_divu(%s, %s);\n",  ind, rs, rt); return;
    case A_MADD:  fprintf(f, "%spsp_madd(%s, %s);\n",  ind, rs, rt); return;
    case A_MADDU: fprintf(f, "%spsp_maddu(%s, %s);\n", ind, rs, rt); return;
    case A_MSUB:  fprintf(f, "%spsp_msub(%s, %s);\n",  ind, rs, rt); return;
    case A_MSUBU: fprintf(f, "%spsp_msubu(%s, %s);\n", ind, rs, rt); return;
    case A_MFHI:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_cpu.hi;\n", ind, rd); return;
    case A_MFLO:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_cpu.lo;\n", ind, rd); return;
    case A_MTHI: fprintf(f, "%spsp_cpu.hi = %s;\n", ind, rs); return;
    case A_MTLO: fprintf(f, "%spsp_cpu.lo = %s;\n", ind, rs); return;

    /* --- loads --- */
    case A_LB:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = (uint32_t)(int32_t)(int8_t)psp_read8(%s + %d);\n",
                ind, rt, rs, in->imm); return;
    case A_LBU:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = psp_read8(%s + %d);\n", ind, rt, rs, in->imm); return;
    case A_LH:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = (uint32_t)(int32_t)(int16_t)psp_read16(%s + %d);\n",
                ind, rt, rs, in->imm); return;
    case A_LHU:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = psp_read16(%s + %d);\n", ind, rt, rs, in->imm); return;
    case A_LW: case A_LL:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = psp_read32(%s + %d);\n", ind, rt, rs, in->imm); return;
    case A_LWL:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = psp_lwl(%s, %s + %d);\n", ind, rt, rt, rs, in->imm); return;
    case A_LWR:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = psp_lwr(%s, %s + %d);\n", ind, rt, rt, rs, in->imm); return;

    /* --- stores --- */
    case A_SB:
        fprintf(f, "%spsp_write8(%s + %d, (uint8_t)%s);\n", ind, rs, in->imm, rt); return;
    case A_SH:
        fprintf(f, "%spsp_write16(%s + %d, (uint16_t)%s);\n", ind, rs, in->imm, rt); return;
    case A_SW:
        fprintf(f, "%spsp_write32(%s + %d, %s);\n", ind, rs, in->imm, rt); return;
    case A_SWL:
        fprintf(f, "%spsp_swl(%s, %s + %d);\n", ind, rt, rs, in->imm); return;
    case A_SWR:
        fprintf(f, "%spsp_swr(%s, %s + %d);\n", ind, rt, rs, in->imm); return;
    case A_SC:
        /* No multiprocessor to contend with, so the store always succeeds. */
        fprintf(f, "%spsp_write32(%s + %d, %s);\n", ind, rs, in->imm, rt);
        if (!DEST_ZERO(in->rt)) fprintf(f, "%s%s = 1;\n", ind, rt);
        return;

    /* Cache and prefetch hints have no meaning without a cache model. */
    case A_CACHE: case A_PREF: case A_SYNC:
        fprintf(f, "%s;\n", ind); return;

    /* --- COP1, single precision only --- */
    case A_MTC1: fprintf(f, "%spsp_cpu.f[%u] = psp_bits_to_f32(%s);\n", ind, in->fs, rt); return;
    case A_MFC1:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = psp_f32_to_bits(psp_cpu.f[%u]);\n", ind, rt, in->fs); return;
    case A_CTC1: fprintf(f, "%spsp_cpu.fcr31 = %s;\n", ind, rt); return;
    case A_CFC1:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = psp_cpu.fcr31;\n", ind, rt); return;
    case A_LWC1:
        fprintf(f, "%spsp_cpu.f[%u] = psp_read_f32(%s + %d);\n", ind, in->ft, rs, in->imm); return;
    case A_SWC1:
        fprintf(f, "%spsp_write_f32(%s + %d, psp_cpu.f[%u]);\n", ind, rs, in->imm, in->ft); return;
    case A_ADD_S:
        fprintf(f, "%spsp_cpu.f[%u] = psp_cpu.f[%u] + psp_cpu.f[%u];\n", ind, in->fd, in->fs, in->ft); return;
    case A_SUB_S:
        fprintf(f, "%spsp_cpu.f[%u] = psp_cpu.f[%u] - psp_cpu.f[%u];\n", ind, in->fd, in->fs, in->ft); return;
    case A_MUL_S:
        fprintf(f, "%spsp_cpu.f[%u] = psp_cpu.f[%u] * psp_cpu.f[%u];\n", ind, in->fd, in->fs, in->ft); return;
    case A_DIV_S:
        fprintf(f, "%spsp_cpu.f[%u] = psp_cpu.f[%u] / psp_cpu.f[%u];\n", ind, in->fd, in->fs, in->ft); return;
    case A_MOV_S:
        fprintf(f, "%spsp_cpu.f[%u] = psp_cpu.f[%u];\n", ind, in->fd, in->fs); return;
    case A_NEG_S:
        fprintf(f, "%spsp_cpu.f[%u] = -psp_cpu.f[%u];\n", ind, in->fd, in->fs); return;
    case A_ABS_S:
        fprintf(f, "%spsp_cpu.f[%u] = psp_fabs(psp_cpu.f[%u]);\n", ind, in->fd, in->fs); return;
    case A_SQRT_S:
        fprintf(f, "%spsp_cpu.f[%u] = psp_fsqrt(psp_cpu.f[%u]);\n", ind, in->fd, in->fs); return;
    case A_CVT_S_W:
        fprintf(f, "%spsp_cpu.f[%u] = (float)(int32_t)psp_f32_to_bits(psp_cpu.f[%u]);\n",
                ind, in->fd, in->fs); return;
    case A_CVT_W_S: case A_TRUNC_W_S:
        fprintf(f, "%spsp_cpu.f[%u] = psp_bits_to_f32((uint32_t)(int32_t)psp_cpu.f[%u]);\n",
                ind, in->fd, in->fs); return;
    case A_C_COND_S:
        fprintf(f, "%spsp_fpu_set_cond(psp_fcmp(%u, psp_cpu.f[%u], psp_cpu.f[%u]));\n",
                ind, in->fcond, in->fs, in->ft); return;

    /* --- COP0. There is no privileged state to model. --- */
    case A_MFC0: case A_CFC0: case A_MFIC:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = 0;  /* no COP0 state modelled */\n", ind, rt); return;
    case A_MTC0: case A_CTC0: case A_MTIC:
        fprintf(f, "%s;  /* COP0 write ignored */\n", ind); return;

    /* --- VFPU: the subset with a real implementation. Everything else in the
       vector unit still falls through to a trap below, which is deliberate --
       see include/psprecomp/vfpu.h. --- */
    case A_LV_S:
        fprintf(f, "%spsp_lv_s(%u, %s + %d);\n", ind, in->vt, rs, in->imm & ~3); return;
    case A_LV_Q:
        fprintf(f, "%spsp_lv_q(%u, %s + %d);\n", ind, in->vt, rs, in->imm & ~3); return;
    case A_SV_S:
        fprintf(f, "%spsp_sv_s(%u, %s + %d);\n", ind, in->vt, rs, in->imm & ~3); return;
    case A_SV_Q:
        fprintf(f, "%spsp_sv_q(%u, %s + %d);\n", ind, in->vt, rs, in->imm & ~3); return;

    case A_VADD:
        fprintf(f, "%spsp_vadd(%u, %u, %u, %u);\n", ind, in->vd, in->vs, in->vt, in->vsize); return;
    case A_VSUB:
        fprintf(f, "%spsp_vsub(%u, %u, %u, %u);\n", ind, in->vd, in->vs, in->vt, in->vsize); return;
    case A_VMUL:
        fprintf(f, "%spsp_vmul(%u, %u, %u, %u);\n", ind, in->vd, in->vs, in->vt, in->vsize); return;
    case A_VDIV:
        fprintf(f, "%spsp_vdiv(%u, %u, %u, %u);\n", ind, in->vd, in->vs, in->vt, in->vsize); return;
    case A_VMIN:
        fprintf(f, "%spsp_vmin(%u, %u, %u, %u);\n", ind, in->vd, in->vs, in->vt, in->vsize); return;
    case A_VMAX:
        fprintf(f, "%spsp_vmax(%u, %u, %u, %u);\n", ind, in->vd, in->vs, in->vt, in->vsize); return;
    case A_VDOT:
        fprintf(f, "%spsp_vdot(%u, %u, %u, %u);\n", ind, in->vd, in->vs, in->vt, in->vsize); return;
    case A_VSCL:
        fprintf(f, "%spsp_vscl(%u, %u, %u, %u);\n", ind, in->vd, in->vs, in->vt, in->vsize); return;
    case A_VCMP:
        fprintf(f, "%spsp_vcmp(%u, %u, %u, %u);\n", ind, in->vd & 0xF, in->vs, in->vt, in->vsize); return;

    /* Prefixes gate the arithmetic above: with one pending, the next vector op
     * traps instead of computing the unprefixed answer. Emitting these is what
     * makes that guard fire at all -- without them the guard is dead code and
     * every prefixed operation silently produces the wrong number. */
    /* VFPU4 unary ops, all through one runtime entry point. */
    case A_VMOV: case A_VABS: case A_VNEG: case A_VZERO: case A_VONE:
    case A_VRCP: case A_VRSQ: case A_VSQRT: case A_VSIN: case A_VCOS:
    case A_VEXP2: case A_VLOG2: case A_VSAT0: case A_VSAT1:
    case A_VNRCP: case A_VNSIN: case A_VASIN: case A_VF2IZ: case A_VI2F: {
        static const struct { a_op op; const char *sel; } U[] = {
            { A_VMOV, "PSP_VU_MOV" },   { A_VABS, "PSP_VU_ABS" },
            { A_VNEG, "PSP_VU_NEG" },   { A_VZERO,"PSP_VU_ZERO" },
            { A_VONE, "PSP_VU_ONE" },   { A_VRCP, "PSP_VU_RCP" },
            { A_VRSQ, "PSP_VU_RSQ" },   { A_VSQRT,"PSP_VU_SQRT" },
            { A_VSIN, "PSP_VU_SIN" },   { A_VCOS, "PSP_VU_COS" },
            { A_VEXP2,"PSP_VU_EXP2" },  { A_VLOG2,"PSP_VU_LOG2" },
            { A_VSAT0,"PSP_VU_SAT0" },  { A_VSAT1,"PSP_VU_SAT1" },
            { A_VNRCP,"PSP_VU_NRCP" },  { A_VNSIN,"PSP_VU_NSIN" },
            { A_VASIN,"PSP_VU_ASIN" },  { A_VF2IZ,"PSP_VU_F2IZ" },
            { A_VI2F, "PSP_VU_I2F" },
        };
        for (size_t k = 0; k < sizeof U / sizeof U[0]; k++) {
            if (U[k].op != in->op) continue;
            fprintf(f, "%spsp_vunary(%s, %u, %u, %u);\n",
                    ind, U[k].sel, in->vd, in->vs, in->vsize);
            return;
        }
        break;
    }

    /* Matrix ops that need no multiply. `vsize` is the matrix order here. */
    case A_VMMUL:
        fprintf(f, "%spsp_vmmul(%u, %u, %u, %u);\n", ind, in->vd, in->vs, in->vt, in->vsize); return;
    case A_VTFM2: case A_VTFM3: case A_VTFM4:
        fprintf(f, "%spsp_vtfm(%u, %u, %u, %u);\n", ind, in->vd, in->vs, in->vt, in->vsize); return;
    case A_VMSCL:
        fprintf(f, "%spsp_vmscl(%u, %u, %u, %u);\n", ind, in->vd, in->vs, in->vt, in->vsize); return;
    case A_VMIDT:
        fprintf(f, "%spsp_vmidt(%u, %u);\n", ind, in->vd, in->vsize); return;
    case A_VMZERO:
        fprintf(f, "%spsp_vmzero(%u, %u);\n", ind, in->vd, in->vsize); return;
    case A_VMONE:
        fprintf(f, "%spsp_vmone(%u, %u);\n", ind, in->vd, in->vsize); return;
    case A_VMMOV:
        fprintf(f, "%spsp_vmmov(%u, %u, %u);\n", ind, in->vd, in->vs, in->vsize); return;

    case A_VPFXS:
        fprintf(f, "%spsp_vfpu_set_prefix(0, 0x%06Xu);\n", ind, in->raw & 0xFFFFFF); return;
    case A_VPFXT:
        fprintf(f, "%spsp_vfpu_set_prefix(1, 0x%06Xu);\n", ind, in->raw & 0xFFFFFF); return;
    case A_VPFXD:
        fprintf(f, "%spsp_vfpu_set_prefix(2, 0x%06Xu);\n", ind, in->raw & 0xFFFFFF); return;

    case A_SYSCALL:
        fprintf(f, "%spsp_syscall(0x%05Xu);\n", ind, (in->raw >> 6) & 0xFFFFF); return;
    case A_BREAK:
        fprintf(f, "%spsp_unimplemented(0x%08Xu, \"break\");\n", ind, in->addr); return;

    default:
        break;
    }

    /* Anything unhandled — the VFPU, and any encoding the decoder does not
     * name — becomes a loud run-time trap. Emitting nothing here would produce
     * a program that runs and is quietly wrong, which is the single worst
     * outcome available. */
    fprintf(f, "%spsp_unimplemented(0x%08Xu, \"%s\");\n", ind, in->addr, a_mnemonic(in->op));
    #undef DEST_ZERO
}

/* ---- branch conditions --------------------------------------------------- */

static void branch_cond(char *buf, size_t n, const a_insn *in) {
    const char *rs = RN[in->rs], *rt = RN[in->rt];
    switch (in->op) {
    case A_BEQ: case A_BEQL:   snprintf(buf, n, "%s == %s", rs, rt); break;
    case A_BNE: case A_BNEL:   snprintf(buf, n, "%s != %s", rs, rt); break;
    case A_BLEZ: case A_BLEZL: snprintf(buf, n, "(int32_t)%s <= 0", rs); break;
    case A_BGTZ: case A_BGTZL: snprintf(buf, n, "(int32_t)%s > 0", rs); break;
    case A_BLTZ: case A_BLTZL:
    case A_BLTZAL: case A_BLTZALL: snprintf(buf, n, "(int32_t)%s < 0", rs); break;
    case A_BGEZ: case A_BGEZL:
    case A_BGEZAL: case A_BGEZALL: snprintf(buf, n, "(int32_t)%s >= 0", rs); break;
    case A_BC1T: case A_BC1TL: snprintf(buf, n, "psp_fpu_cond()"); break;
    case A_BC1F: case A_BC1FL: snprintf(buf, n, "!psp_fpu_cond()"); break;
    default:                   snprintf(buf, n, "0 /* unhandled branch */"); break;
    }
}

/* Emit the standalone, labelled copy of a delay slot that is also a branch
 * target. `falls_through` says whether the construct just emitted can reach
 * the next address by falling through — if so it must jump past this copy
 * rather than running it a second time. */
static void emit_slot_alias(ectx *c, uint32_t a, const a_insn *slot, int falls_through) {
    const a_analysis *an = c->an;
    const uint32_t owner = c->func->addr;
    const uint32_t after = a + 8;

    if (falls_through) {
        if (owned_by(an, after, owner)) fprintf(c->out, "    goto L_%08X;\n", after);
        else                            fprintf(c->out, "    return;\n");
    }
    fprintf(c->out, "L_%08X:\n", a + 4);
    comment(c, slot);
    emit_simple(c, slot, "    ");
}

/* ---- one function -------------------------------------------------------- */

static void emit_function(ectx *c, const a_func *fn) {
    const a_analysis *an = c->an;
    FILE *f = c->out;
    const uint32_t owner = fn->addr;

    /* Pass 1: which owned addresses are branch targets, and which are consumed
     * as delay slots. A delay slot is emitted inline with its branch, so the
     * main pass must skip it. */
    for (uint32_t a = fn->start; a < fn->end; a += 4) {
        if (!owned_by(an, a, owner)) continue;
        a_insn in;
        a_decode(fetch(an, a), a, &in);
        if (in.has_target && in.is_branch && owned_by(an, in.target, owner))
            c->is_label[widx(an, in.target)] = 1;
        if (in.is_jump && !in.is_indirect && in.has_target &&
            owned_by(an, in.target, owner) && !is_function(an, in.target))
            c->is_label[widx(an, in.target)] = 1;
        if (in.has_delay_slot && owned_by(an, a + 4, owner))
            c->is_slot[widx(an, a + 4)] = 1;
    }

    /* A delay slot can also be somebody's branch target. Arriving through the
     * branch, it runs as part of that transfer; arriving by a jump straight to
     * its address, it is an ordinary instruction. Both paths are real, so it
     * has to be emitted twice — inline with its branch, and again under its
     * own label. The fall-through path then needs somewhere to land past the
     * standalone copy, so the following address gets a label too.
     *
     * Exactly one site in Lumberjack's 2206 functions; ignoring it produced
     * the one compile error in 256,566 generated lines. */
    for (uint32_t a = fn->start; a + 4 < fn->end; a += 4) {
        if (!owned_by(an, a, owner)) continue;
        uint32_t i = widx(an, a);
        if (c->is_slot[i] && c->is_label[i] && owned_by(an, a + 4, owner))
            c->is_label[widx(an, a + 4)] = 1;
    }

    fprintf(f, "\n/* ---------------------------------------------------------------\n");
    fprintf(f, " * psp_func_%08X  --  %u instructions, %u bytes\n",
            fn->addr, fn->insns, fn->end - fn->addr);
    if (!fn->has_return)
        fprintf(f, " * No `jr $ra`: ends in a tail call, or discovery lost the trail.\n");
    if (fn->has_indirect)
        fprintf(f, " * Contains a computed jump routed through the dispatch table.\n");
    fprintf(f, " * ------------------------------------------------------------- */\n");
    fprintf(f, "void psp_func_%08X(void) {\n", fn->addr);
    /* Compiles away unless the generated code is built with PSPRECOMP_TRACE. */
    fprintf(f, "    PSP_ENTER(0x%08Xu);\n", fn->addr);

    for (uint32_t a = fn->start; a < fn->end; a += 4) {
        if (!owned_by(an, a, owner)) continue;
        uint32_t i = widx(an, a);
        if (c->is_slot[i]) continue;             /* emitted with its branch */

        a_insn in;
        a_decode(fetch(an, a), a, &in);

        if (c->is_label[i]) fprintf(f, "L_%08X:\n", a);
        comment(c, &in);

        /* The delay-slot instruction, if this transfers control. */
        a_insn slot;
        int have_slot = 0;
        if (in.has_delay_slot && owned_by(an, a + 4, owner)) {
            a_decode(fetch(an, a + 4), a + 4, &slot);
            have_slot = 1;
        }

        if (in.is_branch) {
            char cond[128];
            branch_cond(cond, sizeof cond, &in);

            if (in.is_likely) {
                /* A "likely" branch nullifies its delay slot when NOT taken,
                 * so the slot belongs inside the taken path. The condition is
                 * naturally evaluated before it. */
                fprintf(f, "    if (%s) {\n", cond);
                if (have_slot) { comment(c, &slot); emit_simple(c, &slot, "        "); }
                if (in.is_call) fprintf(f, "        %s = 0x%08Xu;\n", RN[31], a + 8);
                if (owned_by(an, in.target, owner))
                    fprintf(f, "        goto L_%08X;\n", in.target);
                else
                    { emit_static_call(c, in.target); fprintf(f, "        return;\n"); }
                fprintf(f, "    }\n");
            } else {
                /* An ordinary branch always executes its delay slot, and reads
                 * its condition registers BEFORE the slot runs. The slot may
                 * write one of those registers:
                 *
                 *     beq   $a0, $zero, target
                 *     addiu $a0, $a0, 1        <- branch already read old $a0
                 *
                 * so the condition is captured into a temporary first. Doing
                 * that unconditionally costs nothing (the compiler folds it
                 * away when there is no dependency) and removes an entire
                 * class of silent, once-in-a-thousand-iterations bugs. */
                fprintf(f, "    { int _c = (%s);\n", cond);
                if (have_slot) { comment(c, &slot); emit_simple(c, &slot, "      "); }
                if (in.is_call) fprintf(f, "      %s = 0x%08Xu;\n", RN[31], a + 8);
                if (owned_by(an, in.target, owner))
                    fprintf(f, "      if (_c) goto L_%08X; }\n", in.target);
                else {
                    fprintf(f, "      if (_c) { ");
                    if (is_import(an, in.target))      fprintf(f, "psp_import_%08X();", in.target);
                    else if (is_function(an, in.target)) fprintf(f, "psp_func_%08X();", in.target);
                    else                                fprintf(f, "psp_dispatch(0x%08Xu);", in.target);
                    fprintf(f, " return; } }\n");
                }
            }
            if (have_slot && c->is_label[widx(an, a + 4)]) emit_slot_alias(c, a, &slot, 1);
            a += 4;                              /* consumed the delay slot */
            continue;
        }

        if (in.is_call) {                        /* jal / jalr */
            if (have_slot) { comment(c, &slot); emit_simple(c, &slot, "    "); }
            if (in.is_indirect) fprintf(f, "    psp_dispatch(%s);\n", RN[in.rs]);
            else                emit_static_call(c, in.target);
            if (have_slot && c->is_label[widx(an, a + 4)]) emit_slot_alias(c, a, &slot, 1);
            a += 4;
            continue;
        }

        if (in.is_return) {                      /* jr $ra */
            if (have_slot) { comment(c, &slot); emit_simple(c, &slot, "    "); }
            fprintf(f, "    return;\n");
            if (have_slot && c->is_label[widx(an, a + 4)]) emit_slot_alias(c, a, &slot, 0);
            a += 4;
            continue;
        }

        if (in.is_indirect) {                    /* jr $rN — computed jump */
            if (have_slot) { comment(c, &slot); emit_simple(c, &slot, "    "); }
            fprintf(f, "    psp_dispatch(%s);\n    return;\n", RN[in.rs]);
            if (have_slot && c->is_label[widx(an, a + 4)]) emit_slot_alias(c, a, &slot, 0);
            a += 4;
            continue;
        }

        if (in.is_jump) {                        /* j */
            if (have_slot) { comment(c, &slot); emit_simple(c, &slot, "    "); }
            if (owned_by(an, in.target, owner) && !is_function(an, in.target)) {
                fprintf(f, "    goto L_%08X;\n", in.target);
            } else {
                emit_static_call(c, in.target);  /* tail call */
                fprintf(f, "    return;\n");
            }
            if (have_slot && c->is_label[widx(an, a + 4)]) emit_slot_alias(c, a, &slot, 0);
            a += 4;
            continue;
        }

        emit_simple(c, &in, "    ");
    }

    /* Falling off the end happens when discovery lost the trail; returning is
     * the only sane thing left, and the dispatch miss counter will show it. */
    fprintf(f, "}\n");
}

/* ---- files --------------------------------------------------------------- */

static void emit_header(FILE *f, const a_analysis *an, const emit_opts *o) {
    fprintf(f,
        "/* Generated by allegrexrecomp -- do not edit.\n"
        " *\n"
        " * Module: %s\n"
        " * %d recompiled functions, %d imported firmware calls.\n"
        " *\n"
        " * Each psp_func_<addr> is one Allegrex routine translated to C. Every\n"
        " * statement carries its original address and disassembly as a comment.\n"
        " * Register names are macros onto psp_cpu.r[]; the semantics that are not\n"
        " * obvious in C (division edge cases, unaligned loads, shift masking) live\n"
        " * in <psprecomp/recomp_rt.h> so they exist in exactly one place.\n"
        " */\n"
        "#ifndef PSPRECOMP_GENERATED_H\n"
        "#define PSPRECOMP_GENERATED_H\n"
        "\n"
        "#include <psprecomp/recomp_rt.h>\n"
        "#include <psprecomp/dispatch.h>\n"
        "#include <psprecomp/vfpu.h>\n"
        "\n"
        "#ifdef __cplusplus\nextern \"C\" {\n#endif\n"
        "\n"
        "/* Build with -DPSPRECOMP_TRACE to record every function entry. Costs\n"
        " * one store per call when on, and nothing at all when off. A dispatch\n"
        " * miss then reports how the code arrived, not just where it went. */\n"
        "#ifdef PSPRECOMP_TRACE\n"
        "#  define PSP_ENTER(a) psp_trace_enter(a)\n"
        "#else\n"
        "#  define PSP_ENTER(a) ((void)0)\n"
        "#endif\n"
        "\n"
        "/* Register aliases, so the generated code reads like the assembly. */\n",
        o->module ? o->module : "(unknown)", an->nfuncs, an->nimports);

    for (int i = 0; i < 32; i++)
        fprintf(f, "#define %-7s psp_cpu.r[%d]\n", RN[i], i);

    fprintf(f,
        "\n"
        "/* Register every recompiled function with the dispatch table. Call once\n"
        " * before running anything. */\n"
        "void psp_recomp_register(void);\n"
        "\n"
        "/* Provided by the host: a firmware call this module imports, and the\n"
        " * traps for anything not yet translated. */\n"
        "void psp_syscall(uint32_t id);\n"
        "void psp_unimplemented(uint32_t addr, const char *what);\n"
        "\n");

    for (int i = 0; i < an->nfuncs; i++)
        fprintf(f, "void psp_func_%08X(void);\n", an->funcs[i].addr);

    fprintf(f, "\n");
    for (int i = 0; i < an->nimports; i++)
        fprintf(f, "void psp_import_%08X(void);\n", an->imports[i]);

    fprintf(f, "\n#ifdef __cplusplus\n}\n#endif\n\n#endif\n");
}

/* Find the import-table entry describing the thunk at `addr`. */
static const psp_import_entry *import_at(const emit_opts *o, uint32_t addr) {
    for (int i = 0; i < o->nimports; i++)
        if (o->imports[i].addr == addr) return &o->imports[i];
    return NULL;
}

static void emit_imports(FILE *f, const a_analysis *an, const emit_opts *o) {
    fprintf(f,
        "/* Generated by allegrexrecomp -- do not edit.\n"
        " *\n"
        " * The %d firmware functions this module imports. Each thunk dispatches\n"
        " * to the HLE layer by NID -- the identifier the hardware itself uses,\n"
        " * being SHA-1(function name) truncated to four bytes.\n"
        " *\n"
        " * A NID with no HLE implementation reports itself by name and returns 0.\n"
        " * Bringing a game up is largely the process of watching those messages\n"
        " * stop appearing.\n"
        " */\n"
        "#include \"%s_funcs.h\"\n"
        "#include <psprecomp/hle.h>\n\n",
        an->nimports, o->prefix);

    for (int i = 0; i < an->nimports; i++) {
        uint32_t addr = an->imports[i];
        const psp_import_entry *e = import_at(o, addr);
        if (e) {
            fprintf(f, "/* %s :: NID 0x%08X */\n", e->lib, e->nid);
            fprintf(f, "void psp_import_%08X(void) { psp_hle_call(0x%08Xu); }\n\n",
                    addr, e->nid);
        } else {
            /* The thunk is called but does not appear in the import table --
             * so we cannot name it or dispatch it. Trapping is the only honest
             * option; guessing a NID would route it to the wrong function. */
            fprintf(f,
                "/* not present in the import table */\n"
                "void psp_import_%08X(void) { psp_unimplemented(0x%08Xu, \"unlisted import\"); }\n\n",
                addr, addr);
        }
    }
}

int a_emit(const a_analysis *an, const emit_opts *o) {
    char path[1024];

    /* Header */
    snprintf(path, sizeof path, "%s/%s_funcs.h", o->outdir, o->prefix);
    FILE *h = fopen(path, "w");
    if (!h) { fprintf(stderr, "cannot write %s\n", path); return -1; }
    emit_header(h, an, o);
    fclose(h);

    /* Imports */
    snprintf(path, sizeof path, "%s/%s_imports.c", o->outdir, o->prefix);
    FILE *im = fopen(path, "w");
    if (!im) { fprintf(stderr, "cannot write %s\n", path); return -1; }
    emit_imports(im, an, o);
    fclose(im);

    /* Functions */
    snprintf(path, sizeof path, "%s/%s_funcs.c", o->outdir, o->prefix);
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return -1; }

    fprintf(f,
        "/* Generated by allegrexrecomp -- do not edit.\n"
        " *\n"
        " * Module: %s   --   %d functions, %llu instructions.\n"
        " */\n"
        "#include \"%s_funcs.h\"\n",
        o->module ? o->module : "(unknown)", an->nfuncs,
        (unsigned long long)an->insns, o->prefix);

    ectx c;
    c.out = f;
    c.an = an;
    c.is_label = (uint8_t *)calloc(an->nwords ? an->nwords : 1, 1);
    c.is_slot  = (uint8_t *)calloc(an->nwords ? an->nwords : 1, 1);
    if (!c.is_label || !c.is_slot) {
        free(c.is_label); free(c.is_slot); fclose(f);
        return -1;
    }

    for (int i = 0; i < an->nfuncs; i++) {
        c.func = &an->funcs[i];
        emit_function(&c, &an->funcs[i]);
    }

    /* Registration */
    fprintf(f,
        "\n/* ---------------------------------------------------------------\n"
        " * Populate the dispatch table. Indirect calls -- function pointers,\n"
        " * vtables, callbacks, switch tables — resolve through this.\n"
        " * ------------------------------------------------------------- */\n"
        "void psp_recomp_register(void) {\n");
    for (int i = 0; i < an->nfuncs; i++)
        fprintf(f, "    psp_register(0x%08Xu, psp_func_%08X);\n",
                an->funcs[i].addr, an->funcs[i].addr);
    fprintf(f, "}\n");

    free(c.is_label);
    free(c.is_slot);
    fclose(f);

    printf("wrote %s/%s_funcs.c   (%d functions)\n", o->outdir, o->prefix, an->nfuncs);
    printf("wrote %s/%s_funcs.h\n", o->outdir, o->prefix);
    printf("wrote %s/%s_imports.c (%d imports)\n", o->outdir, o->prefix, an->nimports);
    return 0;
}
