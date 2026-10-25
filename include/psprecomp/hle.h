/* psprecomp — high-level emulation of the PSP firmware.
 *
 * A PSP game does not touch hardware. It calls firmware entry points through
 * an import table, and every one of those calls is identified by a **NID** —
 * the first four bytes of SHA-1(function name), little-endian. That is a
 * verifiable fact rather than a convention, and `tests/test_hle.c` checks it
 * for every function registered here: a mistyped NID or a wrong name cannot
 * survive the test.
 *
 * Because the surface is a library rather than hardware, it is *implemented*,
 * not emulated. What a game needs is exactly its import table and nothing
 * else, which `allegrexrecomp funcs` reports — so the work is bounded and
 * knowable in advance.
 *
 * Calling convention is MIPS o32: arguments in $a0-$a3 then the stack, return
 * value in $v0. Handlers take no C arguments and use psp_arg()/psp_ret().
 */
#ifndef PSPRECOMP_HLE_H
#define PSPRECOMP_HLE_H

#include "cpu.h"
#include "mem.h"

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*psp_hle_fn)(void);

/* Register one firmware function. `name` is kept for diagnostics and is what
 * the NID is verified against. */
void psp_hle_register(uint32_t nid, const char *lib, const char *name, psp_hle_fn fn);
void psp_hle_register_unnamed(uint32_t nid, const char *lib, psp_hle_fn fn);
int psp_hle_is_named(int index);

/* Call a firmware function by NID. An unregistered NID reports itself by name
 * where possible and by number otherwise, rather than failing silently. */
void psp_hle_call(uint32_t nid);

/* Look up what is registered, for reporting. Returns NULL if absent. */
const char *psp_hle_name(uint32_t nid);
int         psp_hle_count(void);
/* The message naming an unimplemented call is printed a bounded number of
 * times; these are how much was kept back. `calls` is the number that tells a
 * silenced message from one that stopped happening -- see the comment on
 * UNIMPL_LOGGED in hle.c. */
uint64_t psp_hle_unimplemented_calls(void);
uint64_t psp_hle_unimplemented_logged(void);
void     psp_hle_unimplemented_reset(void);

/* Every zero-returning call ever made, not just the ZERO_HISTORY ring. */
uint64_t psp_hle_zero_calls(void);

/* Call counts per firmware function, busiest first, up to `limit` rows.
 * Unimplemented NIDs are counted individually too. */
void psp_hle_dump_counts(FILE *out, int limit);

/* Every registered entry, for the coverage report a game repo wants: which of
 * a module's imports actually exist yet. */
typedef struct {
    uint32_t    nid;
    const char *lib;
    const char *name;
} psp_hle_entry;

const psp_hle_entry *psp_hle_entries(int *count);

/* Print the recent firmware calls that returned zero. Zero is what a game most
 * often mistakes for an address, so this is the first thing to consult when a
 * wild pointer shows up far from its cause. */
void psp_hle_dump_recent(FILE *out);

/* Register everything the toolkit implements. Call once at startup. */
void psp_hle_init(void);

/* ---- o32 argument access ------------------------------------------------- */

/* Arguments 0-3 arrive in $a0-$a3; 4 and beyond are on the stack, at $sp+16
 * onward. The stack slots for the register arguments exist but are not
 * written by the caller, which is why the split is at 4 and not at 0. */
/* Firmware calls take arguments 5-8 in $t0-$t3, not on the stack.
 *
 * Plain o32 spills the fifth argument onward to `sp+16`, and that is what this
 * used to read. PSP firmware stubs do not: they load $t0-$t3 and branch, which
 * is visible in the delay slot of every such call --
 *
 *     000002EC  addu  $a3, $s1, $zero
 *     000002F0  jal   0x00091F14        ; sceKernelAllocPartitionMemory
 *     000002F4  addiu $t0, $zero, 4096  ; argument 5: the alignment
 *
 * Reading `sp+16` instead returned whatever happened to be on the stack. For
 * that call it produced 0x3AC85C where a power-of-two alignment belonged, so
 * the allocator rejected a 15.9 MB request with ILLEGAL_ATTR -- and every
 * failure this bring-up chased descended from it.
 *
 * Beyond eight, arguments do go on the stack. */
