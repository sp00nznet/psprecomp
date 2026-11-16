/* Runtime tests — the semantic helpers that recompiled code lowers to.
 *
 * These are the instructions whose C translation is *not* obvious, which is
 * exactly the set where a wrong answer hides for months: division edge cases,
 * the unaligned load/store pairs, and the bitfield ops. All inputs are
 * synthetic; nothing here touches a game.
 */

#include "psprecomp/recomp_rt.h"
#include "psprecomp/dispatch.h"

#include <stdio.h>

static int failures;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);        \
            printf(__VA_ARGS__);                               \
            printf("\n");                                      \
            failures++;                                        \
        }                                                      \
    } while (0)

#define CHECK_EQ(got, want, label)                             \
    CHECK((got) == (want), "%s: got 0x%08X, want 0x%08X",       \
          (label), (unsigned)(got), (unsigned)(want))

/* ---- the trampoline ------------------------------------------------------
 *
 * Three functions that hand over to each other in a cycle, as code emitted
 * with --trampoline does. psp_run must run the whole chain one frame deep. */
enum { TR_ROUNDS = 100000, TR_ADDR = 0x08900000u };
static int tr_depth, tr_max_depth;
static long tr_steps;
static void tr_c(void);
static void tr_enter(void) { if (++tr_depth > tr_max_depth) tr_max_depth = tr_depth; tr_steps++; }
static void tr_a(void) { tr_enter(); psp_next = tr_steps < 3L * TR_ROUNDS ? tr_c : NULL; tr_depth--; }
static void tr_b(void) { tr_enter(); psp_next = tr_a; tr_depth--; }
static void tr_c(void) { tr_enter(); psp_dispatch_tail(TR_ADDR); tr_depth--; }   /* indirect */

static void tr_unwinds(void) { psp_next = tr_a; psp_cpu.unwind = 0x08800000u; }
static void tr_stale(void) { tr_steps = -1000; }

static void test_trampoline(void) {
    psp_register(TR_ADDR, tr_b);

    tr_depth = tr_max_depth = 0;
    tr_steps = 0;
    psp_run(tr_a);
    CHECK(tr_steps == 3L * TR_ROUNDS + 1, "the chain runs to its end: %ld steps", tr_steps);
    CHECK(tr_max_depth == 1, "the chain stays one call deep: %d", tr_max_depth);
    CHECK(psp_next == NULL, "psp_next is empty after the chain");

    tr_steps = 0;
    psp_run(tr_unwinds);
    CHECK(tr_steps == 0, "an unwind ends the chain before its next step");
    CHECK(psp_next == NULL, "an unwind leaves psp_next empty");
    psp_cpu.unwind = 0;

    /* A stopped run can leave psp_next set; the next entry from the host must
     * not run it. */
    psp_next = tr_stale;
    tr_steps = 0;
    psp_register(TR_ADDR + 4, tr_b);
    psp_call_from_host(TR_ADDR + 4);
    CHECK(tr_steps == 3L * TR_ROUNDS + 2, "an entry from the host runs only its own chain: %ld",
          tr_steps);

    /* A guest call runs the callee's chain before it returns to the caller. */
    tr_depth = tr_max_depth = 0;
    tr_steps = 0;
    psp_call_guest_fn(tr_a, 0x08800010u);
    CHECK(tr_steps == 3L * TR_ROUNDS + 1, "a guest call runs the callee's whole chain: %ld",
          tr_steps);
    CHECK(tr_max_depth == 1, "the callee's chain stays one call deep: %d", tr_max_depth);
    CHECK(psp_next == NULL, "psp_next is empty after the guest call");
}

