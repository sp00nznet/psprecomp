/* psprecomp — HLE dispatch. See include/psprecomp/hle.h. */

#include "psprecomp/hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HLE_MAX 512

static psp_hle_entry g_entry[HLE_MAX];
static psp_hle_fn    g_fn[HLE_MAX];
static int           g_count;

void psp_hle_register(uint32_t nid, const char *lib, const char *name, psp_hle_fn fn) {
    /* Re-registering replaces, so a game repo can override one function
     * without forking the table. */
    for (int i = 0; i < g_count; i++) {
        if (g_entry[i].nid == nid) { g_fn[i] = fn; g_entry[i].name = name; return; }
    }
    if (g_count >= HLE_MAX) return;
    g_entry[g_count].nid = nid;
    g_entry[g_count].lib = lib;
    g_entry[g_count].name = name;
    g_fn[g_count] = fn;
    g_count++;
}

const char *psp_hle_name(uint32_t nid) {
    for (int i = 0; i < g_count; i++)
        if (g_entry[i].nid == nid) return g_entry[i].name;
    return NULL;
}

int psp_hle_count(void) { return g_count; }

const psp_hle_entry *psp_hle_entries(int *count) {
    if (count) *count = g_count;
    return g_entry;
}

void psp_hle_call(uint32_t nid) {
    for (int i = 0; i < g_count; i++) {
        if (g_entry[i].nid == nid) { g_fn[i](); return; }
    }

    /* Unimplemented. Naming the function is the whole point — bringing a game
     * up is largely the process of watching this message stop appearing, and
     * "0x237DBD4F" is far less use than "sceKernelAllocPartitionMemory".
     *
     * Returning 0 rather than aborting is deliberate: many firmware calls are
     * advisory (version reporting, profiling hooks) and a game will run past
     * them happily. One that genuinely needed the result will fail visibly
     * soon after, with this line already in the log. */
    fprintf(stderr, "psprecomp: unimplemented firmware call 0x%08X\n", nid);
    psp_ret(0);
}

const char *psp_str(uint32_t addr, char *dst, size_t cap) {
    size_t n = 0;
    if (cap == 0) return dst;
    while (n + 1 < cap) {
        uint8_t c = psp_read8(addr + (uint32_t)n);
        if (!c) break;
        dst[n++] = (char)c;
    }
    dst[n] = '\0';
    return dst;
}

void psp_hle_init(void) {
    psp_sysmem_init();
    psp_sysmem_register();
    psp_threadman_init();
    psp_threadman_register();
}
