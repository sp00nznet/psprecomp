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

    char *src = slurp("./t_emit_funcs.c", NULL);
    CHECK(src != NULL, "generated .c is readable");
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
        free(hdr);
    }

    /* Registration wires the address to the function. */
    src = slurp("./t_emit_funcs.c", NULL);
    if (src) {
        expect_contains(src, "psp_register(0x08804000u, psp_func_08804000);",
                        "the function is registered for indirect dispatch");
        free(src);
    }

    a_analysis_free(&an);

    /* ---- fall-out past a gap: the two shapes that must not be confused ----
     *
     * When one body's flow runs into addresses owned by another, emission has
     * to say what happens at the boundary. Both shapes below are ordinary
     * compiler output, and they demand opposite answers.
     *
     *   0x08804000  addiu $sp,$sp,-16      A: prologue
     *   0x08804004  sw    $ra,12($sp)      A
     *   0x08804008  beq   $a0,$zero,0x08804020 A: branches over the gap
     *   0x0880400C  addu  $v0,$a1,$zero    A: delay slot, the fall-out site
     *   0x08804010  addiu $a1,$a1,1        B: another body's entry
     *   0x08804014  ...
     *   0x08804020  lw    $ra,12($sp)      A: the continuation, a branch target
     *
     * Case one: the gap ends in `jr $ra` (a shared epilogue). The hardware
     * leaves the function inside the gap, so the continuation is dead code for
     * this body and the emission must return -- jumping there runs instructions
     * after a `jr $ra`.
     *
     * Case two: nothing in the gap transfers control, so the flow does reach
     * the continuation, and returning instead skips the rest of the body --
     * including, in a real function, its frame restore.
     *
     * The first version of this rule only looked at the gap's LAST address,
     * which is the delay slot of the `jr` in case one, so it saw no terminator
     * and emitted the jump: __udivdi3 returned 0xFFFFFFFFFFFFFFFF on LocoRoco.
     * These two cases pin both halves. */
    {
        /* Absolute addresses, so the assertions can name them. */
        const uint32_t A_ENTRY = BASE;          /* 0x08804000 */
        const uint32_t B_ENTRY = BASE + 0x10u;  /* 0x08804010 */

        static const uint32_t GAP_LEAVES[] = {  /* the gap ends in `jr $ra` */
            0x27BDFFF0u, 0xAFBF000Cu, 0x10800005u, 0x00A01021u,
            0x24A50001u, 0x03E00008u, 0x27BD0010u, 0x00000000u,
            0x8FBF000Cu, 0x03E00008u, 0x27BD0010u,
        };
        static const uint32_t GAP_REACHES[] = { /* nothing leaves the gap */
            0x27BDFFF0u, 0xAFBF000Cu, 0x10800005u, 0x00A01021u,
            0x24A50001u, 0x00A01821u, 0x24A50002u, 0x00000000u,
            0x8FBF000Cu, 0x03E00008u, 0x27BD0010u,
        };

        enum { NW = sizeof GAP_LEAVES / 4 };

        struct { const uint32_t *code; const char *prefix; int b_words;
                 int must_jump; } cases[] = {
            { GAP_LEAVES,  "t_gap1", 3, 0 },   /* B is 0x10..0x1C and ends in a jr */
            { GAP_REACHES, "t_gap2", 2, 1 },   /* B is 0x10..0x18, no terminator */
        };

        for (unsigned ci = 0; ci < sizeof cases / sizeof cases[0]; ci++) {
            uint8_t c2[sizeof GAP_LEAVES];
            for (size_t i = 0; i < NW; i++) {
                c2[i * 4 + 0] = (uint8_t)(cases[ci].code[i]);
                c2[i * 4 + 1] = (uint8_t)(cases[ci].code[i] >> 8);
                c2[i * 4 + 2] = (uint8_t)(cases[ci].code[i] >> 16);
                c2[i * 4 + 3] = (uint8_t)(cases[ci].code[i] >> 24);
            }

            /* Boundaries and ownership are set by hand, not discovered. The
             * split points below are the ones discovery produced on the game --
             * a shared tail owned by another body, and a continuation this body
             * reaches through a branch -- and replaying them exactly is what
             * makes the two cases deterministic rather than dependent on how
             * this test file happens to be walked. */
            a_analysis b;
            memset(&b, 0, sizeof b);
            b.code = c2;
            b.base = BASE;
            b.size = (uint32_t)sizeof c2;
            b.text_size = (uint32_t)sizeof c2;
            b.nwords = NW;
            b.owner = (uint32_t *)malloc(NW * sizeof *b.owner);
            for (size_t i = 0; i < NW; i++) b.owner[i] = A_NO_OWNER;
            for (int i = 0; i < 4; i++) b.owner[i] = A_ENTRY;              /* 0x00..0x0C */
            for (int i = 4; i < 4 + cases[ci].b_words; i++) b.owner[i] = B_ENTRY;
            /* 0x1C stays unowned: the gap does not have to end where another
             * body begins, and the continuation is the next address THIS body
             * owns -- which is what the rule has to find, not the next word. */
            for (int i = 8; i < NW; i++) b.owner[i] = A_ENTRY;             /* 0x20.. */

            b.nfuncs = 2;
            b.funcs = (a_func *)calloc(2, sizeof *b.funcs);
            b.funcs[0].addr = A_ENTRY;
            b.funcs[0].start = A_ENTRY;
            b.funcs[0].end = BASE + (uint32_t)sizeof c2;
            b.funcs[0].insns = 7;
            b.funcs[0].has_return = 1;
            b.funcs[1].addr = B_ENTRY;
            b.funcs[1].start = B_ENTRY;
            b.funcs[1].end = B_ENTRY + 4u * (uint32_t)cases[ci].b_words;
            b.funcs[1].insns = (uint32_t)cases[ci].b_words;
            b.funcs[1].has_return = cases[ci].must_jump ? 0 : 1;

            emit_opts e2;
            e2.outdir = ".";
            e2.prefix = cases[ci].prefix;
            e2.module = "synthetic";
            CHECK(a_emit(&b, &e2) == 0, "%s: emission succeeds", cases[ci].prefix);

            char path[64];
            snprintf(path, sizeof path, "./%s_funcs.c", cases[ci].prefix);
            char *g = slurp(path, NULL);
            CHECK(g != NULL, "%s: generated .c is readable", cases[ci].prefix);
            if (g) {
                expect_contains(g, "falls into 0x08804010, owned by another function",
                                "the boundary is named in the generated code");
                if (cases[ci].must_jump)
                    expect_contains(g, "fall-out lands here",
                                    "the flow reaches the continuation, so go there");
                else {
                    expect_contains(g, "PSP_SP_CHECK(0x08804010u);",
                                    "the flow leaves inside the gap, so return");
                    CHECK(strstr(g, "fall-out lands here") == NULL,
                          "%s: no jump past a `jr $ra` inside the gap",
                          cases[ci].prefix);
                }
                free(g);
            }
            free(b.owner);
            free(b.funcs);
        }
    }

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("all emitter checks passed (synthetic code, no game data)\n");
    return 0;
}
