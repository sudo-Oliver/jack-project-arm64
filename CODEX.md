# ARM64 Jak 1 Boot Baseline

This branch is the Apple Silicon ARM64 experimental port baseline.

## Current state (2026-09-24)

**Jak 1 is playable natively on Apple Silicon, without Rosetta and without OpenGL problems.**

Verified in this state:

- Boots to the title screen and plays the intro (`begin load title-vis`, `GAMEPLAY: enter title`).
- In-game rendering looks correct. You can move Jak around and play.
- Audio is continuous and correctly aligned.
- Runs indefinitely -- observed past `Kernel dispatch #8700`; it exits 0 only when closed by hand.
- Zero `[EE-CRASH]`, zero `suspend called without enough stack`, zero nav-mesh geometry warnings.

Eight ARM64 bugs stood between the reverted baseline and this, each with its own section below.
Only two of them were crashes. The other six produced **silently wrong answers**, which is why
they survived so long and why the symptom was "flickering garbage" rather than a stack trace:

| Bug | Symptom |
|---|---|
| `blend_vf` no-op | every masked VU op discarded its result |
| `splat_vf` wrong lane | every broadcast VU op used the wrong component |
| `ins_vf_element` wrong lane (+ `umov`, `ins`-from-GPR) | every swizzle, so every cross product, so every surface normal |
| allocator saved GPR (X24) not preserved by the kernel | locals silently replaced after `(suspend)` |
| backup stack too small for ARM64 frames | `thread-suspend` copy ran off the buffer, ~85k times per run |
| mips2c `jalr` unimplemented on ARM64 | GOAL callbacks never ran, `v0` kept garbage |
| `push`-then-`RET` (3 sites) | stack leak + infinite re-entry |
| `(build-game)` does not pack CGOs | changes silently never reached the running game |

**The recurring theme, worth internalising before touching anything here:** on ARM64 a wrong
instruction encoding usually does not crash. It computes a plausible wrong number, and the engine
carries on. Three separate bugs above were the *same* `imm5` element-index mistake in three
different emitter functions. When something looks visually wrong, suspect the emitter and write an
**executing** test (`CodeTester.execute_*`), not an expected-hex-string test -- the hex tests
passed the whole time.

### Metal status

**The Jak 1 bucket table is complete on Metal: every renderer the OpenGL table assigns has a
Metal counterpart.** A village1 frame from the same camera on both backends matches.

What the backend owns: the window, `MTLDevice`, command queue and `CAMetalLayer`; MSL compiled at
runtime into one `MTLLibrary`; the bucket table; and the frame loop.

The frame is drawn into an offscreen colour texture and copied onto the drawable by a final
full-screen pass, the same shape the OpenGL backend already has with its render FBO. That is what
lets the two renderers that read the frame they are drawing into work at all --
`MetalRenderState::pause_and_snapshot()` ends the frame's pass keeping colour, depth and stencil,
hands back a copy of the colour, and `resume_scene()` starts the pass again. The depth cue needs
a pass of its own in between; the sprite distorter only samples, so it uses `snapshot_scene()`,
which is both at once.

The depth attachment is `Depth32Float_Stencil8`: the shadow renderer draws stencil shadow
volumes. Every pipeline has to declare the stencil format too, or its draws fail.

| Renderer | Buckets (jak1) | Metal |
|---|---|---|
| `TFragment` | 6 | done |
| `TextureUploadHandler` | 11 | done (the texture animator is jak 2 and later only) |
| `Generic2BucketRenderer` | 10 | done |
| `Merc2BucketRenderer` | 8 | done |
| `DirectRenderer` | 3 | done |
| `Tie3WithEnvmapJak1` | 2 | done, including the wind instances and the envmap second pass |
| `SkyBlendHandler` | 2 | done |
| `Shrub` | 2 | done |
| `SkyRenderer`, `Sprite3`, `ShadowRenderer`, `OceanNear`, `OceanMidAndFar`, `EyeRenderer`, `DepthCue` | 1 each | done |

