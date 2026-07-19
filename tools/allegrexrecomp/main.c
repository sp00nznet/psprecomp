/* allegrexrecomp — command-line front end.
 *
 *   info    <file>                    identify and describe any layer of the stack
 *   ls      <file.iso>                list a disc image
 *   extract <file.iso> <match> <dir>  pull matching files out of a disc image
 *   dis     <file> [addr] [count]     disassemble Allegrex code
 *   cover   <file>                    decode-coverage report over an image
 *
 * Phase 1 stops at `dis` and `cover`; `emit` (the C generator) is phase 3.
 * See ROADMAP.md.
 */

#include "container.h"
#include "decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ISO_ENTRIES 4096

static int usage(void) {
    fprintf(stderr,
        "allegrexrecomp — Allegrex static-recompilation toolkit\n"
        "\n"
        "  allegrexrecomp info    <file>\n"
        "  allegrexrecomp ls      <file.iso>\n"
        "  allegrexrecomp extract <file.iso> <substring> <outdir>\n"
        "  allegrexrecomp dis     <file> [start-addr] [count]\n"
        "  allegrexrecomp cover   <file>\n"
        "\n"
        "Accepts .iso disc images, PBP containers, ~PSP / ~SCE wrappers,\n"
        "ELF/PRX modules, and raw binaries.\n");
    return 2;
}

/* ---- info ---------------------------------------------------------------- */

static void print_iso_info(const char *path, FILE *f) {
    printf("format:   ISO9660 disc image\n");

    iso_entry *entries = (iso_entry *)calloc(MAX_ISO_ENTRIES, sizeof *entries);
    if (!entries) return;

    int n = iso_list(f, entries, MAX_ISO_ENTRIES);
    if (n < 0) { printf("  (could not read the volume descriptor)\n"); free(entries); return; }
    printf("entries:  %d\n", n);
    if (n > MAX_ISO_ENTRIES) {
        printf("  (listing truncated to %d)\n", MAX_ISO_ENTRIES);
        n = MAX_ISO_ENTRIES;
    }

    /* PARAM.SFO is the disc's identity: DISC_ID, TITLE, firmware requirement. */
    for (int i = 0; i < n; i++) {
        if (entries[i].is_dir) continue;
        if (!strstr(entries[i].path, "PSP_GAME/PARAM.SFO")) continue;

        uint8_t *buf = (uint8_t *)malloc(entries[i].size);
        if (!buf) break;
        if (fseek(f, (long)entries[i].lba * ISO_SECTOR, SEEK_SET) == 0 &&
            fread(buf, 1, entries[i].size, f) == entries[i].size) {
            printf("\nPARAM.SFO:\n");
            sfo_dump(buf, entries[i].size, stdout);
        }
        free(buf);
        break;
    }

    /* The boot chain is what a recomp run actually cares about. */
    printf("\nboot chain:\n");
    for (int i = 0; i < n; i++) {
        const char *p = entries[i].path;
        if (entries[i].is_dir) continue;
        if (strstr(p, "EBOOT.BIN") || strstr(p, "BOOT.BIN") ||
            strstr(p, "bootbin")   || strstr(p, ".prx")) {
            printf("  %10u  %s\n", entries[i].size, p);
        }
    }
    free(entries);
}

static void print_psp_info(const uint8_t *d, size_t len);
static void print_elf_info(const uint8_t *d, size_t len);

static void print_pbp_info(const uint8_t *d, size_t len) {
    pbp_info pbp;
    if (pbp_parse(d, len, &pbp) != 0) { printf("  (malformed PBP)\n"); return; }

    printf("format:   PBP container (version 0x%X)\n", pbp.version);
    for (int i = 0; i < PBP_NUM_SEGMENTS; i++) {
        if (!pbp.size[i]) continue;
        printf("  %-10s  offset 0x%08X  size %10u",
               PBP_SEGMENT_NAMES[i], pbp.offset[i], pbp.size[i]);
        /* Name the inner format so you can see at a glance whether the
         * executable payload is encrypted before you try to decode it. */
        if (pbp.offset[i] + 16 <= len) {
            psp_format inner = psp_sniff(d + pbp.offset[i], 16);
            if (inner != FMT_UNKNOWN) printf("  [%s]", psp_format_name(inner));
        }
        printf("\n");
    }
    if (pbp.size[0] && pbp.offset[0] + pbp.size[0] <= len) {
        printf("\nPARAM.SFO:\n");
        sfo_dump(d + pbp.offset[0], pbp.size[0], stdout);
    }

    /* Recurse one level into the executable payload. DATA.PSP is where the
     * actual module lives, and its header names the module and the key tag —
     * the two things you need before you can decrypt or decode it. */
    if (pbp.size[6] && pbp.offset[6] + pbp.size[6] <= len) {
        const uint8_t *inner = d + pbp.offset[6];
        printf("\nDATA.PSP:\n");
        switch (psp_sniff(inner, 16)) {
        case FMT_PSP: print_psp_info(inner, pbp.size[6]); break;
        case FMT_ELF: print_elf_info(inner, pbp.size[6]); break;
        default:      printf("  (unrecognised payload)\n"); break;
        }
    }
}

