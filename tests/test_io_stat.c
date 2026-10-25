/* Real host files, synthetic guest buffers, no game data. */
#include "psprecomp/hle.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

static int failures;
#define CHECK(c, message) do { if (!(c)) { \
    printf("FAIL line %d: %s\n", __LINE__, message); failures++; } } while (0)
static const uint32_t PATH = 0x08850000u, OUT = 0x08851004u;

static void put_path(const char *s) {
    psp_mem_write_block(PATH, s, (uint32_t)strlen(s) + 1);
}
static uint32_t getstat(uint32_t path, uint32_t out) {
    psp_cpu.r[PSP_REG_A0] = path;
    psp_cpu.r[PSP_REG_A1] = out;
    psp_hle_call(0xACE946E8u);
    return psp_cpu.r[PSP_REG_V0];
}
static void poison(void) {
    for (unsigned i = 0; i < 96; i++) psp_write8(OUT - 4 + i, 0xA5);
}
static int untouched(uint32_t at, unsigned size) {
    for (unsigned i = 0; i < size; i++) if (psp_read8(at + i) != 0xA5) return 0;
    return 1;
}
static void date_matches(uint32_t at, time_t value) {
    struct tm expected = {0};
#ifdef _WIN32
    CHECK(gmtime_s(&expected, &value) == 0, "convert host timestamp");
#else
    CHECK(gmtime_r(&value, &expected) != NULL, "convert host timestamp");
#endif
    CHECK(psp_read16(at) == expected.tm_year + 1900, "timestamp year");
    CHECK(psp_read16(at + 2) == expected.tm_mon + 1, "timestamp month");
    CHECK(psp_read16(at + 4) == expected.tm_mday, "timestamp day");
    CHECK(psp_read16(at + 6) == expected.tm_hour, "timestamp hour (UTC)");
    CHECK(psp_read16(at + 8) == expected.tm_min, "timestamp minute");
    CHECK(psp_read16(at + 10) == expected.tm_sec, "timestamp second");
    CHECK(psp_read32(at + 12) == 0, "host stat has second precision");
}
#ifdef __APPLE__
/* Registered separately by CMake, only on macOS. No large-file allocation or
 * implicit skip on Windows; that platform's >4 GiB coverage remains pending. */
static void large_file(void) {
    const uint64_t large_size = (UINT64_C(5) << 30) + 123;
    FILE *f = fopen(PSP_TEST_IO_ROOT "/disc/large.bin", "wb");
    CHECK(f != NULL, "create sparse file");
    if (!f) return;
    int seek_ok = fseeko(f, (off_t)(large_size - 1), SEEK_SET);
    CHECK(seek_ok == 0 && fputc(0, f) != EOF, "extend sparse file");
    CHECK(fclose(f) == 0, "close sparse file");
    put_path("umd0:/large.bin");
    poison();
    CHECK(getstat(PATH, OUT) == 0, "UMD alias resolves real file");
    uint64_t actual = psp_read32(OUT + 8) | ((uint64_t)psp_read32(OUT + 12) << 32);
    CHECK(actual == large_size, "size above 4 GiB is not truncated");
    CHECK(remove(PSP_TEST_IO_ROOT "/disc/large.bin") == 0, "remove sparse file");
}
#endif

