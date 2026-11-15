/* Container tests. Every buffer here is built in the test itself — there is no
 * disc image, no PBP, and no game module in this repo. The point is that the
 * parsers reject malformed input rather than walking off the end of it, which
 * is the failure mode that matters when you point the tool at 4,000 dumps.
 */

#include "container.h"

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

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

static void test_sniff(void) {
    uint8_t buf[16];

    memset(buf, 0, sizeof buf);
    memcpy(buf, "\x7f" "ELF", 4);
    CHECK(psp_sniff(buf, 16) == FMT_ELF, "sniff ELF");

    memcpy(buf, "~PSP", 4);
    CHECK(psp_sniff(buf, 16) == FMT_PSP, "sniff ~PSP");

    memcpy(buf, "~SCE", 4);
    CHECK(psp_sniff(buf, 16) == FMT_SCE, "sniff ~SCE");

    memcpy(buf, "\0PBP", 4);
    CHECK(psp_sniff(buf, 16) == FMT_PBP, "sniff PBP");

    memcpy(buf, "junk", 4);
    CHECK(psp_sniff(buf, 16) == FMT_UNKNOWN, "sniff unknown");

    /* A short buffer must not be read past. */
    CHECK(psp_sniff(buf, 2) == FMT_UNKNOWN, "sniff refuses a short buffer");
}

static void test_pbp(void) {
    /* A PBP is a header of eight offsets; segment sizes are the gaps between
     * them. This is the layout WTF's game-sharing microgames use. */
    uint8_t buf[0x100];
    memset(buf, 0, sizeof buf);
    memcpy(buf, "\0PBP", 4);
    put32(buf + 4, 0x00010000);

    /* PARAM.SFO at 0x28 (len 0x18), then an empty ICON0, then DATA.PSP. */
    const uint32_t offsets[PBP_NUM_SEGMENTS] = {
        0x28, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x80
    };
    for (int i = 0; i < PBP_NUM_SEGMENTS; i++)
        put32(buf + 8 + i * 4, offsets[i]);

    pbp_info p;
    CHECK(pbp_parse(buf, sizeof buf, &p) == 0, "parse a well-formed PBP");
    CHECK(p.size[0] == 0x18, "PARAM.SFO size derived from the next offset: got %u", p.size[0]);
    CHECK(p.size[1] == 0, "an empty segment has zero size");
    CHECK(p.size[6] == 0x40, "DATA.PSP size: got %u", p.size[6]);
    CHECK(p.size[7] == sizeof buf - 0x80, "the last segment runs to EOF: got %u", p.size[7]);

    /* Garbage must be rejected, not parsed into nonsense. */
    memcpy(buf, "XXXX", 4);
    CHECK(pbp_parse(buf, sizeof buf, &p) != 0, "reject a bad PBP magic");
    memcpy(buf, "\0PBP", 4);
    CHECK(pbp_parse(buf, 8, &p) != 0, "reject a truncated PBP header");
}

static void test_psp_header(void) {
    uint8_t buf[PSP_HEADER_SIZE];
    memset(buf, 0, sizeof buf);
    memcpy(buf, "~PSP", 4);
    put16(buf + 0x04, 0x0000);          /* mod_attr */
    put16(buf + 0x06, 0x0002);          /* comp_attr */
    buf[0x08] = 0x01;                   /* ver lo */
    buf[0x09] = 0x01;                   /* ver hi */
    memcpy(buf + 0x0A, "testmodule", 10);
    buf[0x27] = 2;                      /* nsegments */
    put32(buf + 0x28, 0x00100000);      /* elf_size */
    put32(buf + 0x2C, 0x00080000);      /* psp_size */
    put32(buf + 0x30, 0x08900000);      /* boot_entry */
    put32(buf + 0x44, 0x08900000);      /* seg 0 address */
    put32(buf + 0x54, 0x00040000);      /* seg 0 size */
    /* The key tag lives at 0xD0. It is deliberately pinned here: an earlier
     * revision read it from 0x130, which lands in the encrypted key material
     * and yields a different plausible-looking value per module. */
    put32(buf + 0xD0, 0x4C949AF0);
    put32(buf + 0x130, 0xDEADBEEF);     /* must NOT be mistaken for the tag */

    psp_header h;
    CHECK(psp_header_parse(buf, sizeof buf, &h) == 0, "parse a ~PSP header");
    CHECK(strcmp(h.modname, "testmodule") == 0, "module name: got \"%s\"", h.modname);
    CHECK(h.nsegments == 2, "segment count");
    CHECK(h.elf_size == 0x00100000, "decrypted ELF size");
    CHECK(h.boot_entry == 0x08900000, "boot entry");
    CHECK(h.seg_address[0] == 0x08900000, "segment 0 address");
    CHECK(h.tag == 0x4C949AF0, "key tag read from 0xD0, not 0x130");

    /* The module name field is not guaranteed NUL-terminated on disc; the
     * parser must terminate it itself rather than running into the next field. */
    memset(buf + 0x0A, 'A', 28);
    CHECK(psp_header_parse(buf, sizeof buf, &h) == 0, "parse with a full name field");
    CHECK(strlen(h.modname) == 28, "over-long module name is terminated at 28");

    CHECK(psp_header_parse(buf, 16, &h) != 0, "reject a truncated ~PSP header");
}

