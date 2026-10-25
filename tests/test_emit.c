/* Emitter tests — synthetic code only, no game data.
 *
 * The function under test is eight instructions of hand-assembled MIPS chosen
 * to exercise the parts of emission that are actually hard: a conditional
 * branch with a delay slot, a backward-reachable label, and a `jr $ra` whose
 * delay slot must run before the return.
 *
 * The assertions are about the *shape* of the generated C, because that shape
 * is the contract. In particular the branch must read its condition into a
 * temporary before the delay slot runs — if that ever regresses, a delay slot
 * that writes a condition register silently changes which way the branch goes,
 * once in a thousand iterations, in a game nobody can debug.
 */

#include "analyze.h"
#include "emit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define BASE 0x08804000u

/* addiu $sp,$sp,-16      prologue
 * sw    $ra,12($sp)
 * addu  $v0,$a0,$a1
 * beq   $v0,$zero,+2     -> the `jr $ra` at BASE+24
 * addiu $v0,$v0,1        delay slot: runs either way, and writes $v0 which
 *                        the branch above just read
 * lw    $ra,12($sp)
 * jr    $ra
 * addiu $sp,$sp,16       delay slot: runs before the return
 */
static const uint32_t CODE[] = {
    0x27BDFFF0u, 0xAFBF000Cu, 0x00851021u, 0x10400002u,
    0x24420001u, 0x8FBF000Cu, 0x03E00008u, 0x27BD0010u,
};

/* Read-only toggling, for the "did it rewrite a file it should not have?"
 * check below. MSVC spells the POSIX names with an underscore. */
#include <sys/stat.h>
#ifdef _MSC_VER
#  include <io.h>
#  define set_readonly(p) _chmod((p), _S_IREAD)
#  define set_writable(p) _chmod((p), _S_IREAD | _S_IWRITE)
#else
#  define set_readonly(p) chmod((p), S_IRUSR)
#  define set_writable(p) chmod((p), S_IRUSR | S_IWUSR)
#endif

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *b = (char *)malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, (size_t)n, f);
    b[got] = '\0';
    fclose(f);
    if (len) *len = got;
    return b;
}

/* Assert a substring appears, reporting what was missing if not. */
static void expect_contains(const char *hay, const char *needle, const char *why) {
    if (!strstr(hay, needle)) {
        printf("FAIL %s\n  expected to find: %s\n", why, needle);
        failures++;
    }
}

/* ---- the end of a body: where its flow falls out to ---------------------- */

/* Hand-built analyses, not discovered ones.
 *
 * The shape under test is "the address after this body belongs to another
 * body", and discovery promotes and merges entries in an order-dependent way
 * (that ordering is half of what went wrong here), so a test for it must not
 * itself depend on the discovery order. The maps are filled in directly.
 *
 * Case one: 0x08804000 ends after two instructions and falls into 0x08804008,
 * which is a label of the body at 0x0880400C -- pulled below that entry by its
 * own backward jump. Case two: the same fall-out, but the address after the
 * body *is* another function's entry.
 */
static const uint32_t FALL_OUT_LABEL[] = {
    0x3C020016u,   /* 08804000  lui   $v0,0x16      -- falls out of its body */
    0x2442D8DCu,   /* 08804004  addiu $v0,$v0,-10084 */
    0x03E00008u,   /* 08804008  jr    $ra           -- owned by the body below */
    0x0A201002u,   /* 0880400C  j     0x08804008    -- which lives below its entry */
};

static const uint32_t FALL_OUT_FUNC[] = {
    0x3C020016u,   /* 08804000  lui   $v0,0x16      -- falls out of its body */
    0x2442D8DCu,   /* 08804004  addiu $v0,$v0,-10084 */
    0x03E00008u,   /* 08804008  jr    $ra           -- this one IS a function entry */
};

/* ---- a delay slot the branch does not own ---------------------------------
 *
 * A branch that leaves its body still runs its delay slot: the hardware
 * executes it whichever way the branch goes. When the slot belongs to *this*
 * body the emitter folds it into the branch, and the body that owns it skips
 * its own copy. When it belongs to another body -- discovery promotes a slot
 * address to a function entry on every real module -- there is no copy here to
 * fold in, and the walk used to step over the slot anyway. The body's `end`
 * then pointed one instruction past the address control actually reaches, and
 * the fall-out was looked for at an address that is neither a function entry
 * nor a label, so nothing was emitted at all: a silent `return`. A body that
 * stops early usually still holds a frame that only the continuation pops, so
 * its caller resumes with a $sp that is short by a frame.
 *
 * Hand-built analyses, not discovered ones: this shape is about where a body
 * *ends*, and discovery promotes and merges entries in an order-dependent way,
 * so a test for it must not itself depend on the discovery order.
 *
 *   08804000  sltiu $v0,$t2,16        the body under test
 *   08804004  beq   $v0,$zero,08804014
 *   08804008  addiu $v0,$zero,76      <- the delay slot: the first instruction
 *   0880400C  jr    $ra                  of the body below
 *   08804010  nop
 *   08804014  jr    $ra               the branch target, a function of its own
 *   08804018  nop
 */
static const uint32_t SLOT_NOT_OWNED_BRANCH[] = {
    0x2D420010u,   /* 08804000  sltiu $v0,$t2,16                  */
    0x10400003u,   /* 08804004  beq   $v0,$zero,08804014          */
    0x2402004Cu,   /* 08804008  addiu $v0,$zero,76   <- delay slot, not ours */
    0x03E00008u,   /* 0880400C  jr    $ra                         */
    0x00000000u,   /* 08804010  nop                               */
    0x03E00008u,   /* 08804014  jr    $ra                         */
    0x00000000u,   /* 08804018  nop                               */
};

/*  08804000  jal   08804010        the body under test
 *  08804004  addiu $v0,$zero,76     <- the call's delay slot, another body's
 *  08804008  jr    $ra                 first instruction
 *  0880400C  nop
 *  08804010  jr    $ra             the call target, a function of its own
 *  08804014  nop
 */
static const uint32_t SLOT_NOT_OWNED_CALL[] = {
    0x0C201004u,   /* 08804000  jal   08804010                    */
    0x2402004Cu,   /* 08804004  addiu $v0,$zero,76   <- delay slot, not ours */
    0x03E00008u,   /* 08804008  jr    $ra                         */
    0x00000000u,   /* 0880400C  nop                               */
    0x03E00008u,   /* 08804010  jr    $ra                         */
    0x00000000u,   /* 08804014  nop                               */
};

static int count_of(const char *hay, const char *needle) {
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += strlen(needle))
        n++;
    return n;
}

