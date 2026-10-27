/* psprecomp — address → function dispatch. See include/psprecomp/dispatch.h. */

#include "psprecomp/dispatch.h"
#include "psprecomp/cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* An open-addressed hash table. A module has a few thousand functions and
 * lookups happen on every indirect call, so this wants to be cheap; addresses
 * are 4-aligned and densely clustered, which a power-of-two table with a
 * multiplicative hash handles well. */

typedef struct {
    uint32_t addr;
    psp_fn_t fn;
    int      used;
} slot;

static slot   *g_table;
static uint32_t g_cap;      /* always a power of two */
static uint32_t g_count;
static uint64_t g_misses;
static psp_miss_fn_t g_miss;
static psp_miss_ctx_fn_t g_miss_ctx;

void psp_set_miss_context(psp_miss_ctx_fn_t fn) { g_miss_ctx = fn; }
void psp_miss_context(void) { if (g_miss_ctx) g_miss_ctx(); }

static uint32_t hash_addr(uint32_t a) {
    /* Knuth multiplicative. The low two bits are always zero, so shift them
     * out first or a quarter of the table would never be used. */
    return (a >> 2) * 2654435761u;
}

static void grow(void) {
    uint32_t ncap = g_cap ? g_cap * 2 : 4096;
    slot *nt = (slot *)calloc(ncap, sizeof *nt);
    if (!nt) return;

    for (uint32_t i = 0; i < g_cap; i++) {
        if (!g_table[i].used) continue;
        uint32_t m = ncap - 1;
        uint32_t j = hash_addr(g_table[i].addr) & m;
        while (nt[j].used) j = (j + 1) & m;
        nt[j] = g_table[i];
    }
    free(g_table);
    g_table = nt;
    g_cap = ncap;
}

void psp_register(uint32_t addr, psp_fn_t fn) {
    if (!g_table || (g_count + 1) * 4 >= g_cap * 3) grow();   /* keep load < 0.75 */
    if (!g_table) return;

    uint32_t m = g_cap - 1;
    uint32_t j = hash_addr(addr) & m;
    while (g_table[j].used) {
        if (g_table[j].addr == addr) { g_table[j].fn = fn; return; }  /* replace */
        j = (j + 1) & m;
    }
    g_table[j].addr = addr;
    g_table[j].fn = fn;
    g_table[j].used = 1;
    g_count++;
}

/* Register an interior label -- a block in the middle of a function that some
 * computed jump can land on. Unlike psp_register this never overwrites: a real
 * function entry runs the whole function, a label thunk enters partway through,
 * and if an address is both then the entry is the correct answer. */
void psp_register_label(uint32_t addr, psp_fn_t fn) {
    if (!g_table || (g_count + 1) * 4 >= g_cap * 3) grow();
    if (!g_table) return;

    uint32_t m = g_cap - 1;
    uint32_t j = hash_addr(addr) & m;
    while (g_table[j].used) {
        if (g_table[j].addr == addr) return;      /* already known: keep it */
        j = (j + 1) & m;
    }
    g_table[j].addr = addr;
    g_table[j].fn = fn;
    g_table[j].used = 1;
    g_count++;
}

psp_fn_t psp_lookup(uint32_t addr) {
    if (!g_table) return NULL;
    uint32_t m = g_cap - 1;
    uint32_t j = hash_addr(addr) & m;
    while (g_table[j].used) {
        if (g_table[j].addr == addr) return g_table[j].fn;
        j = (j + 1) & m;
    }
    return NULL;
}

/* ---- function-entry trace ------------------------------------------------ */

/* Deep enough to hold a whole initialisation chain. 32 was chosen for a
 * single-threaded bring-up where the interesting chain was short; a module
 * that walks a table of 100 initialisers runs far deeper than that, and the
 * entry it is stuck in falls out of the window exactly when it is needed. */
#define TRACE_DEPTH 4096
static uint32_t g_trace[TRACE_DEPTH];
static uint64_t g_trace_n;

/* A hook on entry to specific functions. Bring-up repeatedly needs to see the
 * arguments to one function out of thousands, and rebuilding the generated code
 * with a printf in it is both slow and easy to leave behind.
 *
 * Several sites at once, because one is not enough to follow a chain: watching
 * the first function hides the second, so a host re-arms from inside each
 * callback and everything in between goes unseen. Worse, an address nobody is
 * watching reports "not reached" exactly like a real negative. The table keeps
 * the single-site call meaning what it always meant (set slot 0, drop the rest)
 * and lets `_add` append independent sites. */