Not ported, and jak 2 / jak 3 only: the glow renderer inside the sprite bucket, the texture
animator, and jak 2's proto visibility. The sprite distorter's non-instanced path is deliberately
not ported: it exists in the OpenGL backend for drivers without instancing, and there is no such
Metal driver.

Two differences from the OpenGL backend that are Metal's, not bugs:

- Metal bakes blend into the pipeline state, depth test and write into a depth-stencil state, and
  clamp and filter into a sampler, so each distinct `DrawMode` gets its objects built once and
  cached by the mode's integer value. `MetalDrawStateCache` is that translation, written once.
  `MetalGeneric2` keeps its own copy on purpose -- generic reads a `DrawMode` differently in two
  places, and sharing the cache would mean sharing those differences into renderers that do not
  want them.
- Metal does support primitive restart at `0xFFFFFFFF` for a 32-bit index buffer, which is the
  same sentinel the `.fr3` strips already use, so the background renderers draw strips directly.
  The sprite and generic renderers expand their strips into triangles anyway, because they build
  their index buffers per frame and the expansion is free there.

OpenGL remains the default and stays fully working. Pick a backend at runtime:

```sh
./build/game/gk -v --game jak1 -- -boot -fakeiso                        # OpenGL (default)
OPENGOAL_RENDERER=metal ./build/game/gk -v --game jak1 -- -boot -fakeiso # Metal
```

### Known artifacts that are not Metal's

Both backends show these identically, so they are not part of the port:

- Thin black slivers along the ocean horizon, seen when looking out to sea in village1. The far
  ocean's geometry, drawn through the direct renderer.
- Jak stands too high on the village1 rope bridges, as if floating. That is collision, not
  rendering; `ropebridge.gc` is full of inline VU (`.div.vf` with `fsf`/`ftf`, masked moves), so
  the ARM64 emitter is the first place to look -- see the history at the top of this file.

Historical note, kept because older logs refer to it:

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

```sh
# Debug boot: skips title/intro, drops you straight into village1. Full logging.
./build/game/gk -v --game jak1 -- -boot -fakeiso -debug

# Retail boot: title screen + intro, i.e. what a player sees.
./build/game/gk -v --game jak1 -- -boot -fakeiso
```

**The missing intro under `-debug` is intentional, not a bug.** `game-info.gc` picks the continue
point like this:

```lisp
((!= *kernel-boot-message* 'play) "demo-start")
(*debug-segment* "village1-hut")   ;; -debug lands here
(else "title-start")
```

`-debug` sets `*debug-segment*`, so you spawn at `village1-hut`. Drop `-debug` to get
`title-start`. Use the debug boot for iterating and the retail boot to confirm the full path.

Do not use `--no-display` for the visual/audio check.

## Expected Log Markers

Debug boot (`-debug`):

```text
Got correct kernel version 2.0
kernel: machine started
begin load village1-vis
Displaying level village1
GAMEPLAY: enter village1
Kernel dispatch #100
Kernel dispatch #1000
```

Retail boot (no `-debug`):

```text
begin load title-vis
GAMEPLAY: enter title
```

The dispatch counter keeps climbing for as long as the game runs -- observed past #8700. The
process should only exit because you closed it, with status 0.

Counts that must all be **zero** in a healthy run -- check these before believing a change is
good, because each one was a real bug that produced no crash:

```sh
grep -ac "EE-CRASH" log                              # faults
grep -ac "suspend called without enough stack" log   # backup stack too small
grep -ac "inverted normals" log                      # cross products / SIMD lanes wrong
grep -ac "zero area" log                             # same
```

A non-zero exit (132 = SIGILL, 138/139 = SIGBUS) is a regression.

## SIGBUS Note