static void print_psp_info(const uint8_t *d, size_t len) {
    psp_header h;
    if (psp_header_parse(d, len, &h) != 0) { printf("  (malformed ~PSP)\n"); return; }

    printf("format:   ~PSP encrypted PRX\n");
    printf("  module        %s\n", h.modname);
    printf("  version       %u.%u\n", h.mod_ver_hi, h.mod_ver_lo);
    printf("  attributes    mod=0x%04X comp=0x%04X\n", h.mod_attr, h.comp_attr);
    printf("  segments      %u\n", h.nsegments);
    printf("  elf size      %u bytes (decrypted)\n", h.elf_size);
    printf("  psp size      %u bytes (this file)\n", h.psp_size);
    printf("  entry         0x%08X\n", h.boot_entry);
    printf("  modinfo       0x%08X\n", h.modinfo_offset);
    printf("  bss           %u bytes\n", h.bss_size);
    printf("  devkit        0x%08X\n", h.devkit_version);
    printf("  decrypt mode  %u\n", h.decrypt_mode);
    printf("  tag           0x%08X\n", h.tag);
    for (int i = 0; i < h.nsegments && i < 4; i++)
        printf("  segment %d     addr 0x%08X size %u\n", i, h.seg_address[i], h.seg_size[i]);
    printf("\n  This module is encrypted. Decryption is phase 2 — see docs/DECRYPT.md.\n");
}

static void print_elf_info(const uint8_t *d, size_t len) {
    elf_info e;
    if (elf_parse(d, len, &e) != 0) { printf("  (malformed ELF)\n"); return; }

    printf("format:   ELF32 MIPS LE (%s)\n",
           e.type == ET_PSP_PRX ? "PRX, relocatable" : "executable");
    printf("  entry         0x%08X\n", e.entry);
    printf("  PT_LOAD       %d\n", e.nsegments);
    for (int i = 0; i < e.nsegments; i++) {
        const elf_segment *s = &e.seg[i];
        printf("  segment %d     addr 0x%08X  file %8u  mem %8u  flags %c%c%c\n",
               i, s->addr, s->filesz, s->memsz,
               (s->flags & 4) ? 'r' : '-',
               (s->flags & 2) ? 'w' : '-',
               (s->flags & 1) ? 'x' : '-');
    }
    if (e.text_size)
        printf("  .text         0x%08X + %u bytes\n", e.text_addr, e.text_size);
}

static int cmd_info(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }

    uint8_t head[16];
    size_t got = fread(head, 1, sizeof head, f);

    /* ISO is the only format whose magic is not at offset 0. Probe sector 16
     * directly rather than reading the whole (multi-gigabyte) image. */
    uint8_t pvd[8];
    int is_iso = 0;
    if (fseek(f, 16 * ISO_SECTOR, SEEK_SET) == 0 &&
        fread(pvd, 1, sizeof pvd, f) == sizeof pvd &&
        memcmp(pvd + 1, "CD001", 5) == 0) {
        is_iso = 1;
    }
    rewind(f);

    printf("file:     %s\n", path);
    if (is_iso) {
        print_iso_info(path, f);
        fclose(f);
        return 0;
    }
    fclose(f);

    psp_blob b;
    if (psp_blob_read(path, &b) != 0) { fprintf(stderr, "cannot read %s\n", path); return 1; }
    printf("size:     %zu bytes\n", b.size);

    switch (psp_sniff(b.data, got < 16 ? b.size : 16)) {
    case FMT_PBP: print_pbp_info(b.data, b.size); break;
    case FMT_PSP: print_psp_info(b.data, b.size); break;
    case FMT_ELF: print_elf_info(b.data, b.size); break;
    case FMT_SCE:
        /* ~SCE is a 0x40-byte shim; whatever follows is the real payload. */
        printf("format:   ~SCE wrapper\n");
        if (b.size > 0x40) {
            printf("  inner       %s\n", psp_format_name(psp_sniff(b.data + 0x40, 16)));
            if (psp_sniff(b.data + 0x40, 16) == FMT_PSP)
                print_psp_info(b.data + 0x40, b.size - 0x40);
        }
        break;
    default:
        printf("format:   unrecognised — treating as raw Allegrex code\n");
        break;
    }

    psp_blob_free(&b);
    return 0;
}