/* Build an analysis by hand over `code` with the given ownership. */
static void hand_built(a_analysis *an, uint32_t *owners, uint32_t nwords,
                       a_func *funcs, int nfuncs, const uint32_t *code) {
    memset(an, 0, sizeof *an);
    an->code = (uint8_t *)code;
    an->base = BASE;
    an->size = nwords * 4;
    an->nwords = nwords;
    an->owner = owners;
    an->funcs = funcs;
    an->nfuncs = nfuncs;
}

static char *emit_case(const a_analysis *an, const char *prefix) {
    emit_opts o;
    memset(&o, 0, sizeof o);
    o.outdir = ".";
    o.prefix = prefix;
    o.module = "synthetic";
    CHECK(a_emit(an, &o) == 0, "%s: emission succeeds", prefix);
    char path[128];
    snprintf(path, sizeof path, "./%s_funcs_000.c", prefix);
    return slurp(path, NULL);
}

static char *emit_case_header(const char *prefix) {
    char path[128];
    snprintf(path, sizeof path, "./%s_funcs.h", prefix);
    return slurp(path, NULL);
}

static void check_fall_out(void) {
    /* Case one: continue at the label, through the thunk that exists for it.
     * On the previous behaviour this file ended the C function here, i.e. a
     * silent `return`, and the flow it was supposed to continue was lost. */
    {
        uint32_t owners[4] = { BASE, BASE, BASE + 0x0Cu, BASE + 0x0Cu };
        a_func funcs[2];
        memset(funcs, 0, sizeof funcs);
        funcs[0].addr = BASE;          funcs[0].start = BASE;
        funcs[0].end  = BASE + 0x08u;  funcs[0].insns = 2;
        funcs[1].addr = BASE + 0x0Cu;  funcs[1].start = BASE + 0x08u;
        funcs[1].end  = BASE + 0x10u;  funcs[1].insns = 2;
        funcs[1].has_return = 1;

        a_analysis an;
        hand_built(&an, owners, 4, funcs, 2, FALL_OUT_LABEL);
        char *src = emit_case(&an, "t_fallout_label");
        CHECK(src != NULL, "case one: generated .c is readable");
        if (src) {
            expect_contains(src, "/* falls through into 0x08804008, a label of another body */",
                            "a body that falls out into another body's label says so");
            expect_contains(src, "psp_at_08804008();",
                            "and continues there, through that label's thunk");
            /* The thunk is defined with the body that owns the label, which is
             * emitted AFTER the body that calls it, possibly in another unit:
             * without a declaration the generated file does not compile at all.
             * The declaration lives in the shared header, which every unit
             * includes before its first body. */
            char *hdr = emit_case_header("t_fallout_label");
            const char *incl = strstr(src, "#include \"t_fallout_label_funcs.h\"");
            const char *first_body = strstr(src, "static void psp_body_");
            CHECK(hdr && strstr(hdr, "void psp_at_08804008(void);") != NULL,
                  "the thunk is declared in the shared header");
            CHECK(incl && (!first_body || incl < first_body),
                  "the unit includes that header before the bodies");
            free(hdr);
            free(src);
        }
    }

    /* Case two: the address after the body is a function entry. Unchanged, and
     * locked here so the new case cannot swallow this one. */
    {
        uint32_t owners[3] = { BASE, BASE, BASE + 0x08u };
        a_func funcs[2];
        memset(funcs, 0, sizeof funcs);
        funcs[0].addr = BASE;          funcs[0].start = BASE;
        funcs[0].end  = BASE + 0x08u;  funcs[0].insns = 2;
        funcs[1].addr = BASE + 0x08u;  funcs[1].start = BASE + 0x08u;
        funcs[1].end  = BASE + 0x0Cu;  funcs[1].insns = 1;
        funcs[1].has_return = 1;

        a_analysis an;
        hand_built(&an, owners, 3, funcs, 2, FALL_OUT_FUNC);
        char *src = emit_case(&an, "t_fallout_func");
        CHECK(src != NULL, "case two: generated .c is readable");
        if (src) {
            expect_contains(src, "/* falls through into the next function */",
                            "a fall-out into a function entry still says so");
            expect_contains(src, "psp_func_08804008();",
                            "and calls that function, as before");
            CHECK(strstr(src, "psp_at_08804008") == NULL,
                  "an entry is called directly, not through a label thunk");
            free(src);
        }
    }
}

static void check_slot_of_another_body(void) {
    /* Case one: a conditional branch whose delay slot is another body's first
     * instruction. On the previous behaviour this body ended in a silent
     * `return` -- one that also skipped the delay slot on the way. */
    {
        uint32_t owners[7] = { BASE, BASE,
                               BASE + 0x08u, BASE + 0x08u, BASE + 0x08u,
                               BASE + 0x14u, BASE + 0x14u };
        a_func funcs[3];
        memset(funcs, 0, sizeof funcs);
        funcs[0].addr = BASE;          funcs[0].start = BASE;
        funcs[0].end  = BASE + 0x0Cu;  funcs[0].insns = 2;
        funcs[1].addr = BASE + 0x08u;  funcs[1].start = BASE + 0x08u;
        funcs[1].end  = BASE + 0x14u;  funcs[1].insns = 3;
        funcs[2].addr = BASE + 0x14u;  funcs[2].start = BASE + 0x14u;
        funcs[2].end  = BASE + 0x1Cu;  funcs[2].insns = 2;
        funcs[2].has_return = 1;

        a_analysis an;
        hand_built(&an, owners, 7, funcs, 3, SLOT_NOT_OWNED_BRANCH);
        char *src = emit_case(&an, "t_slot_branch");
        CHECK(src != NULL, "branch case: generated .c is readable");
        if (src) {
            expect_contains(src, "/* falls into 0x08804008, owned by another function */",
                            "the not-taken path continues at the delay slot");
            expect_contains(src, "psp_func_08804008();",
                            "and reaches it as a function, which is what it is");
            expect_contains(src, "PSP_SP_LEAVE(0x08804008u);",
                            "with the stack observed as a hand-over (#11)");
            const char *cond = strstr(src, "int _c = (r_v0 == r_zero);");
            const char *call = strstr(src, "psp_func_08804008();");
            CHECK(cond && call && cond < call,
                  "the continuation is emitted after the branch, not instead of it");
            /* The slot is emitted twice: once inside the taken path, once as
             * the first statement of the body that owns it, which is where the
             * NOT-taken path reaches it. Neither path can run both, and that is
             * the property to pin -- the two copies are checked where they
             * live, because a lone count would also accept the mistake of
             * folding the slot in on the fall-through path. */
            int n = count_of(src, "r_v0 = r_zero + 76;");
            CHECK(n == 2, "one copy per path, got %d", n);
            const char *take = strstr(src, "if (_c) {");
            const char *in_taken = take ? strstr(take, "r_v0 = r_zero + 76;") : NULL;
            const char *gap = strstr(src, "/* falls into 0x08804008, owned by another function */");
            const char *in_owner = gap ? strstr(gap, "r_v0 = r_zero + 76;") : NULL;
            CHECK(in_taken && in_owner,
                  "one copy in the taken path, one after the fall-out");
            free(src);
        }
    }

    /* Case two: the same shape under a call, which is a separate line of the
     * emitter and would otherwise drift. */
    {
        uint32_t owners[6] = { BASE,
                               BASE + 0x04u, BASE + 0x04u, BASE + 0x04u,
                               BASE + 0x10u, BASE + 0x10u };
        a_func funcs[3];
        memset(funcs, 0, sizeof funcs);
        funcs[0].addr = BASE;          funcs[0].start = BASE;
        funcs[0].end  = BASE + 0x08u;  funcs[0].insns = 1;
        funcs[1].addr = BASE + 0x04u;  funcs[1].start = BASE + 0x04u;
        funcs[1].end  = BASE + 0x10u;  funcs[1].insns = 3;
        funcs[2].addr = BASE + 0x10u;  funcs[2].start = BASE + 0x10u;
        funcs[2].end  = BASE + 0x18u;  funcs[2].insns = 2;
        funcs[2].has_return = 1;

        a_analysis an;
        hand_built(&an, owners, 6, funcs, 3, SLOT_NOT_OWNED_CALL);
        char *src = emit_case(&an, "t_slot_call");
        CHECK(src != NULL, "call case: generated .c is readable");
        if (src) {
            expect_contains(src, "/* falls into 0x08804004, owned by another function */",
                            "after the call, control continues at the delay slot");
            expect_contains(src, "psp_func_08804004();",
                            "and reaches it as a function");
            int n = count_of(src, "r_v0 = r_zero + 76;");
            CHECK(n == 1, "the delay slot is still emitted exactly once, got %d", n);
            free(src);
        }
    }
}

