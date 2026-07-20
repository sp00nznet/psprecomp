# Bring-up: where the recompiled game stops, and what has been ruled out

Lumberjack's recompiled C runs past `module_start` into the game's own startup,
prints its own debug message, and then walks a null pointer as a function table
and spins. This file records the investigation so far â€” mostly so that the
things already eliminated are not eliminated a second time.

## The symptom

```
libc:_getmodreent: no reent structure     <- the game's own libc, via sceKernelPrintf
dispatch miss: 0xFC2027BD not recompiled
dispatch miss: 0x000309FF not recompiled
... eight more
```

## What the garbage targets actually are

Searching `.text` for each miss value as a byte sequence finds eight of the ten,
all at **unaligned (+2)** offsets, at consecutive stride-4 addresses:

```
.text+0x0A, 0x0E, 0x12, 0x16, 0x1A, 0x1E, 0x22, 0x26
```

That is a loop walking a function-pointer table whose base is near address 8
and two bytes misaligned â€” the signature of a null pointer with a small offset
added, then indexed. It is not random corruption.

Crucially, **the libc message appears before the first miss.** The misses are
downstream of that failure, not independent of it.

## Ruled out

**Unresolved jump tables.** The computed-jump sites are now resolved (41 of 61
on Lumberjack, 70 of 102 on `hell2k`) and every one of those ~2100 targets was
already discovered by relocation mining. The garbage targets are not table
entries; they are wild register values.

**`sceKernelGetModuleIdByAddress` returning a fabricated id.** The theory was
that newlib's `_getmodreent` used that id, failed to match it against a
per-module table, and returned null. Changing it to return an error produced
**byte-identical output** â€” same message, same ten misses, same order. Not on
this path. (The error return was kept anyway: fabricating an id we cannot
honour is the sort of plausible lie that fails silently later.)

**A missing `$gp`.** Recompiled code addresses globals relative to `$gp`, and
the host never set it. Setting it from the module info's `gp_value`
(`0x003BC870` for Lumberjack) also produced **byte-identical output**. Setting
`$gp` is still correct and should be part of a real loader â€” it simply is not
what causes this.

**The `_getmodreent` message itself.** This turned out to be a red herring, and
the way it was eliminated is worth keeping: rather than looking for a reference
describing how newlib finds its per-module reent structure, the function is
*in the module* and can simply be read.

Locating the string `_getmodreent` (vaddr `0x0009446C`) and searching `.text`
for the `lui`/`addiu` pair that materialises it gives exactly one referencing
site, `0x0000F3C0`. The code around it:

```
0000F358  lw    $v0, 132($a1)      walk a linked list of module records
0000F35C  beq   $v0, $a3, 0xF3E4   match -> success path
0000F364  lw    $a2, 0($a2)        next node
0000F368  bne   $a2, $zero, 0xF358 loop while the list continues
          ...                      not found: print the warning
0000F3D0  lw    $v0, -14272($v0)   return *(0x003AC840)
```

So the function walks a module registry, fails to find this module â€” because
nothing ever registered it, which *is* a genuine loader gap â€” prints a warning,
and then returns a **fallback**. The word at `0x003AC840` in the image is
`0x003AC4C0`, a valid `.data` address: a global reent structure.

**It does not return null.** The warning is benign and the fallback works. The
message merely happens to be printed before the first dispatch miss; it does
not cause it. Two rounds of investigation were spent on the assumption that it
did.

The missing module registration remains real and should still be fixed by a
loader â€” it is simply not fatal, and not the thing to chase first.

## FOUND: there is executable code outside `.text`

A function-entry trace (build the generated code with `PSPRECOMP_TRACE`) names
the caller instead of only the bad target. The last function entered before the
first miss is `psp_func_000182F4`:

```
000182F4  lui   $a0, 0x3B
          addiu $a0, $a0, 12260     -> 0x003B2FE4
          addiu $a1, $a1, 12292     -> 0x003B3004
          addiu $a2, $a2, 18544     -> 0x003B4870
          j     0x000181B0          tail call, with $a3 also 0x003B4870
```

Two (start, end) pairs handed to a loop: the shape of a static-initializer
runner walking an init/fini array.