static void test_zero_register(void) {
    /* $zero is hardwired. A recompiled store to it must be a no-op, not a
     * write — otherwise one bad decode corrupts every later read of $zero. */
    psp_cpu_reset();
    psp_set_reg(PSP_REG_ZERO, 0xDEADBEEF);
    CHECK_EQ(psp_cpu.r[PSP_REG_ZERO], 0u, "$zero stays zero");

    psp_set_reg(PSP_REG_A0, 0xDEADBEEF);
    CHECK_EQ(psp_cpu.r[PSP_REG_A0], 0xDEADBEEFu, "$a0 is writable");
}

static void test_division(void) {
    /* MIPS does not trap on divide-by-zero; HI/LO are architecturally
     * unpredictable. We define them so a recompiled game is deterministic and
     * can be diffed against the oracle rather than depending on host UB. */
    psp_div(100, 7);
    CHECK_EQ(psp_cpu.lo, 14u, "div quotient");
    CHECK_EQ(psp_cpu.hi, 2u,  "div remainder");

    psp_div(0xFFFFFF9Cu /* -100 */, 7);
    CHECK_EQ(psp_cpu.lo, (uint32_t)-14, "signed div quotient");
    CHECK_EQ(psp_cpu.hi, (uint32_t)-2,  "signed div remainder");

    psp_div(5, 0);
    CHECK_EQ(psp_cpu.hi, 5u, "div by zero leaves the dividend in hi");

    /* INT_MIN / -1 overflows in C — this must not trap or crash. */
    psp_div(0x80000000u, 0xFFFFFFFFu);
    CHECK_EQ(psp_cpu.lo, 0x80000000u, "INT_MIN / -1 quotient");
    CHECK_EQ(psp_cpu.hi, 0u,           "INT_MIN / -1 remainder");

    psp_divu(0xFFFFFFFFu, 2);
    CHECK_EQ(psp_cpu.lo, 0x7FFFFFFFu, "unsigned div treats the operand as unsigned");

    psp_divu(5, 0);
    CHECK_EQ(psp_cpu.lo, 0xFFFFFFFFu, "unsigned div by zero");
}

static void test_multiply(void) {
    psp_mult(0xFFFFFFFFu, 0xFFFFFFFFu);   /* -1 * -1 == 1 */
    CHECK_EQ(psp_cpu.lo, 1u, "signed mult lo");
    CHECK_EQ(psp_cpu.hi, 0u, "signed mult hi");

    psp_multu(0xFFFFFFFFu, 0xFFFFFFFFu);  /* 0xFFFFFFFE00000001 */
    CHECK_EQ(psp_cpu.lo, 0x00000001u, "unsigned mult lo");
    CHECK_EQ(psp_cpu.hi, 0xFFFFFFFEu, "unsigned mult hi");

    /* madd accumulates into the existing HI/LO pair. */
    psp_cpu.hi = 0; psp_cpu.lo = 10;
    psp_madd(3, 4);
    CHECK_EQ(psp_cpu.lo, 22u, "madd accumulates");

    psp_cpu.hi = 0; psp_cpu.lo = 100;
    psp_msub(3, 4);
    CHECK_EQ(psp_cpu.lo, 88u, "msub subtracts");
}

static void test_shifts(void) {
    /* C leaves shift-by->=32 undefined; MIPS masks the amount to 5 bits. */
    CHECK_EQ(psp_sll(1, 32), 1u,  "sll by 32 masks to 0");
    CHECK_EQ(psp_srl(2, 33), 1u,  "srl by 33 masks to 1");
    CHECK_EQ(psp_sra(0x80000000u, 31), 0xFFFFFFFFu, "sra sign-extends");
    CHECK_EQ(psp_srl(0x80000000u, 31), 0x00000001u, "srl does not sign-extend");

    CHECK_EQ(psp_rotr(0x12345678u, 8),  0x78123456u, "rotr by 8");
    CHECK_EQ(psp_rotr(0x12345678u, 0),  0x12345678u, "rotr by 0 is identity");
    CHECK_EQ(psp_rotr(0x12345678u, 32), 0x12345678u, "rotr by 32 is identity");
}

