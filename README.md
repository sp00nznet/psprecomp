# psprecomp

**A static-recompilation toolkit for PlayStation Portable games — turning
Allegrex MIPS into native C, not emulating it.**

The PSP has excellent emulation (PPSSPP is one of the best emulators ever
written) but — as far as we can find — **no static recompiler**. That gap is
worth closing, because the PSP is a genuinely good recompilation target hiding
behind a reputation for being a hard one:

- **One documented CPU.** The Allegrex is a MIPS32r2 core. No second processor
  to chase, no cell-style SPU swarm, no exotic addressing. MIPS is the
  best-understood RISC there is, and it is what the original static-recomp
  work was built on.
- **No TLB.** The memory map is fixed and tiny — scratchpad, VRAM, main RAM,
  each mirrored at three cache-behaviour bases. Address translation is a mask
  and an index, not a page walk.
- **A documented OS boundary.** Games call `sceKernel*` / `sceGu*` / `sceCtrl*`.
  That is a *library* surface, not a hardware surface, which means it can be
  implemented as HLE C rather than emulated — and a complete MIT-licensed
  reverse engineering of that firmware already exists.

`psprecomp` is the reusable toolkit. The games brought up on it live in
separate repos that consume this one as a submodule — that split is deliberate:
the toolkit is the thing other people fork to recompile *their* PSP game.

