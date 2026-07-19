# Tests

Self-contained — **no game data**. There is no disc image, no `EBOOT.BIN`, and
no PRX module in this repository or in these tests. Every input is either an
instruction encoding written by hand from the MIPS32r2 manual, or a buffer the
test builds itself.

```powershell
ctest --test-dir build -C Release --output-on-failure
```

| Suite | Covers |
|---|---|
| `test_decode` | the Allegrex decoder |
| `test_runtime` | the semantic helpers and the memory map |
| `test_container` | the ISO9660 / PBP / `~PSP` / ELF / SFO parsers |

## What each suite is actually protecting

**`test_decode`** does not try to cover 256 opcodes. It covers the ones that
carry real risk:

- The function prologue/epilogue pattern every MIPS compiler emits — if
  `addiu $sp` / `sw $ra` / `lw $ra` / `jr $ra` are wrong, nothing else matters.
- **Branch and jump target arithmetic**, including a backward branch (the form
  that closes a loop) and the `j`/`jal` case where the top 4 bits come from the
  *delay slot's* address, not the instruction's. That difference only shows up
  at a 256 MB boundary, which is exactly where it would go unnoticed.
- **Every Allegrex divergence from stock MIPS32** — `clz`/`clo` in SPECIAL,
  `max`/`min`, `madd`/`msub`, the BSHFL group, `rotr` as `srl` with `rs == 1`.
  A decoder written from the generic manual gets all of them wrong.
- The operand-order cases: BSHFL writes `rd` from `rt`, not `rs`.
- Immediate sign- vs zero-extension in **both** directions, since getting it
  backwards silently corrupts constants.
- VFPU vector width across all four widths, and that VFPU encodings never
  decode as integer instructions.

**`test_runtime`** covers the instructions whose C translation is *not*
obvious — the set where a wrong answer hides for months:

- Divide by zero and `INT_MIN / -1` (both UB in C, both defined on MIPS).
- The `lwl`/`lwr` and `swl`/`swr` unaligned pairs, tested as the compiler
  actually emits them and checked byte-by-byte, including that they leave
  neighbouring bytes alone and degrade to a plain `lw` when aligned.
- Shift-amount masking (C leaves shift-by-32 undefined; MIPS masks to 5 bits).
- That `$zero` stays zero when written.
- That the three cache mirrors alias one backing store, and that an access
  straddling the end of a region is rejected rather than reading past the
  allocation.

**`test_container`** is mostly about *rejection*. These parsers are pointed at
untrusted dumps, so every one is checked for refusing bad magic, truncated
input, absurd counts, and — the case a corrupt dump actually hits — a
program-header offset pointing past the end of the file.