`0x003B2FE4..0x003B3004` holds eight pointers â€” to `0x003B2AF8`, `0x003B2B40`,
`0x003B2B7C`, and five more. And those addresses contain **code**:

```
0x003B2B40:  27BDFFF0   addiu $sp, $sp, -16     a function prologue
             AFBF000C   sw    $ra, 12($sp)
             0C013074   jal   ...
```

`.text` is `0x00000000..0x00091E54`. These constructors sit at `0x003B2AF8+`,
which is inside `.data`. **The toolkit analyses and emits `.text` only**, so
they were never discovered, never emitted and never registered â€” and the
runner calls straight into them.

That is the failure. It is not a wild pointer at all: the pointers are correct,
and the code they point at is real. It is simply code the recompiler never
looked at.

The earlier "garbage targets near address 8" reading was a coincidence of
byte-searching: those values also occur as unaligned spans inside `.text`, which
made them look like misaligned reads of code. They are better explained as
instruction words from the uncovered `.data` region being treated as addresses
once execution derails.

### What this needs

Discovery and emission must cover code wherever it lives, not just `.text`.
Concretely:

- Seed from the init/fini arrays, which are identifiable from the runner or
  from the `.rodata`/`.data` relocations that populate them.
- Allow the analysed extent to include regions outside `.text` rather than
  assuming one contiguous code range (`a_analysis` currently takes a single
  base/size).
- Keep the existing validation: a candidate region is only code if it decodes
  cleanly, so widening the range does not mean decoding all of `.data`.

This is a structural change to the analyser, not a stub or a table fix, and it
is on the critical path to running the game â€” the constructors must execute
before `main` does anything useful.

## What is actually still open

With the reent warning eliminated as the cause, the wild pointer walk has **no
current explanation**. What is known:

- It is a loop reading a function-pointer table at a base near address 8,
  misaligned by two, and calling each entry.
- It is not a jump table, not the reent fallback, not `$gp`, and not the
  ModuleMgr id.

The next move is to find which recompiled function performs that loop, rather
than guessing at causes. The dispatch miss handler knows the bad target but not
the caller; recording the caller â€” or simply recompiling with a breakpoint on
`psp_dispatch` and reading the C stack â€” would name it directly. That is a
better instrument than another hypothesis.

**Relocations are verified as no-ops.** All 27,269 entries in Lumberjack carry
`(offsetSegment, addendSegment) = (0, 0)` against a single segment at vaddr 0,
so applying them at load base 0 changes nothing. This was previously an
untested assumption; it holds, and relocation application is off the loader's
critical path.

**A loader is still wanted** for module registration and TLS/reent setup, and
to set `$gp` properly rather than from the host. None of that is on the
critical path for the current failure.

## Suggested next steps

1. Write a real loader in the runtime rather than the throwaway host: parse the
   ELF, place segments, apply relocations, set `$gp`, and record the module.
   Verify by checking that applying relocations at base 0 is genuinely a no-op
   before assuming it.
2. Give the host an **instruction budget**. Every run in this investigation had
   to be killed on a timer, which makes each experiment more expensive than it
   should be and makes a hang indistinguishable from slow progress.
3. Only then resume chasing `_getmodreent`.

## A note on method

Both falsifications above cost one build and one run each, and both killed a
plausible theory that would otherwise have absorbed real effort. The two
findings that *did* land â€” the encrypted-module key tag offset, and the
identification of these garbage values â€” both came from searching the binary
for a pattern rather than reasoning about what should be happening.

---

# The GE draws, and the game reaches it

## Where it stands

The game now submits display lists: **3 lists, 254 commands, 2 finishes**, and
sets a framebuffer at `0x04088000`. That is the first time anything has reached
the GPU. It gets there through libc startup, `sceCtrl` input setup, and into
what is almost certainly the render path.

It still does not draw. `vertices submitted: 0`, `vtype 0x000000` — the lists
seen so far are state setup, not geometry. 240 of the 254 commands are not
individually decoded, so some of that setup is being ignored rather than
applied.

## Every label is now dispatchable

A recompiled function has one C entry point. The original has as many as
something can jump to. Direct branches become `goto`, and discovery promotes
cross-function branches — but a **computed** jump resolves at runtime, and its
target is routinely a block in the middle of a function that discovery had no
static reason to make callable. Dispatch then missed on `0x0000E594`: an
address whose code was sitting right there in the generated file, under a label
nothing could reach.

