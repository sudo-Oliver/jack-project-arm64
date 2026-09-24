#pragma once

/*!
 * @file arm64_test_compat.h
 * Naming bridge for the ARM64 tests imported from https://github.com/nikolasburns/jak-arm64-macos.
 *
 * That fork and this one implement the ARM64 backend independently and name the registers
 * differently:
 *
 *   - it spells the callee-saved GOAL registers `X19`..`X22`; we spell them `x19`..`x22`
 *   - it calls the SIMD registers `V0`..`V15`; we call them `Q0`..`Q15` (same ids, same registers)
 *   - it keeps a second `gRegInfoARM64` and picks between the two with `get_register_info()`;
 *     we keep one `gRegInfo` in x86 terms and translate at emit time
 *     (`translate_x86_reg_to_arm64`)
 *
 * The aliases live here rather than in goalc/emitter/Register.h so the imported tests stay
 * readable against their original, and so the naming of the backend itself is not disturbed by
 * what the tests happen to call things.
 */

#include "goalc/emitter/IGen.h"
#include "goalc/emitter/IGenARM64.h"
#include "goalc/emitter/Register.h"

namespace emitter {

// Callee-saved GOAL registers, in the imported tests' spelling.
constexpr ARM64_REG X19 = emitter::x19;
constexpr ARM64_REG X20 = emitter::x20;
constexpr ARM64_REG X21 = emitter::x21;
constexpr ARM64_REG X22 = emitter::x22;

// SIMD registers. Their model numbers the SIMD registers from 0 (V0 == id 0, sharing the id
// space with X0); ours keeps GOAL's SIMD values in the x86 XMM id range (16-31) and maps them to
// physical Q0-Q15 at emit time (see qreg() in IGenARM64.cpp). So V<n> here is XMM<n>: the same
// physical register, and the same encoding, but the id our emitter accepts as SIMD.
constexpr X86_REG V0 = emitter::XMM0;
constexpr X86_REG V1 = emitter::XMM1;
constexpr X86_REG V2 = emitter::XMM2;
constexpr X86_REG V3 = emitter::XMM3;
constexpr X86_REG V4 = emitter::XMM4;
constexpr X86_REG V5 = emitter::XMM5;
constexpr X86_REG V6 = emitter::XMM6;
constexpr X86_REG V7 = emitter::XMM7;
constexpr X86_REG V8 = emitter::XMM8;
constexpr X86_REG V9 = emitter::XMM9;
constexpr X86_REG V10 = emitter::XMM10;
constexpr X86_REG V11 = emitter::XMM11;
constexpr X86_REG V12 = emitter::XMM12;
constexpr X86_REG V13 = emitter::XMM13;
constexpr X86_REG V14 = emitter::XMM14;
constexpr X86_REG V15 = emitter::XMM15;

// We have a single RegisterInfo, expressed in x86 terms, so there is nothing to select between.
inline const RegisterInfo& get_register_info(emitter::InstructionSet) {
  return gRegInfo;
}

}  // namespace emitter

namespace emitter {
namespace IGen {

// Same instructions, different names. Ours are named after the x86 instruction they replace,
// theirs after the ARM64 mnemonic. These wrap the dispatching IGen entry points, which is what
// the imported tests call.
inline Instruction movd_gpr32_f32(const ObjectGenerator& gen, Register dst, Register src) {
  return movd_gpr32_xmm32(gen, dst, src);
}
inline Instruction movd_f32_gpr32(const ObjectGenerator& gen, Register dst, Register src) {
  return movd_xmm32_gpr32(gen, dst, src);
}
inline Instruction cmp_f32_f32(const ObjectGenerator& gen, Register a, Register b) {
  return cmp_flt_flt(gen, a, b);
}

namespace ARM64 {

// The same three, in the ARM64-only namespace: some of the imported tests build the expected
// encoding directly rather than through the dispatcher.
inline InstructionARM64 movd_gpr32_f32(Register dst, Register src) {
  return movd_gpr32_xmm32(dst, src);
}
inline InstructionARM64 movd_f32_gpr32(Register dst, Register src) {
  return movd_xmm32_gpr32(dst, src);
}
inline InstructionARM64 cmp_f32_f32(Register a, Register b) {
  return cmp_flt_flt(a, b);
}

}  // namespace ARM64

}  // namespace IGen
}  // namespace emitter
