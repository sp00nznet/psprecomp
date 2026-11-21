/* Dispatch trace tests — the observation hooks a host uses to watch guest
 * execution.
 *
 * The interesting property here is not "a callback runs": it is what happens
 * once more than one site is observed. A single slot made a chain unobservable
 * (arming the second site hid the first) and made an unwatched address report
 * exactly like an unreached one, which is how a bring-up session concludes
 * "never called" about code nobody was watching. These cases pin the slot
 * semantics: append, replace, overflow, clear, and what a callback may do to
 * the set while it is being dispatched.
 *
 * All addresses are synthetic. Nothing here touches a game.
 */

#include "psprecomp/dispatch.h"

#include <stdio.h>

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

#define CHECK_EQ(got, want, label)                             \
    CHECK((got) == (want), "%s: got 0x%08X, want 0x%08X",       \
          (label), (unsigned)(got), (unsigned)(want))

static unsigned hits1, hits2, hits3;
static uint32_t last1, last2, last3;

static void cb1(uint32_t a) { hits1++; last1 = a; }
static void cb2(uint32_t a) { hits2++; last2 = a; }
static void cb3(uint32_t a) { hits3++; last3 = a; }

static void reset_hits(void) {
    hits1 = hits2 = hits3 = 0;
    last1 = last2 = last3 = 0;
}

/* The single-site call keeps its old meaning: one site, replaced wholesale. */
static void test_single_site(void) {
    psp_trace_watch_clear();
    reset_hits();
    psp_trace_watch(0x00100000u, cb1);
    CHECK_EQ(psp_trace_watch_count(), 1, "count after watch");

    psp_trace_enter(0x00100000u);
    CHECK_EQ(hits1, 1, "watched entry fires");
    CHECK_EQ(last1, 0x00100000u, "callback sees the address");

    psp_trace_enter(0x00200000u);
    psp_trace_enter(0x00200000u);
    CHECK_EQ(hits1, 1, "unwatched entry does not fire");
}

/* Added sites are independent of the first one. */
static void test_add_sites(void) {
    psp_trace_watch_clear();
    reset_hits();
    psp_trace_watch(0x00100000u, cb1);
    CHECK_EQ(psp_trace_watch_add(0x00200000u, cb2), 1, "second slot index");
    CHECK_EQ(psp_trace_watch_add(0x00300000u, cb3), 2, "third slot index");
    CHECK_EQ(psp_trace_watch_count(), 3, "count after adds");

    psp_trace_enter(0x00200000u);
    CHECK_EQ(hits2, 1, "added site fires");
    CHECK_EQ(hits1, 0, "first site untouched by a different entry");
    CHECK_EQ(hits3, 0, "third site untouched by a different entry");

    psp_trace_enter(0x00100000u);
    psp_trace_enter(0x00300000u);
    CHECK_EQ(hits1, 1, "first site still fires after adds");
    CHECK_EQ(hits3, 1, "third site fires");
}

/* The table is bounded, and a full table says so instead of dropping the site. */
static void test_full_table(void) {
    psp_trace_watch_clear();
    reset_hits();
    psp_trace_watch(0x00100000u, cb1);
    for (unsigned i = 1; i < PSP_TRACE_WATCH_SLOTS; i++)
        CHECK_EQ(psp_trace_watch_add(0x00100000u + i * 4u, cb2), (int)i, "fill slot");
    CHECK_EQ(psp_trace_watch_count(), PSP_TRACE_WATCH_SLOTS, "table full");
    CHECK_EQ(psp_trace_watch_add(0x00900000u, cb3), -1, "overflow rejected");
    CHECK_EQ(psp_trace_watch_count(), PSP_TRACE_WATCH_SLOTS, "count unchanged by a rejected add");

    psp_trace_enter(0x00900000u);
    CHECK_EQ(hits3, 0, "a rejected site is not silently observed");
}

/* A NULL callback is not a watch: it must not occupy a slot. */
static void test_null_callback(void) {
    psp_trace_watch_clear();
    CHECK_EQ(psp_trace_watch_add(0x00100000u, 0), -1, "NULL callback rejected");
    CHECK_EQ(psp_trace_watch_count(), 0, "no slot taken by a NULL callback");
}

/* `watch` replaces the whole set, which is what a host that wants one site means. */
static void test_replace(void) {
    psp_trace_watch_clear();
    reset_hits();
    psp_trace_watch(0x00100000u, cb1);
    psp_trace_watch_add(0x00200000u, cb2);
    psp_trace_watch(0x00300000u, cb3);
    CHECK_EQ(psp_trace_watch_count(), 1, "replace leaves one site");

    psp_trace_enter(0x00100000u);
    psp_trace_enter(0x00200000u);
    CHECK_EQ(hits1, 0, "replaced site no longer fires");
    CHECK_EQ(hits2, 0, "dropped site no longer fires");
    psp_trace_enter(0x00300000u);
    CHECK_EQ(hits3, 1, "replacement site fires");
}

