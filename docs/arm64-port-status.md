# ARM64 Port Status — Jak 1 Boot

**Branch**: `fix/arm64-missing-instructions`  
**Last updated**: 2026-05-19  
**PR**: #3 (DRAFT)  

---

## Current State

Boot reaches `[EE] Kernel dispatch #1` (all 8 kernel modules link successfully), then crashes with **SIGILL** deep inside a recursive JIT call chain triggered by `(profiler-instant-event "ROOT")`. The crash is inside JIT-emitted GOAL code, not the C runtime.

| Milestone | Status |
|---|---|
| `goalc-test` emitter tests | 620/620 passing |
| `link finish: gcommon` | ✅ |
| `link finish: gkernel` | ✅ |
| All 8 kernel DGOs link | ✅ |
| `kernel: machine started` | ✅ |
| `[EE] Kernel dispatch #1` | ✅ |
| `(profiler-instant-event "ROOT")` | ❌ SIGILL → exit 132 |
| Title screen | ❌ not reached |

---

## What Was Fixed (This Branch + Preceding Master Commits)

### On this branch (PRs #3)

**`40bfa6091`** — `fix(arm64): correct FMOV gpr↔fpr encodings + extend SIGSEGV handler`

1. **FMOV direction swap** (`goalc/emitter/IGenARM64.cpp`):  
   A prior "fix" (commit `eeec7a7a9`) had inverted the four FMOV helpers against the ARM ARM. The real ARM assembler confirms:
   - opcode 110 (`0x9E66` / `0x1E26`) = `FMOV Xd, Dn` → FPR→GPR (writes GPR)
   - opcode 111 (`0x9E67` / `0x1E27`) = `FMOV Dd, Xn` → GPR→FPR (writes FPR)  
   
   The inverted encoding caused `movq_xmm64_gpr64(dst=XMM6, src=GPR)` to emit `FMOV x22, Dn` — clobbering GOAL's reserved offset register (x22) mid-function during gkernel link.

2. **SIGSEGV handler** (`game/runtime.cpp`):  
   Darwin reports write-fault on MAP_JIT memory as SIGSEGV (not SIGBUS) when the faulting instruction is a C-compiler-generated `STR` (e.g. `*dest = 0`). Extended the signal handler to also handle SIGSEGV, added decoding for `STR`/`STRB`/`STRH` unsigned-immediate and `STUR` unscaled-immediate forms.

### Foundational fixes on master (context for reviewers)

| Commit | Fix |
|---|---|
| `6f7eac21c` | `IR_StoreConstOffset` alias workaround (same as `IR_IntToFloat`) |
| `ec4883988` | x29 frame-ptr corruption in C trampolines; `SetSymbolValue` alias fix |
| `3e28786ea` | gcommon top-level hang; route GOAL code allocations to MAP_JIT `kcodeheap` |
| `77677ca6d` | SIGBUS handler: cover all ARM64 store instruction forms |
| `2333349f8` | Q/X register enum collision causing wrong calling convention |
| `eeec7a7a9` | Extractor host detection on ARM64 |
| `6af02c5e1` | UMOV encoding fix; cycle detection in `swizzle_vf` |
| `785f48b90` | NEON implementation for `SkyBlendCPU` |
| `081c4ab04` | General `swizzle_vf` for all 256 patterns |

---

## Remaining Blocker: Dispatch #1 SIGILL

### Symptom

After `kernel: machine started`, the kernel dispatcher calls:
```
(kernel-dispatcher) → (profiler-instant-event "ROOT") → (pc-prof "ROOT" 2)
```
`pc-prof` is defined in `game/kernel/common/kmachine.cpp:1056` and registered via `make_func_symbol_func`. Its GOAL symbol table entry's method dispatch (vtable offset `0x40`) resolves to a generic method-dispatch helper at `kcodeheap+0x06b4`. That helper's `blr x8` at offset `0x86c` has `x8 = own EE address` — causing ~1M recursive self-calls that exhaust the JIT stack, ending in SIGILL.

### Key facts