static void test_elf(void) {
    /* Minimal ELF32 LE MIPS with one executable PT_LOAD. */
    uint8_t buf[256];
    memset(buf, 0, sizeof buf);
    memcpy(buf, "\x7f" "ELF", 4);
    buf[4] = 1;                          /* ELFCLASS32 */
    buf[5] = 1;                          /* ELFDATA2LSB */
    put16(buf + 16, ET_PSP_PRX);         /* e_type */
    put32(buf + 24, 0x08900010);         /* e_entry */
    put32(buf + 28, 52);                 /* e_phoff */
    put16(buf + 42, 32);                 /* e_phentsize */
    put16(buf + 44, 1);                  /* e_phnum */

    uint8_t *ph = buf + 52;
    put32(ph +  0, 1);                   /* PT_LOAD */
    put32(ph +  4, 0x80);                /* p_offset */
    put32(ph +  8, 0x08900000);          /* p_vaddr */
    put32(ph + 16, 0x40);                /* p_filesz */
    put32(ph + 20, 0x40);                /* p_memsz */
    put32(ph + 24, 0x5);                 /* PF_R | PF_X */

    elf_info e;
    CHECK(elf_parse(buf, sizeof buf, &e) == 0, "parse a minimal PRX");
    CHECK(e.type == ET_PSP_PRX, "e_type is PRX");
    CHECK(e.entry == 0x08900010, "entry point");
    CHECK(e.nsegments == 1, "one PT_LOAD");
    CHECK(e.text_addr == 0x08900000, "text address picked from the PF_X segment");
    CHECK(e.text_size == 0x40, "text size");
    CHECK(e.text_offset == 0x80, "text file offset");

    /* A non-executable segment must not be mistaken for .text. */
    put32(ph + 24, 0x6);                 /* PF_R | PF_W, no PF_X */
    CHECK(elf_parse(buf, sizeof buf, &e) == 0, "parse with a data-only segment");
    CHECK(e.text_size == 0, "a non-executable segment is not .text");

    /* Wrong class / endianness / magic must all be rejected. */
    put32(ph + 24, 0x5);
    buf[4] = 2;
    CHECK(elf_parse(buf, sizeof buf, &e) != 0, "reject ELFCLASS64");
    buf[4] = 1; buf[5] = 2;
    CHECK(elf_parse(buf, sizeof buf, &e) != 0, "reject big-endian ELF");
    buf[5] = 1;
    CHECK(elf_parse(buf, 8, &e) != 0, "reject a truncated ELF");

    /* A program-header table pointing past the end of the buffer must not be
     * followed — this is the case a corrupt dump actually hits. */
    put32(buf + 28, 0xFFFF0000);
    CHECK(elf_parse(buf, sizeof buf, &e) == 0, "a bogus phoff parses without crashing");
    CHECK(e.nsegments == 0, "a bogus phoff yields no segments");
}