/* ---- ls / extract -------------------------------------------------------- */

static int cmd_ls(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }

    iso_entry *entries = (iso_entry *)calloc(MAX_ISO_ENTRIES, sizeof *entries);
    if (!entries) { fclose(f); return 1; }

    int n = iso_list(f, entries, MAX_ISO_ENTRIES);
    if (n < 0) { fprintf(stderr, "%s is not an ISO9660 image\n", path); free(entries); fclose(f); return 1; }
    if (n > MAX_ISO_ENTRIES) n = MAX_ISO_ENTRIES;

    for (int i = 0; i < n; i++)
        printf("%c %12u  lba=%-8u %s\n",
               entries[i].is_dir ? 'd' : ' ',
               entries[i].size, entries[i].lba, entries[i].path);

    free(entries);
    fclose(f);
    return 0;
}

static int cmd_extract(const char *path, const char *match, const char *outdir) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }

    iso_entry *entries = (iso_entry *)calloc(MAX_ISO_ENTRIES, sizeof *entries);
    if (!entries) { fclose(f); return 1; }

    int n = iso_list(f, entries, MAX_ISO_ENTRIES);
    if (n < 0) { fprintf(stderr, "%s is not an ISO9660 image\n", path); free(entries); fclose(f); return 1; }
    if (n > MAX_ISO_ENTRIES) n = MAX_ISO_ENTRIES;

    int found = 0;
    for (int i = 0; i < n; i++) {
        if (entries[i].is_dir || !strstr(entries[i].path, match)) continue;

        /* Flatten the ISO path into a filename so nested entries with the same
         * basename (SYSDIR/EBOOT.BIN vs SYSDIR/UPDATE/EBOOT.BIN) do not collide. */
        char flat[256];
        snprintf(flat, sizeof flat, "%s", entries[i].path + 1);
        for (char *p = flat; *p; p++) if (*p == '/') *p = '_';

        char dst[512];
        snprintf(dst, sizeof dst, "%s/%s", outdir, flat);
        if (iso_extract(f, &entries[i], dst) == 0) {
            printf("extracted %s -> %s (%u bytes)\n", entries[i].path, dst, entries[i].size);
            found++;
        } else {
            fprintf(stderr, "failed to extract %s\n", entries[i].path);
        }
    }
    if (!found) fprintf(stderr, "nothing matched \"%s\"\n", match);

    free(entries);
    fclose(f);
    return found ? 0 : 1;
}

/* ---- dis / cover --------------------------------------------------------- */

/* Locate the code to decode: an ELF's .text, or the whole blob if it is raw. */
static int find_code(const psp_blob *b, const uint8_t **code, uint32_t *size, uint32_t *base) {
    psp_format fmt = psp_sniff(b->data, b->size < 16 ? b->size : 16);

    if (fmt == FMT_ELF) {
        elf_info e;
        if (elf_parse(b->data, b->size, &e) == 0 && e.text_size) {
            *code = b->data + e.text_offset;
            *size = e.text_size;
            *base = e.text_addr;
            return 0;
        }
        return -1;
    }
    if (fmt == FMT_PSP || fmt == FMT_SCE) {
        fprintf(stderr,
            "this module is encrypted (%s) — decryption is phase 2, see docs/DECRYPT.md\n",
            psp_format_name(fmt));
        return -1;
    }
    if (fmt == FMT_PBP) {
        pbp_info p;
        if (pbp_parse(b->data, b->size, &p) == 0 && p.size[6]) {
            /* DATA.PSP is segment 6. Recurse one level by re-sniffing it. */
            const uint8_t *inner = b->data + p.offset[6];
            psp_format ifmt = psp_sniff(inner, 16);
            if (ifmt == FMT_ELF) {
                elf_info e;
                if (elf_parse(inner, p.size[6], &e) == 0 && e.text_size) {
                    *code = inner + e.text_offset;
                    *size = e.text_size;
                    *base = e.text_addr;
                    return 0;
                }
            }
            fprintf(stderr, "PBP DATA.PSP is %s — cannot decode yet\n",
                    psp_format_name(ifmt));
            return -1;
        }
        return -1;
    }

    /* Raw code. Default the load base to the standard user-memory entry. */
    *code = b->data;
    *size = (uint32_t)b->size;
    *base = 0x08804000u;
    return 0;
}