Each function now takes an entry address and begins with a switch; each
interior label gets a thunk. 5849 functions, 7833 interior entries, 13682
dispatch entries. Jump tables, indirect calls and cross-function branches
resolve by construction.

This was the **fourth** bug in one family — a boundary change applied to
emission but not to reachability. The others: entry address, cross-function
targets, fall-through. Each was invisible in whatever metric was being watched
at the time. Making reachability total is what ends the pattern; the first
three were each fixed one crash at a time.

## Two ways to see a hang, because one was not enough

The host had no budget, so every run had to be killed on a timer and every
statistic was lost with it. Two mechanisms now:

- **A call budget** in `psp_dispatch`. Useful, but far narrower than it looks:
  direct calls compile to direct C calls, so it only counts *indirect* ones. A
  20-second run registered **38**.
- **A wall-clock watchdog** in the host. This is the one that works. It does not
  care about the shape of the loop, and a spin-wait compiled entirely into
  `goto`s inside a single function — exactly what bring-up gets stuck in — is
  invisible to any counter placed at a call boundary.

Note also that MSVC treats `_IOLBF` as full buffering. A killed run lost all of
its stdout, which is what made the first hang look like an instant silent crash.

## Where it hangs, and the standing hypothesis

Last function entered is `0x00046830`, but that and its neighbours are short
leaf helpers that return promptly — the loop is in a caller and contains **no
calls at all**: not dispatch (38 indirect calls total), not firmware (101
stderr lines, all first-call logs — only 11 `sceKernelGetGPI`).

A pure compute loop that never exits. Immediately before it:

```
unimplemented at 0x0002F898: vfpu?
unimplemented at 0x0002F89C: vcst
unimplemented at 0x0002F8A0: vcst
unimplemented at 0x0002F8B0: vidt
```

**Hypothesis: the unimplemented VFPU ops leave registers holding garbage, and a
loop that iterates until a value converges never converges.** `vcst` loads a
constant and `vidt` builds an identity vector — both matrix setup, both on the
render path, and both trapping to no-ops right now. A loop fed zeros or stale
values where it expects a unit basis is a good way to spin forever.

This is a hypothesis, not a finding. It is cheap to test: implement `vcst` and
`vidt`, identify the `vfpu?` at `0x0002F898`, and re-run. If the loop still
spins, the theory is dead and the next step is to find the loop head by address
rather than by trace.

## Next

1. Implement `vcst` / `vidt`; identify the unknown op at `0x0002F898`.
2. Decode the 240 unrecognised GE commands — some are certainly the vertex and
   texture state that would make `vtype` non-zero.
3. Transformed geometry. The rasterizer currently handles through-mode only and
   *counts* transformed vertices rather than drawing them, deliberately: wrong
   pixels are harder to diagnose than no pixels.

---

# Falsified: the VFPU-garbage theory

The standing hypothesis was that unimplemented VFPU ops left registers holding
garbage and a convergence loop therefore never converged. `vcst`, `vidt`,
`viim` and `vfim` are now implemented — `viim` needed a decoder fix too, since
opcode `0x37` sub-values 6 and 7 were falling through to "unknown" and the
`.word 0xDF3C00FF` at `0x0002F898` was in fact `viim.s v60, 255`.

**The output is byte-for-byte identical.** Same 3 lists, same 254 commands,
same hang. The theory is dead.

That is a clean result and it cost one build: the ops needed implementing
regardless, and the run says plainly that they were not what the loop was
waiting on. Do not revisit this.

## What the run actually points at

The spin is inside `0x000471F4`, which calls `0x00046830` (the last traced
entry) and then loops without calling anything else — which is why neither the
dispatch counter nor the firmware log can see it.

More promising, and visible in stderr just before the first null call:

```
libc:_getmodreent: no reent structure
dispatch miss: 0x00000000 not recompiled       (x17)
```

`_getmodreent` is back. Earlier work established that it returns a *working*
fallback rather than null, so this is not the old theory returning — but
something downstream is reading a function pointer out of a structure that was
never populated, getting zero, and calling it. 17 times, alongside 28 bad
memory accesses.

