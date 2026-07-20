# Bring-up: where the recompiled game stops, and what has been ruled out

Lumberjack's recompiled C runs past `module_start` into the game's own startup,
prints its own debug message, and then walks a null pointer as a function table
and spins. This file records the investigation so far — mostly so that the
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
and two bytes misaligned — the signature of a null pointer with a small offset
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
**byte-identical output** — same message, same ten misses, same order. Not on
this path. (The error return was kept anyway: fabricating an id we cannot
honour is the sort of plausible lie that fails silently later.)

**A missing `$gp`.** Recompiled code addresses globals relative to `$gp`, and
the host never set it. Setting it from the module info's `gp_value`
(`0x003BC870` for Lumberjack) also produced **byte-identical output**. Setting
`$gp` is still correct and should be part of a real loader — it simply is not
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

So the function walks a module registry, fails to find this module — because
nothing ever registered it, which *is* a genuine loader gap — prints a warning,
and then returns a **fallback**. The word at `0x003AC840` in the image is
`0x003AC4C0`, a valid `.data` address: a global reent structure.

**It does not return null.** The warning is benign and the fallback works. The
message merely happens to be printed before the first dispatch miss; it does
not cause it. Two rounds of investigation were spent on the assumption that it
did.

The missing module registration remains real and should still be fixed by a
loader — it is simply not fatal, and not the thing to chase first.

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

`0x003B2FE4..0x003B3004` holds eight pointers — to `0x003B2AF8`, `0x003B2B40`,
`0x003B2B7C`, and five more. And those addresses contain **code**:

```
0x003B2B40:  27BDFFF0   addiu $sp, $sp, -16     a function prologue
             AFBF000C   sw    $ra, 12($sp)
             0C013074   jal   ...
```

`.text` is `0x00000000..0x00091E54`. These constructors sit at `0x003B2AF8+`,
which is inside `.data`. **The toolkit analyses and emits `.text` only**, so
they were never discovered, never emitted and never registered — and the
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
is on the critical path to running the game — the constructors must execute
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
the caller; recording the caller — or simply recompiling with a breakpoint on
`psp_dispatch` and reading the C stack — would name it directly. That is a
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
findings that *did* land — the encrypted-module key tag offset, and the
identification of these garbage values — both came from searching the binary
for a pattern rather than reasoning about what should be happening.