static inline uint32_t psp_arg(int n) {
    if (n < 4) return psp_cpu.r[PSP_REG_A0 + n];
    if (n < 8) return psp_cpu.r[PSP_REG_T0 + (n - 4)];
    return psp_read32(psp_cpu.r[PSP_REG_SP] + (uint32_t)n * 4);
}

static inline void psp_ret(uint32_t v) { psp_cpu.r[PSP_REG_V0] = v; }

/* Read a NUL-terminated string out of guest memory into a host buffer.
 * Always terminates; returns `dst`. */
const char *psp_str(uint32_t addr, char *dst, size_t cap);

/* ---- error codes --------------------------------------------------------- */
/* Only the ones the implemented functions can actually return. Games branch on
 * these, so returning a plausible-looking wrong value is worse than failing. */
#define SCE_KERNEL_ERROR_OK              0
#define SCE_KERNEL_ERROR_ERROR           0x80020001
#define SCE_KERNEL_ERROR_NOTIMPLEMENTED  0x80020002
#define SCE_KERNEL_ERROR_ILLEGAL_ADDR    0x80020005
#define SCE_KERNEL_ERROR_NO_MEMORY       0x80020190
#define SCE_KERNEL_ERROR_ILLEGAL_ATTR    0x80020191
#define SCE_KERNEL_ERROR_UNKNOWN_UID     0x800201A2
#define SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCK 0x800201A9
#define SCE_KERNEL_ERROR_ILLEGAL_THID    0x80020197
#define SCE_KERNEL_ERROR_WAIT_TIMEOUT    0x800201A8
#define SCE_KERNEL_ERROR_THREAD_TERMINATED 0x800201AC
#define SCE_KERNEL_ERROR_SEMA_ZERO       0x800201AD
#define SCE_KERNEL_ERROR_EVF_COND        0x800201AF
/* Added for the power callback slots; the neighbouring codes are corrected on
 * the UMD/RTC branch (their values arrived from the same pspsdk source). */
#define SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT 0x800200D2

/* ---- the subsystems ------------------------------------------------------ */

void psp_sysmem_init(void);
void psp_sysmem_register(void);
void psp_sysmem_reset(void);

void psp_display_init(void);
void psp_display_register(void);
void psp_display_reset(void);
int      psp_display_capture(const char *path);
/* The front buffer as 480x272 pixels of 0xAABBGGRR; -1 before the game has
 * set a framebuffer. */
int      psp_display_read(uint32_t *out);
uint64_t psp_display_vblanks(void);
uint32_t psp_display_framebuffer(void);

void psp_ge_init(void);
void psp_ge_register(void);
void psp_ge_reset(void);
void psp_ge_dump_stats(FILE *out);
uint64_t psp_ge_command_count(void);
uint64_t psp_ge_vertex_count(void);
uint64_t psp_ge_pixels(void);

void psp_sas_init(void);
void psp_sas_register(void);
void psp_sas_reset(void);
uint64_t psp_sas_frames(void);
uint64_t psp_sas_nonzero(void);

void psp_io_init(void);
void psp_io_register(void);
void psp_io_reset(void);
void psp_io_set_root(const char *root);
uint64_t psp_io_bytes_read(void);

void psp_misc_init(void);
void psp_misc_register(void);
void psp_misc_reset(void);
int  psp_exit_requested(void);
/* Submit one controller sample, even when the buttons have not changed.
 * The window calls this once per input poll. Samples accumulate until ReadLatch;
 * PeekLatch observes them without consuming. No hardware sampling timer is
 * emulated; an empty ReadLatch waits a frame then samples the current input. */
void psp_ctrl_set(uint32_t buttons, uint8_t ax, uint8_t ay);
uint64_t psp_audio_blocks(void);

void psp_threadman_init(void);
void psp_threadman_register(void);
/* ThreadMan reset is a quiescent-host operation: all guest threads must have
 * ended first. It reclaims host handles/locks, not guest stack allocations. */
void psp_threadman_reset(void);

/* ---- the scheduler -------------------------------------------------------
 *
 * Default execution is inline. PSPRECOMP_REAL_THREADS=ON opts into one CPU,
 * handed over explicitly: the boot thread (whoever calls
 * psp_threadman_reset) runs the module entry, and a thread that blocks gives
 * the CPU away until it is woken. These are the points where that happens,
 * for use by the rest of the HLE layer. See src/hle/threadman.c. */