/* The three shapes of "a branch whose delay slot another body owns", each built
 * by hand for the reason above: what matters is where the body ends and who
 * owns the slot, and discovery decides that by an order-dependent walk.
 *
 *   08804000  sltiu $v0,$t2,16
 *   08804004  beq   $v0,$zero,08804010     the body under test
 *   08804008  addiu $v0,$zero,76           <- the delay slot: another body's
 *   0880400C  jr    $ra
 *   08804010  nop                          the branch target, inside this body
 *   08804014  jr    $ra
 *   08804018  nop
 */
static const uint32_t TAKEN_SLOT_GOTO[] = {
    0x2D420010u, 0x10400002u, 0x2402004Cu, 0x03E00008u,
    0x00000000u, 0x03E00008u, 0x00000000u,
};

/* The same, with the branch leaving the body: the slot has to run before the
 * transfer, not after it.
 *
 *   08804000  beq   $v0,$zero,08804010     the body under test
 *   08804004  addiu $v0,$zero,76           <- the delay slot: another body's
 *   08804008  jr    $ra
 *   0880400C  nop
 *   08804010  jr    $ra                    the target, a function of its own
 *   08804014  nop
 */
static const uint32_t TAKEN_SLOT_TAIL[] = {
    0x10400003u, 0x2402004Cu, 0x03E00008u, 0x00000000u,
    0x03E00008u, 0x00000000u,
};

/* A likely branch: it nullifies its slot when NOT taken, so the slot belongs in
 * the taken path and nowhere else.
 *
 *   08804000  blezl $v0,08804010           the body under test
 *   08804004  addiu $v0,$zero,76           <- the delay slot: another body's
 *   08804008  jr    $ra
 *   0880400C  nop
 *   08804010  nop                          the branch target, inside this body
 *   08804014  jr    $ra
 *   08804018  nop
 */
static const uint32_t TAKEN_SLOT_LIKELY[] = {
    0x58400003u, 0x2402004Cu, 0x03E00008u, 0x00000000u,
    0x00000000u, 0x03E00008u, 0x00000000u,
};

/* A branch runs its delay slot whether or not it is taken. When the slot is
 * another body's the walk only reaches it by falling through, which is the
 * NOT-taken path -- so the taken path has to run it too, or it transfers with a
 * register the slot was supposed to set still holding its old value.
 *
 * Measured on LocoRoco (UCES00304): 0x1571C is `beq $v0,$zero,0x15774` whose
 * slot at 0x15720 is `lui $v0,0x1C`, and 0x15774 loads through $v0. The taken
 * path left it at zero and the game read the module's own code as a table. */
