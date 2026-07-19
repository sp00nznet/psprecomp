# Roadmap

Phased plan. Phase 1 is done; phase 2 is the gate everything else waits behind.

## Phase 1 — the container stack and the decoder ✅

Getting from a disc dump to *something you can point a decoder at*, and a
decoder correct enough to trust when you do.

- [x] Two-repo split: `psprecomp` (toolkit) + `wtf-psp-recomp` (reference game,
      submodules the toolkit).
- [x] **ISO9660 walker** — full recursive directory tree, version-suffix
      stripping, sector-padding handling, extraction by substring match with
      path flattening (so `SYSDIR/EBOOT.BIN` and `SYSDIR/UPDATE/EBOOT.BIN` do
      not collide).
- [x] **PBP parser** — the 8-segment container, sizes derived from adjacent
      offsets, one-level recursion into `DATA.PSP`.
- [x] **`~PSP` and `~SCE` header parsers** — module name, segment table, entry
      point, decrypted-ELF size, decrypt mode, and the key tag.
- [x] **ELF32/PRX parser** — program headers, `PT_LOAD` extraction, `.text`
      located by `PF_X`, `e_type == 0xFFA0` recognised as a relocatable PRX.
- [x] **PARAM.SFO parser** — the disc/module identity (`DISC_ID`, `TITLE`,
      `PSP_SYSTEM_VER`).
- [x] **Complete Allegrex decoder** — the MIPS32r2 integer set, REGIMM,
      SPECIAL2/SPECIAL3, the single-precision COP1 FPU, and every Allegrex
      divergence from stock MIPS32 (`clz`/`clo` in SPECIAL 0x16/0x17,
      `max`/`min` at 0x2C/0x2D, `madd`/`msub` in SPECIAL, `bitrev`/`wsbw` in
      BSHFL, `rotr` as `srl` with `rs=1`, `mfic`/`mtic` in COP0).
- [x] **Analysis flags on every instruction** — branch vs jump vs call,
      indirect, `jr $ra` as a return specifically, likely-branch nullification,
      delay slots, block termination. This is what phase 3's discovery pass
      consumes.
- [x] **VFPU recognised as a distinct category** rather than silently invalid,
      with the load/store and vadd/vmul/vcmp families named.
- [x] **Runtime semantic helpers** (`recomp_rt.h`) — division edge cases, the
      unaligned load/store pairs in little-endian form, `ext`/`ins`, `clz`,
      `bitrev`, `wsbh`/`wsbw`, shift-amount masking.
- [x] **Memory map** — scratchpad/VRAM/RAM with cache mirrors folded, straddling
      accesses rejected, unmapped accesses counted.
- [x] **`info` / `ls` / `extract` / `dis` / `cover` CLI.**
- [x] **`ctest` suites** — decoder, runtime, containers. All synthetic.
- [x] **Validated against a retail disc** — *WTF: Work Time Fun* (ULUS10172):
      90 entries, correct SFO, the full boot chain, and all five game-sharing
      modules with their per-module key tags.
- [x] Build system (CMake, zero external dependencies), docs, MIT license.

## Phase 2 — decryption (the gate)

Every executable module on a retail disc is a `~PSP`-wrapped, KIRK-encrypted
PRX. `BOOT.BIN` is zeroed. Until this is done, the decoder has nothing real to
chew on. See [`docs/DECRYPT.md`](docs/DECRYPT.md).

- [ ] **KIRK AES-128 core** — CMD1 (decrypt-and-verify) is the one that matters
      for game modules; CMD7 for some system PRXs.
- [ ] **The published per-tag key table** — each module carries a tag at header
      offset `0x130` selecting a key set. WTF's five game-sharing modules use
      five different tags, so the table is exercised immediately rather than
      hard-coded to one case.
- [ ] **`~PSP` → ELF** — reassemble the decrypted segments into a valid ELF32
      using the header's segment table, and verify against the header's
      declared `elf_size`.
