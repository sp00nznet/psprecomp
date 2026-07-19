# Decryption — the phase 2 gate

Every executable module on a retail PSP disc is encrypted. This is the one
thing standing between the phase-1 tooling and real Allegrex code, and it is
the direct analogue of the Lynx's encrypted boot block: a well-documented,
publicly reverse-engineered scheme that is a task rather than a research
project.

## What we are looking at

`BOOT.BIN` — nominally the plain ELF — is **zeroed** on retail discs. Confirmed
on *WTF: Work Time Fun* (ULUS10172): 1,224,764 bytes of `0x00`. The real module
is `EBOOT.BIN`, a `~PSP`-wrapped encrypted PRX.

This is not selective. On the WTF disc, **all 28 executable modules** are
encrypted:

- `SYSDIR/EBOOT.BIN` — `~PSP`, module `hell2k` (the game's dev codename)
- all 20 `USRDIR/module/*.prx` — `~SCE` wrapping `~PSP`
- all 8 `USRDIR/kmodule/*.prx` — `~PSP`
- the `DATA.PSP` inside each of the five game-sharing PBPs

## The header tells us what we need

The `~PSP` header is **plaintext** even though the body is not, which is why
phase 1 can report exactly what phase 2 has to do. From the five WTF
game-sharing modules:

| Module | Title | Decrypted size | Decrypt mode | Key tag |
|---|---|---:|---:|---|
| `b00_bootbin.dat` | Baseball Superstar | 4,212,342 | 10 | `0xF8710C50` |
| `b02_bootbin.dat` | Lumberjack | 4,104,742 | 10 | `0x4597CB4E` |
| `b04_bootbin.dat` | Pendemonium | 4,393,310 | 10 | `0x1F628E58` |
| `b80_bootbin.dat` | Lumberjack Challenge | 4,092,606 | 10 | `0xB4050E6E` |
| `b81_bootbin.dat` | Séance | 4,725,526 | 10 | `0x9B09CE7E` |

Five modules, five *different* tags. That is useful: it means a correct
implementation has to consult the key table properly, and a wrong one that
hard-codes a single key will fail visibly on the second module rather than
appearing to work.

## KIRK

Decryption runs through the PSP's KIRK crypto engine. For game modules the
relevant operation is **CMD1** (decrypt-and-verify): an AES-128-CBC body
decryption whose key is itself derived by AES from a per-tag key, with a
CMAC/ECDSA header signature that we can *check* but do not need to *forge*.

The pieces:

1. **AES-128.** A standard, self-contained implementation — no dependency, ~200
   lines, and testable against the FIPS-197 vectors before it ever sees a game
   module.
2. **The tag → key table.** Each `~PSP` header's tag at offset `0x130` selects a
   key set. These constants have been public in the homebrew community since
   the PSP's active life; they are published facts, the same way the Lynx boot
   ROM's RSA modulus is a published public key. **They are not distributed in
   this repository** — the `keys/` directory is `.gitignore`d and the user
   supplies their own, exactly as with the Lynx boot ROM.
3. **Segment reassembly.** The decrypted output is reassembled into a valid
   ELF32 using the segment table already parsed from the header.

## How we will know it worked

Decryption is the kind of thing that can appear to succeed and produce
plausible garbage, so the check is explicit and layered — each step would catch
a failure the previous one might not:

1. **Declared size.** The reassembled ELF must be exactly `elf_size` bytes, the
   value the plaintext header declares. A wrong key gives a wrong length.
2. **It parses.** The phase-1 ELF parser must accept it, with a sane
   `e_type` (2 or `0xFFA0`) and at least one `PT_LOAD`.
3. **The entry point lands in code.** `e_entry` must fall inside a `PF_X`
   segment.
4. **The opcode histogram is code-shaped.** `allegrexrecomp cover` over `.text`
   must show the distribution real MIPS text has — `lw`, `sw`, `addiu`, `nop`,
   `jr` dominant, and a high decode rate. Random bytes decode as MIPS at a
   surprisingly high rate, but they produce a *flat* histogram; real compiled
   code does not. This is the check that catches a subtly-wrong key.
5. **Cross-check.** The same module decrypted by an independent implementation
   must be byte-identical. `lynxrecomp` validated its RSA boot decrypt the same
   way, against an independent Python implementation.

## What this does not cover

`DATA.PSAR` — the packed payload in PSN and minis releases — uses a separate
(and for signed PSN content, differently-keyed) scheme. WTF's UMD and PSN
releases both put the game in `EBOOT.BIN`/`DATA.PSP`, so PSAR handling is not
on the critical path and is deferred until a title needs it.