`[SIGBUS] count=...` is not automatically a crash in this port: writes into the MAP_JIT code
region fault by design and the handler emulates them, so a large count during linking is normal.

`[EE-CRASH]` is different -- that is a fault the handler could *not* explain, and it is always a
real bug even if the game keeps running afterwards.

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

## Fixed: mips2c `jalr` did nothing on Apple ARM64

This was the last crash. `ExecutionContext::jalr` in `game/mips2c/mips2c_private.h` is how
hand-translated VU/EE code calls back into GOAL. Its dispatch was:

```cpp
#ifdef __linux__                              /* systemv */
#elif defined __APPLE__ && defined __x86_64__ /* systemv */
#elif _WIN32                                  /* win32   */
#endif
```

On Apple ARM64 no branch matches, so the body compiled to nothing: the GOAL function was never
called and `gprs[v0]` kept whatever was left from the previous instruction. Callers then used
that as a pointer. `sp_launch_particles_var` did exactly this and read `EE+0x8b160250`.

Nothing warned about it -- no `#error`, no assert, and the `extern "C"` block had the same gap so
there was not even a missing symbol at link time. It now calls `_call_goal8_asm_arm64` (already
present in `game/kernel/asm_funcs_arm64.s` with a matching signature) and ends in
`#else #error`, so the next unhandled platform fails loudly.

**Worth repeating as a rule:** grep for `__x86_64__` before trusting any platform dispatch in this
tree. `game/mips2c/mips2c_table.cpp` has two more such chains -- those do handle ARM64, but the
pattern is a recurring source of silent no-ops.

## Tooling: symbolizing a crash inside mips2c / C++

`backtrace()` is nearly useless on the EE thread: the frame pointer lives inside EE memory, so it
gives up after a few frames. The chain itself is intact, so `dump_arm64_crash_context` now walks
it by hand and prints an `[EE-CRASH] FP-chain:` block. That is what identified
`sp_launch_particles_var` -- `dladdr` had been misattributing the PC to
`ExecutionContext::lw` because the generated mips2c functions are static and have no exported
symbol.

For line numbers, build a dSYM (the binary has a debug map, 400+ OSO entries, but no dSYM):

```sh
dsymutil build/game/gk -o /tmp/gk.dSYM        # ~2s
# slide = runtime address of a known symbol - its static address:
nm build/game/gk | grep dump_arm64_crash_context
atos -o /tmp/gk.dSYM/Contents/Resources/DWARF/gk -l 0x100000000 <static addr>
```

Beware: with inlining, `atos` and `dladdr` will confidently name the *wrong* function. Trust the
FP-chain over a single symbolized PC, and confirm with a targeted check in the suspect helper
before acting on it.

## Fixed: wrong SIMD element index broke every cross product

The same `imm5` mistake as `splat_vf`, in three more emitter functions. For an Advanced SIMD
element index, the **lowest set bit of `imm5` selects the element size** and the bits above it
hold the index, so 32-bit lanes need `imm5 = (index << 3) | 0b00100`. The code used
`(index << 2) | 4`, and `ins_vf_element` also used `imm4 = srcIdx << 1` instead of `srcIdx << 2`:

| function | was | effect |
|---|---|---|
| `ins_vf_element` | `imm5=(d<<2)\|4`, `imm4=s<<1` | 15 of 16 lane pairs wrong |
| `ins_vf_element_from_gpr32` | `imm5=(i<<2)\|4` | wrong for every lane but 0 |
| `umov_gpr32_vf_element` | `imm5=(i<<2)\|4`, base `0x2E003C00` | wrong lane *and* wrong base (Q must be 0 for a 32-bit destination: `0x0E003C00`) |

`ins_vf_element` is what `IR_SwizzleVF::do_codegen_arm64` builds every swizzle out of, and
`.outer.product.a.vf` / `.outer.product.b.vf` are implemented as two swizzles plus a multiply.
So **every cross product in the engine was wrong**, which is why `initialize-mesh!` reported
things like "nav-mesh has 76 triangles with inverted normals (out of 76 triangles)". After the
fix those warnings go to zero and the picture is correct.

