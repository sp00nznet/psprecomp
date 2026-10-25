/* Synthetic paths and a CMake-created file, no game data. */
#include "psprecomp/hle.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c, message) do { if (!(c)) { \
    printf("FAIL line %d: %s\n", __LINE__, message); failures++; } } while (0)

static uint32_t open_guest(uint32_t path) {
    psp_cpu.r[PSP_REG_A0] = path;
    psp_cpu.r[PSP_REG_A1] = 1;  /* PSP_O_RDONLY */
    psp_cpu.r[PSP_REG_A2] = 0;
    psp_hle_call(0x109F50BCu);
    return psp_cpu.r[PSP_REG_V0];
}
static void close_guest(uint32_t fd) {
    if ((int32_t)fd < 3) return;
    psp_cpu.r[PSP_REG_A0] = fd;
    psp_hle_call(0x810C4BC3u);
}
int main(void) {
    if (psp_mem_init() != 0) return 1;
    if (psp_mem_map_module(0, 4096) != 0) return 1;
    psp_cpu_reset();
    psp_hle_init();
    psp_io_set_root(PSP_TEST_IO_ROOT);
    const uint32_t PATH = 0x08850000u;
    const char name[] = "disc0:/probe.txt";
    CHECK(psp_mem_write_block(PATH, name, sizeof name) == 0, "prepare valid path");
    uint32_t fd = open_guest(PATH);
    CHECK((int32_t)fd >= 3, "the fixture file opens");
    close_guest(fd);

    /* On POSIX, fopen(root/disc/, "rb") succeeds. A missing root would hide
     * the bug by returning ENOENT before the guest-path guard is exercised. */
    psp_write8(PATH, 0);
    fd = open_guest(PATH);
    CHECK(fd == 0x80010002u, "empty path returns ENOENT, not a directory descriptor");
    close_guest(fd);

    /* PRX modules may map address zero. A null argument must still not be
     * interpreted as a filename stored in the module's first bytes. */
    CHECK(psp_mem_write_block(0, name, sizeof name) == 0, "prepare mapped address zero");
    uint64_t before = psp_mem_bad_access;
    fd = open_guest(0);
    CHECK((int32_t)fd < 0, "null path is rejected even if address zero is mapped");
    close_guest(fd);
    CHECK(psp_mem_bad_access == before, "rejecting null path needs no guest read");

    CHECK(psp_mem_write_block(PATH, name, sizeof name) == 0, "restore valid path");
    fd = open_guest(PATH);
    CHECK((int32_t)fd >= 3, "valid paths still open after failures");
    close_guest(fd);
    psp_io_reset();
    psp_mem_free();
    printf("io_open: %d failures\n", failures);
    return failures ? 1 : 0;
}