static void check_taken_path_runs_the_slot(void) {
    /* --- the branch stays inside its body --- */
    {
        uint32_t owners[7] = { BASE, BASE,
                               BASE + 0x08u, BASE + 0x08u,
                               BASE, BASE, BASE };
        a_func funcs[2];
        memset(funcs, 0, sizeof funcs);
        funcs[0].addr = BASE;          funcs[0].start = BASE;
        funcs[0].end  = BASE + 0x1Cu;  funcs[0].insns = 5;
        funcs[0].has_return = 1;
        funcs[1].addr = BASE + 0x08u;  funcs[1].start = BASE + 0x08u;
        funcs[1].end  = BASE + 0x10u;  funcs[1].insns = 2;

        a_analysis an;
        hand_built(&an, owners, 7, funcs, 2, TAKEN_SLOT_GOTO);
        char *src = emit_case(&an, "t_taken_goto");
        CHECK(src != NULL, "goto case: generated .c is readable");
        if (src) {
            const char *cond = strstr(src, "int _c = (r_v0 == r_zero);");
            const char *take = cond ? strstr(cond, "if (_c) {") : NULL;
            const char *slot = take ? strstr(take, "r_v0 = r_zero + 76;") : NULL;
            const char *jump = slot ? strstr(slot, "goto L_08804010;") : NULL;
            CHECK(cond && take && slot && jump,
                  "the taken path runs the delay slot before it branches inside");
            /* One copy per path: this one for the taken path, the other in the
             * body that owns it, which is where the not-taken path picks it up.
             * Neither path can reach both. */
            int n = count_of(src, "r_v0 = r_zero + 76;");
            CHECK(n == 2, "one copy per path, got %d", n);
            free(src);
        }
    }

    /* --- the branch leaves the body through a transfer --- */
    {
        uint32_t owners[6] = { BASE,
                               BASE + 0x04u, BASE + 0x04u, BASE + 0x04u,
                               BASE + 0x10u, BASE + 0x10u };
        a_func funcs[3];
        memset(funcs, 0, sizeof funcs);
        funcs[0].addr = BASE;          funcs[0].start = BASE;
        funcs[0].end  = BASE + 0x04u;  funcs[0].insns = 1;
        funcs[1].addr = BASE + 0x04u;  funcs[1].start = BASE + 0x04u;
        funcs[1].end  = BASE + 0x10u;  funcs[1].insns = 3;
        funcs[2].addr = BASE + 0x10u;  funcs[2].start = BASE + 0x10u;
        funcs[2].end  = BASE + 0x18u;  funcs[2].insns = 2;
        funcs[2].has_return = 1;

        a_analysis an;
        hand_built(&an, owners, 6, funcs, 3, TAKEN_SLOT_TAIL);
        char *src = emit_case(&an, "t_taken_tail");
        CHECK(src != NULL, "tail case: generated .c is readable");
        if (src) {
            const char *cond = strstr(src, "int _c = (r_v0 == r_zero);");
            const char *take = cond ? strstr(cond, "if (_c) {") : NULL;
            const char *slot = take ? strstr(take, "r_v0 = r_zero + 76;") : NULL;
            const char *call = slot ? strstr(slot, "psp_func_08804010();") : NULL;
            CHECK(cond && take && slot && call,
                  "the taken path runs the delay slot before the transfer");
            free(src);
        }
    }

    /* --- a likely branch, where the slot belongs to the taken path alone --- */
    {
        uint32_t owners[7] = { BASE,
                               BASE + 0x08u, BASE + 0x08u, BASE + 0x08u,
                               BASE, BASE, BASE };
        a_func funcs[2];
        memset(funcs, 0, sizeof funcs);
        funcs[0].addr = BASE;          funcs[0].start = BASE;
        funcs[0].end  = BASE + 0x1Cu;  funcs[0].insns = 5;
        funcs[0].has_return = 1;
        funcs[1].addr = BASE + 0x08u;  funcs[1].start = BASE + 0x08u;
        funcs[1].end  = BASE + 0x10u;  funcs[1].insns = 2;

        a_analysis an;
        hand_built(&an, owners, 7, funcs, 2, TAKEN_SLOT_LIKELY);
        char *src = emit_case(&an, "t_taken_likely");
        CHECK(src != NULL, "likely case: generated .c is readable");
        if (src) {
            const char *take = strstr(src, "if ((int32_t)r_v0 <= 0) {");
            const char *slot = take ? strstr(take, "r_v0 = r_zero + 76;") : NULL;
            const char *jump = slot ? strstr(slot, "goto L_08804010;") : NULL;
            CHECK(take && slot && jump,
                  "a likely branch runs its delay slot inside the taken path");
            free(src);
        }
    }
}

/* Transfers that END a body must be emitted so that the call is the LAST thing
 * the body does. All three of them -- a fall-out into a gap owned by another
 * function, a `j` to a function entry, the taken path of a likely branch --
 * compile to a call followed by `return`, and the C compiler turns that into a
 * sibling jump only when nothing sits in between. A stack observation placed
 * after the call costs a host frame per guest transfer instead, so a loop the
 * hardware runs without ever touching its stack consumes the host stack. The
 * ordinary-branch and end-of-body paths already emitted bare calls; these
 * checks exist so the other three sites cannot drift back.
 */

/* A gap: A branches forward over 0x08804008, which is B's entry, so the
 * sequential walk of A reaches an address owned by another function and stops
 * there. A's body therefore ends by transferring into B.
 *
 *   08804000  beq  $v0,$zero,0x08804010     A
 *   08804004  addiu $v0,$v0,1               delay slot
 *   08804008  jr   $ra                      B
 *   0880400C  nop
 *   08804010  jr   $ra                      A's other path
 *   08804014  nop
 */
static const uint32_t GAP_CODE[] = {
    0x10400003u, 0x24420001u, 0x03E00008u, 0x00000000u,
    0x03E00008u, 0x00000000u,
};

/* A `j` out of the body, to a function entry: A transfers at 0x08804000 and
 * never runs 0x08804008.
 *
 *   08804000  j    0x08804010               A
 *   08804004  lui  $v0,0x1234               delay slot
 *   08804008  nop
 *   0880400C  nop
 *   08804010  jr   $ra                      B
 *   08804014  nop
 */
static const uint32_t JUMP_CODE[] = {
    0x0A201004u, 0x3C021234u, 0x00000000u, 0x00000000u,
    0x03E00008u, 0x00000000u,
};

/* A likely branch out of the body: the taken path transfers to a function
 * entry, and its delay slot belongs to that path.
 *
 *   08804000  bltzl $v0,0x08804020          A
 *   08804004  addiu $v0,$v0,1               delay slot, taken path
 *   08804008  jr    $ra                     not-taken path
 *   0880400C  nop
 *   08804010  nop
 *   08804014  nop
 *   08804018  nop
 *   0880401C  nop
 *   08804020  jr    $ra                     B
 *   08804024  nop
 */
static const uint32_t LIKELY_CODE[] = {
    0x58400007u, 0x24420001u, 0x03E00008u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x03E00008u, 0x00000000u,
};

static uint8_t synth[sizeof LIKELY_CODE * 4];

/* Discover from `seeds` and emit, returning the generated .c (caller frees). */
static char *emit_synthetic(const char *prefix, const uint32_t *words, size_t nwords,
                            const uint32_t *seeds, int nseeds, int *nfuncs_out) {
    for (size_t i = 0; i < nwords; i++) {
        synth[i * 4 + 0] = (uint8_t)(words[i]);
        synth[i * 4 + 1] = (uint8_t)(words[i] >> 8);
        synth[i * 4 + 2] = (uint8_t)(words[i] >> 16);
        synth[i * 4 + 3] = (uint8_t)(words[i] >> 24);
    }

    a_analysis an;
    memset(&an, 0, sizeof an);
    an.code = synth;
    an.base = BASE;
    an.size = (uint32_t)(nwords * 4);

    if (a_discover(&an, (uint32_t *)seeds, nseeds) != 0) {
        printf("FAIL discovery failed for %s\n", prefix);
        failures++;
        a_analysis_free(&an);
        return NULL;
    }
    if (nfuncs_out) *nfuncs_out = an.nfuncs;

    emit_opts o;
    o.outdir = ".";
    o.prefix = prefix;
    o.module = "synthetic";
    int rc = a_emit(&an, &o);
    a_analysis_free(&an);
    if (rc != 0) {
        printf("FAIL emission failed for %s\n", prefix);
        failures++;
        return NULL;
    }

    char path[128];
    snprintf(path, sizeof path, "./%s_funcs_000.c", prefix);
    char *src = slurp(path, NULL);
    CHECK(src != NULL, "%s: generated unit 000 is readable", prefix);
    return src;
}

