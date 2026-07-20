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

## The remaining hypothesis

**There is no module loader.** The host copies the `PT_LOAD` segment to its
link address and jumps straight to `module_start`. On hardware the kernel does
substantial work first:

- applies the module's relocations,
- sets up TLS and the per-module reentrancy structure,
- registers the module so it can be found by id or address.

`_getmodreent` is looking for something a loader was supposed to have built.
That the failure appears *before any firmware call made during execution* fits
this and fits nothing else that has been tested.

Note the relocations have so far been used only for *discovery* (finding
function pointers). They have never been *applied* to the image. For a PRX
linked at 0 and loaded at 0 the addend is zero and most relocations are
no-ops — but that assumption has not been verified, and the segment-relative
forms in a PSP PRX are not plainly "add the load base".

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