static struct { uint32_t addr; void (*fn)(uint32_t); } g_watch[PSP_TRACE_WATCH_SLOTS];
static unsigned g_watch_n;

void psp_trace_watch(uint32_t addr, void (*fn)(uint32_t)) {
    g_watch[0].addr = addr;
    g_watch[0].fn = fn;
    g_watch_n = 1;
}

int psp_trace_watch_add(uint32_t addr, void (*fn)(uint32_t)) {
    if (!fn || g_watch_n >= PSP_TRACE_WATCH_SLOTS) return -1;
    g_watch[g_watch_n].addr = addr;
    g_watch[g_watch_n].fn = fn;
    return (int)g_watch_n++;
}

int psp_trace_watch_count(void) { return (int)g_watch_n; }
void psp_trace_watch_clear(void) { g_watch[0].addr = 0; g_watch[0].fn = 0; g_watch_n = 0; }

void psp_trace_enter(uint32_t addr) {
    /* Slot 0 is checked exactly the way the single site used to be, so a host
     * with one site (or none) pays the same compare and branch as before. Only
     * a host that armed extra sites pays for the loop, and the count is
     * snapshotted so a site added from inside a callback is observed from the
     * next entry, not this one. */
    unsigned n = g_watch_n;
    if (addr == g_watch[0].addr && g_watch[0].fn) g_watch[0].fn(addr);
    for (unsigned i = 1; i < n && i < g_watch_n; i++) {
        if (g_watch[i].addr != addr) continue;
        void (*fn)(uint32_t) = g_watch[i].fn;
        if (fn) fn(addr);
    }
    g_trace[g_trace_n % TRACE_DEPTH] = addr;
    g_trace_n++;
}

void psp_trace_reset(void) { g_trace_n = 0; }

/* The most recent traced function entry -- who was running when something else
 * went wrong. */
uint32_t psp_trace_last(void) {
    return g_trace_n ? g_trace[(g_trace_n - 1) % TRACE_DEPTH] : 0;
}

void psp_trace_dump(void) {
    if (!g_trace_n) {
        fprintf(stderr, "  (no function trace -- build the generated code with "
                        "PSPRECOMP_TRACE to enable it)\n");
        return;
    }
    uint64_t n = g_trace_n < TRACE_DEPTH ? g_trace_n : TRACE_DEPTH;
    fprintf(stderr, "  last %llu functions entered, newest first:\n",
            (unsigned long long)n);
    for (uint64_t i = 1; i <= n; i++) {
        uint64_t k = (g_trace_n - i) % TRACE_DEPTH;
        fprintf(stderr, "    psp_func_%08X\n", g_trace[k]);
    }
    fprintf(stderr, "  (%llu function entries total)\n",
            (unsigned long long)g_trace_n);
}

static void default_miss(uint32_t addr) {
    fprintf(stderr,
            "psprecomp: indirect call to 0x%08X, which is not a recompiled "
            "function.\n"
            "  Either discovery missed it, or it is data being called as code.\n",
            addr);
    psp_miss_context();
    abort();
}

void psp_set_miss_handler(psp_miss_fn_t fn) { g_miss = fn; }

/* A budget on dispatched calls.
 *
 * A game's main loop does not return, and during bring-up it is just as likely
 * to be spinning on a condition nothing will ever satisfy. Both look identical
 * from outside: the process sits there. Killing it from the shell loses every
 * statistic that would say which one it is.
 *
 * With a budget the run ends on its own and the host prints its report, so a
 * hang becomes readable evidence instead of a stopped terminal. */
static uint64_t g_calls, g_budget;
static void (*g_over)(void);

void psp_dispatch_set_budget(uint64_t calls, void (*on_exceeded)(void)) {
    g_budget = calls;
    g_over = on_exceeded;
    g_calls = 0;
}

uint64_t psp_dispatch_calls(void) { return g_calls; }

void psp_call_from_host(uint32_t addr) {
    psp_frame f = { 0, psp_cpu.frame };
    psp_cpu.frame = &f;
    psp_dispatch(addr);
    psp_cpu.frame = f.prev;
    psp_cpu.unwind = 0;              /* no unwind crosses a host frame */
}