/* The transfer must END the body: skip the whitespace that follows the call and
 * accept only `return;` or the closing brace. Anything else costs a frame. */
static void expect_tail_transfer(const char *src, const char *call, const char *why) {
    const char *p = strstr(src, call);
    if (!p) { printf("FAIL %s\n  no such call emitted: %s\n", why, call); failures++; return; }
    const char *q = p + strlen(call);
    while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
    if (strncmp(q, "return;", 7) != 0 && *q != '}') {
        printf("FAIL %s\n  a statement follows the transfer, so it cannot stay a "
               "tail jump: %.40s\n", why, q);
        failures++;
    }
}

static void expect_absent(const char *src, const char *needle, const char *why) {
    if (strstr(src, needle)) {
        printf("FAIL %s\n  must not be emitted here: %s\n", why, needle);
        failures++;
    }
}

static void check_terminal_transfer_stays_tail(void) {
    /* --- a gap owned by another function --- */
    {
        uint32_t seeds[2] = { BASE, BASE + 8u };
        int n = 0;
        char *src = emit_synthetic("t_gap", GAP_CODE, 6, seeds, 2, &n);
        CHECK(n == 2, "the gap block discovers two entries, got %d", n);
        if (src) {
            expect_tail_transfer(src, "psp_func_08804008();",
                                 "a fall-out into another function stays a tail jump");
            expect_absent(src, "PSP_SP_CALL(0x08804008u",
                          "no stack observation after the fall-out transfer");
            /* Both labels are 0x08804008 -- the fall-out names its target, and
             * that target's own `jr $ra` names itself -- so the assertion has to
             * be scoped to the gap: the judgement between the comment and the
             * transfer must be the fall-out one. */
            const char *gap = strstr(src, "/* falls into 0x08804008, owned by another function */");
            const char *call = strstr(src, "psp_func_08804008();");
            const char *leave = gap ? strstr(gap, "PSP_SP_LEAVE(0x08804008u);") : NULL;
            const char *check = gap ? strstr(gap, "PSP_SP_CHECK(0x08804008u);") : NULL;
            CHECK(gap && leave && call && gap < leave && leave < call,
                  "the fall-out judges a hand-over, before the transfer");
            CHECK(!check || check > call,
                  "a fall-out is not judged as if it were a return");
            free(src);
        }
    }

    /* --- a `j` to a function entry --- */
    {
        uint32_t seeds[2] = { BASE, BASE + 0x10u };
        int n = 0;
        char *src = emit_synthetic("t_jump", JUMP_CODE, 6, seeds, 2, &n);
        CHECK(n == 2, "the jump block discovers two entries, got %d", n);
        if (src) {
            expect_tail_transfer(src, "psp_func_08804010();",
                                 "a `j` to a function entry stays a tail jump");
            expect_absent(src, "PSP_SP_CALL(0x08804010u",
                          "no stack observation after the `j` transfer");
            free(src);
        }
    }

    /* --- the taken path of a likely branch --- */
    {
        uint32_t seeds[2] = { BASE, BASE + 0x20u };
        int n = 0;
        char *src = emit_synthetic("t_likely", LIKELY_CODE, 10, seeds, 2, &n);
        CHECK(n == 2, "the likely block discovers two entries, got %d", n);
        if (src) {
            expect_tail_transfer(src, "psp_func_08804020();",
                                 "the taken path of a likely branch stays a tail jump");
            expect_absent(src, "PSP_SP_CALL(0x08804020u",
                          "no stack observation on the likely branch's taken path");
            free(src);
        }
    }
}

/* ---- an indirect transfer whose delay slot writes its target ---------------
 *
 * jr and jalr read their register before the delay slot runs, so a slot that
 * writes it does not change where control goes. Hand-written ladders do this
 * deliberately, stepping a pointer through a table of `b; move` pairs:
 *
 *   08804000  jr    $t4
 *   08804004  addiu $t4,$t4,8     <- delay slot: the jump uses the old $t4
 */
static const uint32_t JR_SLOT_WRITES_TARGET[] = {
    0x01800008u,   /* 08804000  jr    $t4        */
    0x258C0008u,   /* 08804004  addiu $t4,$t4,8  */
};

/*  08804000  jalr  $t5           links $ra
 *  08804004  addiu $t5,$t5,8     <- delay slot: the call uses the old $t5
 *  08804008  jalr  $t0, $t1      links $t0, not $ra
 *  0880400C  nop
 *  08804010  jalr  $zero, $t6    no link register
 *  08804014  nop
 *  08804018  jr    $ra
 *  0880401C  nop
 */
static const uint32_t JALR_CASES[] = {
    0x01A0F809u,   /* 08804000  jalr  $t5        */
    0x25AD0008u,   /* 08804004  addiu $t5,$t5,8  */
    0x01204009u,   /* 08804008  jalr  $t0, $t1   */
    0x00000000u,   /* 0880400C  nop              */
    (14u << 21) | 9u, /* 08804010  jalr $zero,$t6 */
    0x00000000u,   /* 08804014  nop              */
    0x03E00008u,   /* 08804018  jr    $ra        */
    0x00000000u,   /* 0880401C  nop              */
};

/* `capture` must appear, and before `slot`, which must appear before `use`. */
static void expect_order(const char *src, const char *capture, const char *slot,
                         const char *use, const char *why) {
    const char *c = strstr(src, capture);
    const char *s = c ? strstr(c, slot) : NULL;
    const char *u = s ? strstr(s, use) : NULL;
    if (!u) {
        printf("FAIL %s\n  expected, in order: %s / %s / %s\n", why, capture, slot, use);
        failures++;
    }
}

/* Do not let a later transfer satisfy an earlier call's ordering assertion. */
static void expect_order_before(const char *src, const char *capture, const char *slot,
                                const char *use, const char *boundary, const char *why) {
    const char *c = strstr(src, capture);
    const char *end = c ? strstr(c, boundary) : NULL;
    const char *s = c ? strstr(c, slot) : NULL;
    const char *u = s ? strstr(s, use) : NULL;
    CHECK(end && s && u && s < end && u < end, "%s", why);
}

