# CLAUDE.md

<!-- Add your custom instructions below. Repowise will never modify anything outside the REPOWISE markers. -->
<!-- Examples: coding style rules, test commands, workflow preferences, constraints -->

## Project: jack-project-arm64 (OpenGOAL fork)

This is a fork adding **native ARM64 (Apple Silicon)** support to OpenGOAL — a port of the PS2 Jak & Daxter trilogy. The upstream targets x86-64; this fork fills in the ARM64 backend.

### Build & test workflow

```sh
export LIBRARY_PATH="$LIBRARY_PATH:/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib"
cmake -B build --preset=Release-macos-arm64-clang   # one-time configure
cmake --build build --parallel $((`sysctl -n hw.logicalcpu`))
cmake --build build --target gk --parallel $((`sysctl -n hw.logicalcpu`))  # just gk
./build/goalc-test                                   # all 616 unit tests
./build/goalc-test --gtest_filter="CodeTester.*"     # emitter/codegen tests only
./build/offline-test --iso_data_path iso_data/jak1 --game jak1  # needs ISO
```

**CRITICAL: compiler changes require a CGO rebuild.** Edits to `goalc/compiler/IR.cpp`, `goalc/emitter/*`, or any `.gc` only affect future GOAL compilations, so the CGOs must be regenerated:
```sh
./build/goalc/goalc -g jak1 -c "(mi)"   # ~52s, rebuilds every CGO under out/jak1/iso/
```
**Use `(mi)`, not `(build-game)`.** `(build-game)` is `(make-group "all-code")` and `all-code` contains only `*all-gc*`: it compiles `.gc` files to `.o` in `out/jak1/obj/` but never packs the CGOs. `-fakeiso` loads `out/jak1/iso/GAME.CGO`, so `(build-game)` alone leaves the game running stale code. `(build-kernel)` does pack KERNEL.CGO, which is why kernel edits appear to take effect while engine edits silently do not.

**Boot timing (Apple Silicon, `-boot -debug -fakeiso`):** roughly 90 seconds from launch to
`GAMEPLAY: enter village1`. (Earlier notes claimed 14-25 minutes; that was the pre-fix state.)
The game then runs indefinitely -- to test, use `timeout 150 ./build/game/gk ...` and expect
exit 124, or run it without a timeout and close it yourself for exit 0.

### ARM64-specific rules

- Every `do_codegen_arm64` in `goalc/compiler/IR.cpp` **must** have semantic parity with its `do_codegen_x86` sibling. When adding a case to one, add it to the other.
- The emitter (`goalc/emitter/IGenARM64.cpp`) contains single-instruction helpers. When an x86 idiom has no single ARM64 equivalent (e.g., 3-reg+offset addressing, PC-relative byte load), the **caller in `IR.cpp`** must emit the multi-instruction sequence — never silently ignore an offset.
- `test/goalc/test_arm64_*.cpp` is an ARM64 differential suite imported from an independent ARM64
  fork (see "The imported ARM64 differential suite" in CODEX.md). Run `./build/goalc-test` before
  and after any emitter or codegen change; a failure there describes ARM64 semantics, not our
  implementation, so fix the backend rather than the test unless the file explains why our design
  differs.
- Always add a `CodeTester.*_arm64` test in `test/goalc/test_CodeTester.cpp` for every new emitter function. **Prefer an executing test (`CodeTester.execute_*`) over an expected-hex-string test.** A wrong ARM64 encoding usually does not crash -- it computes a plausible wrong number and the engine carries on. Three separate shipped bugs (`splat_vf`, `ins_vf_element`, `ins_vf_element_from_gpr32`) were the same `imm5` element-index mistake, and hex-string tests passed for all of them.
- **SIMD element index encoding:** in `imm5` the lowest set bit selects the element size and the bits above it hold the index. 32-bit (S) lanes need `imm5 = (index << 3) | 0b00100`, and `INS`'s `imm4 = srcIdx << 2`. Verify any new encoding against the system assembler: write the mnemonic to a `.s`, `clang -c -target arm64-apple-macos`, then `otool -t`.
- **Platform dispatch:** grep for `__x86_64__` before trusting an `#if` chain. mips2c's `jalr` had no ARM64 branch and silently compiled to nothing, so GOAL callbacks never ran. Always end such chains with `#else #error`.
- **"Callee-saved" here means "survives a GOAL kernel context switch"**, which is stricter than AAPCS64. `thread-suspend` saves only `s0-s4` (= `x19/X23/X6/X7/X26` after `translate_x86_reg_to_arm64`). Never put a register in `m_saved_gprs` that is not in that set.
- `InstructionSet::ARM64` is detected via `gen->instr_set()`. Use existing dispatch patterns in `IR.cpp`.
- ARM64 ABI on Darwin: x0–x7 args, x8 scratch/indirect-result, x16–x17 IP regs, x18 platform (do not touch), x19–x28 callee-saved, x29 FP, x30 LR. GOAL additionally reserves x22 (offset/R15) and x23 (GOAL stack) — see `game/kernel/asm_funcs_arm64.s`.
- Stack must stay 16-byte aligned at all call sites.