- `PC_PROFILER_ENABLE` is `#t` in `goal_src/jak1/kernel/gcommon.gc:29` — so `(profiler-instant-event ...)` always expands to `(pc-prof ...)` calls
- The function at `kcodeheap+0x06b4` is a **generic method-dispatch helper** (not `pc-prof` itself)
- Recursion depth: ~1,000,000 frames before stack overflow
- This is in JIT-emitted GOAL code, NOT the C kernel

### What was tried and ruled out (do not re-investigate)

- Depth-counting `pthread_jit_write_protect_np` enter/exit — no change
- 64MB JIT stack (vs 16MB default) — same crash, just deeper SP
- `LOW_PROTECT` skip on Apple ARM64 — unblocks earlier crash but doesn't fix this
- Custom SIGILL signal handler — never fires; OS kills via Mach exception before signal delivery

### Quick diagnostic: disable the profiler

Before investing in lldb disassembly, try this 1-line change in `goal_src/jak1/kernel/gcommon.gc:29`:

```lisp
;; Change this:
(define PC_PROFILER_ENABLE #t)
;; To:
(define PC_PROFILER_ENABLE #f)
```

Then recompile GOAL and reboot:
```sh
# in goalc REPL:
(mi)
# then run gk again
```

**Interpretation:**
- If boot continues past dispatch #1 → the profiler is the *only* blocker. Fix it in isolation.
- If boot still SIGILLs on something else → the method-dispatch recursion is a general ARM64 JIT bug and there are more blockers ahead.

This tells you the scope before spending hours in lldb.

### Recoverable diagnostic stash

There is a git stash with `[CGOS #N]` per-call logging and `[SIG-TRACE #N]` signal decoding already wired into `kscheme.cpp` / `runtime.cpp`. To retrieve it:

```sh
git stash list
# Look for: "wip: depth-counter + sig diagnostics + 64MB stack — superseded by revert"
git stash show -p stash@{N}   # inspect
git stash apply stash@{N}     # apply if useful
```

The diagnostics print `fptr`, `depth`, and `rsp` on every `call_goal_on_stack` entry, which shows the recursion building up in real-time.

### Next attack vector (lldb)

1. Load `gk` under lldb after `kernel: machine started` output:
   ```sh
   process attach --pid $(pgrep gk)
   ```