The swizzle *patterns* in `IR.cpp` were fine all along -- only the instruction they were built
from was broken. Verified against the system assembler for all 16 lane pairs, and
`CodeTester.execute_ins_vf_element_arm64` executes each pair and checks the moved lane.

`ins_element_s` (added earlier for `IR_BlendVF`) was a same-lane duplicate of this function and
has been removed; the blend now uses `ins_vf_element` directly.

## Fixed: backup stacks were sized for x86 frames

`thread-suspend` copies exactly `stack-size` bytes of the thread's stack into the process heap.
Each ARM64 call pushes x29/x30 and keeps SP 16-byte aligned, where x86's `CALL` pushes only an
8-byte return address, so the same GOAL call depth needs more bytes here. It was consistently
16 over: `Stack: 144/128`, logged **85352 times in a single run**, and the copy loop then ran off
the bottom of the buffer into whatever the process heap had put in front of it.

`backup-stack-size` in `gkernel-h.gc` adds 64 bytes of headroom on ARM64, applied in the two
places that set a backup stack size: `stack-size-set!` and `new cpu-thread`. Call sites keep
their x86-tuned numbers.

Watch for `suspend called without enough stack` in the log -- it should be zero. If it comes
back, the margin is too small for some new call depth, not a reason to ignore the message.

## Next up: Metal

### What exists

- `game/graphics/pipelines/metal.h` -- plain C++ so the runtime can include it. The Objective-C
  objects live behind an opaque `MetalContext`.
- `game/graphics/pipelines/metal.mm` -- Objective-C++, Apple-only, built with `-fobjc-arc` and
  linked against Metal / QuartzCore / Foundation (see `game/CMakeLists.txt`).
- `MetalDisplay : GfxDisplay` -- window, display manager, input manager, SDL event pump, and a
  clear-draw-present frame.
- `GfxPipeline::Metal` plus selection in `Gfx::GetRenderer` / `Gfx::Init`, opt-in through
  `OPENGOAL_RENDERER=metal`.
- `game/graphics/metal_renderer/shaders/` -- MSL sources. `solid_color.metal` is ported;
  `metal_shader_types.h` holds the layouts shared with C++.
- `game/graphics/metal_renderer/MetalShaderLibrary.{h,cpp}` -- reads the sources and compiles
  them at runtime.
- The DMA path: `metal_send_chain` / `metal_vsync` / `metal_sync_path` with a `MetalGraphicsData`
  mirroring the OpenGL backend's `GraphicsData`, and a bucket walk in `MetalDisplay::render`.
  Verified: `[Metal] first frame from GOAL: walked 70 of 70 buckets`.

`texture_upload_now`, `texture_relocate`, `set_levels` and `set_active_levels` are still no-ops --
they need the Metal texture pool and loader, which do not exist yet.

### The bucket loop is the hook point

`MetalDisplay::render` walks the chain exactly as `OpenGLRenderer::dispatch_buckets_jak1` does: a
call into the default-registers chain, then one 16-byte slot per bucket, `jak1::BucketId::MAX_BUCKETS`
(70) of them. Right now each bucket's data is skipped; a ported renderer consumes its own bucket
instead, and must leave the DMA cursor exactly at the next bucket boundary -- the OpenGL version
asserts on that, and getting it wrong desynchronises every later bucket.

Note the Metal build currently paces slower than OpenGL (dispatch ~#1600 vs ~#8700 over the same
wall time). Not investigated yet; expected to change once real rendering replaces the skip loop.

### Shaders are compiled at runtime, not into a .metallib