### Common gotchas

- **Repo name**: this fork is `jack-project-arm64`, not `jak-project`. `common/util/FileUtil.cpp::try_get_project_path_from_path` handles both markers — keep that.
- macOS ARM64 requires the `LIBRARY_PATH` export before `cmake -B build` (see README).
- The game binary `gk` is code-signed at build with Hardened Runtime + allow-jit — required for JIT execution on Apple Silicon.
- GitHub Actions are currently paused due to billing on the `sudo-Oliver` account — check Billing & Plans before expecting CI feedback.

### When to use subagents

- **arm64-emitter-reviewer** — use proactively after any change to `goalc/emitter/IGenARM64.*` or a `do_codegen_arm64` method.
- **Explore** — general code search across the 1.5M-line tree.

<!-- REPOWISE:START — Do not edit below this line. Auto-generated by Repowise. -->
## IMPORTANT: Codebase Intelligence Instructions for jak-project

> This repository is indexed by [Repowise](https://repowise.dev).
> Use the MCP tools below for orientation, discovery, and enriched context
> (documentation, ownership, history, decisions). **Always verify against
> actual source files before making changes** — the index may be stale.

Last indexed: 2026-05-08 (commit 9ff664be2)
### Entry Points
- `lsp/state/app.h`
- `decompiler/extractor/main.cpp`
- `decompiler/main.cpp`
- `game/main.cpp`
- `goalc/build_actor/main.cpp`
- `goalc/build_level/main.cpp`
- `goalc/main.cpp`
- `lsp/main.cpp`
- `tools/formatter/main.cpp`
- `tools/level_tools/level_dump/main.cpp`
### Hotspots (High Churn)
| File | Churn | 90d Commits | Owner |
|------|-------|-------------|-------|
| `goalc/compiler/IR.cpp` | 100.0th %ile | 5 | water111 |
| `goalc/emitter/IGenARM64.cpp` | 100.0th %ile | 6 | Tyler Wilding |
| `third-party/SDL/src/gpu/vulkan/SDL_gpu_vulkan.c` | 99.9th %ile | 1 | Tyler Wilding |
| `third-party/SDL/src/gpu/d3d12/SDL_gpu_d3d12.c` | 99.9th %ile | 1 | Tyler Wilding |
| `third-party/SDL/src/gpu/SDL_gpu.c` | 99.9th %ile | 1 | Tyler Wilding |

### Repowise MCP Tools

This project has a Repowise MCP server configured. These tools provide documentation, ownership, architectural decisions, and risk signals. Use them for orientation and discovery — then read actual source to verify before editing.

**Recommended workflow:**

1. Start with `get_overview()` on a new task to orient yourself.
2. Call `get_context(targets=["path/to/file.py"])` for enriched context on unfamiliar files — but always read the source before editing.
3. Call `get_risk(targets=["path/to/file.py"])` before changing hotspot files.
4. Don't know where something lives? Call `search_codebase(query="authentication flow")`.
5. Need to understand why code is structured a certain way? Call `get_why(query="why JWT over sessions")` before architectural changes.
6. After **architectural changes**, consider calling `update_decision_records(action="create", ...)` to record the rationale.
7. Need to understand how two modules connect? Call `get_dependency_path(source="src/auth", target="src/db")`.
8. Before cleanup tasks, call `get_dead_code()` to find confirmed unused code.
9. For documentation or diagrams, call `get_architecture_diagram(scope="src/auth")`.

**Note:** MCP tool responses reflect the last index run. If the index is stale, verify against source files.

| Tool | When to use |
|------|-------------|
| `get_overview()` | Orient yourself on a new task |
| `get_context(targets=[...])` | Enriched context on unfamiliar files |
| `get_risk(targets=[...])` | Before changing hotspot files |
| `get_why(query="...")` | Before architectural changes |
| `update_decision_records(action=...)` | After architectural changes — record decisions |
| `search_codebase(query="...")` | When locating code |
| `get_dependency_path(source=..., target=...)` | When tracing module connections |
| `get_dead_code()` | Before any cleanup or removal |
| `get_architecture_diagram(scope=...)` | For visual structure or documentation |

<!-- REPOWISE:END -->
