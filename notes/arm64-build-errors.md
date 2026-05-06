# ARM64 Build Status (macOS Apple Silicon)

**Date:** 2026-05-06  
**Platform:** Darwin 25.4.0 / Apple Silicon (arm64)  
**Compiler:** AppleClang 21.0.0  
**Build:** `cmake -B build -G Ninja && ninja -C build`

## Ergebnis: BUILD ERFOLGREICH

Keine Kompilierfehler. Alle Binaries sind native arm64:

```
build/goalc/goalc     → Mach-O 64-bit executable arm64
build/game/gk         → Mach-O 64-bit executable arm64
build/goalc/libcompiler.dylib → arm64
```

## Warum kein Fehler? — Vorhandene ARM64-Portierung

Das Projekt hat bereits signifikante ARM64-Unterstützung:

### 1. SSE → NEON: sse2neon Drop-in (`common/util/simd_util.h`)

```cpp
#ifndef __aarch64__
#include <immintrin.h>
#else
#include "third-party/sse2neon/sse2neon.h"  // DLTcollab/sse2neon
#endif
```

Alle SSE-Intrinsics (`_mm_*`, `__m128`, etc.) werden über sse2neon auf NEON gemappt.
Betroffene Dateien die `simd_util.h` nutzen:
- `game/common/vu.h`
- `game/graphics/opengl_renderer/background/background_common.cpp`
- `game/graphics/opengl_renderer/ocean/OceanMid_PS2.cpp`
- `game/graphics/opengl_renderer/foreground/Merc2.cpp`
- `game/graphics/opengl_renderer/SkyBlendCPU.cpp`
- `common/custom_data/TFrag3Data.cpp`

### 2. Inline-Assembly: x86 `cpuid` hat ARM64-Stub (`common/util/os.cpp:50`)

```cpp
#elif __x86_64__
void __cpuidex(int result[4], int eax, int ecx) {
  asm("cpuid\n\t" : ...);  // nur x86
}
#else
// ARM64: gibt Nullen zurück, AVX-Detection disabled
void __cpuidex(int result[4], int eax, int ecx) {
  lg::warn("cpuid not implemented on this platform");
  for (int i = 0; i < 4; i++) { result[i] = 0; }
}
#endif
```

Konsequenz: `gCpuInfo.has_avx = false`, `has_avx2 = false` auf ARM64. Korrekt.

### 3. ABI / GOAL-Runtime: Vollständige ARM64 Assembly (`game/kernel/asm_funcs_arm64.s`)

7.8 KB ARM64-Assembly mit allen nötigen Calling-Convention-Bridges:
- `_arg_call_arm64` — GOAL → C++ (register args)
- `_stack_call_arm64` — GOAL → C++ (stack args)
- `_mips2c_call_arm64` — mips2c trampoline
- `_call_goal_asm_arm64` — C++ → GOAL (3 args)
- `_call_goal8_asm_arm64` — C++ → GOAL (8 args)
- `_call_goal_on_stack_asm_arm64` — C++ → GOAL mit Stack-Switch

kscheme.cpp für jak1/jak2/jak3/jakx hat jeweils `#ifdef __aarch64__`-Zweig
der die ARM64-Varianten der asm-Funktionen verwendet.

## Was NICHT funktioniert: GOAL-Compiler JIT-Emitter

### Problem: `goalc/emitter/IGenARM64.cpp`

196 Funktionen definiert, **189 davon sind Stubs**:

```cpp
InstructionARM64 mov_gpr64_gpr64(Register dst, Register src) {
  ASSERT_MSG(false, "not yet implemented");  // ← typisch
}
```

Das bedeutet: `goalc` kompiliert zwar (C++-Code), aber **kann keinen GOAL-Code zu nativem ARM64-Maschinencode JIT-kompilieren**. Jeder Versuch, GOAL-Quellcode zu übersetzen, crashed mit ASSERT.

**Nur ~7 Funktionen tatsächlich implementiert** (Ende der Datei, ab ~Zeile 440).

### Weitere TODOs (aus Quellcode)

| Datei | Zeile | Problem |
|-------|-------|---------|
| `goalc/emitter/IGenARM64.cpp` | 11 | "just silencing errors while things are not implemented" |
| `goalc/emitter/CodeTester.h` | 76 | CodeTester ist x86-spezifisch |
| `goalc/debugger/disassemble.h` | 48 | ARM64-Disassembly nicht implementiert |
| `game/kernel/asm_funcs_arm64.s` | 98 | mips2c XMM-Handling "weird", möglicherweise falsch |
| `game/kernel/asm_funcs_arm64.s` | 101 | Stack-Alignment bei mips2c fragwürdig |
| `common/util/os.cpp` | 50 | ARM CPU-Feature-Detection fehlt (gibt nur Nullen) |

## Zusammenfassung: Was funktioniert vs. was fehlt

| Komponente | Status |
|------------|--------|
| C++-Runtime kompilieren (`game/`) | ✅ Fertig |
| SSE → NEON (sse2neon) | ✅ Fertig |
| ARM64 ABI-Bridges (asm_funcs_arm64.s) | ✅ Fertig |
| mips2c Runtime | ✅ Kompiliert (asm vorhanden, Details unklar) |
| GOAL-Compiler JIT-Emitter (IGenARM64) | ❌ 189/196 Stubs |
| CodeTester / Unit Tests | ❌ x86-spezifisch |
| Disassembler | ❌ fehlt |
| CPU Feature Detection (NEON/SVE) | ⚠️ gibt Nullen zurück |

## Nächste Schritte

1. **Runtime validieren** — `gk` starten, prüfen ob es ohne Crash läuft
2. **IGenARM64.cpp implementieren** — Hauptarbeit, ~189 Funktionen
   - Referenz: `goalc/emitter/IGen.h` (x86-Implementierung als Vorlage)
   - ARM64-Encodings: https://developer.arm.com/documentation/ddi0487/latest
   - Tool: https://armconverter.com/
3. **mips2c-Trampoline validieren** — Stack-Alignment-Bug in `asm_funcs_arm64.s:101`
4. **CPU Detection** — `sys/auxv.h` + HWCAP auf Linux; `sysctlbyname` auf macOS