An offline `.metallib` needs the Metal Toolchain (`xcodebuild -downloadComponent MetalToolchain`),
which is not installed here -- `xcrun metal` fails with *"cannot execute tool 'metal' due to
missing Metal Toolchain"*. Runtime compilation via `newLibraryWithSource:` avoids that entirely
and matches how the OpenGL backend already loads GLSL, so editing a shader does not require a
rebuild. Switch to an offline `.metallib` later if startup cost becomes a problem.

`MTLCompileOptions` has no include search path, so `MetalShaderLibrary` expands
`#include "..."` itself. It is a **textual** expander and does not evaluate `#ifdef`, so it only
expands includes that exist in the shader folder and leaves every other include line untouched
for Metal's own preprocessor. That is what makes the `#ifdef __METAL_VERSION__` split in
`metal_shader_types.h` work -- without it, startup fails with
`Metal shader file not found: .../shaders/common/common_types.h`, `metal_make_display` returns
null, and you get **no window at all while the game keeps running headless**. If the Metal window
does not appear, check the log for `[Metal]` errors first.

### How this port is kept honest

Comparing screenshots does not scale, and it is not the method. The method is that **anything
which decides how a pixel looks lives in exactly one place, shared by both backends.** A second
copy is a second chance to disagree, and the disagreement shows up as a wrong picture somewhere
nobody is looking.

What is already shared, and therefore cannot diverge:

| Shared | Where |
|---|---|
| `.fr3` level data, level load/unload | `loader/Loader.cpp`, both backends hold one |
| PS2 texture conversion, texture pool bookkeeping | `texture/TexturePool.cpp` |
| GPU allocation (textures, vertex/index buffers) | `game/graphics/gpu_resources.h` -- one interface, a backend installs itself |
| Time-of-day colour interpolation | `interp_time_of_day` |
| Camera matrix, including the PC depth-range shift | `make_new_cam_mat` |
| Frustum + occlusion culling, index lists from vis strings | `cull_check_all_slow`, `make_index_list_from_vis_string` |
| What a GS alpha test means | `alpha_test_double_draw` |

What cannot be shared, and is therefore written **once as a translation table** rather than
decided per renderer: `DrawMode` to Metal state. Blend belongs to a pipeline state, depth test and
write to a depth-stencil state, clamp and filter to a sampler, so each distinct mode gets its
objects built once and cached by the mode's integer value. That lives in `MetalTFragment.mm`
today and moves out the moment a second renderer needs it.

What is left is the bucket list. The OpenGL backend is a table of 70 buckets, 16 distinct renderer
classes for Jak 1. That table is the port's checklist, not a search problem:

| Renderer | Buckets (jak1) | Metal |
|---|---|---|
| `TFragment` | 6 | **done** for NORMAL/LOWRES; TRANS, DIRT, ICE and the near variants still owed |
| `TextureUploadHandler` | 11 | needed next -- without it, animated and uploaded textures never reach the pool |
| `Generic2BucketRenderer` | 10 | |
| `Merc2BucketRenderer` | 8 | the characters |
| `DirectRenderer` | 3 | also the fallback path several others use |
| `Tie3WithEnvmapJak1` | 2 | the props: cliffs, huts, fences. Biggest single visual gap |
| `SkyBlendHandler` | 2 | |
| `Shrub` | 2 | closest in shape to tfrag3, so cheapest after it |
| `SkyRenderer`, `Sprite3`, `ShadowRenderer`, `OceanNear`, `OceanMidAndFar`, `EyeRenderer`, `DepthCue`, `EmptyBucketRenderer` | 1 each | |

Screenshots are the acceptance step at the end of a bucket, not the way the work is found. Both
backends write one at the same point in the game:

```sh
OPENGOAL_SCREENSHOT_LEVEL=village1 OPENGOAL_SCREENSHOT_DELAY=150 \
  OPENGOAL_GL_SCREENSHOT=/tmp/gl.png ./build/game/gk -v --game jak1 -- -boot -debug -fakeiso
OPENGOAL_SCREENSHOT_LEVEL=village1 OPENGOAL_SCREENSHOT_DELAY=150 \
  OPENGOAL_METAL_SCREENSHOT=/tmp/metal.png OPENGOAL_RENDERER=metal ./build/game/gk ...
```

