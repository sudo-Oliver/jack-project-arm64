# ARM64 Jak 1 Boot Baseline

This branch is the Apple Silicon ARM64 experimental port baseline.

Current target state:

- Jak 1 boots natively on Apple Silicon without Rosetta.
- Sony Presents screen appears.
- Naughty Dog intro and title/start-screen audio continue after Sony.
- The process must not crash immediately after Sony.
- Post-Sony video may still be black or broken. That renderer bug is separate.

Known bad state:

- Commit `7b226be70` regressed post-Sony boot on fresh clones.
- It added late graphics, trampoline, MIPS2C, and renderer experiments.
- Fresh clones of that commit may crash directly after Sony.

Baseline restore:

- The tested stable behavior matches the tree state before `7b226be70`,
  around `d4bf68543`.
- The restore commit on `experimental` reverts those late experiments so all
  Macs cloning or pulling `experimental` start from the same boot baseline.

## Required Host

Use native ARM64 tools:

```sh
rtk uname -m
rtk file build/game/gk
```

Expected:

```text
arm64
build/game/gk: Mach-O 64-bit executable arm64
```

If this says `x86_64`, the build/runtime is wrong for this branch.

## ISO / Data Setup

For Jak 1 PAL / Europe:

```sh
rtk task set-game-jak1
rtk task set-decomp-pal
```

The game data must be extracted into:

```text
iso_data/jak1_pal/
```

A `.iso` file alone is not enough for `-fakeiso` boot. The extracted disc
contents must exist in `iso_data/jak1_pal`.

Then extract/build project data:

```sh
rtk task extract
```

## C++ Build

Use Ninja on Apple Silicon:

```sh
rtk cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
rtk cmake --build build --target goalc gk -j 8
```

## GOAL Build

Start the compiler:

```sh
rtk ./build/goalc/goalc --game jak1
```

In the GOAL REPL, build everything:

```lisp
(mi)
```

**Use `(mi)`, not `(build-game)`.** `(build-game)` is `(make-group "all-code")`, and `all-code`
contains only `*all-gc*` -- it compiles every `.gc` to a `.o` in `out/jak1/obj/` but never packs
the CGOs. `-fakeiso` loads `out/jak1/iso/GAME.CGO`, so changes made with `(build-game)` alone
never reach the running game and you end up testing a stale build. `(mi)` is `(make-group "iso")`,
which includes `*all-cgos*`. `(build-kernel)` does pack KERNEL.CGO, which is why kernel changes
appear to work while engine changes silently do not.

From the shell, the same thing:

```sh
./build/goalc/goalc -g jak1 -c "(mi)"
```

Any change to the compiler or emitter also requires this rebuild -- the old codegen stays baked
into the CGOs otherwise.

## Boot

Use `-fakeiso` for this baseline:

```sh
rtk ./build/game/gk -v --game jak1 -- -boot -fakeiso -debug
```

Do not use `--no-display` for the visual/audio baseline check.

## Expected Log Markers

Good baseline markers:

```text
Got correct kernel version 2.0
begin load title-vis [tit.DGO]
GAMEPLAY: enter title
Load music village1
Kernel dispatch #100
Kernel dispatch #1000
```

The compiled version should be the current restore commit on `experimental`,
or a local baseline checkout such as `d4bf68543`. It must not be the known bad
`7b226be70` unless the restore patch is applied locally.

## SIGBUS Note

`[SIGBUS] count=...` is not automatically a crash in this port. The current
runtime can recover from many SIGBUS faults and continue dispatching frames.

Real regression:

- process exits or aborts directly after Sony Presents
- no Naughty Dog / title audio continues
- no `Kernel dispatch #100` or later markers appear

## Fixed: masked vector ops were a no-op on ARM64

`IGen::ARM64::blend_vf` had no real implementation for a partial write mask. Any mask other
than "none" or "all" silently returned `mov dst, src1`, discarding the computed result. Every
masked VU operation in the engine (`.add.w.vf.xyz`, `.sub.w.vf.xyz`, and so on) therefore did
nothing, leaving the destination register holding stale data.

One visible consequence: `collide-cache-using-y-probe-test` computed its bounding box from
uninitialised registers, so the collision traversal accepted far too many fragments. It pushed
534 entries into `*collide-list*`, which holds 256. The overflow ran past the end of the buffer
and overwrote the `type` objects sitting above it on the global heap — specifically
`drawable-tree-tfrag`, `-trans-tfrag`, `-dirt-tfrag`, `-lowres-tfrag`, `-instance-tie` and
`-instance-shrub`. Their method tables were destroyed, so the next frame dispatched method 12
through a smashed table, jumped to `#f` instead of code, and died with SIGBUS.

