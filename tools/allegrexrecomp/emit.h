/* The C emitter — turning discovered functions into readable native C.
 *
 * One `psp_func_<addr>` per discovered function, every line carrying its
 * address and original disassembly as a comment, lowered to the helpers in
 * <psprecomp/recomp_rt.h>. The output is meant to be *read*, not merely
 * compiled: a recomp project is only useful to other people if they can open
 * the generated file and see what the original was doing.
 *
 * The hard part is delay slots. Every MIPS branch and jump executes the
 * instruction *after* it before control transfers, and "likely" branches
 * nullify theirs when not taken. See the notes in emit.c — that single
 * detail is where most of this file's care goes.
 */
#ifndef ALLEGREX_EMIT_H
#define ALLEGREX_EMIT_H

#include "analyze.h"
#include "container.h"
#include "hints.h"

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *outdir;     /* directory to write into */
    const char *prefix;     /* file/base name, e.g. "recomp" */
    const char *module;     /* module name, for the file header comment */

    /* The module's import table, so each generated thunk can dispatch to the
     * HLE layer by NID and carry the firmware function's library and NID in a
     * comment. Without it the thunks can only trap. */
    const psp_import_entry *imports;
    int                     nimports;

    /* Per-title hints, or NULL. An address listed there is emitted as a
     * dispatch entry of the function that owns it, which is how a hand-written
     * routine's computed targets become reachable without the analysis having
     * to guess them (hints.h). */
    const a_hints *hints;
} emit_opts;

/* Emit the whole module: <prefix>_funcs.h, <prefix>_imports.c,
 * <prefix>_register.c, and the functions split across <prefix>_funcs_NNN.c so
 * the corpus compiles in parallel and rebuilds incrementally. A file whose
 * contents did not change is left untouched. Returns 0 on success. */
int a_emit(const a_analysis *an, const emit_opts *o);

/* Emit one function to `out`, for reading rather than compiling. Returns -1 if
 * no discovered function starts at `addr`. */
int a_emit_one(const a_analysis *an, uint32_t addr, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* ALLEGREX_EMIT_H */