- [`wtf-psp-recomp`](https://github.com/sp00nznet/wtf-psp-recomp) —
  *WTF: Work Time Fun* (the flagship; an anthology of ~30 microgames, five of
  which ship as separately-bootable modules).

> **No game data here.** Disc images, `EBOOT.BIN`, PRX modules and PSP
> decryption keys are all `.gitignore`d. This repo is the recompiler, the
> runtime, and docs — bring your own UMD or PSN dump.

## How it works

```
  game.iso ──► allegrexrecomp ──► EBOOT.BIN / DATA.PSP     peel the container
              (ISO9660 · PBP)      (a ~PSP-wrapped PRX)     stack down to a module
                  │
                  ▼
          ┌────────────────────┐   KIRK AES + the published per-tag key set;
          │  decrypt  (phase 2)│   ~PSP  ──►  a plain ELF32 MIPS PRX
          └────────────────────┘
                  │
                  ▼
          ┌────────────────────┐   decode Allegrex, discover functions
          │  allegrexrecomp    │   (following jump tables + the module's
          │  (tools/)          │   export table), translate each to readable C:
          └────────────────────┘   psp_func_<addr>
                  │  generated/recomp_funcs.c
                  ▼
          ┌────────────────────┐   the runtime the generated C links against:
          │  psprecomp (lib)   │   CPU state · memory map · semantic helpers ·
          │  (src/, include/)  │   sceKernel/sceGu HLE · dispatch
          └────────────────────┘
                  │
                  ▼   (+ a per-game host: load module, register, run, present)
          native executable — the recompiled game runs
```

## Status — containers and KIRK CMD1 are up; the PRX tag layer is the gate

Phase 1 is done and it is real, not a skeleton. Pointed at a retail PSN dump of
*WTF: Work Time Fun*, the tool walks the disc, identifies every layer, and
reports exactly what stands between here and decodable code:

```
$ allegrexrecomp info "WTF - Work Time Fun (USA) (PSP) (PSN).iso"
format:   ISO9660 disc image
entries:  90

PARAM.SFO:
  DISC_ID            ULUS10172
  TITLE              WTF: work time fun™
  PSP_SYSTEM_VER     2.71

boot chain:
     1225104  /PSP_GAME/SYSDIR/EBOOT.BIN
     4213168  /PSP_GAME/USRDIR/gamesharing_en/b00_bootbin.dat
     ...
```

**What works today**

- ✅ **The container stack** (`container.c`) — ISO9660 walker, PBP parser,
  `~PSP` / `~SCE` header parsers, and an ELF32/PRX parser, each rejecting
  malformed input rather than walking off the end of it. Verified against a
  retail disc: 90 entries, correct `PARAM.SFO`, the full boot chain, and the
  five game-sharing modules with their per-module key tags.
  ([`docs/CONTAINERS.md`](docs/CONTAINERS.md))
- ✅ **A complete Allegrex decoder** (`decode.c`) — the full MIPS32r2 integer
  set, the single-precision COP1 FPU, and the Allegrex-specific opcodes that
  sit in *different slots than stock MIPS32* (`clz`/`clo` in SPECIAL, `max`/`min`,
  `madd`/`msub`, `bitrev`/`wsbw` in BSHFL, `rotr` as `srl` with `rs=1`). Every
  one of those divergences is pinned by a unit test, because a decoder written
  against the generic MIPS manual gets all of them wrong.
- ✅ **The VFPU, identified but not yet named** — VFPU encodings decode to a
  distinct `A_VFPU_UNKNOWN` rather than falling through as invalid, so the
  emitter can refuse them loudly and `cover` can report how much of a given
  game actually needs them. The load/store and the vadd/vmul families *are*
  named. ([`docs/VFPU.md`](docs/VFPU.md))
- ✅ **The runtime semantic helpers** (`recomp_rt.h`) — the instructions whose
  C translation is *not* obvious, in exactly one place: divide-by-zero and
  `INT_MIN / -1`, the `lwl`/`lwr`/`swl`/`swr` unaligned pairs in their
  little-endian form, `ext`/`ins`, `bitrev`, `clz`, and shift-amount masking
  (C leaves shift-by-32 undefined; MIPS masks to 5 bits).
- ✅ **The memory map** (`mem.c`) — scratchpad / VRAM / main RAM with the three
  cache mirrors folded onto one backing store, straddling accesses rejected,
  and unmapped accesses counted rather than silently swallowed.
- ✅ **AES-128/192/256 and AES-CMAC** (`crypto/`) — self-contained, no
  dependency, validated against **FIPS-197**, **NIST SP 800-38A** and
  **RFC 4493** before ever touching a game module. The AES S-box is *derived*
  from its algebraic definition rather than transcribed as a table, so it is
  either completely right or obviously wrong.
- ✅ **KIRK CMD1, complete** (`crypto/kirk.c`) — key unwrapping, both CMAC
  verifications, body decryption, with padding and unaligned lengths handled
  and every size field bounds-checked. Verified end to end against synthetic
  blobs: a wrong key is caught at the **header** CMAC, a tampered body at the
  **data** CMAC. That separation is the point — it makes "did the key work?"
  a yes/no answer instead of a judgement call about whether the output looks
  like code. **No PSP key material is needed to prove any of this**, because
  CMD1's structure does not depend on which key you feed it.
- ✅ **External key loading** — `--keys`, `$PSPRECOMP_KEYS`, or
  `./keys/psp_keys.txt`. Nothing is bundled; a missing key is a named
  diagnostic, not a mystery.
- ✅ **`ctest`, all synthetic** — decoder, runtime, container, crypto and KIRK
  suites. No game data, no ROM, no disc image, and no key material in the repo
  or in the tests.

**The gate: every module on a retail disc is encrypted**

`BOOT.BIN` is zeroed (as it is on essentially every retail UMD), so the real
code is in `EBOOT.BIN` — a `~PSP`-wrapped PRX. So is every `.prx` in
`USRDIR/module/`, and so is the `DATA.PSP` inside each game-sharing PBP. All 28
modules on the WTF disc, without exception:

```
$ allegrexrecomp info b00_bootbin.dat
format:   PBP container (version 0x10000)
  PARAM.SFO   offset 0x00000028  size        440
  DATA.PSP    offset 0x000001E0  size    4212688  [~PSP encrypted PRX]

  TITLE              Baseball Superstar

DATA.PSP:
format:   ~PSP encrypted PRX
  module        boot_bin
  elf size      4212342 bytes (decrypted)
  decrypt mode  10
  tag           0xF8710C50
```

KIRK CMD1 — the layer that actually decrypts — is done. What remains is the
per-tag transform *above* it that constructs the CMD1 header. And we established
that by measurement, not assumption: `decrypt` scans for an embedded CMD1
metadata block using a ~100-bit structural signature and finds none anywhere in
these modules, which means the CMD1 header is **constructed** by the tag layer
rather than sitting at a fixed offset. Guessing that offset would have produced
silently-wrong output.

```
$ allegrexrecomp decrypt b02_bootbin.dat
module:   boot_bin
tag:      0x4597CB4E   decrypt mode 10
sizes:    4105088 encrypted -> 4104742 decrypted

probing for a KIRK CMD1 metadata block...
  none found in the first 0x400 bytes
```

The same run turned up that WTF's main executable uses **decrypt mode 9** while
all five game-sharing modules use **mode 10** — so the disc exercises at least
two variants of the transform. See [`docs/DECRYPT.md`](docs/DECRYPT.md) and
[`ROADMAP.md`](ROADMAP.md) phase 2b.

**Worth being clear about:** phase 2b is not what gates recompilation. Phase 3
needs *plaintext*, not necessarily *our* decryptor —
[`pspdecrypt`](https://github.com/John-K/pspdecrypt) (GPL-3.0), run as a
separate process exactly like PPSSPP-as-oracle, produces it today. Finishing
our own decryptor is about being self-contained, which is a goal but not a
blocker.

**Not started yet:** function discovery, the C emitter, the HLE library, the
interpreter oracle.

```
# each Allegrex routine will become a readable C function; today the same
# decoder already produces the annotated disassembly that becomes the comments:
08804000  27BDFFE0  addiu      $sp, $sp, -32
08804004  AFBF001C  sw         $ra, 28($sp)
08804008  AFB00018  sw         $s0, 24($sp)
0880400C  00808021  addu       $s0, $a0, $zero
08804010  0E240040  jal        0x08900100
08804014  00000000  nop
08804018  8FBF001C  lw         $ra, 28($sp)
0880401C  8FB00018  lw         $s0, 24($sp)
08804020  03E00008  jr         $ra
08804024  27BD0020  addiu      $sp, $sp, 32
```

## Building

Requires CMake and a C compiler (MSVC on Windows; gcc/clang elsewhere). The
core has **no external dependencies** — a fresh clone builds with nothing
installed.

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
# -> build/tools/allegrexrecomp/Release/allegrexrecomp.exe
# -> the psprecomp runtime static library
```

```
allegrexrecomp info    <file>                    identify any layer of the stack
allegrexrecomp ls      <file.iso>                list a disc image
allegrexrecomp extract <file.iso> <match> <dir>  pull files out of a disc image
allegrexrecomp dis     <file> [addr] [count]     disassemble Allegrex code
allegrexrecomp cover   <file>                    decode-coverage report
```

`info` accepts anything in the stack — a disc image, a PBP, a `~PSP` or `~SCE`
module, a plain ELF/PRX — and recurses one level, so pointing it at a
game-sharing PBP reports the inner module's name, size and key tag in one go.

## On licensing, and why there is no emulator vendored here

There is no MIT-licensed PSP emulator. PPSSPP is GPLv2+, JPCSP is GPLv3.
Neither can be linked into an MIT toolkit, and neither is needed:

| Project | License | How it is used |
|---|---|---|
| [uofw](https://github.com/uofw/uofw) | **MIT** | A complete reverse engineering of the PSP firmware in readable C. This is the reference for the `sceKernel` / `sceGe` / `sceCtrl` HLE layer. |
| [pspsdk](https://github.com/pspdev/pspsdk) | **BSD** | Headers and the published ABI for `sceGu` / `sceKernel` / `sceAudio`. (`tools/PrxEncrypter` is GPLv3 and is excluded.) |
| **PPSSPP** | GPLv2+ | **Oracle only** — run as a separate process and compared against. No code is copied, linked, or vendored. |
| [pspautotests](https://github.com/hrydgard/pspautotests) | — | A hardware-validated behavioural corpus to check the runtime against. |

That is the same arrangement `lynxrecomp` has with Handy and Mednafen, and
`ps3recomp` has with RPCS3: the emulator is a thing you *diff against*, never a
thing you link. See [`docs/ORACLE.md`](docs/ORACLE.md).

## Why PSP (and why this is new ground)

Static recompilation has been done for the N64, PSX, and a scattering of 8- and
16-bit consoles. The PSP has been skipped, and the usual reason given is the
VFPU — a 128-register vector unit with a genuinely strange encoding, prefix
modifiers that rewrite operands, and matrix-shaped register addressing.

That reputation is half-earned. The VFPU is hard, but it is also *localized*:
it shows up in maths libraries and geometry setup, not in the game logic that
makes up the bulk of a module. Recompiling the integer core — which is
ordinary, well-documented MIPS32r2 — gets you most of a game, and the `cover`
subcommand exists to measure exactly how much VFPU a given title actually needs
before committing to it. Measuring first is cheaper than assuming.

The other half of the reason is the encryption, and that is simply a task.

## Repository layout

```
include/psprecomp/    runtime API (cpu, mem, recomp_rt)
src/                  runtime: CPU state, the memory map
tools/allegrexrecomp/ the toolkit: Allegrex decoder, container stack
                      (ISO9660 · PBP · ~PSP · ~SCE · ELF/PRX), CLI
tests/                ctest: decoder, runtime helpers, container parsers —
                      all synthetic, no game data
docs/                 ARCHITECTURE · CONTAINERS · DECRYPT · RECOMPILER ·
                      VFPU · ORACLE
```

## Documentation

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — PSP hardware: Allegrex, the memory map, the GE.
- [`docs/CONTAINERS.md`](docs/CONTAINERS.md) — the four-layer container stack from disc to module.
- [`docs/DECRYPT.md`](docs/DECRYPT.md) — the `~PSP` encryption and the plan to get past it.
- [`docs/RECOMPILER.md`](docs/RECOMPILER.md) — how `allegrexrecomp` works and the readability goals.
- [`docs/VFPU.md`](docs/VFPU.md) — what the VFPU is, what we decode, and what we do not.
- [`docs/ORACLE.md`](docs/ORACLE.md) — using PPSSPP as a reference without touching its code.
- [`ROADMAP.md`](ROADMAP.md) — phased plan.

## Credits & references

All code here is original, but it stands on a great deal of prior
reverse-engineering and documentation. With thanks to:

- **[uofw](https://github.com/uofw/uofw)** — the unofficial official firmware, a
  complete MIT-licensed reverse engineering of the PSP's system modules. The
  reference for what `sceKernel` and friends actually do.
- **[pspsdk / pspdev](https://github.com/pspdev/pspsdk)** — the BSD-licensed
  homebrew SDK, and the reference for the `sceGu` graphics API and the module
  export/import ABI.
- **[PPSSPP](https://github.com/hrydgard/ppsspp)** and its
  [hardware documentation](https://www.ppsspp.org/docs/psp-hardware/cpu/allegrex-overview/)
  — the best public description of the Allegrex and the VFPU that exists.
  Used as a behavioural oracle and as documentation. **Reference only; no code
  copied.**
- **[pspautotests](https://github.com/hrydgard/pspautotests)** — hardware-verified
  tests, used as ground truth for runtime behaviour.
- The **PSP homebrew and reverse-engineering community**, whose published work
  on the PRX format, the module info structures, and the KIRK engine is what
  makes any of this tractable.
- **IDA Pro / Ghidra** with a MIPS processor module, as an independent
  disassembly oracle for cross-validating function discovery.

> Running a game requires a UMD or PSN dump that **you** own, and the PSP
> decryption key material, neither of which is distributed here. No game data,
> no firmware, and no keys ship in this repo.

## License

MIT — see [`LICENSE`](LICENSE). Independent, non-commercial preservation work;
not affiliated with or endorsed by Sony Interactive Entertainment.