The delay counts frames from when the named level comes into use, not absolute frame numbers: the
two backends do not pace identically during the boot, so the same absolute frame is a different
moment in the game.

### Remaining work, in order

1. **Shaders.** Port the remaining GLSL pairs in `game/graphics/opengl_renderer/shaders/`
   (44 `ShaderId`s, 88 files) to MSL and register them in `metal_shaders::all_shaders()`.
   GLSL uniforms become fields in a struct in `metal_shader_types.h`, since MSL has no
   free-floating uniforms. Replace `Shader.cpp`'s compile/link path with `MTLLibrary` /
   `MTLRenderPipelineState`.
2. **First bucket: `tfrag3`.** It covers most of the world and is the easiest to eyeball against
   the OpenGL reference. Be aware this is not just the renderer class: `TFragment.cpp` pulls in
   `background_common`, `Loader` (level geometry from the `.fr3` files, not from DMA),
   `TexturePool` and `SharedRenderState`. Those need Metal equivalents first, and that is the
   bulk of the work -- the bucket dispatch itself is already in place.
3. **Textures.** `game/graphics/texture/` to `MTLTexture`, keeping the existing PS2 format
   conversions.
4. **Render targets.** `Fbo.h` to `MTLRenderPassDescriptor` / offscreen `MTLTexture`, needed for
   the multi-pass effects (glow probes, shadows, sky blend).
5. **Remaining buckets** one at a time: `DirectRenderer`/`DirectRenderer2`, `merc2`/`emerc`,
   `tie`/`etie`, `shrub`, `sky`, `ocean`, `shadow`, `sprite`, `TextureAnimator`.

**Keep OpenGL working and selectable throughout.** Being able to flip between the two on the same
build is the only cheap way to tell a Metal bug from yet another codegen bug -- and given the
history in this file, assume there are more codegen bugs.

## The imported ARM64 differential suite

