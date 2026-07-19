/* allegrexrecomp — the container stack between a disc dump and MIPS code.
 *
 * Getting from a UMD/PSN dump to something you can decode means peeling four
 * layers, and a real game uses all of them:
 *
 *   .iso            ISO9660, 2048-byte sectors        -> PSP_GAME/SYSDIR/EBOOT.BIN
 *   PBP             plain 8-segment container         -> DATA.PSP (a PRX)
 *   ~PSP            encrypted PRX wrapper (KIRK)      -> an ELF, once decrypted
 *   ~SCE            simple 0x40-byte wrapper          -> a ~PSP or an ELF
 *   ELF/PRX         ELF32 MIPS LE, e_type 0xFFA0      -> the code we decode
 *
 * Only the ~PSP layer needs crypto. Everything else here is plain parsing, and
 * plenty of real content — including WTF's game-sharing microgames — is PBP all
 * the way down to an unencrypted PRX, which is why bring-up can start before
 * the decryptor exists. See docs/CONTAINERS.md.
 */
#ifndef ALLEGREX_CONTAINER_H
#define ALLEGREX_CONTAINER_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FMT_UNKNOWN = 0,
    FMT_ISO9660,   /* a UMD or PSN disc image */
    FMT_PBP,       /* \0PBP container */
    FMT_PSP,       /* ~PSP encrypted PRX */
    FMT_SCE,       /* ~SCE wrapper */
    FMT_ELF,       /* plain ELF32 (incl. PRX, e_type 0xFFA0) */
} psp_format;

/* Sniff a buffer's leading bytes. `len` must be at least 16. */
psp_format psp_sniff(const uint8_t *data, size_t len);
const char *psp_format_name(psp_format f);

/* ---- a whole file slurped into memory ------------------------------------ */

typedef struct {
    uint8_t *data;
    size_t   size;
} psp_blob;

int  psp_blob_read(const char *path, psp_blob *out);
void psp_blob_free(psp_blob *b);

/* ---- ISO9660 ------------------------------------------------------------- */

#define ISO_SECTOR 2048

typedef struct {
    char     path[256];   /* "/PSP_GAME/SYSDIR/EBOOT.BIN" */
    uint32_t lba;
    uint32_t size;
    int      is_dir;
} iso_entry;

/* Walk the directory tree. Returns the number of entries written to `out`,
 * or -1 on error. Pass out=NULL to just count. */
int iso_list(FILE *f, iso_entry *out, int max_entries);

/* Copy one entry's extent out of the image. */
int iso_extract(FILE *f, const iso_entry *e, const char *dst_path);

/* ---- PARAM.SFO ----------------------------------------------------------- */

/* Print every key/value in a PARAM.SFO blob. This is where DISC_ID, TITLE and
 * PSP_SYSTEM_VER live — the identity a game repo records in its NOTES.md. */
int sfo_dump(const uint8_t *data, size_t len, FILE *out);

/* ---- PBP ----------------------------------------------------------------- */

#define PBP_NUM_SEGMENTS 8

extern const char *const PBP_SEGMENT_NAMES[PBP_NUM_SEGMENTS];

typedef struct {
    uint32_t version;
    uint32_t offset[PBP_NUM_SEGMENTS];
    uint32_t size[PBP_NUM_SEGMENTS];   /* derived from adjacent offsets */
} pbp_info;

int pbp_parse(const uint8_t *data, size_t len, pbp_info *out);

/* ---- ~PSP header --------------------------------------------------------- */

#define PSP_HEADER_SIZE 0x150

typedef struct {
    char     modname[29];
    uint16_t mod_attr;
    uint16_t comp_attr;
    uint8_t  mod_ver_lo, mod_ver_hi;
    uint8_t  nsegments;
    uint32_t elf_size;      /* size of the decrypted ELF */
    uint32_t psp_size;      /* size of this ~PSP file */
    uint32_t boot_entry;
    uint32_t modinfo_offset;
    uint32_t bss_size;
    uint32_t seg_address[4];
    uint32_t seg_size[4];
    uint32_t devkit_version;
    uint8_t  decrypt_mode;
    uint32_t tag;           /* selects the KIRK key set */
} psp_header;

int psp_header_parse(const uint8_t *data, size_t len, psp_header *out);

/* ---- ELF / PRX ----------------------------------------------------------- */

#define ET_PSP_PRX 0xFFA0

typedef struct {
    uint32_t addr;
    uint32_t offset;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
} elf_segment;

typedef struct {
    uint16_t type;          /* ET_EXEC (2) or ET_PSP_PRX */
    uint32_t entry;
    int      nsegments;
    elf_segment seg[8];
    /* The .text extent we hand to the decoder: the first executable segment. */
    uint32_t text_addr, text_offset, text_size;
} elf_info;

int elf_parse(const uint8_t *data, size_t len, elf_info *out);

#ifdef __cplusplus
}
#endif

#endif /* ALLEGREX_CONTAINER_H */
