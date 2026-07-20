/* psprecomp — the address → function dispatch table.
 *
 * Most calls in recompiled code are direct: `jal 0x1234` becomes a plain C
 * call to psp_func_00001234(). Some are not — `jalr $t9` and `jr $t9` take
 * their target from a register, and MIPS uses that for function pointers,
 * vtables, callbacks and switch tables.
 *
 * Those go through here: the generated code registers every function it
 * defines, and an indirect transfer looks the target up at run time. This is
 * the same mechanism lynxrecomp uses for computed jumps, and it is what lets a
 * partially-discovered module still run — an unregistered target is a named,
 * catchable failure rather than a jump into nowhere.
 */
#ifndef PSPRECOMP_DISPATCH_H
#define PSPRECOMP_DISPATCH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*psp_fn_t)(void);

/* Register one recompiled function. The generated code calls this for every
 * function it defines, from a single init routine. */
void psp_register(uint32_t addr, psp_fn_t fn);

/* Look up a target. Returns NULL if nothing is registered there. */
psp_fn_t psp_lookup(uint32_t addr);

/* Call through the table. An unregistered address invokes the miss handler
 * (below) rather than crashing, so bring-up gets a report naming the address
 * instead of an access violation. */
void psp_dispatch(uint32_t addr);

/* Called when psp_dispatch hits an unregistered address. The default prints
 * the address and aborts; a host can override it to log and continue, which is
 * useful while discovery is still incomplete. */
typedef void (*psp_miss_fn_t)(uint32_t addr);
void psp_set_miss_handler(psp_miss_fn_t fn);

/* Number of registered functions, and how many misses have occurred — the
 * latter is a direct measure of how much the analysis is still missing. */
uint32_t psp_dispatch_count(void);
uint64_t psp_dispatch_misses(void);

void psp_dispatch_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* PSPRECOMP_DISPATCH_H */