`test/goalc/test_arm64_*.cpp` (12 files, ~690 cases together with the rest of `goalc-test`) come
from [nikolasburns/jak-arm64-macos](https://github.com/nikolasburns/jak-arm64-macos), ISC
licensed, itself built on [DiMiTriFrog/jak2-macos-arm64](https://github.com/DiMiTriFrog/jak2-macos-arm64).
That fork implements the ARM64 backend **independently of this one** -- the two share no commits
-- which is exactly what makes the tests worth having: they describe ARM64 semantics, not our
implementation of it. A failure there is a difference that has to be explained.

`test/goalc/arm64_test_compat.h` bridges the naming: they spell the callee-saved GOAL registers
`X19`-`X22` where we use `x19`-`x22`, call the SIMD registers `V0`-`V15` where we use the x86 XMM
id range, and name a few emitters after the ARM64 mnemonic where ours are named after the x86
instruction they replace.

Importing them found three silent defects, all fixed in `fa2aeb068`: `vpsubd` subtracting 64-bit
lanes instead of 32-bit, `parallel_compare_e_*` emitting CMTST instead of CMEQ, and a constant
shift of 64 asserting where x86 masks with `& 63`.

### What did not come across, and why

Each removal is marked in the file it came from, with the reasoning. The short version:

- **Literal-pool symbol tests** (5 in `test_arm64_ir_symbols.cpp`, 1 in `test_arm64_ir_asm_basic.cpp`).
  They resolve a symbol by patching an 8-byte literal after the function. We patch MOVZ/MOVK
  immediates instead. **Equivalents for our mechanism are still owed** -- they have to simulate the
  linker before executing.
- **Scratch-register exclusion tests** (2 in `test_arm64_ir_vector.cpp`, 2 in `test_arm64_ir_int128.cpp`).
  Their vector helpers build results in a fixed X16/V16 pair, so those must be excluded per
  register class, checked through `to_rai(InstructionSet)`. We have neither: our sequences are
  scratch-free on the SIMD side, and our `RegAllocInstr` is x86-shaped with translation at emit
  time.
- **`test_arm64_trampolines.cpp` and `test_arm64_mips2c.cpp`**, which test their header-only
  runtime trampoline encoder. We use hand-written `game/kernel/asm_funcs_arm64.s`.
  `common/jit_memory.h` did come across -- it is theirs, and it is a better home for the W^X
  protection flips than open-coded `mprotect`.
- **The continuation-handoff lowering tests** (2 of 4 in `test_arm64_continuation_handoff.cpp`).
  See the open item below.

### Open items this import surfaced

1. **`test_arm64_runtime_bridge.cpp` is not in the build.** It drives `_arg_call_arm64`,
   `_stack_call_arm64`, `_mips2c_call_arm64` and `_call_goal*_asm_arm64` and checks that
   callee-saved GPR and SIMD sentinels survive them. Five cases fail and one crashes the runner --
   against **the same symbol names** in our `asm_funcs_arm64.s`, so this is a real suspicion about
   our bridges, not a naming mismatch. This is the highest-value thread to pull next.
2. **The `(.ret)` continuation handoff is guarded by hand, not by the lowering.** They lower
   `IR_AsmPush` with an rax-role source to `mov x30, <src>`, so every site is handled
   automatically. We instead write `#if ARM64_PORT (.mov lr temp)` next to the `(.push temp)` at
   each site: `gkernel.gc:500`, `gkernel.gc:1650`, `gkernel.gc:2040`, `gstate.gc:463`. The
   remaining `(.ret)` sites without such a guard -- `return-from-thread` (gkernel.gc:452),
   `return-from-thread-dead` (:482), `thread-suspend` (:643) and `new catch-frame` (:1579) --
   reach `ret` with x30 set by the caller or by the asm bridges. That holds today and is enforced
   nowhere. Their lowering is the more robust design and is worth adopting.
3. **An asm function can colour a callee-saved register here, which x86 rejects.**
   `do_asm_function_x86` throws when `used_saved_regs` is non-empty without `allow-saved-regs`,
   because an asm function gets no prologue to save it. Adding the same check to
   `do_asm_function_arm64` immediately rejects `return-from-thread`, whose coloring uses `q15`. So
   either that coloring is unsafe or our saved-register set is wrong; until that is settled the
   ARM64 path keeps ignoring `allow_saved_regs`.
4. **x26 is saved twice in every prologue that uses it.** SIMD and GPR ids share one space --
   `XMM10` is also id 26 -- so the prologue cannot tell which the allocator meant and saves both
   interpretations, 32 bytes where 16 would do. Correct, because restoring is symmetric, but it
   costs a wasted pair of instructions. Separating the id spaces is the real fix.

Their `PORTING-NOTES.md` §1 is also worth reading before touching any SSE-to-NEON translation:
min/max NaN ordering, denormals, conversion rounding modes and shuffle lane order all produce
plausible wrong numbers rather than crashes -- the same shape as the three defects above.

## Test coverage gap on ARM64

`test/goalc/CMakeLists.txt` compiles `test_CodeTester.cpp` and the imported `test_arm64_*.cpp`
files on ARM64. Every execution-level
GOAL test is commented out, including `test_vector_float.cpp`, which exercises masked vector ops
(`:mask`) directly -- that is why a no-op blend survived so long. Those fixtures need a booting
game to host the test runner, so re-enabling them is gated on the boot work above.

Until then, prefer executing `CodeTester` tests over encoding-only ones for anything with real
semantics: `CodeTester.execute_blend_vf_arm64` runs the emitted sequence and checks results,
which an expected-hex-string test would not have caught.