static void check_indirect_target_before_slot(void) {
    uint32_t seed = BASE;
    char *src = emit_synthetic("t_jr_slot", JR_SLOT_WRITES_TARGET, 2, &seed, 1, NULL);
    if (src) {
        expect_order(src, "uint32_t _t = r_t4;", "r_t4 = r_t4 + 8;", "psp_dispatch(_t);",
                     "jr captures its target before the delay slot advances it");
        expect_absent(src, "psp_dispatch(r_t4);",
                      "jr does not read $t4 after the delay slot wrote it");
        free(src);
    }

    src = emit_synthetic("t_jalr", JALR_CASES,
                         (uint32_t)(sizeof JALR_CASES / sizeof JALR_CASES[0]), &seed, 1, NULL);
    if (src) {
        expect_order_before(src, "uint32_t _t = r_t5;", "r_t5 = r_t5 + 8;",
                            "psp_call_guest_addr(_t, 0x08804008u);", "L_08804008:",
                            "jalr captures its target before the slot and uses that call's helper");
        expect_order_before(src, "uint32_t _t = r_t1;", "r_t0 = 0x08804010u;",
                            "psp_call_guest_addr(_t, 0x08804010u);", "L_08804010:",
                            "non-RA jalr uses its own helper and return site");
        expect_order_before(src, "uint32_t _t = r_t6;", "/* 08804014",
                            "psp_call_guest_addr(_t, 0x00000000u);", "L_08804018:",
                            "zero-link jalr preserves the existing zero return-site sentinel");
        expect_absent(src, "r_zero =", "zero-link jalr does not assign the zero register");
        expect_absent(src, "psp_dispatch(r_t5);",
                      "jalr does not read $t5 after the delay slot wrote it");
        expect_contains(src, "r_ra = 0x08804008u;", "jalr $t5 links $ra");
        expect_contains(src, "r_t0 = 0x08804010u;", "jalr $t0, $t1 links $t0");
        expect_absent(src, "r_ra = 0x08804010u;", "jalr $t0, $t1 leaves $ra alone");
        free(src);
    }
}

/* ---- jal .+8: the get-PC idiom ---------------------------------------------
 *
 * Hand-written code loads the PC by calling its own return address. That is
 * not a call: the "target" is the rest of the same routine. Treated as one, it
 * becomes a phantom function that runs once as the callee and again when the
 * caller falls through into it.
 *
 *   08804000  jal   08804008     $ra = 08804008
 *   08804004  nop
 *   08804008  addu  $v0,$ra,$zero
 *   0880400C  jr    $ra
 *   08804010  nop
 */
static const uint32_t GET_PC_CODE[] = {
    0x0E201002u,   /* 08804000  jal   08804008     */
    0x00000000u,   /* 08804004  nop                */
    0x03E01021u,   /* 08804008  addu  $v0,$ra,$zero */
    0x03E00008u,   /* 0880400C  jr    $ra          */
    0x00000000u,   /* 08804010  nop                */
};

static void check_get_pc_is_not_a_call(void) {
    uint32_t seed = BASE;
    int n = 0;
    char *src = emit_synthetic("t_get_pc", GET_PC_CODE, 5, &seed, 1, &n);
    CHECK(n == 1, "jal .+8 does not split the routine: %d functions, want 1", n);
    if (src) {
        const char *jal  = strstr(src, "/* 08804000  jal");
        const char *link = jal ? strstr(jal, "r_ra = 0x08804008u;") : NULL;
        const char *next = link ? strstr(link, "r_v0 = r_ra + r_zero;") : NULL;
        CHECK(next != NULL, "jal .+8 links $ra and carries straight on");
        /* Depending on discovery order the old output called the phantom
         * directly or went through dispatch; both run the tail twice. */
        expect_absent(src, "psp_func_08804008();", "jal .+8 makes no direct call");
        /* Every `jr $ra` carries a guarded psp_dispatch(_t) for $ra used as
         * a continuation, so look for a dispatch to a fixed address. */
        expect_absent(src, "psp_dispatch(0x", "jal .+8 does not dispatch either");
        free(src);
    }
}

/* ---- `jr $ra` as a continuation, not a return ------------------------------
 *
 * Call a helper, then later *branch* to a stub ending in `jr $ra`: $ra still
 * holds the call's return site, so the stub jumps back inside the same
 * function. Emitted as `return`, it leaves early with the frame on the stack
 * (WTF's decompressor at 0x000824D0 leaked 896 bytes this way).
 *
 *   08804000  addiu $sp,$sp,-16
 *   08804004  sw    $ra,0($sp)
 *   08804008  jal   08804038          $ra = 08804010
 *   0880400C  nop
 *   08804010  addiu $v0,$v0,1          <- return site, reached again by the stub
 *   08804014  slti  $v1,$v0,3
 *   08804018  bne   $v1,$zero,0880402C
 *   0880401C  nop
 *   08804020  lw    $ra,0($sp)
 *   08804024  jr    $ra                the real return
 *   08804028  addiu $sp,$sp,16
 *   0880402C  jr    $ra                continuation: back to 08804010
 *   08804030  nop
 *   08804034  nop
 *   08804038  jr    $ra                the helper
 *   0880403C  nop
 */
static const uint32_t JR_RA_CONTINUATION[] = {
    0x27BDFFF0u, 0xAFBF0000u, 0x0E20100Eu, 0x00000000u,
    0x24420001u, 0x28430003u, 0x14600004u, 0x00000000u,
    0x8FBF0000u, 0x03E00008u, 0x27BD0010u, 0x03E00008u,
    0x00000000u, 0x00000000u, 0x03E00008u, 0x00000000u,
};

static void check_jr_ra_continuation(void) {
    uint32_t seed = BASE;
    char *src = emit_synthetic("t_jr_ra_cont", JR_RA_CONTINUATION, 16, &seed, 1, NULL);
    if (src) {
        expect_contains(src, "case 0x08804010u:", "the call's return site is a label");
        expect_contains(src, "psp_call_guest_fn(psp_func_08804038, 0x08804010u);",
                        "the runtime owns the call's return-site frame");
        expect_contains(src, "if (psp_ra_is_jump(_t)) { psp_dispatch(_t); return; }",
                        "a jr $ra elsewhere is a jump, not a return");
        free(src);
    }
}

/* ---- a branch-and-link is a conditional call (#16) --------------------------
 *
 *   08804000  bgezal $zero, 08804014   (bal: call 08804014)
 *   08804004  nop
 *   08804008  addiu  $v0, $zero, 1     <- runs after the call returns
 *   0880400C  jr     $ra
 *   08804010  nop
 *   08804014  jr     $ra               the callee
 *   08804018  nop
 *
 * The taken path was emitted as a tail transfer, so the callee's return ended
 * the caller too and the addiu never ran. */
static const uint32_t BAL_CALL[] = {
    0x04110004u, 0x00000000u, 0x24020001u, 0x03E00008u,
    0x00000000u, 0x03E00008u, 0x00000000u,
};

