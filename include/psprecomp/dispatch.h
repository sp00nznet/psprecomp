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
void psp_register_label(uint32_t addr, psp_fn_t fn);
void psp_dispatch_set_budget(uint64_t calls, void (*on_exceeded)(void));
uint64_t psp_dispatch_calls(void);

/* Look up a target. Returns NULL if nothing is registered there. */
psp_fn_t psp_lookup(uint32_t addr);

/* Call through the table. An unregistered address invokes the miss handler
 * (below) rather than crashing, so bring-up gets a report naming the address
 * instead of an access violation. */
void psp_dispatch(uint32_t addr);

/* Call guest code from the host (a thread entry, a callback, an interrupt
 * handler, a constructor). Pushes a host frame: the guest's `jr $ra` at its end
 * is a return whatever $ra holds, and no unwind crosses it. */
void psp_call_from_host(uint32_t addr);

/* Guest calls own their return-site frame here, not in an emitted body.
 * Keeping stack-local frame addresses out of bodies permits sibling-call
 * optimization for terminal transfers between them. The caller sets guest
 * link registers and propagates any nonzero unwind after this returns.
 * Direct calls preserve their no-lookup behavior; fn must be non-NULL. */
void psp_call_guest_fn(psp_fn_t fn, uint32_t return_site);
void psp_call_guest_addr(uint32_t addr, uint32_t return_site);

/* What a `jr $ra` to `ra` is. On the hardware it is just a jump; recompiled,
 * calls are C calls, so it is one of three things:
 *  - a return, when `ra` is the current call's return site (or the call was
 *    made by the host): returns 0;
 *  - a return past nested calls, when `ra` is an outer call's return site --
 *    a helper reached by a branch-and-link that leaves by branching to its
 *    caller's epilogue: sets psp_cpu.unwind and returns 0, and each call site
 *    returns until the one that owns `ra` resumes;
 *  - a jump inside the current call, when `ra` matches no call -- $ra used as a
 *    continuation: returns 1, and the caller dispatches to `ra`. */
int psp_ra_is_jump(uint32_t ra);

/* Called when psp_dispatch hits an unregistered address. The default prints
 * the address and aborts; a host can override it to log and continue, which is
 * useful while discovery is still incomplete. */
/* ---- function-entry trace ------------------------------------------------
 * Generated functions call psp_trace_enter() on entry when the code is built
 * with PSPRECOMP_TRACE; otherwise the call compiles away entirely.
 *
 * This exists because a dispatch miss reports the bad *target* and says
 * nothing about who jumped there. Recompiled functions are ordinary C, so the
 * information is on the host stack — but reading it portably is more trouble
 * than recording it, and a short history of entered functions also shows how
 * the code arrived, not just where it was. */
void psp_trace_enter(uint32_t addr);
void psp_trace_dump(void);
void psp_trace_reset(void);
uint32_t psp_trace_last(void);
void psp_trace_watch(uint32_t addr, void (*fn)(uint32_t));
void psp_trace_loop(uint32_t addr);
void psp_trace_mark(uint32_t addr);
void psp_trace_sp(uint32_t fn, uint32_t sp_in, uint32_t sp_out);
/* The same observation at a transfer that ends a body, where the expected
 * direction is inverted: the body hands over mid-frame. */
void psp_trace_sp_leave(uint32_t fn, uint32_t sp_in, uint32_t sp_out);
void psp_trace_sp_call(uint32_t callee, uint32_t sp_before, uint32_t sp_after);
uint64_t psp_sp_call_violations(void);
uint64_t psp_sp_violations(void);
uint32_t psp_sp_first_bad(void);
int32_t  psp_sp_first_delta(void);
/* Stack imbalances grouped by site, largest magnitude first: the SHAPE of the
 * noise rather than its first messages. A per-message cap makes a log that
 * stopped read as "no large delta". One row per distinct (site, delta) with its
 * count, and the largest magnitude is kept even when the table is full.
 * `site` is the function of a body check, or the callee of a call check. */
typedef struct {
    uint32_t site;
    int32_t  delta;
    uint64_t count;
    uint32_t in;        /* $sp expected / $sp before the call */
    uint32_t out;       /* $sp observed / $sp after the call */
    uint32_t last;      /* last function entered (body checks) */
} psp_sp_site;

int psp_sp_hist(psp_sp_site *out, int max);
int psp_sp_call_hist(psp_sp_site *out, int max);
void psp_trace_watch_label(uint32_t addr, void (*fn)(uint32_t));
void psp_trace_marks_init(uint32_t lo, uint32_t words);
int psp_trace_was_marked(uint32_t addr);
uint32_t psp_trace_loop_addr(void);
uint64_t psp_trace_loop_hits(void);

/* Optional hook that prints extra context when a dispatch miss happens. The
 * HLE layer installs one so a wild pointer is reported alongside the firmware
 * calls that recently returned zero -- dispatch itself must not depend on the
 * HLE layer, hence the indirection. */
typedef void (*psp_miss_ctx_fn_t)(void);
void psp_set_miss_context(psp_miss_ctx_fn_t fn);
void psp_miss_context(void);

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
