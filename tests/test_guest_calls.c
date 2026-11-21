/* Guest return-site ownership and propagation, synthetic callbacks only. */
#include "psprecomp/cpu.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/hle.h"
#include <stdio.h>

static int failures, calls, continued;
static uint32_t expected_ret, missed;
static psp_frame *expected_prev;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); failures++; } } while (0)
#define OUTER_RET 0x08804010u
#define INNER_RET 0x08804020u
#define TARGET    0x08805000u
#define ABSENT    0x08806000u

static void leaf(void) {
    CHECK(psp_cpu.frame != NULL, "callee has a guest frame");
    if (psp_cpu.frame) {
        CHECK(psp_cpu.frame->ret == expected_ret, "callee sees its return site");
        CHECK(psp_cpu.frame->prev == expected_prev, "callee retains the outer frame");
    }
    CHECK(!psp_ra_is_jump(expected_ret), "own return site is a return");
    calls++;
    psp_cpu.r[PSP_REG_V0] = 42;
}
static void own_unwind(void) { psp_cpu.unwind = expected_ret; }
static void foreign_unwind(void) { psp_cpu.unwind = INNER_RET; }
static void return_to_outer(void) {
    CHECK(psp_cpu.frame && psp_cpu.frame->ret == INNER_RET, "inner frame has its own return site");
    CHECK(!psp_ra_is_jump(OUTER_RET), "return to an outer frame requests an unwind");
    CHECK(psp_cpu.unwind == OUTER_RET, "unwind names the outer return site");
}
static void nested(void) {
    psp_frame *frame = psp_cpu.frame;
    psp_call_guest_fn(return_to_outer, INNER_RET);
    CHECK(psp_cpu.frame == frame, "inner call restores the outer guest frame");
    if (psp_cpu.unwind) return;  /* emitted bodies propagate this way */
    continued++;
}
static void tail_leaf(void) {
    CHECK(psp_cpu.frame && psp_cpu.frame->ret == OUTER_RET, "tail transfer keeps caller frame");
    calls++;
}
static void tail_body(void) { tail_leaf(); }
static void on_miss(uint32_t address) { missed = address; }
static void from_host_body(void) {
    psp_frame *host = psp_cpu.frame;
    CHECK(host && !host->ret && host->prev == expected_prev, "host call supplies a return barrier");
    psp_frame *previous = expected_prev;
    expected_prev = host;
    expected_ret = OUTER_RET;
    psp_call_guest_fn(leaf, OUTER_RET);
    CHECK(psp_cpu.frame == host && !psp_cpu.unwind, "guest helper restores its host frame");
    expected_prev = previous;
    psp_cpu.unwind = INNER_RET;  /* no unwind may escape the host entry */
}
static void wait_body(void) {
    psp_frame *frame = psp_cpu.frame;
    uint64_t time = psp_threadman_vtime_us(), parks = psp_threadman_parks();
    psp_threadman_frame_wait();
    CHECK(psp_threadman_vtime_us() > time, "wait advances virtual time");
#if PSPRECOMP_REAL_THREADS
    CHECK(psp_threadman_parks() > parks, "threaded wait used the scheduler park path");
#else
    CHECK(psp_threadman_parks() == parks, "inline wait does not create a host park");
#endif
    CHECK(psp_cpu.frame == frame && frame && frame->ret == OUTER_RET && frame->prev == expected_prev,
          "active guest frame survives scheduler save and resume");
    CHECK(!psp_cpu.unwind, "scheduler resume preserves the unwind state");
}

int main(void) {
    if (psp_mem_init() != 0) return 1;
    psp_cpu_reset();
    psp_hle_init();
    psp_dispatch_reset();
    psp_frame root = {0, NULL};
    psp_cpu.frame = &root;
    expected_prev = &root;
    expected_ret = OUTER_RET;
    psp_cpu.r[PSP_REG_A0] = 123;
    psp_cpu.r[PSP_REG_RA] = 456;
    psp_dispatch_set_budget(1000, NULL);

    psp_call_guest_fn(leaf, OUTER_RET);
    CHECK(calls == 1 && psp_cpu.r[PSP_REG_V0] == 42, "direct call runs without registration");
    CHECK(psp_dispatch_calls() == 0, "direct call does not add a dispatched call");
    CHECK(psp_cpu.frame == &root && !psp_cpu.unwind, "direct call restores context");
    CHECK(psp_cpu.r[PSP_REG_A0] == 123 && psp_cpu.r[PSP_REG_RA] == 456, "helper does not rewrite guest arguments or link register");

    psp_register(TARGET, leaf);
    psp_call_guest_addr(TARGET, OUTER_RET);
    CHECK(calls == 2 && psp_dispatch_calls() == 1, "indirect call uses the real dispatch path");
    CHECK(psp_cpu.frame == &root && !psp_cpu.unwind, "indirect call restores context");

    psp_call_guest_fn(own_unwind, OUTER_RET);
    CHECK(!psp_cpu.unwind && psp_cpu.frame == &root, "direct helper consumes its own unwind");
    psp_register(TARGET, own_unwind);
    psp_call_guest_addr(TARGET, OUTER_RET);
    CHECK(!psp_cpu.unwind && psp_cpu.frame == &root, "indirect helper consumes its own unwind");

    psp_call_guest_fn(foreign_unwind, OUTER_RET);
    CHECK(psp_cpu.unwind == INNER_RET && psp_cpu.frame == &root, "direct helper preserves a foreign unwind");
    psp_cpu.unwind = 0;
    psp_register(TARGET, foreign_unwind);
    psp_call_guest_addr(TARGET, OUTER_RET);
    CHECK(psp_cpu.unwind == INNER_RET && psp_cpu.frame == &root, "indirect helper preserves a foreign unwind");
    psp_cpu.unwind = 0;

    psp_call_guest_fn(nested, OUTER_RET);
    CHECK(!continued && !psp_cpu.unwind && psp_cpu.frame == &root, "outer helper catches a propagated unwind");
    psp_call_guest_fn(tail_body, OUTER_RET);
    CHECK(calls == 3 && psp_cpu.frame == &root, "tail transfer adds no guest return frame");
    expected_ret = 0;  /* jalr with rd = zero retains the existing sentinel. */
    psp_call_guest_fn(leaf, 0);
    CHECK(calls == 4 && psp_cpu.frame == &root && !psp_cpu.unwind, "zero return-site sentinel is preserved");
    psp_register(TARGET, leaf);
    psp_call_guest_addr(TARGET, 0);
    CHECK(calls == 5 && psp_cpu.frame == &root && !psp_cpu.unwind, "indirect zero return-site sentinel is preserved");
    psp_register(TARGET, from_host_body);
    psp_call_from_host(TARGET);
    CHECK(calls == 6 && psp_cpu.frame == &root && !psp_cpu.unwind, "host barrier restores caller and consumes unwinds");
    psp_call_guest_fn(wait_body, OUTER_RET);
    CHECK(psp_cpu.frame == &root && !psp_cpu.unwind, "waiting helper restores the original frame");

    psp_set_miss_handler(on_miss);
    uint64_t before = psp_dispatch_misses();
    psp_call_guest_addr(ABSENT, OUTER_RET);
    CHECK(missed == ABSENT && psp_dispatch_misses() == before + 1, "missing indirect target keeps the miss handler");
    CHECK(psp_cpu.frame == &root && !psp_cpu.unwind, "miss path restores the outer frame");

    psp_cpu.frame = NULL;
    psp_threadman_reset();
    psp_dispatch_reset();
    printf("guest_calls: %d failures\n", failures);
    return failures != 0;
}