static void check_link_branch_is_a_call(void) {
    uint32_t seed = BASE;
    char *src = emit_synthetic("t_bal", BAL_CALL, 7, &seed, 1, NULL);
    if (src) {
        expect_contains(src, "r_ra = 0x08804008u;", "bal links $ra");
        expect_contains(src, "psp_call_guest_fn(psp_func_08804014, 0x08804008u);",
                        "bal uses a runtime frame and returns after its slot");
        expect_absent(src, "08804014(); return;", "the taken path is not a tail transfer");
        expect_order(src, "PSP_CALL_END(0x08804008u);", "      } }", "r_v0 = r_zero + 1;",
                     "the code after the call follows it");
        free(src);
    }
}

/* ---- a jump's delay slot that another body owns ----------------------------
 *
 *   08804000  addiu  $sp, $sp, -16
 *   08804004  jr     $ra
 *   08804008  addiu  $sp, $sp, 16      <- also seeded as an entry
 *
 * Discovery can make a delay slot an entry: Pendemonium builds the struct
 * offset 0x44AF0 with lui/ori, which looks like a code address and lands on
 * the slot of a `jr $ra`. The slot then belongs to its own body, and the
 * `jr $ra` was emitted without it, so every call leaked the 16-byte frame. */
static const uint32_t JR_FOREIGN_SLOT[] = { 0x27BDFFF0u, 0x03E00008u, 0x27BD0010u };

static void check_jump_runs_foreign_slot(void) {
    const uint32_t seeds[2] = { BASE, BASE + 8 };
    char *src = emit_synthetic("t_fslot", JR_FOREIGN_SLOT, 3, seeds, 2, NULL);
    if (src) {
        expect_order(src, "psp_body_08804000(uint32_t _entry)", "r_sp = r_sp + 16;",
                     "PSP_SP_CHECK(0x08804004u);",
                     "jr $ra runs a delay slot that another body owns");
        free(src);
    }
}