/* Give the CPU away without blocking on anything of the guest's. A frame wait
 * is the usual caller: one core means "let a frame pass" is "let the others
 * run". */
void psp_threadman_yield(void);
/* Park for `us` of virtual time, as sceKernelDelayThread does. For HLE calls
 * that block on hardware for a known time, such as blocking audio output. */
void psp_threadman_delay(uint64_t us);
void psp_threadman_frame_wait(void);

/* Interrupt masking is per thread, and the cookie is the previous state --
 * which is why resuming a nested suspend does not enable interrupts. */
int  psp_threadman_intr_suspend(void);
void psp_threadman_intr_resume(uint32_t cookie);
int  psp_threadman_intr_disabled(void);

/* What the scheduler did, for a host's report. `switches` counts CPU
 * handovers and `parks` counts blocked calls: a run with threads and no
 * switches has not actually run them. */
uint64_t psp_threadman_switches(void);
uint64_t psp_threadman_parks(void);
uint64_t psp_threadman_vtime_us(void);
/* Parks after which another thread had run (so the CPU really was handed over),
 * and the ones that gave up before the thread they waited for was scheduled.
 * See the handover in src/hle/threadman.c. */
uint64_t psp_threadman_handovers(void);
uint64_t psp_threadman_handover_expired(void);
void     psp_threadman_dump(FILE *out);

/* ---- callbacks -----------------------------------------------------------
 *
 * A callback runs on the thread that created it, at the points where that
 * thread waits or explicitly asks (sceKernelCheckCallback) -- not at the point
 * of the notification, which may be another thread entirely. See the callbacks
 * section of src/hle/threadman.c. */

/* Whether an id names a callback the guest has actually created. A subsystem
 * that records a callback id (scePower's slots) has to ask first. */
int psp_callback_valid(uint32_t uid);

/* What delivery did, for a host's report: callbacks that ran, notifications
 * dropped because the target is not a recompiled function, and passes that hit
 * the per-tick ceiling. */
uint64_t psp_threadman_callback_deliveries(void);
uint64_t psp_threadman_callback_unresolved(void);
uint64_t psp_threadman_callback_deferred(void);

/* scePower: the power callback slots. No power event exists in this HLE, so a
 * registered callback is never fired; what is real is the slot handed back to
 * the caller. See the header of src/hle/power.c. */
void     psp_power_init(void);
void     psp_power_register(void);
void     psp_power_reset(void);
uint64_t psp_power_registered(void);

/* InterruptManager: sub-interrupt handlers, and the points where an interrupt
 * is delivered. See the header of src/hle/intrman.c for why a game can be
 * stuck in a loop that no amount of implementing `sleep` gets it out of. */
void psp_intrman_init(void);
void psp_intrman_register(void);
void psp_intrman_reset(void);

/* Deliver one interrupt to every enabled handler. Called from the places where
 * the guest explicitly waits for time to pass -- SleepThread, DelayThread and
 * the vblank wait -- since with no timer there is nothing else to hook. Safe to
 * call from inside a handler: delivery does not re-enter. */
void psp_intr_tick(void);

/* 0 until a game registers a handler, and the first number worth looking at
 * when a wait never ends. */
int      psp_intr_handlers(void);      /* registered */
int      psp_intr_enabled(void);       /* registered *and* enabled */
uint64_t psp_intr_deliveries(void);
uint64_t psp_intr_unresolved(void);    /* handler address that is not recompiled */

/* How many threads were created and how many were actually started. The gap
 * between the two is the first thing to look at when a game waits on a flag
 * nothing ever arms: a created-but-never-started worker explains it exactly,
 * and no other instrument shows the difference. */
uint64_t psp_threadman_created(void);
uint64_t psp_threadman_started(void);

/* Bytes of user memory still available — the cheapest end-to-end check that
 * the allocator is behaving. */
uint32_t psp_sysmem_free(void);

/* Raw allocation for use by other HLE subsystems (thread stacks, mostly).
 * Returns 0 on failure. These bypass the UID table because nothing in the
 * guest ever refers to them. */
uint32_t psp_sysmem_alloc(uint32_t size, int from_high);
void     psp_sysmem_release(uint32_t addr);

#ifdef __cplusplus
}
#endif

#endif /* PSPRECOMP_HLE_H */