int psp_ra_is_jump(uint32_t ra) {
    const psp_frame *f = psp_cpu.frame;
    if (!f || !f->ret || ra == f->ret) return 0;
    for (const psp_frame *g = f->prev; g && g->ret; g = g->prev)
        if (g->ret == ra) { psp_cpu.unwind = ra; return 0; }
    return 1;
}

void psp_dispatch(uint32_t addr) {
    if (g_budget && ++g_calls >= g_budget) {
        g_budget = 0;                 /* fire once */
        if (g_over) g_over();
    }
    psp_fn_t fn = psp_lookup(addr);
    if (fn) {
        /* Log the first few *successful* indirect calls. Every session so far
         * has examined only the failures; a working virtual call sitting next
         * to a broken one says where vptrs come from. */
        extern int psp_log_indirect;
        static int logged;
        if (psp_log_indirect && logged < 12) {
            logged++;
            fprintf(stderr, "indirect ok: target=0x%08X from fn 0x%08X\n",
                    addr, psp_trace_last());
        }
        fn();
        return;
    }

    g_misses++;
    /* Aborting by default is deliberate. A silently-ignored indirect call
     * produces a program that runs and is wrong, which is far more expensive
     * to debug than one that stops and names the address. A host that wants to
     * survive misses during bring-up installs its own handler. */
    (g_miss ? g_miss : default_miss)(addr);
}

uint32_t psp_dispatch_count(void)  { return g_count; }
uint64_t psp_dispatch_misses(void) { return g_misses; }

void psp_dispatch_reset(void) {
    free(g_table);
    g_table = NULL;
    g_cap = g_count = 0;
    g_misses = 0;
    g_miss = NULL;
}

/* The last loop back-edge taken.
 *
 * Entry tracing cannot see a body that loops after its last call: no function
 * is entered, so the trace simply stops with the newest entry being some
 * innocent function that already returned. That blind spot cost three rounds of
 * disassembling the wrong code. Recording back-edges closes it -- a spinning
 * loop keeps writing here even though nothing else moves. */
static uint32_t g_loop_addr;
static uint64_t g_loop_hits;

void psp_trace_loop(uint32_t addr) { g_loop_addr = addr; g_loop_hits++; }
uint32_t psp_trace_loop_addr(void) { return g_loop_addr; }
uint64_t psp_trace_loop_hits(void) { return g_loop_hits; }

int psp_log_indirect;


/* Label-level reachability.
 *
 * psp_trace_watch hooks PSP_ENTER, which is emitted once per function body. A
 * watch on any address that is not a function entry therefore never fires, and
 * reports "not reached" for something it simply cannot observe -- a negative
 * indistinguishable from a real one. That flaw produced a confidently wrong
 * conclusion about an allocation failing.
 *
 * Marking every label closes it: any address the emitter gave a label to can
 * now be watched, and psp_trace_watch_ok() says up front whether an address is
 * observable at all. */
static uint8_t *g_marked;
static uint32_t g_marked_lo, g_marked_n;

/* Same slot rule as the entry watch, for the same reason: a chain of labels
 * needs more than one observable site, and an address nobody watches is
 * indistinguishable from an address never reached. */
static struct { uint32_t addr; void (*fn)(uint32_t); } g_mark[PSP_TRACE_WATCH_SLOTS];
static unsigned g_mark_n;

void psp_trace_mark(uint32_t addr) {
    if (g_marked && addr >= g_marked_lo && addr - g_marked_lo < g_marked_n * 4)
        g_marked[(addr - g_marked_lo) >> 2] = 1;
    unsigned n = g_mark_n;
    if (addr == g_mark[0].addr && g_mark[0].fn) g_mark[0].fn(addr);
    for (unsigned i = 1; i < n && i < g_mark_n; i++) {
        if (g_mark[i].addr != addr) continue;
        void (*fn)(uint32_t) = g_mark[i].fn;
        if (fn) fn(addr);
    }
}

void psp_trace_watch_label(uint32_t addr, void (*fn)(uint32_t)) {
    g_mark[0].addr = addr;
    g_mark[0].fn = fn;
    g_mark_n = 1;
}

