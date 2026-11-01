/* Hints-file tests - synthetic code only, no game data.
 *
 * Two things are under test, and only the second one is about emission.
 *
 * The parser first: the file is written by the game's author, in a format the
 * toolkit accepts and rejects by rule, and every rejection has to name the line
 * that is wrong. A hints file that is silently half-applied is worse than one
 * that refuses to load, because the build then looks like it did something.
 *
 * Then the effect the issue asked for: an address listed under [[label]] is
 * emitted as a dispatch entry of the function that owns it - a `case` in the
 * body's entry switch, a `psp_at_<addr>` thunk, and a `psp_register_label` line
 * - while the same body without the file emits none of the three.
 *
 * The real case behind the issue (Killzone: Liberation's argument ladders, whose
 * rungs are reached by a computed jump) cannot be replayed here: no game data
 * ships with this repository. Its shape is, though - a body that is discovered,
 * whose interior address nothing branches to -- which is exactly the condition
 * that made the rungs unreachable.
 */

#include "analyze.h"
#include "emit.h"
#include "hints.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
            printf(__VA_ARGS__);                               \
            printf("\n");                                      \
            failures++;                                        \
        }                                                      \
    } while (0)

#define BASE 0x08804000u

/* One body, discovered from its entry, with an interior address nothing points
 * at. Nothing branches to 0x08804008 and it is not a call's return site, so the
 * analysis has no reason to name it - the same silence a ladder rung gets.
 *
 *   08804000  addiu $sp,$sp,-16
 *   08804004  sw    $ra,12($sp)
 *   08804008  addiu $v0,$zero,1      <- the address the hints file names
 *   0880400C  lw    $ra,12($sp)
 *   08804010  jr    $ra
 *   08804014  addiu $sp,$sp,16       delay slot
 */
static const uint32_t CODE[] = {
    0x27BDFFF0u, 0xAFBF000Cu, 0x24020001u, 0x8FBF000Cu, 0x03E00008u, 0x27BD0010u,
};
#define HINTED (BASE + 8u)

static char *slurp(const char *path) {
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
    return b;
}

/* Write `text` to a file the test owns, so a_hints_load has something real to
 * open. The file is removed by the caller. */
static int write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t n = strlen(text);
    size_t got = fwrite(text, 1, n, f);
    fclose(f);
    return got == n ? 0 : -1;
}

static void expect_contains(const char *hay, const char *needle, const char *why) {
    if (!hay || !strstr(hay, needle)) {
        printf("FAIL %s\n  expected to find: %s\n", why, needle);
        failures++;
    }
}

static void expect_absent(const char *hay, const char *needle, const char *why) {
    if (hay && strstr(hay, needle)) {
        printf("FAIL %s\n  must not be present: %s\n", why, needle);
        failures++;
    }
}

/* ---- the file shape ------------------------------------------------------ */

/* The shape from the issue, with everything a real file carries: comments, a
 * blank line, a multi-line array, hex and decimal, `_` separators, a repeated
 * address and CRLF line endings (the toolkit documents PowerShell). */
static void check_parses_the_documented_shape(void) {
    const char *path = "./t_hints_shape.toml";
    const char *text =
        "# Per-title hints, kept in the game's repository.\r\n"
        "\r\n"
        "[[label]]\r\n"
        "at = [\r\n"
        "  0x0031D93C,   # int ladder\r\n"
        "  0x0031D9_44,\r\n"
        "  0x0031D94C,\r\n"
        "  3266900,      # 0x0031D954, in decimal\r\n"
        "  0x0031D93C,   # listed twice: not an error, not a second entry\r\n"
        "]\r\n"
        "\r\n"
        "[[label]]\r\n"
        "at = [0x0031D974]\r\n";

    CHECK(write_file(path, text) == 0, "the hints file is written");

    a_hints h;
    char err[512];
    CHECK(a_hints_load(path, &h, err, sizeof err) == 0, "the documented shape loads: %s", err);
    CHECK(h.nlabels == 5, "five distinct addresses, got %d", h.nlabels);
    if (h.nlabels == 5) {
        CHECK(h.labels[0] == 0x0031D93Cu, "first address is hex");
        CHECK(h.labels[1] == 0x0031D944u, "`_` is a digit separator");
        CHECK(h.labels[2] == 0x0031D94Cu, "third address");
        CHECK(h.labels[3] == 0x0031D954u, "a decimal address is the same address");
        CHECK(h.labels[4] == 0x0031D974u, "a second [[label]] table adds to the first");
    }
    a_hints_free(&h);
    remove(path);
}

static void check_no_file_is_not_a_failure(void) {
    a_hints h;
    char err[512];
    CHECK(a_hints_load(NULL, &h, err, sizeof err) == 0, "no hints file loads as empty");
    CHECK(h.nlabels == 0, "and carries no addresses");
    a_hints_free(&h);
}

/* ---- what the parser refuses --------------------------------------------- */