ARM64 has no single BLENDPS equivalent, so the expansion lives in
`IR_BlendVF::do_codegen_arm64`: one `mov` to seed the destination, then a per-lane `INS`
for each lane the mask selects. Seeding from whichever source the destination already aliases
avoids clobbering the other operand. `CodeTester.execute_blend_vf_arm64` executes the sequence
for all 16 masks against all three aliasings and checks every lane.

Anything compiled before this fix is stale — rebuild the GOAL code (`(build-kernel)` then
`(build-game)`, or `(mi)`) or the old broken codegen stays in the CGOs.

## Fixed: `RET` does not read the stack on ARM64

`deactivate` (`goal_src/jak1/kernel/gkernel.gc`) and the `abandon-thread` macro both used the
x86 idiom "push a target address, then `RET` jumps to it". On ARM64 `RET` is `BR X30`: it reads
the link register and ignores the stack entirely. So the push stayed on the stack and `RET`
returned to the caller, which called straight back in. Each round leaked the 16 bytes of the
push, and after roughly 70k rounds the process stack had walked ~1.1 MB down from `EE+0x197f60`
into unmapped memory at `EE+0x80000`.

Both sites now set `LR` under `#if ARM64_PORT` and keep the push only for x86, the same shape
`throw-dispatch` already used. Note `(.mov lr temp)` must go through the coloring system --
`:color #f` requires both operands to be rlet-constrained, and `temp` is a normal variable.

**This is a general rule for this port**, not a one-off: any GOAL asm that relies on `CALL`
pushing a return address or `RET` popping one needs an explicit ARM64 path. Search for `.push`
followed by `.ret` before assuming a kernel asm function is correct.

## Debugging notes for the EE thread

Two things make crashes here hard to see, both now handled in `game/runtime.cpp`:

- **A stack overflow cannot report itself.** Delivering a signal needs room for a signal frame,
  and an overflowed stack has none, so the process died with no handler output at all. `ee_runner`
  now installs a `sigaltstack` and the SIGBUS/SIGSEGV/SIGILL handlers use `SA_ONSTACK`, so the
  rich `[EE-CRASH]` dump works even for stack exhaustion.
- **lldb is unusable against this runtime.** Attaching breaks the `MAP_JIT` write-protect
  toggling and faults during `gkernel` linking, long before any real bug. Use the in-process
  dump, and fall back to the macOS crash report in `~/Library/Logs/DiagnosticReports/gk-*.ips`
  (it carries `pc`, `lr` and `sp` but no usable frames, since JIT code has no unwind info).

To turn a faulting address into a GOAL object, match it against the `[EE-LINK] link finish:`
lines in the boot log: each prints `seg0/seg1/seg2` base+size. Only ranges inside
`0x4000000..0x5000000` (kcodeheap) are real; v2 level objects print garbage segment data and
will produce false matches.

## Fixed: broadcast VU ops splatted the wrong lane

`IGen::ARM64::splat_vf` built the `DUP Vd.4S, Vn.S[index]` immediate as `(index << 2) | 0b100`.
In `imm5` the lowest set bit selects the element size and the bits above it hold the index, so
32-bit lanes need `(index << 3) | 0b00100`. The old formula produced:

| element | intended lane | actual |
|---|---|---|
| X | 0 | 0 (correct by luck) |
| Y | 1 | 0 |
| Z | 2 | 1 |
| W | 3 | 1 |

Only X was ever right. Every broadcast VU op (`.add.w.vf.xyz`, `.sub.x.vf`, `.mul.w.vf`, ...)
silently used the wrong component. It produced plausible floats, so nothing crashed outright --
it just computed wrong geometry everywhere.

Together with the blend fix this is what finally made `collide-cache-using-y-probe-test` correct:
`*collide-list*` peaked at 1734 entries (capacity 256) before, and 41 after.

`CodeTester.splat_vf_arm64` checks the encodings against the system assembler and
`CodeTester.execute_splat_vf_arm64` executes the splat and verifies every output lane.

## Fixed: allocator's callee-saved GPR was not preserved by the kernel