static void test_bitops(void) {
    CHECK_EQ(psp_clz(0x00000000u), 32u, "clz of zero");
    CHECK_EQ(psp_clz(0x80000000u), 0u,  "clz of the top bit");
    CHECK_EQ(psp_clz(0x00000001u), 31u, "clz of the bottom bit");
    CHECK_EQ(psp_clo(0xFFFFFFFFu), 32u, "clo of all-ones");
    CHECK_EQ(psp_clo(0xF0000000u), 4u,  "clo of a nibble");

    CHECK_EQ(psp_ext(0x12345678u, 4, 8),  0x67u,       "ext middle bits");
    CHECK_EQ(psp_ext(0x12345678u, 0, 32), 0x12345678u, "ext full word");
    CHECK_EQ(psp_ins(0xFFFFFFFFu, 0x0u, 8, 8), 0xFFFF00FFu, "ins clears a byte");
    CHECK_EQ(psp_ins(0x00000000u, 0xABu, 8, 8), 0x0000AB00u, "ins sets a byte");

    CHECK_EQ(psp_seb(0x000000FFu), 0xFFFFFFFFu, "seb sign-extends 0xFF");
    CHECK_EQ(psp_seb(0x0000007Fu), 0x0000007Fu, "seb leaves 0x7F positive");
    CHECK_EQ(psp_seh(0x0000FFFFu), 0xFFFFFFFFu, "seh sign-extends 0xFFFF");

    CHECK_EQ(psp_wsbh(0x12345678u), 0x34127856u, "wsbh swaps within halfwords");
    CHECK_EQ(psp_wsbw(0x12345678u), 0x78563412u, "wsbw reverses the word");

    CHECK_EQ(psp_bitrev(0x00000001u), 0x80000000u, "bitrev of bit 0");
    CHECK_EQ(psp_bitrev(0x80000000u), 0x00000001u, "bitrev of bit 31");
    CHECK_EQ(psp_bitrev(psp_bitrev(0x12345678u)), 0x12345678u, "bitrev is its own inverse");

    CHECK_EQ(psp_max(5, (uint32_t)-3), 5u,           "max is signed");
    CHECK_EQ(psp_min(5, (uint32_t)-3), (uint32_t)-3, "min is signed");
    CHECK_EQ(psp_slt((uint32_t)-1, 1), 1u, "slt is signed");
    CHECK_EQ(psp_sltu((uint32_t)-1, 1), 0u, "sltu is unsigned");
}

static void test_memory(void) {
    CHECK(psp_mem_init() == 0, "memory init");

    psp_write32(0x08800000u, 0x12345678u);
    CHECK_EQ(psp_read32(0x08800000u), 0x12345678u, "round-trip a word");
    CHECK_EQ(psp_read8 (0x08800000u), 0x78u, "little-endian byte 0");
    CHECK_EQ(psp_read8 (0x08800003u), 0x12u, "little-endian byte 3");
    CHECK_EQ(psp_read16(0x08800000u), 0x5678u, "little-endian halfword");

    /* The three cache-behaviour mirrors must alias the same storage. */
    CHECK_EQ(psp_read32(0x48800000u), 0x12345678u, "uncached mirror aliases RAM");
    CHECK_EQ(psp_read32(0x88800000u), 0x12345678u, "kernel mirror aliases RAM");

    /* Unmapped access is counted, not fatal — the counter is how a recompiled
     * game tells you the analysis missed something. */
    uint64_t before = psp_mem_bad_access;
    CHECK_EQ(psp_read32(0x00000000u), 0u, "unmapped read returns zero");
    CHECK(psp_mem_bad_access == before + 1, "unmapped read is counted");

    /* An access straddling the end of a region must be rejected outright
     * rather than reading past the allocation. */
    before = psp_mem_bad_access;
    (void)psp_read32(PSP_VRAM_BASE + PSP_VRAM_SIZE - 2);
    CHECK(psp_mem_bad_access == before + 1, "straddling read is rejected");

    psp_mem_free();
}