static void check_rejects_with_the_reason_and_the_line(void) {
    struct {
        const char *name;
        const char *text;
        const char *want;    /* substring the message must carry */
        int         line;    /* the line the message must name */
    } cases[] = {
        { "t_hints_err_table.toml",
          "[[label]]\nat = [0x08804000]\n\n[[force-code]]\nat = [0x08804000]\n",
          "unknown hints table", 4 },
        { "t_hints_err_key.toml",
          "[[label]]\nat = [0x08804000]\nsize = 4\n",
          "unknown key", 3 },
        { "t_hints_err_align.toml",
          "[[label]]\nat = [0x08804002]\n",
          "not 4-byte aligned", 2 },
        { "t_hints_err_top.toml",
          "at = [0x08804000]\n",
          "outside a table", 1 },
        { "t_hints_err_open.toml",
          "[[label]]\nat = [0x08804000\n",
          "not closed", 2 },
        { "t_hints_err_array.toml",
          "[[label]]\nat = 0x08804000\n",
          "array", 2 },
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        CHECK(write_file(cases[i].name, cases[i].text) == 0, "the bad file is written");

        a_hints h;
        char err[512];
        int rc = a_hints_load(cases[i].name, &h, err, sizeof err);
        CHECK(rc != 0, "%s is rejected", cases[i].name);
        CHECK(strstr(err, cases[i].want) != NULL,
              "%s: the message says why (%s), got: %s", cases[i].name, cases[i].want, err);
        char at[64];
        snprintf(at, sizeof at, ":%d:", cases[i].line);
        CHECK(strstr(err, at) != NULL,
              "%s: the message names line %d, got: %s", cases[i].name, cases[i].line, err);
        CHECK(h.nlabels == 0 && h.labels == NULL,
              "%s: a failed load leaves nothing behind", cases[i].name);
        remove(cases[i].name);
    }
}

/* ---- what it does to emission ------------------------------------------- */

/* Emit CODE with `hints` (or none), returning the generated unit as text.
 * `register_out`, when given, receives the generated registration file. */
static char *emit_synthetic(const char *prefix, const a_hints *hints, char **register_out) {
    static uint8_t code[sizeof CODE];
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
    if (a_discover(&an, &seed, 1) != 0) {
        printf("FAIL discovery failed for %s\n", prefix);
        failures++;
        a_analysis_free(&an);
        return NULL;
    }
    CHECK(an.nfuncs == 1, "%s: one function, got %d", prefix, an.nfuncs);

    emit_opts o;
    memset(&o, 0, sizeof o);
    o.outdir = ".";
    o.prefix = prefix;
    o.module = "synthetic";
    o.hints = hints;

    int rc = a_emit(&an, &o);
    a_analysis_free(&an);
    if (rc != 0) {
        printf("FAIL emission failed for %s\n", prefix);
        failures++;
        return NULL;
    }

    char path[128];
    snprintf(path, sizeof path, "./%s_funcs_000.c", prefix);
    char *src = slurp(path);

    if (register_out) {
        snprintf(path, sizeof path, "./%s_register.c", prefix);
        *register_out = slurp(path);
    }
    return src;
}

static void check_a_hint_becomes_a_dispatch_entry(void) {
    a_hints hints;
    memset(&hints, 0, sizeof hints);
    uint32_t forced = HINTED;
    hints.labels = &forced;
    hints.nlabels = 1;

    char *reg = NULL;
    char *with = emit_synthetic("t_hint_on", &hints, &reg);
    char *without = emit_synthetic("t_hint_off", NULL, NULL);

    CHECK(with != NULL && without != NULL, "both emissions produced a unit");
    if (with && without) {
        /* The contract from the issue, all three parts of it. */
        char case_line[64];
        snprintf(case_line, sizeof case_line, "case 0x%08Xu: goto L_%08X;", HINTED, HINTED);
        expect_contains(with, case_line, "the hinted address is a case in the entry switch");

        char label[64];
        snprintf(label, sizeof label, "L_%08X: PSP_MARK(0x%08Xu);", HINTED, HINTED);
        expect_contains(with, label, "and carries its own label");

        char thunk[128];
        snprintf(thunk, sizeof thunk,
                 "void psp_at_%08X(void) { psp_body_%08X(0x%08Xu); }", HINTED, BASE, HINTED);
        expect_contains(with, thunk, "and a thunk that enters the body there");

        char regline[96];
        snprintf(regline, sizeof regline, "psp_register_label(0x%08Xu, psp_at_%08X);",
                 HINTED, HINTED);
        expect_contains(reg, regline, "and a dispatch registration for it");

        /* The control: without the file the same body is emitted, and none of
         * this exists. If these ever pass with the file removed, the assertions
         * above prove nothing. */
        expect_absent(without, "psp_at_", "without hints no entry thunk is emitted");
        expect_absent(without, case_line, "without hints there is no case for it");
        expect_absent(without, regline, "without hints it is not registered");
        expect_contains(without, "L_08804000: PSP_MARK(0x08804000u);",
                        "the entry label is still there without hints");
    }

    free(with);
    free(without);
    free(reg);
}

/* A hint for an address that belongs to no discovered function is reported and
 * does not stop the build: a game repository may carry hints for code only one
 * entry point reaches. The test can only assert that the emission survives and
 * that no entry was invented for it. */
static void check_a_hint_outside_every_function_is_survivable(void) {
    a_hints hints;
    memset(&hints, 0, sizeof hints);
    uint32_t stray = 0x08805000u;   /* nowhere near the synthetic image */
    hints.labels = &stray;
    hints.nlabels = 1;

    char *reg = NULL;
    char *src = emit_synthetic("t_hint_stray", &hints, &reg);

    CHECK(src != NULL, "an address outside the image does not stop the emission");
    expect_absent(src, "psp_at_08805000", "no entry is invented for an address nothing owns");
    expect_absent(reg, "psp_register_label(0x08805000u", "and nothing is registered for it");

    free(src);
    free(reg);
}

int main(void) {
    check_no_file_is_not_a_failure();
    check_parses_the_documented_shape();
    check_rejects_with_the_reason_and_the_line();
    check_a_hint_becomes_a_dispatch_entry();
    check_a_hint_outside_every_function_is_survivable();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("all hints-file checks passed (synthetic code, no game data)\n");
    return 0;
}
