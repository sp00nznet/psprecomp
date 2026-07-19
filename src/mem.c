/* psprecomp — PSP memory map. See include/psprecomp/mem.h. */

#include "psprecomp/mem.h"

#include <stdlib.h>

psp_memory psp_mem;
uint64_t   psp_mem_bad_access;

int psp_mem_init(void) {
    psp_mem.ram     = (uint8_t *)calloc(1, PSP_RAM_SIZE);
    psp_mem.vram    = (uint8_t *)calloc(1, PSP_VRAM_SIZE);
    psp_mem.scratch = (uint8_t *)calloc(1, PSP_SCRATCH_SIZE);
    if (!psp_mem.ram || !psp_mem.vram || !psp_mem.scratch) {
        psp_mem_free();
        return -1;
    }
    psp_mem_bad_access = 0;
    return 0;
}

void psp_mem_free(void) {
    free(psp_mem.ram);
    free(psp_mem.vram);
    free(psp_mem.scratch);
    psp_mem.ram = psp_mem.vram = psp_mem.scratch = NULL;
}

void *psp_mem_ptr(uint32_t addr, uint32_t size) {
    /* Collapse the three cache-behaviour mirrors onto one backing store. */
    const uint32_t a = addr & PSP_ADDR_MASK;

    if (a >= PSP_RAM_BASE && a < PSP_RAM_BASE + PSP_RAM_SIZE) {
        uint32_t off = a - PSP_RAM_BASE;
        if (off + size > PSP_RAM_SIZE) return NULL;   /* straddles the end */
        return psp_mem.ram + off;
    }
    if (a >= PSP_VRAM_BASE && a < PSP_VRAM_BASE + PSP_VRAM_SIZE) {
        uint32_t off = a - PSP_VRAM_BASE;
        if (off + size > PSP_VRAM_SIZE) return NULL;
        return psp_mem.vram + off;
    }
    if (a >= PSP_SCRATCH_BASE && a < PSP_SCRATCH_BASE + PSP_SCRATCH_SIZE) {
        uint32_t off = a - PSP_SCRATCH_BASE;
        if (off + size > PSP_SCRATCH_SIZE) return NULL;
        return psp_mem.scratch + off;
    }
    return NULL;
}

/* Reads of unmapped memory return 0 and are counted. Silently returning 0 is
 * what hardware roughly does, but a recompiled game should never be doing it
 * in a steady state — the counter is how you notice. */
#define READ_BODY(TYPE)                              \
    void *p = psp_mem_ptr(addr, (uint32_t)sizeof(TYPE)); \
    if (!p) { psp_mem_bad_access++; return 0; }      \
    TYPE v;                                          \
    memcpy(&v, p, sizeof v);                         \
    return v;

uint8_t  psp_read8 (uint32_t addr) { READ_BODY(uint8_t)  }
uint16_t psp_read16(uint32_t addr) { READ_BODY(uint16_t) }
uint32_t psp_read32(uint32_t addr) { READ_BODY(uint32_t) }
float    psp_read_f32(uint32_t addr) { READ_BODY(float)  }

#define WRITE_BODY(TYPE)                             \
    void *p = psp_mem_ptr(addr, (uint32_t)sizeof(TYPE)); \
    if (!p) { psp_mem_bad_access++; return; }         \
    memcpy(p, &val, sizeof(TYPE));

void psp_write8 (uint32_t addr, uint8_t  val) { WRITE_BODY(uint8_t)  }
void psp_write16(uint32_t addr, uint16_t val) { WRITE_BODY(uint16_t) }
void psp_write32(uint32_t addr, uint32_t val) { WRITE_BODY(uint32_t) }
void psp_write_f32(uint32_t addr, float  val) { WRITE_BODY(float)    }

int psp_mem_write_block(uint32_t addr, const void *src, uint32_t len) {
    void *p = psp_mem_ptr(addr, len);
    if (!p) return -1;
    memcpy(p, src, len);
    return 0;
}

int psp_mem_read_block(void *dst, uint32_t addr, uint32_t len) {
    void *p = psp_mem_ptr(addr, len);
    if (!p) return -1;
    memcpy(dst, p, len);
    return 0;
}