static void test_unaligned(void) {
    CHECK(psp_mem_init() == 0, "memory init");

    /* Lay down a known byte pattern: 00 11 22 33 44 55 66 77. */
    for (uint32_t i = 0; i < 8; i++)
        psp_write8(0x08800000u + i, (uint8_t)(i * 0x11));

    /* The LE compiler idiom for an unaligned 32-bit load at address A is
     *     lwr rt, A       lwl rt, A+3
     * Bytes 1..4 are 11 22 33 44, so the loaded word must be 0x44332211. */
    uint32_t rt = 0xCCCCCCCCu;
    rt = psp_lwr(rt, 0x08800001u);
    rt = psp_lwl(rt, 0x08800004u);
    CHECK_EQ(rt, 0x44332211u, "unaligned load via lwr+lwl");

    /* The store idiom, mirrored. Writing 0xAABBCCDD at address 1 must leave
     * bytes 1..4 as DD CC BB AA and must not disturb bytes 0, 5, 6, 7. */
    psp_swr(0xAABBCCDDu, 0x08800001u);
    psp_swl(0xAABBCCDDu, 0x08800004u);
    CHECK_EQ(psp_read8(0x08800000u), 0x00u, "swl/swr left byte 0 alone");
    CHECK_EQ(psp_read8(0x08800001u), 0xDDu, "unaligned store byte 1");
    CHECK_EQ(psp_read8(0x08800002u), 0xCCu, "unaligned store byte 2");
    CHECK_EQ(psp_read8(0x08800003u), 0xBBu, "unaligned store byte 3");
    CHECK_EQ(psp_read8(0x08800004u), 0xAAu, "unaligned store byte 4");
    CHECK_EQ(psp_read8(0x08800005u), 0x55u, "swl/swr left byte 5 alone");

    /* Round-trip: read back what we just stored. */
    rt = 0;
    rt = psp_lwr(rt, 0x08800001u);
    rt = psp_lwl(rt, 0x08800004u);
    CHECK_EQ(rt, 0xAABBCCDDu, "unaligned store then load round-trips");

    /* An aligned address must behave exactly like a plain lw. */
    psp_write32(0x08800010u, 0xFEEDFACEu);
    rt = 0;
    rt = psp_lwr(rt, 0x08800010u);
    rt = psp_lwl(rt, 0x08800013u);
    CHECK_EQ(rt, 0xFEEDFACEu, "the unaligned pair degrades to lw when aligned");

    psp_mem_free();
}

/* The stack report has to be readable at the volume a real game reaches. Two
 * properties make it so, and both are invisible without a test: a balanced
 * check costs nothing, and the largest magnitude is the FIRST row whatever
 * order the imbalances arrived in -- otherwise a -656 entry hides behind a
 * crowd of +16s, which is exactly what happened on LocoRoco (24 printed lines,
 * all small, while the run was losing 656 bytes per frame). */
static void test_sp_histogram(void) {
    psp_trace_sp(0x08801000u, 0x00001000u, 0x00001000u);   /* balanced */
    psp_trace_sp(0x08801000u, 0x00001000u, 0x00001010u);   /* +16 */
    psp_trace_sp(0x08801000u, 0x00001000u, 0x00001010u);   /* +16 again */
    psp_trace_sp(0x08801004u, 0x00001000u, 0x00000F00u);   /* -256, the leak */
    psp_trace_sp(0x08801008u, 0x00001000u, 0x00001020u);   /* +32 */

    psp_sp_site rows[8];
    int n = psp_sp_hist(rows, 8);
    CHECK(n == 3, "a balanced check is not recorded; three sites remain (got %d)", n);
    if (n < 3) return;

    CHECK_EQ(rows[0].site, 0x08801004u, "the largest magnitude comes first");
    CHECK_EQ((uint32_t)rows[0].delta, (uint32_t)-256, "and it is the leak, sign kept");
    CHECK_EQ(rows[0].count, 1, "its count is its own");
    CHECK_EQ(rows[1].delta, 32, "then +32");
    CHECK_EQ(rows[2].delta, 16, "then +16");

    for (int i = 0; i < n; i++) {
        if (rows[i].site == 0x08801000u && rows[i].delta == 16) {
            CHECK_EQ(rows[i].count, 2,
                     "the same (site, delta) twice is one row with count 2");
            CHECK_EQ(rows[i].in, 0x00001000u, "the first $sp expected is kept");
            CHECK_EQ(rows[i].out, 0x00001010u, "and the first $sp observed");
            return;
        }
    }
    CHECK(0, "the +16 row is missing");
}