That is the thread to pull: find what writes the table those 17 null calls read
from. A null callback that is silently skipped is exactly the shape of a
program that then waits forever for something that callback was supposed to do.

## Next

1. Trace the 17 null dispatches to their call sites and find the unpopulated
   table.
2. Disassemble `0x000471F4` and read the loop condition directly — it will name
   the memory it is waiting on.
3. The 240 undecoded GE commands still hide whatever would set `vtype`.

---

# The hang, located exactly

## Read the trace in the right direction

`psp_trace_dump()` prints **newest first**. Reading the tail of a redirected
log therefore shows the *oldest* of the last 32 entries, and two rounds of
disassembly went into short leaf helpers that were never the problem. Running
the watchdog at 20s and 60s and diffing the two dumps is what caught it: both
stop at exactly **470 function entries**, so execution genuinely stalls rather
than crawling.

## The loop

`0x0004DD14`, reached through `0x0004DBB0` ? `0x0004DCF8`:

```
0004DD14  addu $t0, $a0, $a2          ; base + offset
0004DD18  lw   $a2, 4($t0)            ; compare key
0004DD1C  bne  $a2, $t1, 0x0004DD38
...
0004DD40  lhu  $v0, 16($t0)           ; next index
0004DD44  bnel $v0, $t2, 0x0004DD14   ; loop while != sentinel
0004DD48  sll  $a2, $v0, 4            ; 16-byte stride
```

A chain walk over 16-byte entries: read the next index from `+16`, stop when it
equals the sentinel `$t2` (loaded from `+24` of the header back at
`0x0004DA60`). It never stops.

## What that rules out

Host memory is `calloc`ed and `.bss` is therefore zero, so an **uninitialised
table would exit immediately** — `v0` would read 0 and the sentinel is 0. The
loop spinning means the table holds *garbage*: non-zero next-indices that never
reach the sentinel, most likely a chain that points back into itself.

The prime suspect is the **28 bad memory accesses**. A structure initialiser
whose writes landed outside mapped memory would leave exactly this: a header
that looks plausible and a chain that goes nowhere. The 17 null dispatches and
`libc:_getmodreent: no reent structure` are probably the same root cause seen
from a different angle.

## Next

1. Log the address of every bad memory access with the function that made it.
   That list is short (28) and one of them almost certainly writes this table.
2. Dump the 16-byte entries at `$a0` when the loop is entered — whether the
   chain self-references or runs off the end says which write went missing.
3. Only then go back to the GE. The 240 undecoded commands and `vtype == 0`
   are downstream of this: the game never gets far enough to submit geometry.

---

# Bad memory accesses: 28 to 3

Logging the *address* of each bad access, plus the function that made it, took
this from a bare count to a diagnosis in one run. The output clustered
immediately:

```
bad write32 at 0x00833FE0 (last fn 0x000123A4)   ... 18 accesses, one structure
bad write32 at 0x02336F68 (last fn 0x000473A4)   ... 7 accesses
bad read32  at 0x0C0247C5 (last fn 0x0000F3C0)   ... 3, unaligned
```

The first two clusters sit just past the host's `MODULE_SIZE` of `0x00500000`,
while the module's own `memsz` is only `0x004B0000`. That gap is the game's
**internal heap**, which starts after `.bss` and grows — straight out of the
mapping. Enlarging the module mapping to `0x03000000` drops bad accesses from
**28 to 3**.

The remaining three are a genuinely bad pointer: `0x0C0247C5` is unaligned and
outside any mapped region. Unresolved.

## This did not fix the hang

Same stall, same `0x0004DD14`, same 470 function entries. Worth stating plainly:
the structure writes that were being lost are *not* what feeds the unterminated
chain walk. Fixing them was correct and necessary, and it was not sufficient.

Two clean falsifications now — the VFPU-garbage theory and this one. Both cost
one run each, and both removed a plausible explanation that would otherwise
keep being re-proposed.

## Next

1. Dump the 16-byte table entries at the loop's `$a0` on entry. The chain either
   self-references or runs past the end; which one it is names the bug. This is
   the measurement that should have been taken before either of the two theories
   above — both were reasoning about the binary rather than reading it.
2. Chase the unaligned `0x0C0247C5` from `0x0000F3C0`.