static void test_clear(void) {
    psp_trace_watch_clear();
    reset_hits();
    psp_trace_watch(0x00100000u, cb1);
    psp_trace_watch_add(0x00200000u, cb2);
    psp_trace_watch_clear();
    CHECK_EQ(psp_trace_watch_count(), 0, "cleared");
    psp_trace_enter(0x00100000u);
    psp_trace_enter(0x00200000u);
    CHECK_EQ(hits1 + hits2, 0, "nothing fires after clear");
}

/* A callback may append a site; it is observed from the next entry, not the
 * current one, because the dispatch walks the table it found. */
static void cb_append(uint32_t a) {
    (void)a;
    hits1++;
    if (hits1 == 1) psp_trace_watch_add(0x00200000u, cb2);
}

static void test_callback_appends(void) {
    psp_trace_watch_clear();
    reset_hits();
    psp_trace_watch(0x00100000u, cb_append);
    psp_trace_enter(0x00100000u);
    CHECK_EQ(hits2, 0, "site added during dispatch is not fired for that entry");
    psp_trace_enter(0x00200000u);
    CHECK_EQ(hits2, 1, "site added during dispatch is live for the next entry");
}

/* A callback that replaces the set ends the dispatch: no stale slot is called. */
static void cb_replace(uint32_t a) {
    (void)a;
    hits1++;
    psp_trace_watch(0x00300000u, cb3);
}

static void test_callback_replaces(void) {
    psp_trace_watch_clear();
    reset_hits();
    psp_trace_watch(0x00100000u, cb_replace);
    psp_trace_watch_add(0x00200000u, cb2);
    psp_trace_enter(0x00100000u);
    CHECK_EQ(hits1, 1, "replacing callback ran once");
    CHECK_EQ(psp_trace_watch_count(), 1, "set replaced from inside a callback");
    psp_trace_enter(0x00200000u);
    CHECK_EQ(hits2, 0, "dropped site does not fire afterwards");
    psp_trace_enter(0x00300000u);
    CHECK_EQ(hits3, 1, "replacement site fires");
}

/* Label watches follow the same rules, on a different emitter hook. */
static void test_label_sites(void) {
    psp_trace_watch_label_clear();
    reset_hits();
    psp_trace_watch_label(0x00400000u, cb1);
    CHECK_EQ(psp_trace_watch_label_add(0x00500000u, cb2), 1, "label second slot");
    CHECK_EQ(psp_trace_watch_label_count(), 2, "label count");

    psp_trace_mark(0x00400000u);
    psp_trace_mark(0x00500000u);
    psp_trace_mark(0x00600000u);
    CHECK_EQ(hits1, 1, "first label fires");
    CHECK_EQ(hits2, 1, "second label fires");
    CHECK_EQ(hits1 + hits2, 2, "an unmarked label fires nothing");

    psp_trace_watch_label(0x00600000u, cb3);
    CHECK_EQ(psp_trace_watch_label_count(), 1, "label replace");
    psp_trace_mark(0x00400000u);
    CHECK_EQ(hits1, 1, "replaced label stops firing");
    psp_trace_mark(0x00600000u);
    CHECK_EQ(hits3, 1, "label replacement fires");
    psp_trace_watch_label_clear();
    CHECK_EQ(psp_trace_watch_label_count(), 0, "label clear");
}

/* Marking labels and watching them are independent: the reachability bitmap
 * keeps working while several sites are watched. */
static void test_marks_bitmap(void) {
    psp_trace_watch_label_clear();
    reset_hits();
    psp_trace_marks_init(0x00700000u, 4);
    psp_trace_watch_label(0x00700008u, cb1);
    psp_trace_watch_label_add(0x00700004u, cb2);

    psp_trace_mark(0x00700008u);
    CHECK_EQ(psp_trace_was_marked(0x00700008u), 1, "marked word recorded");
    CHECK_EQ(psp_trace_was_marked(0x00700004u), 0, "unmarked word stays clear");
    CHECK_EQ(psp_trace_was_marked(0x00800000u), -1, "outside the bitmap is -1");
    CHECK_EQ(hits1, 1, "watched label fired with a bitmap installed");
}

/* The entry history is untouched by the slot change. */
static void test_trace_history(void) {
    psp_trace_watch_clear();
    psp_trace_reset();
    psp_trace_enter(0x00ABCDE0u);
    CHECK_EQ(psp_trace_last(), 0x00ABCDE0u, "trace still records entries");
}

int main(void) {
    test_single_site();
    test_add_sites();
    test_full_table();
    test_null_callback();
    test_replace();
    test_clear();
    test_callback_appends();
    test_callback_replaces();
    test_label_sites();
    test_marks_bitmap();
    test_trace_history();

    if (failures) {
        printf("dispatch: %d failure(s)\n", failures);
        return 1;
    }
    printf("dispatch: all checks passed (%d slots)\n", PSP_TRACE_WATCH_SLOTS);
    return 0;
}