static int cmd_dis(const char *path, uint32_t start, uint32_t count) {
    psp_blob b;
    if (psp_blob_read(path, &b) != 0) { fprintf(stderr, "cannot read %s\n", path); return 1; }

    const uint8_t *code; uint32_t size, base;
    if (find_code(&b, &code, &size, &base) != 0) { psp_blob_free(&b); return 1; }

    uint32_t addr = start ? start : base;
    if (addr < base || addr >= base + size) {
        fprintf(stderr, "0x%08X is outside the code extent 0x%08X..0x%08X\n",
                addr, base, base + size);
        psp_blob_free(&b);
        return 1;
    }

    uint32_t off = addr - base;
    uint32_t emitted = 0;
    char text[128];

    while (off + 4 <= size && (count == 0 || emitted < count)) {
        uint32_t word = (uint32_t)code[off] | ((uint32_t)code[off + 1] << 8) |
                        ((uint32_t)code[off + 2] << 16) | ((uint32_t)code[off + 3] << 24);
        a_insn in;
        a_decode(word, addr, &in);
        a_format(&in, text, sizeof text);
        printf("%08X  %08X  %s\n", addr, word, text);
        off += 4;
        addr += 4;
        emitted++;
    }

    psp_blob_free(&b);
    return 0;
}

/* Decode coverage over a whole image. This is the honest measure of how far
 * the decoder is from handling a real game: every unrecognised word is a
 * concrete gap, and VFPU words are counted separately because they are a known
 * partial area rather than an unknown one. */
static int cmd_cover(const char *path) {
    psp_blob b;
    if (psp_blob_read(path, &b) != 0) { fprintf(stderr, "cannot read %s\n", path); return 1; }

    const uint8_t *code; uint32_t size, base;
    if (find_code(&b, &code, &size, &base) != 0) { psp_blob_free(&b); return 1; }

    uint64_t total = 0, ok = 0, vfpu = 0, bad = 0;
    uint32_t hist[A_OP_COUNT];
    memset(hist, 0, sizeof hist);

    for (uint32_t off = 0; off + 4 <= size; off += 4) {
        uint32_t word = (uint32_t)code[off] | ((uint32_t)code[off + 1] << 8) |
                        ((uint32_t)code[off + 2] << 16) | ((uint32_t)code[off + 3] << 24);
        a_insn in;
        a_decode(word, base + off, &in);
        total++;
        hist[in.op]++;
        if (in.op == A_INVALID)           bad++;
        else if (in.op == A_VFPU_UNKNOWN) vfpu++;
        else                              ok++;
    }

    printf("image:    %s\n", path);
    printf("extent:   0x%08X + %u bytes (%llu words)\n",
           base, size, (unsigned long long)total);
    if (!total) { psp_blob_free(&b); return 0; }

    printf("decoded:  %llu (%.2f%%)\n", (unsigned long long)ok, 100.0 * (double)ok / (double)total);
    printf("vfpu:     %llu (%.2f%%)  — recognised as VFPU, not yet named\n",
           (unsigned long long)vfpu, 100.0 * (double)vfpu / (double)total);
    printf("unknown:  %llu (%.2f%%)  — decoder gaps, or data misread as code\n",
           (unsigned long long)bad, 100.0 * (double)bad / (double)total);

    /* Top opcodes: a sanity check that we are looking at code at all. Real
     * MIPS text is dominated by lw/sw/addiu/nop; a flat histogram means the
     * extent is data. */
    printf("\ntop opcodes:\n");
    for (int rank = 0; rank < 12; rank++) {
        int best = -1;
        uint32_t bestn = 0;
        for (int i = 1; i < A_OP_COUNT; i++)
            if (hist[i] > bestn) { bestn = hist[i]; best = i; }
        if (best < 0) break;
        printf("  %-10s %8u  (%.2f%%)\n", a_mnemonic((a_op)best), bestn,
               100.0 * (double)bestn / (double)total);
        hist[best] = 0;
    }

    psp_blob_free(&b);
    return 0;
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char **argv) {
    if (argc < 3) return usage();

    const char *cmd = argv[1];

    if (!strcmp(cmd, "info"))    return cmd_info(argv[2]);
    if (!strcmp(cmd, "ls"))      return cmd_ls(argv[2]);
    if (!strcmp(cmd, "cover"))   return cmd_cover(argv[2]);
    if (!strcmp(cmd, "extract")) {
        if (argc < 5) return usage();
        return cmd_extract(argv[2], argv[3], argv[4]);
    }
    if (!strcmp(cmd, "dis")) {
        uint32_t start = (argc > 3) ? (uint32_t)strtoul(argv[3], NULL, 0) : 0;
        uint32_t count = (argc > 4) ? (uint32_t)strtoul(argv[4], NULL, 0) : 64;
        return cmd_dis(argv[2], start, count);
    }

    return usage();
}