- [ ] **Cross-check**: the decrypted ELF must parse cleanly with the phase-1
      ELF parser, its entry point must land inside a `PF_X` segment, and
      `cover` over its `.text` must show a code-shaped opcode histogram
      (`lw`/`sw`/`addiu`/`nop` dominant) rather than a flat one.
- [ ] `allegrexrecomp decrypt` subcommand, and `info`/`dis`/`cover` transparently
      decrypting so the rest of the tool stops caring about the wrapper.

## Phase 3 — function discovery + the C emitter

- [ ] **Recursive-descent discovery** — seed from the module entry point and the
      PRX export table, follow calls and branches, carve function boundaries at
      `jr $ra`, record indirect targets as external.
- [ ] **Jump-table resolution** — the `lui`/`addiu`/`sll`/`addu`/`lw`/`jr`
      idiom that MIPS compilers emit for `switch`. Unresolved tables become
      dispatch-table lookups rather than analysis failures.
- [ ] **The delay-slot problem.** Every MIPS branch executes the following
      instruction before the branch takes effect. The emitter must reorder or
      duplicate it correctly, and likely-branches nullify theirs when not taken.
      This is the single most error-prone part of MIPS recompilation and gets
      its own test suite.
- [ ] **The C emitter** — one `psp_func_<addr>` per routine, every line carrying
      its address and disassembly as a comment, lowered to `recomp_rt.h`
      helpers, intra-function flow as labels + `goto`, `jal` as a direct call,
      indirect jumps through a dispatch table.
- [ ] **Hints file** — per-title force-code/force-data, function names, and
      HLE overrides, so a title's hard-won analysis is data rather than a patch.

## Phase 4 — the HLE library

Games do not touch hardware directly; they call the firmware. That surface is a
library, which means it is implemented, not emulated.

- [ ] **`sceKernel`** — threads, semaphores, event flags, mutexes, callbacks,
      the memory partitions, timers. Modelled on the MIT-licensed `uofw`
      reference implementation.
- [ ] **`sceIo`** — the file API over a host directory standing in for the UMD.
- [ ] **`sceCtrl`** — the pad, mapped to host input.
- [ ] **`sceDisplay` + `sceGe`** — the display list processor. The GE is a
      fixed-function GPU with a documented command stream; it is emulated as a
      peripheral, the same split as the Lynx's Suzy/Mikey.
- [ ] **`sceAudio` / `sceSas`** — PCM out and the hardware voice mixer.
- [ ] **Module import resolution** — a PRX's import stubs bound to HLE
      implementations at load time, with an unimplemented-call trap that names
      the missing function instead of crashing.

## Phase 5 — running the recompiled C

- [ ] **Dispatch table** — `addr -> psp_fn_t`, for indirect calls and jump
      tables. Static `jal` stays a direct C call.
- [ ] **The interpreter oracle** — an Allegrex interpreter sharing this repo's
      decoder and runtime, so the recompiled path can be diffed against it
      instruction-for-instruction. When they disagree, the bug is in exactly
      one of them. This is the bring-up tool that everything else leans on.
- [ ] **PPSSPP cross-check** — trace comparison against the external oracle at
      the syscall and frame level. See [`docs/ORACLE.md`](docs/ORACLE.md).
- [ ] **First pixels** — a recompiled module reaching a rendered frame.

## Phase 6 — corpus and player-facing layer

- [ ] `scripts/sweep` over a large PSP corpus as a correctness harness:
      decrypt-all, decode-all, report coverage. Every module that fails is a
      concrete decoder or container bug, and the aggregate VFPU percentage
      tells us which titles are cheap targets.
- [ ] VFPU completion, driven by what the corpus says is actually used.
- [ ] Save states, an SDL2 + Dear ImGui frontend, controller remapping —
      built on the runtime, kept behind an opt-in CMake option so the core
      stays dependency-free.