On this port "callee-saved" has to mean "survives a GOAL kernel context switch", which is
stricter than AAPCS64. `thread-suspend`/`thread-resume` (and `catch-frame`/`throw-dispatch`) save
exactly `s0-s4`, declared in `gkernel.gc` as `rbx/rbp/r10/r11/r12`.
`translate_x86_reg_to_arm64` maps those to `x19/X23/X6/X7/X26`, so only those registers actually
survive a `(suspend)`.

`RegisterInfo` handed the allocator **X24**, which is in none of those. Any local the allocator
put there was replaced by whatever the context switch left behind. In `display-loop` that local
was `disp`: it went in as `*display*` (`0x4c8f04`) and came out of the first `(suspend)` as `pp`
(`0x1b3f64`), so `(-> disp on-screen)` read a garbage frame index and the renderer dereferenced a
null `display-frame`.

`m_saved_gprs` now uses **X26** (= `r12` = `s4`). Since register IDs are shared between GPRs and
SIMD in the OpenGOAL namespace, X26 aliases q10, so q10 left the SIMD allocation lists and q8
(freed up by dropping X24) took its place.

**When touching the ARM64 register model, check `translate_x86_reg_to_arm64` first.** A register
that is callee-saved per the ABI is not automatically safe here.

## Fixed: two more `push`-then-`RET` sites

`enter-state` in `goal_src/jak1/kernel/gstate.gc` had an inlined copy of the `abandon-thread`
body with the same x86 assumption described above. It leaked 16 bytes per call and returned to
its caller, which re-entered `enter-state`, walking the stack ~1.08 MB down from `EE+0x18a3e0`
until it hit the low-memory guard.

Note that guard: `runtime.cpp` mprotects `EE[0, EE_MAIN_MEM_LOW_PROTECT)` -- the first 512 KB
(`0x80000`) -- as `PROT_READ` to catch null-pointer writes. A GOAL stack that runs down into it
faults at `EE+0x7fff0` with `sp` sitting exactly at `0x80000`. That exact pair of numbers means
"stack ran off the bottom", not "bad pointer".

A scan for the idiom across `goal_src/jak1` now comes back clean; `goal_src/jak2` and
`goal_src/jak3` still contain it and will need the same treatment if those ports are revived.

## Tooling: turning a crash address into a GOAL function

JIT'd GOAL code has no symbols and no unwind info, and the x86 disassembler in `goalc` produces
garbage for ARM64. Two additions close that gap:

- `(dump-function-map "<file>")` in the goalc REPL writes `object seg offset length name` for
  every compiled function.
- `(dump-function-ir "<function>" "<file>")` writes one function's instruction offsets with the
  IR and the GOAL source line that produced each one. It never decodes machine code, so it is
  ARM64-safe.

`scripts/arm64/resolve_goal_addr.py` ties them together with the `[EE-LINK] link finish:` lines
from a boot log:

```sh
./build/goalc/goalc -g jak1 -c '(begin (mi) (dump-function-map "/tmp/funcmap.txt"))'
./build/game/gk -v --game jak1 -- -boot -fakeiso -debug > /tmp/gk.log 2>&1
scripts/arm64/resolve_goal_addr.py /tmp/funcmap.txt /tmp/gk.log 0x19880
# -> 0x19880: gstate seg0 +0xfc0 -> enter-state +0xcdc
```

Then `(dump-function-ir "enter-state" ...)` and look up `+0xcdc` to get the source line.
Regenerate the function map after any compiler or GOAL change -- offsets move.

## Known remaining bug: MIPS2C read outside EE memory

Boot now reaches `machine started`, `Displaying level village1` and `GAMEPLAY: enter village1`,
and the display loop renders several frames before failing.

The current crash is in C++ rather than GOAL: `Mips2C::ExecutionContext::lw` reads
`EE+0x8b160250`, which is past the end of the 128 MB EE memory. That is the hand-translated VU
code under `game/mips2c/`, i.e. the next layer of the renderer -- the same area the
`2ecf78f88` revert touched.

## Test coverage gap on ARM64

`test/goalc/CMakeLists.txt` compiles only `test_CodeTester.cpp` on ARM64. Every execution-level
GOAL test is commented out, including `test_vector_float.cpp`, which exercises masked vector ops
(`:mask`) directly -- that is why a no-op blend survived so long. Those fixtures need a booting
game to host the test runner, so re-enabling them is gated on the boot work above.

Until then, prefer executing `CodeTester` tests over encoding-only ones for anything with real
semantics: `CodeTester.execute_blend_vf_arm64` runs the emitted sequence and checks results,
which an expected-hex-string test would not have caught.