/* $sp across a call is a different observation from $sp across a body, and the
 * report keeps them apart: one names the callee, the other the body. */
static void test_sp_call_histogram(void) {
    psp_trace_sp_call(0x08802000u, 0x00002000u, 0x00002000u);   /* balanced */
    psp_trace_sp_call(0x08802004u, 0x00002000u, 0x00001F00u);   /* -256 */
    psp_trace_sp_call(0x08802008u, 0x00002000u, 0x00002040u);   /* +64 */

    psp_sp_site rows[4];
    int n = psp_sp_call_hist(rows, 4);
    CHECK(n == 2, "a balanced call is not recorded (got %d)", n);
    if (n < 2) return;
    CHECK_EQ(rows[0].site, 0x08802004u, "the callee that lost 256 bytes is named first");
    CHECK_EQ((uint32_t)rows[0].delta, (uint32_t)-256, "with its sign");
    CHECK_EQ(rows[1].delta, 64, "then the +64 next to it");
}

/* A fragment that falls out hands over mid-frame, and that is not a leak: the
 * frame was allocated by the first fragment of the same function and the
 * epilogue that returns it lives in its last one. Calling it a leak buries the
 * sign that does matter at such a transfer -- a hand-over with $sp *above* the
 * entry, which means a frame the fragment never took was released. */
static void test_stack_handover(void) {
    uint64_t v = psp_sp_violations();

    psp_trace_sp_leave(0x08804000u, 0x09FFF000u, 0x09FFF000u - 2112u);
    CHECK(psp_sp_violations() == v, "handing over mid-frame is not a violation");

    psp_trace_sp_leave(0x08804000u, 0x09FFF000u, 0x09FFF000u + 64u);
    CHECK(psp_sp_violations() == v + 1, "handing over above the entry is a violation");
    {
        /* ...and it gets a row in the site report (#8), the mid-frame one does not. */
        psp_sp_site rows[16];
        int n = psp_sp_hist(rows, 16), hi = 0, lo = 0;
        for (int i = 0; i < n; i++) {
            if (rows[i].site != 0x08804000u) continue;
            if (rows[i].delta == 64) hi++;
            if (rows[i].delta == -2112) lo++;
        }
        CHECK(hi == 1 && lo == 0, "a high hand-over is reported by site, a mid-frame one is not");
    }

    v = psp_sp_violations();

    /* The return exit keeps the opposite rule, and both live in the same
     * counter: a `jr $ra` must restore everything it took. */
    psp_trace_sp(0x08804000u, 0x09FFF000u, 0x09FFF000u - 16u);
    CHECK(psp_sp_violations() == v + 1, "a return exit that keeps 16 bytes is a violation");

    psp_trace_sp(0x08804000u, 0x09FFF000u, 0x09FFF000u);
    CHECK(psp_sp_violations() == v + 1, "a balanced return is not a violation");
}

int main(void) {
    test_zero_register();
    test_division();
    test_multiply();
    test_shifts();
    test_bitops();
    test_memory();
    test_unaligned();
    test_sp_histogram();
    test_sp_call_histogram();
    test_stack_handover();
    test_trampoline();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("all runtime checks passed\n");
    return 0;
}