static void test_sfo(void) {
    /* PARAM.SFO: a key table and a data table, both at header-declared offsets.
     * We check that a malformed one is refused rather than walked. */
    uint8_t buf[0x100];
    memset(buf, 0, sizeof buf);
    put32(buf + 0x00, 0x46535000);   /* "\0PSF" */
    put32(buf + 0x04, 0x01010000);   /* version */
    put32(buf + 0x08, 0x30);         /* key table offset */
    put32(buf + 0x0C, 0x50);         /* data table offset */
    put32(buf + 0x10, 1);            /* one entry */

    put16(buf + 0x14, 0);            /* key offset */
    put16(buf + 0x16, 0x0204);       /* UTF-8 */
    put32(buf + 0x18, 10);           /* data length */
    put32(buf + 0x1C, 16);           /* data max */
    put32(buf + 0x20, 0);            /* data offset */

    memcpy(buf + 0x30, "DISC_ID", 8);
    memcpy(buf + 0x50, "ULUS10172", 10);

    /* sfo_dump writes to a FILE*; discard the output, we only care that it
     * accepts a good blob and rejects a bad one without reading out of range. */
    FILE *devnull = tmpfile();
    CHECK(devnull != NULL, "open a scratch file");
    if (devnull) {
        CHECK(sfo_dump(buf, sizeof buf, devnull) == 0, "dump a well-formed SFO");

        put32(buf + 0x00, 0xDEADBEEF);
        CHECK(sfo_dump(buf, sizeof buf, devnull) != 0, "reject a bad SFO magic");

        /* An absurd entry count must be refused, not looped over. */
        put32(buf + 0x00, 0x46535000);
        put32(buf + 0x10, 0xFFFFFFFF);
        CHECK(sfo_dump(buf, sizeof buf, devnull) != 0, "reject an absurd entry count");

        fclose(devnull);
    }
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int has_seed(const uint32_t *seeds, int n, uint32_t val) {
    for (int i = 0; i < n; i++) {
        if (seeds[i] == val) return 1;
    }
    return 0;
}

static void test_prx_relocate_and_seeds(void) {
    /* Synthetic two-segment PRX (e_type 0xFFA0) with SHT_PRXRELOC (0x700000A0)
     * covering R_MIPS_32, R_MIPS_26, HI16/LO16 with >=0x8000 carry, addrbase=1,
     * and ofsbase=1 (patching inside segment 1). */
    uint8_t buf[0x300];
    memset(buf, 0, sizeof buf);
    buf[0] = 0x7F; buf[1] = 'E'; buf[2] = 'L'; buf[3] = 'F';
    buf[4] = 1; buf[5] = 1; buf[6] = 1;
    put16(buf + 0x10, 0xFFA0);          /* e_type = ET_PSP_PRX */
    put16(buf + 0x12, 8);               /* e_machine = EM_MIPS */
    put32(buf + 0x14, 1);               /* e_version */
    put32(buf + 0x18, 0x00000010);      /* e_entry */
    put32(buf + 0x1C, 0x34);            /* e_phoff */
    put32(buf + 0x20, 0x200);           /* e_shoff */
    put16(buf + 0x28, 0x34);            /* e_ehsize */
    put16(buf + 0x2A, 0x20);            /* e_phentsize */
    put16(buf + 0x2C, 2);               /* e_phnum */
    put16(buf + 0x2E, 0x28);            /* e_shentsize */
    put16(buf + 0x30, 3);               /* e_shnum */

    /* Segment 0: .text at vaddr 0x00000000, file 0x100..0x140 */
    uint8_t *ph0 = buf + 0x34;
    put32(ph0 +  0, 1);                 /* PT_LOAD */
    put32(ph0 +  4, 0x100);             /* p_offset */
    put32(ph0 +  8, 0x00000000);        /* p_vaddr */
    put32(ph0 + 16, 0x40);              /* p_filesz */
    put32(ph0 + 20, 0x40);              /* p_memsz */
    put32(ph0 + 24, 0x5);               /* PF_R | PF_X */

    /* Segment 1: .data at vaddr 0x00000080, file 0x140..0x160, memsz 0x30 (0x10 BSS) */
    uint8_t *ph1 = buf + 0x54;
    put32(ph1 +  0, 1);                 /* PT_LOAD */
    put32(ph1 +  4, 0x140);             /* p_offset */
    put32(ph1 +  8, 0x00000080);        /* p_vaddr */
    put32(ph1 + 16, 0x20);              /* p_filesz */
    put32(ph1 + 20, 0x30);              /* p_memsz */
    put32(ph1 + 24, 0x6);               /* PF_R | PF_W */

    /* Segment 0 words */
    put32(buf + 0x100, 0x08000004);     /* j 0x00000010 */
    put32(buf + 0x104, 0x3C040000);     /* lui $a0, 0x0000 */
    put32(buf + 0x108, 0x24844020);     /* addiu $a0, $a0, 0x4020 -> +0x08804000 = 0x08808020 (lo>=0x8000) */
    put32(buf + 0x10C, 0x3C050000);     /* lui $a1, 0x0000 */
    put32(buf + 0x110, 0x24A50014);     /* addiu $a1, $a1, 0x0014 -> code seed in .text */
    put32(buf + 0x120, 0x00000018);     /* R_MIPS_32 -> 0x08804018 */
    put32(buf + 0x124, 0x00000004);     /* R_MIPS_32 with addrbase=1 -> 0x08804084 */

    /* Segment 1 words */
    put32(buf + 0x148, 0x0000001C);     /* R_MIPS_32 with ofsbase=1, addrbase=0 -> 0x0880401C (code seed!) */

    /* SHT_PRXRELOC entries at 0x160 (8 entries = 0x40 bytes) */
    uint8_t *rel = buf + 0x160;
    put32(rel +  0, 0x00); put32(rel +  4, (0u << 16) | (0u << 8) | 4u); /* R_MIPS_26 */
    put32(rel +  8, 0x04); put32(rel + 12, (0u << 16) | (0u << 8) | 5u); /* R_MIPS_HI16 */
    put32(rel + 16, 0x08); put32(rel + 20, (0u << 16) | (0u << 8) | 6u); /* R_MIPS_LO16 */
    put32(rel + 24, 0x0C); put32(rel + 28, (0u << 16) | (0u << 8) | 5u); /* R_MIPS_HI16 */
    put32(rel + 32, 0x10); put32(rel + 36, (0u << 16) | (0u << 8) | 6u); /* R_MIPS_LO16 */
    put32(rel + 40, 0x20); put32(rel + 44, (0u << 16) | (0u << 8) | 2u); /* R_MIPS_32 */
    put32(rel + 48, 0x24); put32(rel + 52, (1u << 16) | (0u << 8) | 2u); /* R_MIPS_32 addrbase=1 */
    put32(rel + 56, 0x08); put32(rel + 60, (0u << 16) | (1u << 8) | 2u); /* R_MIPS_32 ofsbase=1 */

    /* Section headers at 0x200 */
    uint8_t *sh1 = buf + 0x228;
    put32(sh1 +  4, 1);                 /* SHT_PROGBITS */
    put32(sh1 +  8, 6);                 /* SHF_ALLOC | SHF_EXECINSTR */
    put32(sh1 + 12, 0x00000000);        /* sh_addr */
    put32(sh1 + 16, 0x100);             /* sh_offset */
    put32(sh1 + 20, 0x40);              /* sh_size */

    uint8_t *sh2 = buf + 0x250;
    put32(sh2 +  4, 0x700000A0u);       /* SHT_PRXRELOC */
    put32(sh2 + 16, 0x160);             /* sh_offset */
    put32(sh2 + 20, 0x40);              /* sh_size */

    elf_info e;
    CHECK(elf_parse(buf, sizeof buf, &e) == 0, "parse two-segment synthetic PRX");
    CHECK(e.nsegments == 2, "two PT_LOAD segments");

    /* Before relocation: psp_collect_pointer_seeds must honour ofsbase=1 (fails on upstream main). */
    uint32_t raw_seeds[16];
    int n_raw = psp_collect_pointer_seeds(buf, sizeof buf, &e, e.seg[0].offset - e.seg[0].addr, raw_seeds, 16);
    CHECK(has_seed(raw_seeds, n_raw, 0x00000014), "unrelocated HI16/LO16 seed collected");
    CHECK(has_seed(raw_seeds, n_raw, 0x00000018), "unrelocated R_MIPS_32 seed collected");
    CHECK(has_seed(raw_seeds, n_raw, 0x0000001C), "unrelocated R_MIPS_32 with ofsbase=1 collected");

    /* Relocate at base = 0x08804000 */
    const uint32_t base = 0x08804000u;
    uint8_t *reloc = NULL;
    size_t reloc_sz = 0;
    CHECK(prx_relocate(buf, sizeof buf, &e, base, &reloc, &reloc_sz) == 0, "prx_relocate succeeds");
    CHECK(reloc != NULL && reloc_sz == 0xB0, "relocated image covers segment 1 + BSS (0x80 + 0x30)");
    if (reloc && reloc_sz == 0xB0) {
        CHECK(get32(reloc + 0x00) == 0x0A201004u, "R_MIPS_26 relocated to j 0x08804010");
        CHECK(get32(reloc + 0x04) == 0x3C040881u, "R_MIPS_HI16 carries +1 when lo16 >= 0x8000");
        CHECK(get32(reloc + 0x08) == 0x24848020u, "R_MIPS_LO16 relocated to 0x8020");
        CHECK(get32(reloc + 0x20) == 0x08804018u, "R_MIPS_32 relocated");
        CHECK(get32(reloc + 0x24) == 0x08804084u, "R_MIPS_32 with addrbase=1 adds seg[1].addr");
        CHECK(get32(reloc + 0x88) == 0x0880401Cu, "R_MIPS_32 with ofsbase=1 patches segment 1");

        e.text_addr += base;
        for (int i = 0; i < e.nsegments; i++) e.seg[i].addr += base;
        uint32_t rel_seeds[16];
        int n_rel = psp_collect_pointer_seeds_at(buf, sizeof buf, reloc, reloc_sz, base,
                                                 &e, (uint32_t)(0 - base), rel_seeds, 16);
        CHECK(has_seed(rel_seeds, n_rel, 0x08804014u), "relocated HI16/LO16 seed collected");
        CHECK(has_seed(rel_seeds, n_rel, 0x08804018u), "relocated R_MIPS_32 seed collected");
        CHECK(has_seed(rel_seeds, n_rel, 0x0880401Cu), "relocated R_MIPS_32 with ofsbase=1 seed collected");
    }
    free(reloc);
}

int main(void) {
    test_sniff();
    test_pbp();
    test_psp_header();
    test_elf();
    test_sfo();
    test_prx_relocate_and_seeds();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("all container checks passed\n");
    return 0;
}