int psp_trace_watch_label_add(uint32_t addr, void (*fn)(uint32_t)) {
    if (!fn || g_mark_n >= PSP_TRACE_WATCH_SLOTS) return -1;
    g_mark[g_mark_n].addr = addr;
    g_mark[g_mark_n].fn = fn;
    return (int)g_mark_n++;
}

int psp_trace_watch_label_count(void) { return (int)g_mark_n; }
void psp_trace_watch_label_clear(void) { g_mark[0].addr = 0; g_mark[0].fn = 0; g_mark_n = 0; }

void psp_trace_marks_init(uint32_t lo, uint32_t words) {
    free(g_marked);
    g_marked = (uint8_t *)calloc(words ? words : 1, 1);
    g_marked_lo = lo;
    g_marked_n = words;
}

int psp_trace_was_marked(uint32_t addr) {
    if (!g_marked || addr < g_marked_lo || addr - g_marked_lo >= g_marked_n * 4) return -1;
    return g_marked[(addr - g_marked_lo) >> 2];
}

/* Stack-balance violations.
 *
 * A recompiled function must leave $sp as it found it. One that does not
 * corrupts every callee-saved register its caller restores afterwards: the
 * restores read `sp + offset`, so a shifted $sp reads a neighbouring slot and
 * loads a plausible wrong value instead of failing. That is invisible until
 * the wrong value is dereferenced, far from the cause.
 *
 * Checking the invariant at every return finds the culprit directly.
 *
 * The per-message cap below is a second trap, and it bites the other way: the
 * log stops after a dozen lines, so "no large delta in the log" reads as "no
 * large delta" when it only means "the log stopped". The table here keeps every
 * distinct (site, delta) pair with its count, and keeps the largest magnitude
 * seen even when the table is full, so one line of report names the biggest
 * imbalance whatever the volume. Measured on LocoRoco: every printed line was a
 * +16/+32/+48 while the run's actual story was two -656 body checks and the
 * cascade of -656 call rows that ended in a `jalr` on 0xFFFFFFFF. */
#define PSP_SP_SITES 96

typedef struct {
    uint32_t site;      /* fn (body check) or callee (call check) */
    int32_t  delta;
    uint64_t count;
    uint32_t in;        /* $sp expected / $sp before the call */
    uint32_t out;       /* $sp observed / $sp after the call */
    uint32_t last;      /* last function entered (body checks) */
} sp_row;

typedef struct { sp_row rows[PSP_SP_SITES]; int n; sp_row worst; } sp_table;

static sp_table g_sp_tbl, g_spc_tbl;

static int64_t sp_mag(int32_t d) { return d < 0 ? -(int64_t)d : (int64_t)d; }

static void sp_record(sp_table *t, uint32_t site, int32_t d,
                      uint32_t in, uint32_t out, uint32_t last) {
    sp_row r = { site, d, 1, in, out, last };
    if (t->worst.count == 0 || sp_mag(d) > sp_mag(t->worst.delta)) t->worst = r;
    for (int i = 0; i < t->n; i++)
        if (t->rows[i].site == site && t->rows[i].delta == d) {
            t->rows[i].count++;
            return;
        }
    if (t->n < PSP_SP_SITES) { t->rows[t->n++] = r; return; }
    /* Table full: evict the smallest magnitude, so a report that cannot hold
     * everything still holds the part that matters. */
    int victim = 0;
    for (int i = 1; i < t->n; i++)
        if (sp_mag(t->rows[i].delta) < sp_mag(t->rows[victim].delta)) victim = i;
    if (sp_mag(d) > sp_mag(t->rows[victim].delta)) t->rows[victim] = r;
}

/* Largest magnitude first: the report reads the story, not the arrival order.
 * The worst row is also kept on its own, so the entry that matters survives a
 * table that fills up with small deltas. */
static int sp_dump(const sp_table *t, psp_sp_site *out, int max) {
    int n = t->n < max ? t->n : max;
    int used[PSP_SP_SITES];
    for (int i = 0; i < t->n; i++) used[i] = 0;
    for (int k = 0; k < n; k++) {
        int best = -1;
        for (int i = 0; i < t->n; i++) {
            if (used[i]) continue;
            if (best < 0 || sp_mag(t->rows[i].delta) > sp_mag(t->rows[best].delta)) best = i;
        }
        used[best] = 1;
        out[k].site  = t->rows[best].site;
        out[k].delta = t->rows[best].delta;
        out[k].count = t->rows[best].count;
        out[k].in    = t->rows[best].in;
        out[k].out   = t->rows[best].out;
        out[k].last  = t->rows[best].last;
    }
    return n;
}

