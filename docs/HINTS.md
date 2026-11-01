# Hints - what the analysis cannot know, written down instead of patched

Some of what a recompilation needs is not in the module. A hand-written routine
marshals its arguments by jumping into a ladder of `b; move` pairs: the rungs
sit inside the function, nothing branches to them, and no static analysis has a
reason to name them - so dispatch has nothing to land on when the computed jump
arrives. Today that knowledge lives in a patch. The hints file makes it data,
and it lives in **the game's own repository**, beside the recompiled sources,
not in the toolkit.

The file is TOML, read by `allegrexrecomp emit --hints <path>`. Addresses stay
in the game repository; the toolkit only learned to read them.

## The format, as it stands

One kind, and only this one:

```toml
# Killzone: Liberation - the argument ladders in the thunk at 0x0031D8A4.
# Reached by `jr $t4`, never branched to, so discovery cannot see them.
[[label]]
at = [
  0x0031D93C,   # int ladder, 7 rungs, step 8
  0x0031D944,
  0x0031D94C,
  0x0031D954,
  0x0031D95C,
  0x0031D964,
  0x0031D96C,
]
```

An address listed under `[[label]]` is emitted as a **dispatch entry of the
function that owns it**: a `case` in the body's entry switch, a
`psp_at_<addr>` thunk, and a `psp_register_label(addr, thunk)` line in
`_register.c`. That is exactly what every other interior label gets, so a
hinted address is reachable by `psp_dispatch` and by a recompiled jump alike.

Comments, blank lines, `0x` or decimal numbers, `_` digit separators and an
`at` array spread over several lines are all accepted. Listing the same address
twice is not an error.

More kinds - force-code/force-data, function names, HLE overrides - go in
tables of their own, beside `label`, so a later toolkit reads the same file.
They are not implemented yet, and this build **rejects** a table it does not
know by name rather than ignoring it: a hint that is silently dropped is worse
than a hint that refuses to load, because the build then looks like it applied
something it did not.

## When an address cannot be honoured

Two cases, both reported on stderr, neither fatal:

- The address is **not inside any discovered function**. Either it is a typo, or
  the code it names is only reached from an entry point discovery has not found
  yet. A game repository may legitimately carry hints ahead of that, so the
  build continues - but nothing is emitted for it.
- The address is a **delay slot** of its function. It is emitted inline with its
  branch, and both the entry switch and the thunk loop skip delay slots on
  purpose, so it cannot also be an entry.

## What it does not do

It does not analyse anything, and it does not guess. A hints file that names
every address of a module is a description of the module, which is fine - but
the intended use is the handful of places where a title disagreed with the
analysis and a person settled it by reading the disassembly.
