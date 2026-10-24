/* Discovery tests -- synthetic code only, no game data.
 *
 * Jump-table recovery. A `switch` compiles to a bound check, a scaled index,
 * a table load and `jr`; resolving one turns every case into a discovered
 * entry. Each block below is a hand-assembled switch with its table placed in
 * the image after the code, followed by one more word that looks like a code
 * pointer, so a resolver that ignores the bound reads one entry too many.
 */

#include "analyze.h"

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

#define BASE  0x08804000u
#define TABLE 0x08804100u      /* %hi 0x0880, %lo 0x4100 */

static uint8_t image[0x200];

static void put32(uint32_t addr, uint32_t w) {
    uint8_t *p = image + (addr - BASE);
    p[0] = (uint8_t)w; p[1] = (uint8_t)(w >> 8); p[2] = (uint8_t)(w >> 16); p[3] = (uint8_t)(w >> 24);
}

/* Lay out `code` at BASE and `table` at TABLE, discover from BASE. */
static void run(a_analysis *an, const uint32_t *code, int ncode,
                const uint32_t *table, int ntable) {
    memset(image, 0, sizeof image);
    for (int i = 0; i < ncode; i++)  put32(BASE + (uint32_t)i * 4, code[i]);
    for (int i = 0; i < ntable; i++) put32(TABLE + (uint32_t)i * 4, table[i]);

    memset(an, 0, sizeof *an);
    an->code = image;
    an->base = BASE;
    an->size = (uint32_t)ncode * 4;
    an->image = image;
    an->image_base = BASE;
    an->image_size = sizeof image;

    uint32_t seed = BASE;
    CHECK(a_discover(an, &seed, 1) == 0, "discovery runs");
}

static int is_func(const a_analysis *an, uint32_t addr) {
    for (int i = 0; i < an->nfuncs; i++) if (an->funcs[i].addr == addr) return 1;
    return 0;
}

/* The table address is `lui` alone, with %lo(table) in the lw displacement --
 * the form GCC emits for most switches. An unrelated lui/addiu pair writes the
 * index register before it is reloaded: a search for "the nearest addiu that
 * writes an operand" walks past the reload and builds the table address from
 * that pair instead. */
static const uint32_t LUI_LW_LO[] = {
    0x3C030881u,   /* 08804000  lui   $v1, 0x0881          decoy pair: $a0 = */
    0x24640100u,   /* 08804004  addiu $a0, $v1, 0x100        0x08810100, dead  */
    0x8CC40000u,   /* 08804008  lw    $a0, 0($a2)          the real key      */
    0x2C850003u,   /* 0880400C  sltiu $a1, $a0, 3          three cases       */
    0x10A0000Du,   /* 08804010  beq   $a1, $zero, 08804048 -> default        */
    0x00000000u,   /* 08804014  nop                                          */
    0x00042080u,   /* 08804018  sll   $a0, $a0, 2                            */
    0x3C010880u,   /* 0880401C  lui   $at, 0x0880          %hi(table)        */
    0x00240821u,   /* 08804020  addu  $at, $at, $a0                          */
    0x8C214100u,   /* 08804024  lw    $at, 0x4100($at)     %lo(table)        */
    0x00200008u,   /* 08804028  jr    $at                                    */
    0x00000000u,   /* 0880402C  nop                                          */
    0x03E00008u, 0x00000000u,   /* 08804030  case 0 */
    0x03E00008u, 0x00000000u,   /* 08804038  case 1 */
    0x03E00008u, 0x00000000u,   /* 08804040  case 2 */
    0x03E00008u, 0x00000000u,   /* 08804048  default */
};
static const uint32_t LUI_LW_LO_TABLE[] = {
    0x08804030u, 0x08804038u, 0x08804040u,
    0x08804048u,   /* past the bound: a valid code pointer, but not a case */
};

/* The table address is `lui` + `addiu`, with a displacement of 0. */
static const uint32_t LUI_ADDIU[] = {
    0x2C850002u,   /* 08804000  sltiu $a1, $a0, 2          two cases         */
    0x10A0000Cu,   /* 08804004  beq   $a1, $zero, 08804038 -> default        */
    0x00000000u,   /* 08804008  nop                                          */
    0x3C0B0880u,   /* 0880400C  lui   $t3, 0x0880                            */
    0x00044080u,   /* 08804010  sll   $t0, $a0, 2                            */
    0x256A4100u,   /* 08804014  addiu $t2, $t3, 0x4100                       */
    0x010A4021u,   /* 08804018  addu  $t0, $t0, $t2                          */
    0x8D090000u,   /* 0880401C  lw    $t1, 0($t0)                            */
    0x01200008u,   /* 08804020  jr    $t1                                    */
    0x00000000u,   /* 08804024  nop                                          */
    0x03E00008u, 0x00000000u,   /* 08804028  case 0 */
    0x03E00008u, 0x00000000u,   /* 08804030  case 1 */
    0x03E00008u, 0x00000000u,   /* 08804038  default */
};
static const uint32_t LUI_ADDIU_TABLE[] = {
    0x08804028u, 0x08804030u,
    0x08804038u,   /* past the bound */
};

static void check_jump_tables(void) {
    a_analysis an;

    run(&an, LUI_LW_LO, 20, LUI_LW_LO_TABLE, 4);
    CHECK(an.ntables == 1, "lui/lw %%lo: one table, got %d", an.ntables);
    CHECK(an.ntable_targets == 3, "lui/lw %%lo: three cases (the sltiu bound), got %d",
          an.ntable_targets);
    CHECK(an.nindirects == 0, "lui/lw %%lo: no unresolved site, got %d", an.nindirects);
    CHECK(is_func(&an, 0x08804030u) && is_func(&an, 0x08804038u) && is_func(&an, 0x08804040u),
          "lui/lw %%lo: every case is discovered");
    a_analysis_free(&an);

    run(&an, LUI_ADDIU, 16, LUI_ADDIU_TABLE, 3);
    /* Discovery re-walks after resolving, and meets the site again: it is
     * still one table. */
    CHECK(an.ntables == 1, "lui/addiu: one table, counted once, got %d", an.ntables);
    CHECK(an.ntable_targets == 2, "lui/addiu: two cases (the sltiu bound), got %d",
          an.ntable_targets);
    CHECK(an.nindirects == 0, "lui/addiu: no unresolved site, got %d", an.nindirects);
    a_analysis_free(&an);
}

int main(void) {
    check_jump_tables();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("all analysis checks passed (synthetic code, no game data)\n");
    return 0;
}