int main(int argc, char **argv) {
    CHECK(psp_mem_init() == 0, "memory init");
    if (failures) return 1;
    psp_cpu_reset();
    psp_hle_init();
    psp_io_set_root(PSP_TEST_IO_ROOT);
    uint64_t bad_before = psp_mem_bad_access;
    if (argc > 1) {
#ifdef __APPLE__
        if (argc == 2 && !strcmp(argv[1], "--large-file")) large_file();
        else CHECK(0, "unknown test argument");
#else
        (void)argv;
        CHECK(0, "large-file test is only registered on macOS");
#endif
        CHECK(psp_mem_bad_access == bad_before, "large file causes no guest memory faults");
        psp_mem_free();
        printf("io_stat_large_file: %d failures\n", failures);
        return failures ? 1 : 0;
    }
    put_path("disc0:/probe.txt");
    poison();
    CHECK(getstat(PATH, OUT) == 0, "existing file succeeds");
    CHECK((psp_read32(OUT) & 0xF000u) == 0x2000u, "regular-file mode");
    CHECK((psp_read32(OUT + 4) & 0x38u) == 0x20u, "regular-file attribute");
    CHECK(psp_read32(OUT + 8) == 2 && psp_read32(OUT + 12) == 0, "actual 64-bit file size");
    CHECK(untouched(OUT - 4, 4) && untouched(OUT + 88, 4), "exactly 88 output bytes");
    for (unsigned i = 64; i < 88; i++) CHECK(psp_read8(OUT + i) == 0, "no invented device-specific metadata");
#ifdef _WIN32
    struct _stat64 st = {0};
    CHECK(_stat64(PSP_TEST_IO_ROOT "/disc/probe.txt", &st) == 0, "host stat");
#else
    struct stat st = {0};
    CHECK(stat(PSP_TEST_IO_ROOT "/disc/probe.txt", &st) == 0, "host stat");
#endif
    date_matches(OUT + 48, st.st_mtime);
    date_matches(OUT + 32, st.st_atime);
#ifdef __APPLE__
    date_matches(OUT + 16, st.st_birthtime);
#else
    date_matches(OUT + 16, st.st_ctime);
#endif
    /* A mapped but incomplete output must be rejected atomically. */
    const uint32_t EDGE = PSP_RAM_BASE + PSP_RAM_SIZE - 87u;
    for (unsigned i = 0; i < 87; i++) psp_write8(EDGE + i, 0xA5);
    CHECK((int32_t)getstat(PATH, EDGE) < 0, "truncated output rejected");
    CHECK(untouched(EDGE, 87), "truncated output left untouched");
    CHECK((int32_t)getstat(PATH, 0) < 0, "null output rejected");
    CHECK((int32_t)getstat(PATH, 0x03000000u) < 0, "unmapped output rejected");
    poison();
    CHECK((int32_t)getstat(0, OUT) < 0, "null path rejected");
    CHECK(untouched(OUT, 88), "null path leaves output alone");
    CHECK((int32_t)getstat(0x03000000u, OUT) < 0, "unmapped path rejected");
    put_path("disc0:/missing.txt");
    CHECK(getstat(PATH, OUT) == 0x80010002u, "missing file returns ENOENT");
    CHECK(untouched(OUT, 88), "missing file leaves output alone");
    put_path("");
    CHECK((int32_t)getstat(PATH, OUT) < 0, "empty path rejected");
    CHECK(untouched(OUT, 88), "empty path leaves output alone");
    for (unsigned i = 0; i < 512; i++) psp_write8(PATH + i, 'x');
    CHECK((int32_t)getstat(PATH, OUT) < 0, "unterminated path rejected");
    psp_write8(PATH + 511, 0);
    CHECK((int32_t)getstat(PATH, OUT) < 0, "maximum-length absent path fails safely");
    CHECK((int32_t)getstat(EDGE, OUT) < 0, "path crossing end of RAM rejected");
    CHECK(untouched(OUT, 88), "path boundary errors leave output alone");
    /* A nested directory uses the same native metadata path as a file. */
    put_path("disc0:/subdir");
    CHECK(getstat(PATH, OUT) == 0, "nested directory succeeds");
    CHECK((psp_read32(OUT) & 0xF000u) == 0x1000u, "directory mode");
    CHECK((psp_read32(OUT + 4) & 0x38u) == 0x10u, "directory attribute");
    CHECK(psp_mem_bad_access == bad_before, "invalid arguments cause no guest memory faults");
    psp_mem_free();
    printf("io_stat: %d failures\n", failures);
    return failures ? 1 : 0;
}