2. Get the EE base address (varies per run — it's a runtime `mmap`):
   ```
   (lldb) p (void*)g_ee_main_mem
   # example result: 0x7000000000
   ```
   kcodeheap sits at EE `0x4000000` → host `g_ee_main_mem + 0x4000000` (e.g. `0x7004000000`).
3. Disassemble the recursive function:
   ```
   (lldb) disassemble --start-address 0x7004000000+0x06b4 --count 60
   # (substitute your actual g_ee_main_mem + 0x4000000)
   ```
4. Key questions:
   - What symbol does `pc-prof` resolve to in the GOAL symbol table at runtime?
   - Does its method-table entry at offset `0x40` point to the same function?
   - On x86-64, does the same lookup recurse? If not, what's different in the ARM64 JIT's method-dispatch codegen?
5. Compare the ARM64 JIT output for the method-dispatch IR against the x86-64 output — look for `IR_CallIndirect` or `IR_VirtualMethodCall` in `goalc/compiler/IR.cpp`.

### Likely root cause (untested hypothesis)

The generic method-dispatch helper resolves `pc-prof`'s type's vtable — and the vtable slot 0x40 for that type is pointing back at the dispatch helper itself. This would be a **GOAL type system bug in the ARM64 JIT**, not a C runtime bug. Possible cause: the method-dispatch codegen emits an address that the linker resolves to the helper's own EE address instead of the C function's trampoline.

---

## Build Workflow

### One-time setup

```sh
export LIBRARY_PATH="$LIBRARY_PATH:/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib"
cmake -B build --preset=Release-macos-arm64-clang
```

### Regular builds

```sh
cmake --build build --parallel $(sysctl -n hw.logicalcpu)
```

> **Note**: If CMake reports "build.ninja is dirty" and rebuilds nothing, the manifest is stale. Run `cmake -B build --preset=Release-macos-arm64-clang` again then rebuild.

### Run tests

```sh
# All emitter/codegen tests (must pass 620/620):
./build/goalc-test --gtest_filter="CodeTester.*"

# Full test suite:
./build/goalc-test
```

### Boot the game

```sh
./build/game/gk -boot -debug --iso_data_path iso_data/jak1
```

Expected output before the SIGILL:
```
...
link finish: gcommon
link finish: gkernel-h
link finish: gkernel
link finish: pskernel
link finish: gstring-h
link finish: gstring
link finish: dgo-h
link finish: gstate
kernel: machine started
[EE] Kernel dispatch #1
# → SIGILL crash here
```

---

## lldb-MCP Setup (for live JIT inspection)

The `.mcp.json` includes an `lldb` MCP server entry that uses `$HOME` for portability. Each developer installs it once:

```sh
git clone https://github.com/stass/lldb-mcp ~/.claude/mcp-servers/lldb-mcp
cd ~/.claude/mcp-servers/lldb-mcp
python3 -m venv .venv
.venv/bin/pip install -e .
```

After installation the `lldb` server in `.mcp.json` resolves `$HOME` at runtime — no path edits needed.

Useful lldb-mcp commands for this work:

```
# Attach to running gk
lldb_attach(pid=<pgrep gk output>)

# Dump all registers on EE thread
lldb_info_registers()

# Disassemble at a JIT address
lldb_disassemble(address="0x7004000000 + <ee_offset>", count=40)

# Examine memory
lldb_examine(address="0x7000000000 + <symbol_ee_addr>", count=16, format="x")
```

---

## Key Files for Continued Work

| File | Relevance |
|---|---|
| `goalc/emitter/IGenARM64.cpp` | ARM64 instruction encoder — any new instructions go here |
| `goalc/compiler/IR.cpp` | JIT code generator — `do_codegen_arm64` methods |
| `game/runtime.cpp` | Signal handlers, EE memory layout, `g_ee_main_mem` |
| `game/kernel/jak1/kscheme.cpp` | C-to-GOAL trampolines, symbol table |
| `game/kernel/common/kmachine.cpp` | `pc-prof` and other built-in GOAL builtins |
| `game/kernel/asm_funcs_arm64.s` | Low-level GOAL call stubs |
| `goal_src/jak1/kernel/gcommon.gc` | `PC_PROFILER_ENABLE` flag (line 29), `profiler-instant-event` |
| `test/goalc/test_emitter.cpp` | Emitter unit tests (620 ARM64 cases) |

---

## EE Memory Layout

The EE (Emotion Engine emulator) address space is mapped via `mmap` in `game/runtime.cpp`. The base address (`g_ee_main_mem`) varies per run but is typically around `0x7000000000` on macOS ARM64.

| EE address | Host address | Contents |
|---|---|---|
| `0x00000000` | `g_ee_main_mem + 0x0` | EE RAM (symbols, heap, data) |
| `0x04000000` | `g_ee_main_mem + 0x4000000` | `kcodeheap` start (MAP_JIT, 16MB) |
| `0x05000000` | `g_ee_main_mem + 0x5000000` | `kcodeheap` end |

`kcodeheap` is the MAP_JIT region where all JIT-compiled GOAL code lives. Any write to this region from C++ triggers SIGBUS/SIGSEGV (handled by `sigbus_handler` in `runtime.cpp`) because Darwin enforces W^X on MAP_JIT pages.

To get the actual base in a running process:
```sh
# From lldb:
(lldb) p (void*)g_ee_main_mem

# Or from the gk log — look for:
# [EE] main memory at 0x7000000000
```

---

## ABI Reference

Darwin ARM64 calling convention as used by this port:

| Register | Role |
|---|---|
| x0–x7 | Arguments / return values |
| x8 | Indirect result / scratch |
| x16–x17 | Intra-procedure-call scratch (IP0/IP1) |
| x18 | Platform reserved — **do not touch** |
| x19–x28 | Callee-saved |
| x29 | Frame pointer (callee-saved) |
| x30 | Link register |
| **x20** | GOAL: `pp` (process pointer) |
| **x21** | GOAL: `st` (symbol table pointer) |
| **x22** | GOAL: `offset` / `R15` (EE base = `0x7000000000`) |
| **x23** | GOAL: GOAL stack pointer |
