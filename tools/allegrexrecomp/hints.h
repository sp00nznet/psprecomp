/* Per-title hints - analysis results that belong to the game, not to a patch.
 *
 * A recomp project accumulates hard-won knowledge about one title: this block
 * is code, that routine's arguments are assembled by a ladder of computed
 * jumps, this address is a function nothing calls directly. Today that
 * knowledge lives in patches and in someone's head. The hints file makes it
 * data, kept in the game's repository beside the recompiled sources (ROADMAP,
 * "Hints file").
 *
 * This is the first kind, and only this one:
 *
 *     # A hand-written routine that jumps into a ladder of `b; move` pairs
 *     # never branches to the rungs, so they are not labels and dispatch has
 *     # nothing to land on. Naming them makes them reachable.
 *     [[label]]
 *     at = [0x0031D93C, 0x0031D944, 0x0031D94C]
 *
 * An address listed there is emitted as a dispatch entry of the function that
 * owns it: a `case` in the body's entry switch, a `psp_at_<addr>` thunk, and a
 * `psp_register_label` line. Nothing about the analysis changes - the file only
 * answers a question discovery could not.
 *
 * More kinds (force-code/force-data, function names, HLE overrides) go in
 * tables of their own, beside `label`, so a later toolkit reads the same file.
 *
 * The parser is deliberately small: it accepts the shape above (and comments,
 * blank lines, `0x` or decimal numbers, `_` digit separators, a multi-line
 * `at` array) and rejects everything else with the file, the line and the
 * reason. A hint that is silently ignored is worse than a hint that fails to
 * load, because the build then looks like it applied something it did not.
 *
 * One constraint from the toolkit: this lives in the recompiler only. The
 * runtime never reads a hints file, and no external library is pulled in for
 * it -- the subset above is a few dozen lines of parsing.
 */
#ifndef ALLEGREX_HINTS_H
#define ALLEGREX_HINTS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct a_hints {
    uint32_t *labels;   /* addresses forced to be dispatch entries */
    int       nlabels;
} a_hints;

/* Load `path` into `*out` (zeroed first, so a failed load leaves it empty).
 *
 * Returns 0 on success and -1 on failure, with a "path:line: reason" message in
 * `err` when `err` is not NULL. Addresses are validated here rather than at
 * emission: a misaligned or out-of-range address is a typo in the file, and
 * saying so while the file is being read beats a warning buried in a
 * quarter-million-line emission. */
int a_hints_load(const char *path, a_hints *out, char *err, size_t errlen);

void a_hints_free(a_hints *h);

#ifdef __cplusplus
}
#endif

#endif /* ALLEGREX_HINTS_H */