int psp_sp_hist(psp_sp_site *out, int max)      { return sp_dump(&g_sp_tbl, out, max); }
int psp_sp_call_hist(psp_sp_site *out, int max) { return sp_dump(&g_spc_tbl, out, max); }

static uint64_t g_sp_bad;
static uint32_t g_sp_first;
static int32_t  g_sp_delta;

void psp_trace_sp(uint32_t fn, uint32_t sp_in, uint32_t sp_out) {
    if (sp_in == sp_out) return;
    sp_record(&g_sp_tbl, fn, (int32_t)(sp_out - sp_in), sp_in, sp_out, psp_trace_last());
    /* Print them all. A *positive* delta usually means a split continuation
     * that contains an epilogue but not its matching prologue -- discovery
     * makes those separate bodies, so the invariant does not hold for them and
     * they are noise. A *negative* delta is the dangerous case: stack consumed
     * and never returned. */
    if (g_sp_bad < 24)
        fprintf(stderr, "sp UNBALANCED in 0x%08X: %+d%s\n", fn,
                (int)(sp_out - sp_in),
                (int32_t)(sp_out - sp_in) < 0 ? "   <-- leak" : "");
    if (!g_sp_bad) { g_sp_first = fn; g_sp_delta = (int32_t)(sp_out - sp_in); }
    g_sp_bad++;
}

/* Stack balance at a transfer that ENDS a body: a fall-out into another
 * fragment, or a `j` to another function's entry.
 *
 * The invariant is not the one at a `jr $ra`. A fragment that falls out hands
 * over mid-frame: the frame was allocated by the first fragment of the same
 * original function and the matching epilogue lives in its last one, so $sp is
 * legitimately *below* this fragment's entry by exactly the frame it allocated.
 * Measured on LocoRoco: every negative delta reported at a fall-out site is
 * that frame, returned later in the same function -- 0x1A294 allocates 2112 and
 * 0x1A380 returns 2112 in its `jr $ra` delay slot, 0x3E4 allocates 64 and
 * 0x4C4 returns 64. On one run those were 206 reports, and calling them leaks
 * hid the sign that does mean something here: a fragment that hands over with
 * $sp *above* its entry released a frame it never took, which is what makes its
 * predecessor's restores read the wrong slots. */
void psp_trace_sp_leave(uint32_t fn, uint32_t sp_in, uint32_t sp_out) {
    if (sp_in == sp_out) return;
    int32_t d = (int32_t)(sp_out - sp_in);
    if (d < 0) return;                    /* this fragment's own frame: expected */
    sp_record(&g_sp_tbl, fn, d, sp_in, sp_out, psp_trace_last());
    if (g_sp_bad < 24)
        fprintf(stderr, "sp HANDED OVER HIGH in 0x%08X: %+d   <-- released a frame "
                        "it never took\n", fn, (int)d);
    if (!g_sp_bad) { g_sp_first = fn; g_sp_delta = d; }
    g_sp_bad++;
}

uint64_t psp_sp_violations(void) { return g_sp_bad; }
uint32_t psp_sp_first_bad(void)  { return g_sp_first; }
int32_t  psp_sp_first_delta(void){ return g_sp_delta; }

/* Stack imbalance observed across a call. Complements psp_trace_sp: that one
 * checks a body against its own entry and is blind to leaks that straddle a
 * body boundary; this one names the callee directly. */
static uint64_t g_spc_bad;
void psp_trace_sp_call(uint32_t callee, uint32_t sp_before, uint32_t sp_after) {
    if (sp_before == sp_after) return;
    sp_record(&g_spc_tbl, callee, (int32_t)(sp_after - sp_before),
              sp_before, sp_after, psp_trace_last());
    if (g_spc_bad < 16)
        fprintf(stderr, "sp NOT RESTORED by callee 0x%08X: %+d\n",
                callee, (int)(sp_after - sp_before));
    g_spc_bad++;
}
uint64_t psp_sp_call_violations(void) { return g_spc_bad; }
