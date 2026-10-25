/* Controller samples and guest buffers only, no game data. */
#include "psprecomp/hle.h"
#include "psprecomp/dispatch.h"
#include <stdio.h>

static int failures;
#define CHECK(c, s) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, s); failures++; } } while (0)
#define CROSS 0x4000u
#define CIRCLE 0x2000u
#define OUT 0x08851004u

static uint32_t latch(int peek, uint32_t out) {
    psp_cpu.r[PSP_REG_A0] = out;
    psp_hle_call(peek ? 0xB1D0E5CDu : 0x0B588501u);
    return psp_cpu.r[PSP_REG_V0];
}
static void masks(uint32_t make, uint32_t brk, uint32_t press, uint32_t release) {
    CHECK(psp_read32(OUT) == make, "make mask");
    CHECK(psp_read32(OUT + 4) == brk, "break mask");
    CHECK(psp_read32(OUT + 8) == press, "press mask");
    CHECK(psp_read32(OUT + 12) == release, "release mask");
}
static void clean(void) { psp_misc_reset(); }
static int injected;
static void sample_during_wait(void) {
    if (!injected) {
        psp_ctrl_set(CIRCLE, 128, 128);
        injected = 1;
    }
}
int main(void) {
    if (psp_mem_init() != 0) return 1;
    psp_cpu_reset();
    psp_hle_init();
    psp_write32(OUT - 4, 0x12345678u);
    psp_write32(OUT + 16, 0x87654321u);
    clean();
    psp_ctrl_set(CROSS, 20, 220);
    CHECK(latch(0, OUT) == 1, "one submitted sample is consumed");
    masks(CROSS, 0, CROSS, ~CROSS);
    CHECK(psp_read32(OUT - 4) == 0x12345678u && psp_read32(OUT + 16) == 0x87654321u,
          "exactly 16 output bytes");
    CHECK(latch(1, OUT) == 0, "peek after read has no samples");
    masks(0, 0, 0, 0);
    psp_ctrl_set(CROSS, 128, 128);
    CHECK(latch(1, OUT) == 1, "peek sees one held sample");
    masks(0, 0, CROSS, ~CROSS);
    CHECK(latch(1, OUT) == 1, "peek does not consume");
    CHECK(latch(0, OUT) == 1, "read consumes after peek");
    masks(0, 0, CROSS, ~CROSS);
    psp_ctrl_set(0, 128, 128);
    CHECK(latch(0, OUT) == 1, "release sample consumed");
    masks(0, CROSS, 0, UINT32_MAX);
    clean();
    psp_ctrl_set(CROSS, 128, 128);
    psp_ctrl_set(0, 128, 128);
    psp_ctrl_set(CIRCLE, 128, 128);
    CHECK(latch(0, OUT) == 3, "samples accumulate, not only the last transition");
    masks(CROSS | CIRCLE, CROSS, CROSS | CIRCLE, UINT32_MAX);
    clean();
    psp_ctrl_set(CROSS, 30, 90);
    psp_ctrl_set(CROSS, 90, 30);
    CHECK(latch(0, OUT) == 2, "unchanged digital buttons still count as samples");
    masks(CROSS, 0, CROSS, ~CROSS);
    clean();
    psp_ctrl_set(CROSS, 128, 128);
    const uint32_t edge = PSP_RAM_BASE + PSP_RAM_SIZE - 15;
    for (unsigned i = 0; i < 15; i++) psp_write8(edge + i, 0xA5);
    uint64_t bad = psp_mem_bad_access;
    CHECK((int32_t)latch(0, edge) < 0, "partial output rejected");
    for (unsigned i = 0; i < 15; i++) CHECK(psp_read8(edge + i) == 0xA5, "partial output not written");
    CHECK((int32_t)latch(0, 0) < 0, "null output rejected");
    CHECK((int32_t)latch(1, 0x03000000u) < 0, "unmapped peek output rejected");
    CHECK(latch(0, OUT) == 1, "invalid buffers do not consume pending samples");
    masks(CROSS, 0, CROSS, ~CROSS);
    CHECK(psp_mem_bad_access == bad, "invalid arguments cause no guest memory fault");
    /* Empty reads wait for a frame, then sample the current input. */
    uint64_t before = psp_threadman_vtime_us();
    CHECK(latch(0, OUT) == 1, "empty read obtains the next sample");
    CHECK(psp_threadman_vtime_us() > before, "empty read yields instead of spinning");
    masks(0, 0, CROSS, ~CROSS);
    psp_ctrl_set(CIRCLE, 128, 128);  /* leave an unread sample for reset */
    clean();
    CHECK(latch(1, OUT) == 0, "reset clears pending samples");
    masks(0, 0, 0, 0);
    CHECK(latch(0, OUT) == 1, "idle read obtains a released sample");
    masks(0, 0, 0, UINT32_MAX);
    /* The frame-wait path can receive input while the latch lock is released.
     * Use its existing interrupt-delivery hook to submit exactly one sample. */
    clean();
    psp_register(0x08802000u, sample_during_wait);
    psp_cpu.r[PSP_REG_A0] = 1;
    psp_cpu.r[PSP_REG_A1] = 2;
    psp_cpu.r[PSP_REG_A2] = 0x08802000u;
    psp_cpu.r[PSP_REG_A3] = 0;
    psp_hle_call(0xCA04A2B9u);
    CHECK(psp_cpu.r[PSP_REG_V0] == 0, "register input injection during wait");
    psp_cpu.r[PSP_REG_A0] = 1;
    psp_cpu.r[PSP_REG_A1] = 2;
    psp_hle_call(0xFB8E22ECu);
    CHECK(psp_cpu.r[PSP_REG_V0] == 0, "enable input injection during wait");
    CHECK(latch(0, OUT) == 1, "sample arriving during wait is not duplicated by fallback");
    CHECK(injected == 1, "frame wait delivered the input sample");
    masks(CIRCLE, 0, CIRCLE, ~CIRCLE);
    psp_intrman_reset();
    clean();
    /* The pre-existing positive-buffer API keeps buttons, analog and layout. */
    psp_ctrl_set(CIRCLE, 12, 234);
    psp_cpu.r[PSP_REG_A0] = OUT;
    psp_cpu.r[PSP_REG_A1] = 1;
    psp_hle_call(0x1F803938u);
    CHECK(psp_cpu.r[PSP_REG_V0] == 1, "positive buffer return unchanged");
    CHECK(psp_read32(OUT + 4) == CIRCLE, "positive buffer buttons unchanged");
    CHECK(psp_read8(OUT + 8) == 12 && psp_read8(OUT + 9) == 234, "analog values unchanged");
    for (unsigned i = 10; i < 16; i++) CHECK(psp_read8(OUT + i) == 0, "positive buffer padding unchanged");
    CHECK(psp_read32(OUT) == 0, "positive buffer starts at timestamp zero after reset");
    psp_cpu.r[PSP_REG_A0] = OUT;
    psp_cpu.r[PSP_REG_A1] = 2;
    psp_hle_call(0x1F803938u);
    CHECK(psp_cpu.r[PSP_REG_V0] == 2, "positive buffer still returns requested count");
    CHECK(psp_read32(OUT) == 1 && psp_read32(OUT + 16) == 2, "timestamps advance per returned record");
    CHECK(psp_read32(OUT + 20) == CIRCLE, "second positive record keeps the button snapshot");
    psp_cpu.r[PSP_REG_A0] = OUT;
    psp_cpu.r[PSP_REG_A1] = 0;
    psp_hle_call(0x1F803938u);
    CHECK(psp_cpu.r[PSP_REG_V0] == 1 && psp_read32(OUT) == 3, "zero-count compatibility is unchanged");
    CHECK(latch(0, OUT) == 1, "positive buffer reads do not consume latch samples");
    masks(CIRCLE, 0, CIRCLE, ~CIRCLE);
    psp_misc_reset();
    psp_mem_free();
    printf("ctrl_latch: %d failures\n", failures);
    return failures ? 1 : 0;
}