int main(void) {
    uint8_t code[sizeof CODE];
    for (size_t i = 0; i < sizeof CODE / sizeof CODE[0]; i++) {
        code[i * 4 + 0] = (uint8_t)(CODE[i]);
        code[i * 4 + 1] = (uint8_t)(CODE[i] >> 8);
        code[i * 4 + 2] = (uint8_t)(CODE[i] >> 16);
        code[i * 4 + 3] = (uint8_t)(CODE[i] >> 24);
    }

    a_analysis an;
    memset(&an, 0, sizeof an);
    an.code = code;
    an.base = BASE;
    an.size = (uint32_t)sizeof code;

    uint32_t seed = BASE;
    CHECK(a_discover(&an, &seed, 1) == 0, "discovery runs");
    CHECK(an.nfuncs == 1, "one function found, got %d", an.nfuncs);
    if (an.nfuncs != 1) return 1;
    CHECK(an.funcs[0].addr == BASE, "function entry is the seed");
    CHECK(an.funcs[0].has_return, "function reaches a `jr $ra`");
    CHECK(an.insns == 8, "all eight instructions visited, got %llu",
          (unsigned long long)an.insns);

    emit_opts o;
    o.outdir = ".";
    o.prefix = "t_emit";
    o.module = "synthetic";
    CHECK(a_emit(&an, &o) == 0, "emission succeeds");

    /* The corpus is split across units. A stale monolithic file from an older
     * build must not be here to answer for a unit that was never written --
     * that is exactly how a test reports a pass for something it did not
     * observe. */
    CHECK(slurp("./t_emit_funcs.c", NULL) == NULL,
          "no monolithic t_emit_funcs.c: functions go in numbered units");

    char *src = slurp("./t_emit_funcs_000.c", NULL);
    CHECK(src != NULL, "generated unit 0 is readable");
    if (!src) return 1;

    expect_contains(src, "void psp_func_08804000(void)",
                    "function is named after its address");

    /* The prologue, lowered to plain C. */
    expect_contains(src, "r_sp = r_sp + -16;", "addiu lowers to arithmetic");
    expect_contains(src, "psp_write32(r_sp + 12, r_ra);", "sw lowers to a memory write");
    expect_contains(src, "r_v0 = r_a0 + r_a1;", "addu lowers to addition");

    /* Every statement carries its address and disassembly. */
    expect_contains(src, "/* 08804000  addiu", "statements carry their disassembly");

    /* THE important one: the branch condition is captured into a temporary
     * *before* the delay slot executes, because the delay slot writes $v0 and
     * the hardware read the old value. */
    expect_contains(src, "int _c = (r_v0 == r_zero);",
                    "branch condition is captured before the delay slot");
    {
        const char *cond = strstr(src, "int _c = (r_v0 == r_zero);");
        const char *slot = strstr(src, "r_v0 = r_v0 + 1;");
        const char *jump = cond ? strstr(cond, "if (_c) goto") : NULL;
        CHECK(cond && slot && jump && cond < slot && slot < jump,
              "ordering must be condition -> delay slot -> branch");
    }

    /* The branch target is a label, and the return runs its delay slot first. */
    expect_contains(src, "L_08804018:", "branch target becomes a label");
    {
        const char *lbl = strstr(src, "L_08804018:");
        const char *slot = lbl ? strstr(lbl, "r_sp = r_sp + 16;") : NULL;
        const char *ret = slot ? strstr(slot, "return;") : NULL;
        CHECK(slot && ret, "the `jr $ra` delay slot is emitted before the return");
    }

    /* A `jr $ra` keeps the other rule: the body must leave $sp as it found it,
     * so its exit is judged against its own entry. */
    expect_contains(src, "PSP_SP_CHECK(0x08804018u);",
                    "a return exit is checked against its own entry");

    /* $zero is never assigned: `addu $v0,$a0,$a1` reads it, but nothing may
     * write it. A generated `r_zero =` would corrupt every later use. */
    CHECK(strstr(src, "r_zero =") == NULL, "nothing ever assigns $zero");

    free(src);

    /* The header declares the function and the registration entry point. */
    char *hdr = slurp("./t_emit_funcs.h", NULL);
    CHECK(hdr != NULL, "generated .h is readable");
    if (hdr) {
        expect_contains(hdr, "void psp_func_08804000(void);", "header declares the function");
        expect_contains(hdr, "void psp_recomp_register(void);", "header declares registration");
        expect_contains(hdr, "#define r_sp", "header defines register aliases");
        expect_absent(hdr, "psp_frame _f", "generated bodies do not own escaping call frames");
        expect_absent(hdr, "psp_cpu.frame =", "call scopes do not publish stack-local addresses");
        expect_contains(hdr, "if (psp_cpu.unwind) return;", "call scopes propagate foreign unwinds");
        free(hdr);
    }

    /* Registration lives in its own unit, after every function is emitted --
     * the interior labels it dispatches to are not known before then. */
    src = slurp("./t_emit_register.c", NULL);
    CHECK(src != NULL, "generated registration unit is readable");
    if (src) {
        expect_contains(src, "psp_register(0x08804000u, psp_func_08804000);",
                        "the function is registered for indirect dispatch");
        free(src);
    }

    /* Re-emitting identical output must leave the files untouched. That is the
     * whole point of splitting the corpus: a build system can only skip work it
     * can see was not redone. A timestamp check cannot prove it (both emits
     * land in the same second), so make the file read-only -- rewriting it
     * anyway then fails loudly instead of silently. */
    CHECK(set_readonly("./t_emit_funcs_000.c") == 0, "unit 0 can be made read-only");
    CHECK(a_emit(&an, &o) == 0, "re-emitting unchanged output rewrites nothing");
    set_writable("./t_emit_funcs_000.c");

    /* ...and it must still rewrite one whose contents drifted. */
    {
        FILE *f = fopen("./t_emit_funcs_000.c", "wb");
        if (f) { fputs("/* clobbered */\n", f); fclose(f); }
        CHECK(a_emit(&an, &o) == 0, "re-emitting over a changed file succeeds");
        src = slurp("./t_emit_funcs_000.c", NULL);
        CHECK(src && strstr(src, "void psp_func_08804000(void)") != NULL,
              "a changed unit is written back");
        free(src);
    }

    a_analysis_free(&an);

    /* ---- viim/vfim: destination register and non-finite immediates ---------
     * One real word from LocoRoco (UCES00304). Two separate defects used to
     * show up on it, so both are asserted here:
     *   - the destination came out as 1 where the encoding says 22, so the
     *     generated C wrote the wrong VFPU register;
     *   - the NaN halfword rendered as `nanf`, an invalid token that stopped
     *     the emitted file from compiling at all.
     */
    static const uint32_t CODE_VFIM[] = {
        0xDF967E81u,   /* vfim v22,  NaN halfword        */
        0xDFFF7C00u,   /* vfim v127, +Inf halfword       */
        0xDF96FC00u,   /* vfim v22,  -Inf halfword       */
        0xDF969B81u,   /* vfim v22,  -0.00366401672      */
        0x03E00008u,   /* jr $ra                         */
        0x00000000u,
    };
    uint8_t code2[sizeof CODE_VFIM];
    for (size_t i = 0; i < sizeof CODE_VFIM / sizeof CODE_VFIM[0]; i++) {
        code2[i * 4 + 0] = (uint8_t)(CODE_VFIM[i]);
        code2[i * 4 + 1] = (uint8_t)(CODE_VFIM[i] >> 8);
        code2[i * 4 + 2] = (uint8_t)(CODE_VFIM[i] >> 16);
        code2[i * 4 + 3] = (uint8_t)(CODE_VFIM[i] >> 24);
    }

    a_analysis an2;
    memset(&an2, 0, sizeof an2);
    an2.code = code2;
    an2.base = BASE;
    an2.size = (uint32_t)sizeof code2;

    uint32_t seed2 = BASE;
    CHECK(a_discover(&an2, &seed2, 1) == 0, "vfim discovery runs");

    emit_opts o2;
    o2.outdir = ".";
    o2.prefix = "t_vfim";
    o2.module = "synthetic";
    CHECK(a_emit(&an2, &o2) == 0, "vfim emission succeeds");

    char *vsrc = slurp("./t_vfim_funcs_000.c", NULL);
    CHECK(vsrc != NULL, "generated vfim .c is readable");
    if (vsrc) {
        /* The destination is the encoding's vt field, not the generic vd. */
        expect_contains(vsrc, "psp_vimm(22, ",
                        "the destination is the encoding's vt field (bits 16..22)");
        expect_contains(vsrc, "psp_vimm(127, ",
                        "a destination of 127 survives the round trip");
        /* The immediates: all three branches of the non-finite handling. */
        expect_contains(vsrc, "psp_vimm(22, NAN);",
                        "a NaN halfword is spelled with the C99 NAN macro");
        expect_contains(vsrc, "psp_vimm(127, INFINITY);",
                        "a +Inf halfword is spelled INFINITY");
        expect_contains(vsrc, "psp_vimm(22, -INFINITY);",
                        "a -Inf halfword keeps its sign");
        expect_contains(vsrc, "psp_vimm(22, -0.00366401672f);",
                        "a finite halfword is emitted as before");
        /* `%.9gf` glued the format's literal `f` onto printf's `nan` and `inf`
         * output, giving these two invalid tokens. Neither may come back. */
        CHECK(strstr(vsrc, "nanf") == NULL, "no invalid `nanf` token");
        CHECK(strstr(vsrc, "inff") == NULL, "no invalid `inff` token");
        free(vsrc);
    }
    a_analysis_free(&an2);

    /* vi2f / vf2iz carry a 5-bit scale in bits 20..16, which the emitter used
     * to drop: vi2f.q C200, C100, 16 converted 16.16 fixed point as if it
     * were a whole number, 65536 times too large. */
    {
        static const uint32_t CODE_CVT[] = {
            0xD2908488u,   /* vi2f.q  C200, C100, 16 */
            0xD2258488u,   /* vf2iz.q C200, C100, 5  */
            0xD2008488u,   /* vf2in.q C200, C100, 0  */
            0xD2438488u,   /* vf2iu.q C200, C100, 3  */
            0xD27F8488u,   /* vf2id.q C200, C100, 31 */
            0x03E00008u,   /* jr $ra                 */
            0x00000000u,
        };
        uint32_t seed = BASE;
        char *csrc = emit_synthetic("t_cvt", CODE_CVT, 7, &seed, 1, NULL);
        if (csrc) {
            expect_contains(csrc, "psp_vcvt(PSP_VU_I2F, 8, 4, 4, 16);",
                            "vi2f passes its scale to the runtime");
            expect_contains(csrc, "psp_vcvt(PSP_VU_F2IZ, 8, 4, 4, 5);",
                            "vf2iz passes its scale to the runtime");
            /* The other three roundings used to emit psp_unimplemented. */
            expect_contains(csrc, "psp_vcvt(PSP_VU_F2IN, 8, 4, 4, 0);", "vf2in is emitted");
            expect_contains(csrc, "psp_vcvt(PSP_VU_F2IU, 8, 4, 4, 3);", "vf2iu is emitted");
            expect_contains(csrc, "psp_vcvt(PSP_VU_F2ID, 8, 4, 4, 31);", "vf2id is emitted");
            free(csrc);
        }
    }

    check_fall_out();
    check_slot_of_another_body();
    check_taken_path_runs_the_slot();
    check_terminal_transfer_stays_tail();
    check_indirect_target_before_slot();
    check_get_pc_is_not_a_call();
    check_jr_ra_continuation();
    check_link_branch_is_a_call();
    check_jump_runs_foreign_slot();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("all emitter checks passed (synthetic code, no game data)\n");
    return 0;
}
