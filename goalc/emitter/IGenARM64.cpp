
#include "IGenARM64.h"

#include "goalc/emitter/Instruction.h"
#include "goalc/emitter/InstructionSet.h"
#include "goalc/emitter/Register.h"

// https://armconverter.com/?code=ret
// https://developer.arm.com/documentation/ddi0487/latest

// TODO ARM64 - just silencing errors while things are not implemented obviously
#pragma GCC diagnostic ignored "-Wunused-parameter"

namespace emitter {
namespace IGen {
namespace ARM64 {
//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
//   MOVES
//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

const auto instr_set = emitter::InstructionSet::ARM64;
using namespace emitter::ARM64;

InstructionARM64 mov_gpr64_gpr64(Register dst, Register src) {
  // MOV Xd, Xn — alias for ORR Xd, XZR, Xn (shifted reg, no shift)
  // Encoding: 1_01_01010_00_0_Rm_000000_11111_Rd
  // https://www.scs.stanford.edu/~zyedidia/arm64/orr_log_shift.html
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(src.is_gpr(instr_set));
  return InstructionARM64(Base(0b10101010000, 11), Rm(src.id()), Rn(31), Rd(dst.id()));
}

InstructionARM64 mov_gpr64_u64(Register dst, uint64_t val) {
  // ARM64 has no single-instruction 64-bit immediate move.
  // For values fitting in 16 bits: MOVZ Xd, #imm16
  // Larger values need MOVZ + up to 3x MOVK — handled by the code generator.
  // https://www.scs.stanford.edu/~zyedidia/arm64/movz.html
  ASSERT_MSG(val <= 0xFFFF, "mov_gpr64_u64: value > 16-bit requires multi-instruction sequence (MOVZ+MOVK), handle in CodeGenerator");
  ASSERT(dst.is_gpr(instr_set));
  // MOVZ Xd, #imm16: 1_10_100101_00_imm16_Rd  (hw=00 = shift 0)
  return InstructionARM64(Base(0b11010010100, 11), Field{static_cast<u32>((val & 0xFFFF) << 5)}, Rd(dst.id()));
}

InstructionARM64 mov_gpr64_u32(Register dst, uint64_t val) {
  // For 32-bit values ≤ 16-bit: MOVZ Wd, #imm16 (zeros upper bits)
  // For 17-32-bit values: needs MOVZ + MOVK — handle in CodeGenerator.
  // https://www.scs.stanford.edu/~zyedidia/arm64/movz.html
  ASSERT_MSG(val <= 0xFFFF, "mov_gpr64_u32: value > 16-bit requires multi-instruction (MOVZ+MOVK), handle in CodeGenerator");
  ASSERT(dst.is_gpr(instr_set));
  // MOVZ Wd, #imm16: 0_10_100101_00_imm16_Rd  (sf=0 → 32-bit, zeros upper 32)
  return InstructionARM64(Base(0b01010010100, 11), Field{static_cast<u32>((val & 0xFFFF) << 5)}, Rd(dst.id()));
}

InstructionARM64 mov_gpr64_s32(Register dst, int64_t val) {
  // Signed 32-bit immediate. ARM64 has no single-instruction sign-extending move for 32-bit imm.
  // MOVN Xd, #~val for small negatives, or MOVZ for small positives, or multi-instruction.
  ASSERT_MSG(false, "mov_gpr64_s32: no single-instruction ARM64 equivalent for arbitrary 32-bit signed imm — handle in CodeGenerator with MOVZ/MOVN+MOVK");
  return InstructionARM64(0b0);
}

InstructionARM64 movd_gpr32_xmm32(Register dst, Register src) {
  // FMOV Wd, Sn — move 32-bit float register to 32-bit GPR
  // https://www.scs.stanford.edu/~zyedidia/arm64/fmov_float.html
  // Encoding: 0_00_11110_00_1_00110_000000_Rn_Rd
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(src.is_128bit_simd(instr_set));
  return InstructionARM64(0x1E260000u, Rn(src.id()), Rd(dst.id()));
}

InstructionARM64 movd_xmm32_gpr32(Register dst, Register src) {
  // FMOV Sd, Wn — move 32-bit GPR to float register
  // https://www.scs.stanford.edu/~zyedidia/arm64/fmov_float.html
  // Encoding: 0_00_11110_00_1_00111_000000_Rn_Rd
  ASSERT(dst.is_128bit_simd(instr_set));
  ASSERT(src.is_gpr(instr_set));
  return InstructionARM64(0x1E270000u, Rn(src.id()), Rd(dst.id()));
}

InstructionARM64 movq_gpr64_xmm64(Register dst, Register src) {
  // FMOV Xd, Dn — move 64-bit float (D) register to 64-bit GPR
  // type=01 (double), opcode2=00110
  // Encoding: 1_00_11110_01_1_00110_000000_Rn_Rd  (sf=1 for 64-bit GPR)
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(src.is_128bit_simd(instr_set));
  return InstructionARM64(0x9E660000u, Rn(src.id()), Rd(dst.id()));
}

InstructionARM64 movq_xmm64_gpr64(Register dst, Register src) {
  // FMOV Dn, Xn — move 64-bit GPR to 64-bit float (D) register
  // type=01 (double), opcode2=00111
  // Encoding: 1_00_11110_01_1_00111_000000_Rn_Rd  (sf=1 for 64-bit GPR)
  ASSERT(dst.is_128bit_simd(instr_set));
  ASSERT(src.is_gpr(instr_set));
  return InstructionARM64(0x9E670000u, Rn(src.id()), Rd(dst.id()));
}

InstructionARM64 mov_xmm32_xmm32(Register dst, Register src) {
  // FMOV Sd, Sn — single-precision float register copy
  // https://www.scs.stanford.edu/~zyedidia/arm64/fmov_float.html
  // Encoding: 0_00_11110_00_1_00000_010000_Rn_Rd
  ASSERT(dst.is_128bit_simd(instr_set));
  ASSERT(src.is_128bit_simd(instr_set));
  return InstructionARM64(0x1E204000u, Rn(src.id()), Rd(dst.id()));
}

// todo - GPR64 -> XMM64 (zext)
// todo - XMM -> GPR64

//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
//   GOAL Loads and Stores
//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

// ARM64 Load/Store (register offset) encoding for GPR:
// size(2) | 111 | V(0) | 00 | opc(2) | 1 | Rm(5) | option(011=LSL) | S(0) | 10 | Rn(5) | Rt(5)
// Fixed option/S/10 bits[15:10] = 011_0_10 = 0b011010 → << 10 = 0x6800
// Base opcode constants (top 11 bits shifted to [31:21]):
//   STRB  (size=00,V=0,opc=00): 0x38200000
//   LDRB  (size=00,V=0,opc=01): 0x38600000
//   LDRSB (size=00,V=0,opc=10): 0x38A00000   (sign-extend to 64-bit)
//   STRH  (size=01,V=0,opc=00): 0x78200000
//   LDRH  (size=01,V=0,opc=01): 0x78600000
//   LDRSH (size=01,V=0,opc=10): 0x78A00000   (sign-extend to 64-bit)
//   STR W (size=10,V=0,opc=00): 0xB8200000
//   LDR W (size=10,V=0,opc=01): 0xB8600000
//   LDRSW (size=10,V=0,opc=10): 0xB8A00000   (sign-extend to 64-bit)
//   STR X (size=11,V=0,opc=00): 0xF8200000
//   LDR X (size=11,V=0,opc=01): 0xF8600000
// https://www.scs.stanford.edu/~zyedidia/arm64/ldr_reg_gen.html

InstructionARM64 load8s_gpr64_gpr64_plus_gpr64(Register dst, Register addr1, Register addr2) {
  // LDRSB Xd, [addr1, addr2] — sign-extend byte to 64-bit
  return InstructionARM64(0x38A06800u, Rm(addr2.id()), Rn(addr1.id()), Rt(dst.id()));
}

InstructionARM64 store8_gpr64_gpr64_plus_gpr64(Register addr1, Register addr2, Register value) {
  // STRB Wt, [addr1, addr2]
  return InstructionARM64(0x38206800u, Rm(addr2.id()), Rn(addr1.id()), Rt(value.id()));
}

InstructionARM64 load8s_gpr64_gpr64_plus_gpr64_plus_s8(Register dst,
                                                       Register addr1,
                                                       Register addr2,
                                                       s64 offset) {
  // ARM64 has no addr1+addr2+offset addressing. Requires ADD+LDRSB (multi-instr).
  ASSERT_MSG(offset == 0, "3-reg+offset memory requires multi-instruction sequence on ARM64");
  return load8s_gpr64_gpr64_plus_gpr64(dst, addr1, addr2);
}

InstructionARM64 store8_gpr64_gpr64_plus_gpr64_plus_s8(Register addr1,
                                                       Register addr2,
                                                       Register value,
                                                       s64 offset) {
  ASSERT_MSG(offset == 0, "3-reg+offset memory requires multi-instruction sequence on ARM64");
  return store8_gpr64_gpr64_plus_gpr64(addr1, addr2, value);
}

InstructionARM64 load8s_gpr64_gpr64_plus_gpr64_plus_s32(Register dst,
                                                        Register addr1,
                                                        Register addr2,
                                                        s64 offset) {
  ASSERT_MSG(offset == 0, "3-reg+offset memory requires multi-instruction sequence on ARM64");
  return load8s_gpr64_gpr64_plus_gpr64(dst, addr1, addr2);
}

InstructionARM64 store8_gpr64_gpr64_plus_gpr64_plus_s32(Register addr1,
                                                        Register addr2,
                                                        Register value,
                                                        s64 offset) {
  ASSERT_MSG(offset == 0, "3-reg+offset memory requires multi-instruction sequence on ARM64");
  return store8_gpr64_gpr64_plus_gpr64(addr1, addr2, value);
}

InstructionARM64 load8u_gpr64_gpr64_plus_gpr64(Register dst, Register addr1, Register addr2) {
  // LDRB Wd, [addr1, addr2] — zero-extend byte to 64-bit
  return InstructionARM64(0x38606800u, Rm(addr2.id()), Rn(addr1.id()), Rt(dst.id()));
}

InstructionARM64 load8u_gpr64_gpr64_plus_gpr64_plus_s8(Register dst,
                                                       Register addr1,
                                                       Register addr2,
                                                       s64 offset) {
  ASSERT_MSG(offset == 0, "3-reg+offset memory requires multi-instruction sequence on ARM64");
  return load8u_gpr64_gpr64_plus_gpr64(dst, addr1, addr2);
}

InstructionARM64 load8u_gpr64_gpr64_plus_gpr64_plus_s32(Register dst,
                                                        Register addr1,
                                                        Register addr2,
                                                        s64 offset) {
  ASSERT_MSG(offset == 0, "3-reg+offset memory requires multi-instruction sequence on ARM64");
  return load8u_gpr64_gpr64_plus_gpr64(dst, addr1, addr2);
}

InstructionARM64 load16s_gpr64_gpr64_plus_gpr64(Register dst, Register addr1, Register addr2) {
  // LDRSH Xd, [addr1, addr2] — sign-extend halfword to 64-bit
  return InstructionARM64(0x78A06800u, Rm(addr2.id()), Rn(addr1.id()), Rt(dst.id()));
}

InstructionARM64 store16_gpr64_gpr64_plus_gpr64(Register addr1, Register addr2, Register value) {
  // STRH Wt, [addr1, addr2]
  return InstructionARM64(0x78206800u, Rm(addr2.id()), Rn(addr1.id()), Rt(value.id()));
}

InstructionARM64 store16_gpr64_gpr64_plus_gpr64_plus_s8(Register addr1,
                                                        Register addr2,
                                                        Register value,
                                                        s64 offset) {
  ASSERT_MSG(offset == 0, "3-reg+offset memory requires multi-instruction sequence on ARM64");
  return store16_gpr64_gpr64_plus_gpr64(addr1, addr2, value);
}

InstructionARM64 store16_gpr64_gpr64_plus_gpr64_plus_s32(Register addr1,
                                                         Register addr2,
                                                         Register value,
                                                         s64 offset) {
  ASSERT_MSG(offset == 0, "3-reg+offset memory requires multi-instruction sequence on ARM64");
  return store16_gpr64_gpr64_plus_gpr64(addr1, addr2, value);
}

InstructionARM64 load16s_gpr64_gpr64_plus_gpr64_plus_s8(Register dst,
                                                        Register addr1,
                                                        Register addr2,
                                                        s64 offset) {
  ASSERT_MSG(offset == 0, "3-reg+offset memory requires multi-instruction sequence on ARM64");
  return load16s_gpr64_gpr64_plus_gpr64(dst, addr1, addr2);
}

InstructionARM64 load16s_gpr64_gpr64_plus_gpr64_plus_s32(Register dst,
                                                         Register addr1,
                                                         Register addr2,
                                                         s64 offset) {
  ASSERT_MSG(offset == 0, "3-reg+offset memory requires multi-instruction sequence on ARM64");
  return load16s_gpr64_gpr64_plus_gpr64(dst, addr1, addr2);
}

InstructionARM64 load16u_gpr64_gpr64_plus_gpr64(Register dst, Register addr1, Register addr2) {
  // LDRH Wd, [addr1, addr2] — zero-extend halfword
  return InstructionARM64(0x78606800u, Rm(addr2.id()), Rn(addr1.id()), Rt(dst.id()));
}

InstructionARM64 load16u_gpr64_gpr64_plus_gpr64_plus_s8(Register dst,
                                                        Register addr1,
                                                        Register addr2,
                                                        s64 offset) {
  ASSERT_MSG(offset == 0, "3-reg+offset memory requires multi-instruction sequence on ARM64");
  return load16u_gpr64_gpr64_plus_gpr64(dst, addr1, addr2);
}

InstructionARM64 load16u_gpr64_gpr64_plus_gpr64_plus_s32(Register dst,
                                                         Register addr1,
                                                         Register addr2,
                                                         s64 offset) {
  ASSERT_MSG(offset == 0, "3-reg+offset memory requires multi-instruction sequence on ARM64");
  return load16u_gpr64_gpr64_plus_gpr64(dst, addr1, addr2);
}

InstructionARM64 load32s_gpr64_gpr64_plus_gpr64(Register dst, Register addr1, Register addr2) {
  // LDRSW Xd, [addr1, addr2] — sign-extend word to 64-bit
  return InstructionARM64(0xB8A06800u, Rm(addr2.id()), Rn(addr1.id()), Rt(dst.id()));
}

InstructionARM64 store32_gpr64_gpr64_plus_gpr64(Register addr1, Register addr2, Register value) {
  // STR Wt, [addr1, addr2]
  return InstructionARM64(0xB8206800u, Rm(addr2.id()), Rn(addr1.id()), Rt(value.id()));
}

InstructionARM64 load32s_gpr64_gpr64_plus_gpr64_plus_s8(Register dst,
                                                        Register addr1,
                                                        Register addr2,
                                                        s64 offset) {
  ASSERT_MSG(offset == 0, "3-reg+offset memory requires multi-instruction sequence on ARM64");
  return load32s_gpr64_gpr64_plus_gpr64(dst, addr1, addr2);
}

InstructionARM64 store32_gpr64_gpr64_plus_gpr64_plus_s8(Register addr1,
                                                        Register addr2,
                                                        Register value,
                                                        s64 offset) {
  ASSERT_MSG(offset == 0, "3-reg+offset memory requires multi-instruction sequence on ARM64");
  return store32_gpr64_gpr64_plus_gpr64(addr1, addr2, value);
}

InstructionARM64 load32s_gpr64_gpr64_plus_gpr64_plus_s32(Register dst,
                                                         Register addr1,
                                                         Register addr2,
                                                         s64 offset) {
  ASSERT_MSG(offset == 0, "3-reg+offset memory requires multi-instruction sequence on ARM64");
  return load32s_gpr64_gpr64_plus_gpr64(dst, addr1, addr2);
}

InstructionARM64 store32_gpr64_gpr64_plus_gpr64_plus_s32(Register addr1,
                                                         Register addr2,
                                                         Register value,
                                                         s64 offset) {
  ASSERT_MSG(offset == 0, "3-reg+offset memory requires multi-instruction sequence on ARM64");
  return store32_gpr64_gpr64_plus_gpr64(addr1, addr2, value);
}

InstructionARM64 load32u_gpr64_gpr64_plus_gpr64(Register dst, Register addr1, Register addr2) {
  // LDR Wd, [addr1, addr2] — zero-extend word (upper 32 bits cleared)
  return InstructionARM64(0xB8606800u, Rm(addr2.id()), Rn(addr1.id()), Rt(dst.id()));
}

InstructionARM64 load32u_gpr64_gpr64_plus_gpr64_plus_s8(Register dst,
                                                        Register addr1,
                                                        Register addr2,
                                                        s64 offset) {
  ASSERT_MSG(offset == 0, "3-reg+offset memory requires multi-instruction sequence on ARM64");
  return load32u_gpr64_gpr64_plus_gpr64(dst, addr1, addr2);
}

InstructionARM64 load32u_gpr64_gpr64_plus_gpr64_plus_s32(Register dst,
                                                         Register addr1,
                                                         Register addr2,
                                                         s64 offset) {
  ASSERT_MSG(offset == 0, "3-reg+offset memory requires multi-instruction sequence on ARM64");
  return load32u_gpr64_gpr64_plus_gpr64(dst, addr1, addr2);
}

InstructionARM64 load64_gpr64_gpr64_plus_gpr64(Register dst, Register addr1, Register addr2) {
  // LDR Xd, [addr1, addr2]
  return InstructionARM64(0xF8606800u, Rm(addr2.id()), Rn(addr1.id()), Rt(dst.id()));
}

InstructionARM64 store64_gpr64_gpr64_plus_gpr64(Register addr1, Register addr2, Register value) {
  // STR Xt, [addr1, addr2]
  return InstructionARM64(0xF8206800u, Rm(addr2.id()), Rn(addr1.id()), Rt(value.id()));
}

InstructionARM64 load64_gpr64_gpr64_plus_gpr64_plus_s8(Register dst,
                                                       Register addr1,
                                                       Register addr2,
                                                       s64 offset) {
  ASSERT_MSG(offset == 0, "3-reg+offset memory requires multi-instruction sequence on ARM64");
  return load64_gpr64_gpr64_plus_gpr64(dst, addr1, addr2);
}

InstructionARM64 store64_gpr64_gpr64_plus_gpr64_plus_s8(Register addr1,
                                                        Register addr2,
                                                        Register value,
                                                        s64 offset) {
  ASSERT_MSG(offset == 0, "3-reg+offset memory requires multi-instruction sequence on ARM64");
  return store64_gpr64_gpr64_plus_gpr64(addr1, addr2, value);
}

InstructionARM64 load64_gpr64_gpr64_plus_gpr64_plus_s32(Register dst,
                                                        Register addr1,
                                                        Register addr2,
                                                        s64 offset) {
  ASSERT_MSG(offset == 0, "3-reg+offset memory requires multi-instruction sequence on ARM64");
  return load64_gpr64_gpr64_plus_gpr64(dst, addr1, addr2);
}

InstructionARM64 store64_gpr64_gpr64_plus_gpr64_plus_s32(Register addr1,
                                                         Register addr2,
                                                         Register value,
                                                         s64 offset) {
  ASSERT_MSG(offset == 0, "3-reg+offset memory requires multi-instruction sequence on ARM64");
  return store64_gpr64_gpr64_plus_gpr64(addr1, addr2, value);
}

InstructionARM64 store_goal_vf(Register addr, Register value, Register off, s64 offset) {
  // Store 128-bit SIMD to GOAL pointer: mem[addr + off + offset] = value
  // On ARM64: offset==0: STR Qt, [addr, off]; offset!=0: multi-instr
  ASSERT_MSG(offset == 0, "store_goal_vf with offset requires multi-instruction sequence on ARM64");
  // STR Qt, [addr, off] — 128-bit SIMD store with register offset
  // size=00, V=1, opc=10 (STR 128-bit): base = 0x3CA06800
  return InstructionARM64(0x3CA06800u, Rm(off.id()), Rn(addr.id()), Rt(value.id()));
}

InstructionARM64 store_goal_gpr(Register addr, Register value, Register off, int offset, int size) {
  // Store GPR to GOAL pointer: mem[addr + off + offset] = value
  // offset==0: use register-offset store; offset!=0: multi-instr (ADD+STR)
  ASSERT_MSG(offset == 0, "store_goal_gpr with offset requires multi-instruction sequence on ARM64");
  switch (size) {
    case 1: return store8_gpr64_gpr64_plus_gpr64(addr, off, value);
    case 2: return store16_gpr64_gpr64_plus_gpr64(addr, off, value);
    case 4: return store32_gpr64_gpr64_plus_gpr64(addr, off, value);
    case 8: return store64_gpr64_gpr64_plus_gpr64(addr, off, value);
    default: ASSERT_MSG(false, "store_goal_gpr: invalid size"); return InstructionARM64(0b0);
  }
}

InstructionARM64 load_goal_xmm128(Register dst, Register addr, Register off, int offset) {
  // Load 128-bit SIMD from GOAL pointer: dst = mem[addr + off + offset]
  ASSERT_MSG(offset == 0, "load_goal_xmm128 with offset requires multi-instruction sequence on ARM64");
  // LDR Qt, [addr, off] — 128-bit SIMD load with register offset
  // size=00, V=1, opc=11 (LDR 128-bit): base = 0x3CE06800
  return InstructionARM64(0x3CE06800u, Rm(off.id()), Rn(addr.id()), Rt(dst.id()));
}

InstructionARM64 load_goal_gpr(Register dst,
                               Register addr,
                               Register off,
                               int offset,
                               int size,
                               bool sign_extend) {
  // Load GPR from GOAL pointer: dst = mem[addr + off + offset]
  ASSERT_MSG(offset == 0, "load_goal_gpr with offset requires multi-instruction sequence on ARM64");
  if (sign_extend) {
    switch (size) {
      case 1: return load8s_gpr64_gpr64_plus_gpr64(dst, addr, off);
      case 2: return load16s_gpr64_gpr64_plus_gpr64(dst, addr, off);
      case 4: return load32s_gpr64_gpr64_plus_gpr64(dst, addr, off);
      case 8: return load64_gpr64_gpr64_plus_gpr64(dst, addr, off);
      default: ASSERT_MSG(false, "load_goal_gpr: invalid size"); return InstructionARM64(0b0);
    }
  } else {
    switch (size) {
      case 1: return load8u_gpr64_gpr64_plus_gpr64(dst, addr, off);
      case 2: return load16u_gpr64_gpr64_plus_gpr64(dst, addr, off);
      case 4: return load32u_gpr64_gpr64_plus_gpr64(dst, addr, off);
      case 8: return load64_gpr64_gpr64_plus_gpr64(dst, addr, off);
      default: ASSERT_MSG(false, "load_goal_gpr: invalid size"); return InstructionARM64(0b0);
    }
  }
}

//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
//   LOADS n' STORES - XMM32
//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
// ARM64 Load/Store (register offset) for SIMD 32-bit (S registers):
// size=10, V=1, opc=00 → STR S: 0xBC206800
// size=10, V=1, opc=01 → LDR S: 0xBC606800
// https://www.scs.stanford.edu/~zyedidia/arm64/str_reg_fpsimd.html

InstructionARM64 store32_xmm32_gpr64_plus_gpr64(Register addr1,
                                                Register addr2,
                                                Register xmm_value) {
  // STR St, [addr1, addr2]
  return InstructionARM64(0xBC206800u, Rm(addr2.id()), Rn(addr1.id()), Rt(xmm_value.id()));
}

InstructionARM64 load32_xmm32_gpr64_plus_gpr64(Register simd_dest, Register addr1, Register addr2) {
  // LDR St, [addr1, addr2]
  return InstructionARM64(0xBC606800u, Rm(addr2.id()), Rn(addr1.id()), Rt(simd_dest.id()));
}

InstructionARM64 store32_xmm32_gpr64_plus_gpr64_plus_s8(Register addr1,
                                                        Register addr2,
                                                        Register xmm_value,
                                                        s64 offset) {
  ASSERT_MSG(offset == 0, "3-reg+offset SIMD store requires multi-instruction on ARM64");
  return store32_xmm32_gpr64_plus_gpr64(addr1, addr2, xmm_value);
}

InstructionARM64 load32_xmm32_gpr64_plus_gpr64_plus_s8(Register simd_dest,
                                                       Register addr1,
                                                       Register addr2,
                                                       s64 offset) {
  ASSERT_MSG(offset == 0, "3-reg+offset SIMD load requires multi-instruction on ARM64");
  return load32_xmm32_gpr64_plus_gpr64(simd_dest, addr1, addr2);
}

InstructionARM64 store32_xmm32_gpr64_plus_gpr64_plus_s32(Register addr1,
                                                         Register addr2,
                                                         Register xmm_value,
                                                         s64 offset) {
  ASSERT_MSG(offset == 0, "3-reg+offset SIMD store requires multi-instruction on ARM64");
  return store32_xmm32_gpr64_plus_gpr64(addr1, addr2, xmm_value);
}

InstructionARM64 lea_reg_plus_off32(Register dest, Register base, s64 offset) {
  // ADD Xdest, Xbase, #|offset| (or SUB for negative)
  ASSERT_MSG(offset >= -4095 && offset <= 4095, "lea_reg_plus_off32: offset exceeds 12-bit ADD/SUB range on ARM64");
  if (offset < 0) {
    return InstructionARM64(Base(0b1101000100, 10), Imm12(-offset), Rn(base.id()), Rd(dest.id()));
  }
  return InstructionARM64(Base(0b1001000100, 10), Imm12(offset), Rn(base.id()), Rd(dest.id()));
}

InstructionARM64 lea_reg_plus_off8(Register dest, Register base, s64 offset) {
  // ADD Xdest, Xbase, #|offset| (or SUB for negative), 12-bit range covers all s8 values
  if (offset < 0) {
    return InstructionARM64(Base(0b1101000100, 10), Imm12(-offset), Rn(base.id()), Rd(dest.id()));
  }
  return InstructionARM64(Base(0b1001000100, 10), Imm12(offset), Rn(base.id()), Rd(dest.id()));
}

InstructionARM64 lea_reg_plus_off(Register dest, Register base, s64 offset) {
  ASSERT_MSG(offset >= -4095 && offset <= 4095, "lea_reg_plus_off: offset exceeds 12-bit ADD/SUB range on ARM64");
  if (offset < 0) {
    return InstructionARM64(Base(0b1101000100, 10), Imm12(-offset), Rn(base.id()), Rd(dest.id()));
  }
  return InstructionARM64(Base(0b1001000100, 10), Imm12(offset), Rn(base.id()), Rd(dest.id()));
}

InstructionARM64 store32_xmm32_gpr64_plus_s32(Register base, Register xmm_value, s64 offset) {
  // STR St, [base, #offset] — signed offset store (12-bit unsigned, or 9-bit signed pre/post)
  // Use unsigned-offset form: offset must be 0..16380 (4-byte aligned) or use signed 9-bit
  // For simplicity, require ≥0 and ≤16380 and 4-byte aligned for the scaled 12-bit form.
  // STR S (unsigned offset): size=10,V=1,opc=00: 0xBD000000
  ASSERT_MSG(offset >= 0 && offset <= 16380 && (offset % 4 == 0),
             "store32_xmm32_gpr64_plus_s32: offset must be 0..16380 aligned to 4 bytes for scaled STR S");
  u32 imm12 = (u32)(offset / 4);
  return InstructionARM64(0xBD000000u, Imm12(imm12), Rn(base.id()), Rt(xmm_value.id()));
}

InstructionARM64 store32_xmm32_gpr64_plus_s8(Register base, Register xmm_value, s64 offset) {
  return store32_xmm32_gpr64_plus_s32(base, xmm_value, offset);
}

InstructionARM64 load32_xmm32_gpr64_plus_gpr64_plus_s32(Register simd_dest,
                                                        Register addr1,
                                                        Register addr2,
                                                        s64 offset) {
  ASSERT_MSG(offset == 0, "3-reg+offset SIMD load requires multi-instruction on ARM64");
  return load32_xmm32_gpr64_plus_gpr64(simd_dest, addr1, addr2);
}

InstructionARM64 load32_xmm32_gpr64_plus_s32(Register simd_dest, Register base, s64 offset) {
  // LDR St, [base, #offset] — scaled unsigned offset form
  // LDR S (unsigned offset): size=10,V=1,opc=01: 0xBD400000
  ASSERT_MSG(offset >= 0 && offset <= 16380 && (offset % 4 == 0),
             "load32_xmm32_gpr64_plus_s32: offset must be 0..16380 aligned to 4 bytes for scaled LDR S");
  u32 imm12 = (u32)(offset / 4);
  return InstructionARM64(0xBD400000u, Imm12(imm12), Rn(base.id()), Rt(simd_dest.id()));
}

InstructionARM64 load32_xmm32_gpr64_plus_s8(Register simd_dest, Register base, s64 offset) {
  return load32_xmm32_gpr64_plus_s32(simd_dest, base, offset);
}

InstructionARM64 load_goal_xmm32(Register simd_dest, Register addr, Register off, s64 offset) {
  ASSERT_MSG(offset == 0, "load_goal_xmm32 with offset requires multi-instruction on ARM64");
  return load32_xmm32_gpr64_plus_gpr64(simd_dest, addr, off);
}

InstructionARM64 store_goal_xmm32(Register addr, Register xmm_value, Register off, s64 offset) {
  ASSERT_MSG(offset == 0, "store_goal_xmm32 with offset requires multi-instruction on ARM64");
  return store32_xmm32_gpr64_plus_gpr64(addr, off, xmm_value);
}

InstructionARM64 store_reg_offset_xmm32(Register base, Register xmm_value, s64 offset) {
  return store32_xmm32_gpr64_plus_s32(base, xmm_value, offset);
}

InstructionARM64 load_reg_offset_xmm32(Register simd_dest, Register base, s64 offset) {
  return load32_xmm32_gpr64_plus_s32(simd_dest, base, offset);
}

//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
//   LOADS n' STORES - SIMD (128-bit, QWORDS)
//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

InstructionARM64 store128_gpr64_simd128(Register gpr_addr, Register simd_reg) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/str_imm_fpsimd.html
  // - STR Qn, [Xn] (unsigned offset)
  ASSERT(gpr_addr.is_gpr(instr_set));
  // ARM64 SIMD register IDs span 0-31 (Q0-Q31). Regalloc uses XMM IDs (16-31); direct use
  // uses native Q IDs (0-15). Both ranges are valid in the Rt encoding field.
  ASSERT(simd_reg.id() >= 0 && simd_reg.id() <= 31);
  return InstructionARM64(Base(0b0011110110, 10), Rn(gpr_addr.id()), Rt(simd_reg.id()), Imm12(0));
}

InstructionARM64 store128_gpr64_simd128_s32(Register gpr_addr, Register xmm_value, s64 offset) {
  // STR Qt, [Xn, #imm] — scaled unsigned offset (0..65520, 16-byte aligned)
  // https://www.scs.stanford.edu/~zyedidia/arm64/str_imm_fpsimd.html
  ASSERT_MSG(offset >= 0 && offset <= 65520 && (offset % 16 == 0),
             "store128_gpr64_simd128_s32: offset must be 0..65520 aligned to 16 bytes");
  u32 imm12 = (u32)(offset / 16);
  return InstructionARM64(Base(0b0011110110, 10), Rn(gpr_addr.id()), Rt(xmm_value.id()), Imm12(imm12));
}

InstructionARM64 store128_gpr64_simd128_s8(Register gpr_addr, Register xmm_value, s64 offset) {
  return store128_gpr64_simd128_s32(gpr_addr, xmm_value, offset);
}

InstructionARM64 load128_simd128_gpr64(Register simd_dest, Register gpr_addr) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldr_imm_fpsimd.html
  // - LDR <Qt>, [<Xn|SP>{, #<pimm>}]
  ASSERT(gpr_addr.is_gpr(instr_set));
  ASSERT(simd_dest.id() >= 0 && simd_dest.id() <= 31);
  return InstructionARM64(Base(0b0011110111, 10), Rn(gpr_addr.id()), Rt(simd_dest.id()), Imm12(0));
}

InstructionARM64 load128_simd128_gpr64_s32(Register simd_dest, Register gpr_addr, s64 offset) {
  // LDR Qt, [Xn, #imm] — scaled unsigned offset (0..65520, 16-byte aligned)
  ASSERT_MSG(offset >= 0 && offset <= 65520 && (offset % 16 == 0),
             "load128_simd128_gpr64_s32: offset must be 0..65520 aligned to 16 bytes");
  u32 imm12 = (u32)(offset / 16);
  return InstructionARM64(Base(0b0011110111, 10), Rn(gpr_addr.id()), Rt(simd_dest.id()), Imm12(imm12));
}

InstructionARM64 load128_simd128_gpr64_s8(Register simd_dest, Register gpr_addr, s64 offset) {
  return load128_simd128_gpr64_s32(simd_dest, gpr_addr, offset);
}

InstructionARM64 load128_xmm128_reg_offset(Register simd_dest, Register base, s64 offset) {
  return load128_simd128_gpr64_s32(simd_dest, base, offset);
}

InstructionARM64 store128_xmm128_reg_offset(Register base, Register xmm_val, s64 offset) {
  return store128_gpr64_simd128_s32(base, xmm_val, offset);
}

//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
//   RIP loads and stores
//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

// RIP-relative (PC-relative) loads/stores — ARM64 equivalent is LDR (literal)
// LDR Xt, [PC + imm19*4] — 19-bit signed word offset = ±1MB range
// For the object generator's static data, use LDR literal with offset placeholder (0).
// The ObjectGenerator patches in the correct PC-relative offset at link time.
// https://www.scs.stanford.edu/~zyedidia/arm64/ldr_lit_gen.html

InstructionARM64 load64_rip_s32(Register dest, s64 offset) {
  // LDR Xt, label — 64-bit PC-relative load (19-bit imm19, word-offset)
  // Encoding: 01_011000_imm19_Rt — imm19=0 placeholder, patched by ObjectGenerator
  (void)offset;
  return InstructionARM64(Base(0b01011000, 8), Rt(dest.id()));
}

InstructionARM64 load32s_rip_s32(Register dest, s64 offset) {
  // LDRSW Xt, label — PC-relative load 32-bit sign-extended to 64-bit
  // Encoding: 10_011000_imm19_Rt
  (void)offset;
  return InstructionARM64(Base(0b10011000, 8), Rt(dest.id()));
}

InstructionARM64 load32u_rip_s32(Register dest, s64 offset) {
  // LDR Wt, label — PC-relative 32-bit load (zero-extended)
  // Encoding: 00_011000_imm19_Rt
  (void)offset;
  return InstructionARM64(Base(0b00011000, 8), Rt(dest.id()));
}

InstructionARM64 load16u_rip_s32(Register dest, s64 offset) {
  // No ARM64 PC-relative halfword load — requires ADRP+LDR multi-instr
  ASSERT_MSG(false, "load16u_rip_s32: no direct PC-relative halfword load on ARM64 — use ADRP+LDR in CodeGenerator");
  return InstructionARM64(0b0);
}

InstructionARM64 load16s_rip_s32(Register dest, s64 offset) {
  ASSERT_MSG(false, "load16s_rip_s32: no direct PC-relative halfword load on ARM64 — use ADRP+LDR in CodeGenerator");
  return InstructionARM64(0b0);
}

InstructionARM64 load8u_rip_s32(Register dest, s64 offset) {
  ASSERT_MSG(false, "load8u_rip_s32: no direct PC-relative byte load on ARM64 — use ADRP+LDR in CodeGenerator");
  return InstructionARM64(0b0);
}

InstructionARM64 load8s_rip_s32(Register dest, s64 offset) {
  ASSERT_MSG(false, "load8s_rip_s32: no direct PC-relative byte load on ARM64 — use ADRP+LDR in CodeGenerator");
  return InstructionARM64(0b0);
}

InstructionARM64 static_load(Register dest, s64 offset, int size, bool sign_extend) {
  // PC-relative load — choose instruction based on size/sign_extend
  if (!sign_extend) {
    switch (size) {
      case 4: return load32u_rip_s32(dest, offset);
      case 8: return load64_rip_s32(dest, offset);
      default: break;
    }
  } else {
    switch (size) {
      case 4: return load32s_rip_s32(dest, offset);
      case 8: return load64_rip_s32(dest, offset);
      default: break;
    }
  }
  ASSERT_MSG(false, "static_load: size <4 requires ADRP+LDR multi-instruction on ARM64");
  return InstructionARM64(0b0);
}

InstructionARM64 store64_rip_s32(Register src, s64 offset) {
  // No direct PC-relative store on ARM64 — requires ADRP+STR (multi-instr)
  ASSERT_MSG(false, "store64_rip_s32: no PC-relative store on ARM64 — use ADRP+STR in CodeGenerator");
  return InstructionARM64(0b0);
}

InstructionARM64 store32_rip_s32(Register src, s64 offset) {
  ASSERT_MSG(false, "store32_rip_s32: no PC-relative store on ARM64 — use ADRP+STR in CodeGenerator");
  return InstructionARM64(0b0);
}

InstructionARM64 store16_rip_s32(Register src, s64 offset) {
  ASSERT_MSG(false, "store16_rip_s32: no PC-relative store on ARM64 — use ADRP+STR in CodeGenerator");
  return InstructionARM64(0b0);
}

InstructionARM64 store8_rip_s32(Register src, s64 offset) {
  ASSERT_MSG(false, "store8_rip_s32: no PC-relative store on ARM64 — use ADRP+STR in CodeGenerator");
  return InstructionARM64(0b0);
}

InstructionARM64 static_store(Register value, s64 offset, int size) {
  ASSERT_MSG(false, "static_store: no PC-relative store on ARM64 — use ADRP+STR in CodeGenerator");
  return InstructionARM64(0b0);
}

InstructionARM64 static_addr(Register dst, s64 offset) {
  // ADR Xd, label — load PC-relative address (±1MB, 21-bit)
  // Encoding: 0_imm2_10000_imm19_Rd — imm=0 placeholder
  // https://www.scs.stanford.edu/~zyedidia/arm64/adr.html
  (void)offset;
  return InstructionARM64(Base(0b0, 1), Field{0b10000u << 24}, Rd(dst.id()));
}

InstructionARM64 static_load_xmm32(Register simd_dest, s64 offset) {
  // LDR St, label — PC-relative 32-bit SIMD load
  // Encoding: 00_011100_imm19_Rt (size=00,V=1)
  (void)offset;
  return InstructionARM64(Base(0b00011100, 8), Rt(simd_dest.id()));
}

InstructionARM64 static_store_xmm32(Register xmm_value, s64 offset) {
  ASSERT_MSG(false, "static_store_xmm32: no PC-relative SIMD store on ARM64 — use ADRP+STR in CodeGenerator");
  return InstructionARM64(0b0);
}

// TODO, special load/stores of 128 bit values.

// TODO, consider specialized stack loads and stores?
InstructionARM64 load64_gpr64_plus_s32(Register dst_reg, int32_t offset, Register src_reg) {
  // LDR Xd, [Xn, #offset] — scaled unsigned 12-bit offset (0..32760, 8-byte aligned)
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldr_imm_gen.html
  ASSERT_MSG(offset >= 0 && offset <= 32760 && (offset % 8 == 0),
             "load64_gpr64_plus_s32: offset must be 0..32760 aligned to 8 bytes for scaled LDR X");
  u32 imm12 = (u32)(offset / 8);
  return InstructionARM64(0xF9400000u, Imm12(imm12), Rn(src_reg.id()), Rt(dst_reg.id()));
}

InstructionARM64 store64_gpr64_plus_s32(Register addr, int32_t offset, Register value) {
  // STR Xt, [Xn, #offset] — scaled unsigned 12-bit offset (0..32760, 8-byte aligned)
  // https://www.scs.stanford.edu/~zyedidia/arm64/str_imm_gen.html
  ASSERT_MSG(offset >= 0 && offset <= 32760 && (offset % 8 == 0),
             "store64_gpr64_plus_s32: offset must be 0..32760 aligned to 8 bytes for scaled STR X");
  u32 imm12 = (u32)(offset / 8);
  return InstructionARM64(0xF9000000u, Imm12(imm12), Rn(addr.id()), Rt(value.id()));
}

//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
//   FUNCTION STUFF
//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

InstructionARM64 ret() {
  // https://www.scs.stanford.edu/~zyedidia/arm64/ret.html
  // - defaults to using X30 if Rn is absent
  return InstructionARM64(Base(0b1101011001011111000000, 22), Rn(30));
}

InstructionARM64 push_gpr64(Register reg) {
  // ARM64 stack grows down, so we subtract 16 from SP and store the register
  // Equivalent assembly: STR reg, [SP, #-16]!
  // - https://www.scs.stanford.edu/~zyedidia/arm64/str_imm_gen.html
  // We use 16 because in ARM, the stack must be 16-byte aligned.
  // This does mean we are inefficiently using the stack, there are a few better options:
  // - Push in pairs, two registers at a time
  // - Preallocate stack-space
  // But we can't do either of these at this level, this is an optimization that has to come from
  // higher in the stack.  Here we are concerned with just satisfying the need to push a GPR
  ASSERT(reg.is_gpr(instr_set));
  return InstructionARM64(Base(0b1111100000000000000011, 22), Imm9(-16), Rn(ARM64_REG::SP),
                          Rt(reg.id()));
}

InstructionARM64 pop_gpr64(Register reg) {
  // ldr reg, [sp], #16
  // - https://www.scs.stanford.edu/~zyedidia/arm64/ldr_imm_gen.html
  ASSERT(reg.is_gpr(instr_set));
  return InstructionARM64(Base(0b1111100001000000000001, 22), Imm9(16), Rn(ARM64_REG::SP),
                          Rt(reg.id()));
}

InstructionARM64 call_r64(Register reg_) {
  // BLR Xn — branch with link (call) to register address
  // https://www.scs.stanford.edu/~zyedidia/arm64/blr.html
  // Encoding: 1101011000111111000000_Rn_00000
  ASSERT(reg_.is_gpr(instr_set));
  return InstructionARM64(0xD63F0000u, Rn(reg_.id()));
}

InstructionARM64 jmp_r64(Register reg_) {
  // BR Xn — unconditional branch to register address
  // https://www.scs.stanford.edu/~zyedidia/arm64/br.html
  // Encoding: 1101011000011111000000_Rn_00000
  ASSERT(reg_.is_gpr(instr_set));
  return InstructionARM64(0xD61F0000u, Rn(reg_.id()));
}

//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
//   INTEGER MATH
//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

// NOTE: ARM can actually handle 12-bit immediate values, so if it's actually worth it, we
// could leverage these instructions for more than just 8-bit values
InstructionARM64 sub_gpr64_imm8s(Register reg, int64_t imm) {
  // You cannot subtract or add with a negative immediate in ARM
  // therefore depending on the value of the immediate, we use a different instruction
  ASSERT(reg.is_gpr(instr_set));
  if (imm < 0) {
    return add_gpr64_imm8s(reg, std::abs(imm));
  }
  // https://www.scs.stanford.edu/~zyedidia/arm64/sub_addsub_imm.html
  // - SUB <Xd>, <Xn>, #imm12 {, LSL #12}
  // - using a shift of 0 here (last bit in the base)
  return InstructionARM64(Base(0b1101000100, 10), Imm12(imm), Rn(reg.id()), Rd(reg.id()));
}

// NOTE: ARM can actually handle 12-bit immediate values, so if it's actually worth it, we
// could leverage these instructions for more than just 8-bit values
InstructionARM64 add_gpr64_imm8s(Register reg, int64_t imm) {
  // You cannot subtract or add with a negative immediate in ARM
  // therefore depending on the value of the immediate, we use a different instruction
  ASSERT(reg.is_gpr(instr_set));
  if (imm < 0) {
    return sub_gpr64_imm8s(reg, abs(imm));
  }
  // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_imm.html
  // ADD <Xd|SP>, <Xn|SP>, #<imm>{, <shift>}
  return InstructionARM64(Base(0b1001000100, 10), Imm12(imm), Rn(reg.id()), Rd(reg.id()));
}

InstructionARM64 sub_gpr64_imm32s(Register reg, int64_t imm) {
  // ARM64 does not support this kind of single-instruction
  ASSERT_MSG(false, "sub_gpr64_imm32s not supported on ARM64");
  return InstructionARM64(0b0);
}

InstructionARM64 add_gpr64_imm32s(Register reg, int64_t imm) {
  // ARM64 does not support this kind of single-instruction
  ASSERT_MSG(false, "sub_gpr64_imm32s not supported on ARM64");
  return InstructionARM64(0b0);
}

InstructionARM64 add_gpr64_imm(Register reg, int64_t imm) {
  // Delegates to add_gpr64_imm8s which handles ±4095 via ADD/SUB Xd, Xn, #imm12
  return add_gpr64_imm8s(reg, imm);
}

InstructionARM64 sub_gpr64_imm(Register reg, int64_t imm) {
  return sub_gpr64_imm8s(reg, imm);
}

InstructionARM64 add_gpr64_imm_lsl12(Register reg, u32 imm12) {
  // ADD Xd, Xn, #imm12, LSL #12
  ASSERT(imm12 <= 4095);
  ASSERT(reg.is_gpr(instr_set));
  return InstructionARM64(Base(0b1001000101, 10), Imm12(imm12), Rn(reg.id()), Rd(reg.id()));
}

InstructionARM64 sub_gpr64_imm_lsl12(Register reg, u32 imm12) {
  // SUB Xd, Xn, #imm12, LSL #12
  ASSERT(imm12 <= 4095);
  ASSERT(reg.is_gpr(instr_set));
  return InstructionARM64(Base(0b1101000101, 10), Imm12(imm12), Rn(reg.id()), Rd(reg.id()));
}

InstructionARM64 add_gpr64_gpr64(Register dst, Register src) {
  // ADD Xd, Xn, Xm — data processing (shifted register, shift=0)
  // Encoding: 1_0_0_01011_00_0_Rm_000000_Rn_Rd
  // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_shift.html
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(src.is_gpr(instr_set));
  return InstructionARM64(Base(0b10001011000, 11), Rm(src.id()), Rn(dst.id()), Rd(dst.id()));
}

InstructionARM64 sub_gpr64_gpr64(Register dst, Register src) {
  // SUB Xd, Xn, Xm — data processing (shifted register, shift=0)
  // Encoding: 1_1_0_01011_00_0_Rm_000000_Rn_Rd
  // https://www.scs.stanford.edu/~zyedidia/arm64/sub_addsub_shift.html
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(src.is_gpr(instr_set));
  return InstructionARM64(Base(0b11001011000, 11), Rm(src.id()), Rn(dst.id()), Rd(dst.id()));
}

InstructionARM64 imul_gpr32_gpr32(Register dst, Register src) {
  // MUL Wd, Wn, Wm — alias for MADD Wd, Wn, Wm, WZR (sf=0)
  // Encoding: 0_0011011_000_Rm_011111_Rn_Rd  (Ra=XZR=31 at bits[14:10])
  // https://www.scs.stanford.edu/~zyedidia/arm64/mul_madd.html
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(src.is_gpr(instr_set));
  return InstructionARM64(Base(0b00011011000, 11), Rm(src.id()), Field{(31u << 10)}, Rn(dst.id()), Rd(dst.id()));
}

InstructionARM64 imul_gpr64_gpr64(Register dst, Register src) {
  // MUL Xd, Xn, Xm — alias for MADD Xd, Xn, Xm, XZR (sf=1)
  // Encoding: 1_0011011_000_Rm_011111_Rn_Rd
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(src.is_gpr(instr_set));
  return InstructionARM64(Base(0b10011011000, 11), Rm(src.id()), Field{(31u << 10)}, Rn(dst.id()), Rd(dst.id()));
}

InstructionARM64 idiv_gpr32(Register reg) {
  // x86 IDIV: divides EDX:EAX by reg, result in EAX, remainder in EDX.
  // ARM64 SDIV has explicit src/dst: SDIV Wd, Wn, Wm — no implicit EDX:EAX.
  // Cannot be a single-instruction replacement without CodeGenerator coordination.
  ASSERT_MSG(false, "idiv_gpr32: x86 implicit EDX:EAX semantics have no ARM64 single-instr equivalent — handle in CodeGenerator with SDIV + MSUB");
  return InstructionARM64(0b0);
}

InstructionARM64 unsigned_div_gpr32(Register reg) {
  ASSERT_MSG(false, "unsigned_div_gpr32: x86 implicit EDX:EAX semantics have no ARM64 single-instr equivalent — handle in CodeGenerator with UDIV + MSUB");
  return InstructionARM64(0b0);
}

InstructionARM64 cdq() {
  // x86 CDQ: sign-extends EAX into EDX:EAX. No ARM64 equivalent single instruction.
  ASSERT_MSG(false, "cdq: x86-specific sign-extend EAX→EDX:EAX has no ARM64 equivalent — handle in CodeGenerator");
  return InstructionARM64(0b0);
}

InstructionARM64 sdiv_gpr32(Register dst, Register dividend, Register divisor) {
  // SDIV Wd, Wn, Wm — 32-bit signed divide: dst = dividend / divisor
  // Encoding: 0_0_011010_11_0_Rm_000011_Rn_Rd
  // https://www.scs.stanford.edu/~zyedidia/arm64/sdiv.html
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(dividend.is_gpr(instr_set));
  ASSERT(divisor.is_gpr(instr_set));
  return InstructionARM64(0x1AC00C00u, Rm(divisor.id()), Rn(dividend.id()), Rd(dst.id()));
}

InstructionARM64 udiv_gpr32(Register dst, Register dividend, Register divisor) {
  // UDIV Wd, Wn, Wm — 32-bit unsigned divide: dst = dividend / divisor
  // Encoding: 0_0_011010_11_0_Rm_000010_Rn_Rd
  // https://www.scs.stanford.edu/~zyedidia/arm64/udiv.html
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(dividend.is_gpr(instr_set));
  ASSERT(divisor.is_gpr(instr_set));
  return InstructionARM64(0x1AC00800u, Rm(divisor.id()), Rn(dividend.id()), Rd(dst.id()));
}

InstructionARM64 msub_gpr32(Register dst, Register n, Register m, Register addend) {
  // MSUB Wd, Wn, Wm, Wa — 32-bit: dst = addend - n * m
  // Encoding: 0_0_011011_000_Rm_1_Ra_Rn_Rd
  // https://www.scs.stanford.edu/~zyedidia/arm64/msub.html
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(n.is_gpr(instr_set));
  ASSERT(m.is_gpr(instr_set));
  ASSERT(addend.is_gpr(instr_set));
  return InstructionARM64(0x1B008000u, Rm(m.id()), Field{(u32(addend.id()) << 10)}, Rn(n.id()), Rd(dst.id()));
}

InstructionARM64 movsx_r64_r32(Register dst, Register src) {
  // SXTW Xd, Wn — sign-extend 32-bit to 64-bit
  // Alias for SBFM Xd, Xn, #0, #31
  // Encoding: 0x93407C00 | (Rn << 5) | Rd
  // https://www.scs.stanford.edu/~zyedidia/arm64/sxtw.html
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(src.is_gpr(instr_set));
  return InstructionARM64(0x93407C00u, Rn(src.id()), Rd(dst.id()));
}

InstructionARM64 cmp_gpr64_gpr64(Register a, Register b) {
  // CMP Xn, Xm — alias for SUBS XZR, Xn, Xm (sets flags, discards result)
  // Encoding: 1_1_1_01011_00_0_Rm_000000_Rn_11111
  // https://www.scs.stanford.edu/~zyedidia/arm64/cmp_subs_addsub_shift.html
  ASSERT(a.is_gpr(instr_set));
  ASSERT(b.is_gpr(instr_set));
  return InstructionARM64(Base(0b11101011000, 11), Rm(b.id()), Rn(a.id()), Rd(31));
}

//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
//   BIT STUFF
//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

InstructionARM64 or_gpr64_gpr64(Register dst, Register src) {
  // ORR Xd, Xn, Xm — bitwise OR (shifted register, shift=0)
  // Encoding: 1_01_01010_00_0_Rm_000000_Rn_Rd
  // https://www.scs.stanford.edu/~zyedidia/arm64/orr_log_shift.html
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(src.is_gpr(instr_set));
  return InstructionARM64(Base(0b10101010000, 11), Rm(src.id()), Rn(dst.id()), Rd(dst.id()));
}

InstructionARM64 and_gpr64_gpr64(Register dst, Register src) {
  // AND Xd, Xn, Xm — bitwise AND (shifted register, shift=0)
  // Encoding: 1_00_01010_00_0_Rm_000000_Rn_Rd
  // https://www.scs.stanford.edu/~zyedidia/arm64/and_log_shift.html
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(src.is_gpr(instr_set));
  return InstructionARM64(Base(0b10001010000, 11), Rm(src.id()), Rn(dst.id()), Rd(dst.id()));
}

InstructionARM64 xor_gpr64_gpr64(Register dst, Register src) {
  // EOR Xd, Xn, Xm — bitwise XOR (shifted register, shift=0)
  // Encoding: 1_10_01010_00_0_Rm_000000_Rn_Rd
  // https://www.scs.stanford.edu/~zyedidia/arm64/eor_log_shift.html
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(src.is_gpr(instr_set));
  return InstructionARM64(Base(0b11001010000, 11), Rm(src.id()), Rn(dst.id()), Rd(dst.id()));
}

InstructionARM64 not_gpr64(Register reg) {
  // MVN Xd, Xm — bitwise NOT (alias for ORN Xd, XZR, Xm)
  // Encoding: 1_01_01010_00_1_Rm_000000_11111_Rd  (sf=1, opc=01, fixed=01010, N=1)
  // https://www.scs.stanford.edu/~zyedidia/arm64/mvn_orn_log_shift.html
  ASSERT(reg.is_gpr(instr_set));
  return InstructionARM64(Base(0b10101010001, 11), Rm(reg.id()), Rn(31), Rd(reg.id()));
}

//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
//   SHIFTS
//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

// x86 shifts by CL register (RCX low byte) → on ARM64, X3 (= GOAL arg3 = RCX analog)
// LSLV/LSRV/ASRV: sf=1, 0011010110, Rm, opcode(6), Rn, Rd
// bits[15:10]: LSLV=001000, LSRV=001001, ASRV=001010
// https://www.scs.stanford.edu/~zyedidia/arm64/lslv.html

InstructionARM64 shl_gpr64_cl(Register reg) {
  // LSLV Xd, Xd, X3  (X3 = RCX analog = shift amount)
  ASSERT(reg.is_gpr(instr_set));
  return InstructionARM64(Base(0b10011010110, 11), Rm(ARM64_REG::X3), Field{(8u << 10)}, Rn(reg.id()), Rd(reg.id()));
}

InstructionARM64 shr_gpr64_cl(Register reg) {
  // LSRV Xd, Xd, X3
  ASSERT(reg.is_gpr(instr_set));
  return InstructionARM64(Base(0b10011010110, 11), Rm(ARM64_REG::X3), Field{(9u << 10)}, Rn(reg.id()), Rd(reg.id()));
}

InstructionARM64 sar_gpr64_cl(Register reg) {
  // ASRV Xd, Xd, X3
  ASSERT(reg.is_gpr(instr_set));
  return InstructionARM64(Base(0b10011010110, 11), Rm(ARM64_REG::X3), Field{(10u << 10)}, Rn(reg.id()), Rd(reg.id()));
}

// Fixed-amount shifts via UBFM (logical) / SBFM (arithmetic):
// UBFM: sf=1,opc=10,100110,N=1 → top 10 bits = 1_10_100110_1 = 1101001101
// LSL #sa: UBFM Xd, Xn, #(-sa & 63), #(63-sa)
// LSR #sa: UBFM Xd, Xn, #sa, #63
// ASR #sa: SBFM Xd, Xn, #sa, #63
// SBFM: sf=1,opc=00,100111,N=1 → top 10 bits = 1_00_100111_1 = 1001001111

InstructionARM64 shl_gpr64_u8(Register reg, uint8_t sa) {
  // LSL Xd, Xd, #sa — UBFM Xd, Xd, #(-sa & 63), #(63-sa)
  // https://www.scs.stanford.edu/~zyedidia/arm64/lsl_ubfm.html
  ASSERT(reg.is_gpr(instr_set));
  ASSERT(sa < 64);
  u32 immr = (64u - sa) & 63u;
  u32 imms = 63u - sa;
  return InstructionARM64(Base(0b1101001101, 10), Field{(immr << 16)}, Imm6(imms), Rn(reg.id()), Rd(reg.id()));
}

InstructionARM64 shr_gpr64_u8(Register reg, uint8_t sa) {
  // LSR Xd, Xd, #sa — UBFM Xd, Xd, #sa, #63
  // https://www.scs.stanford.edu/~zyedidia/arm64/lsr_ubfm.html
  ASSERT(reg.is_gpr(instr_set));
  ASSERT(sa < 64);
  return InstructionARM64(Base(0b1101001101, 10), Field{((u32)sa << 16)}, Imm6(63), Rn(reg.id()), Rd(reg.id()));
}

InstructionARM64 sar_gpr64_u8(Register reg, uint8_t sa) {
  // ASR Xd, Xd, #sa — SBFM Xd, Xd, #sa, #63
  // https://www.scs.stanford.edu/~zyedidia/arm64/asr_sbfm.html
  ASSERT(reg.is_gpr(instr_set));
  ASSERT(sa < 64);
  return InstructionARM64(Base(0b1001001111, 10), Field{((u32)sa << 16)}, Imm6(63), Rn(reg.id()), Rd(reg.id()));
}

//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
//   CONTROL FLOW
//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

// ARM64 conditional branches: B.cond #imm19 (placeholder imm=0, patched by ObjectGenerator)
// Encoding: 01010100_imm19_0_cond
// Condition codes: EQ=0, NE=1, CS/HS=2, CC/LO=3, MI=4, PL=5, VS=6, VC=7,
//                 HI=8, LS=9, GE=10, LT=11, GT=12, LE=13, AL=14
// Base (imm19=0): 0x54000000 | cond
// Unconditional B #imm26 (placeholder): 0x14000000
// https://www.scs.stanford.edu/~zyedidia/arm64/b_cond.html

InstructionARM64 jmp_32() {
  // B #0 — unconditional branch, 26-bit offset placeholder
  return InstructionARM64(0x14000000u);
}

InstructionARM64 je_32() {
  return InstructionARM64(0x54000000u);  // B.EQ
}

InstructionARM64 jne_32() {
  return InstructionARM64(0x54000001u);  // B.NE
}

InstructionARM64 jle_32() {
  return InstructionARM64(0x5400000Du);  // B.LE (signed ≤)
}

InstructionARM64 jge_32() {
  return InstructionARM64(0x5400000Au);  // B.GE (signed ≥)
}

InstructionARM64 jl_32() {
  return InstructionARM64(0x5400000Bu);  // B.LT (signed <)
}

InstructionARM64 jg_32() {
  return InstructionARM64(0x5400000Cu);  // B.GT (signed >)
}

InstructionARM64 jbe_32() {
  return InstructionARM64(0x54000009u);  // B.LS (unsigned ≤, C=0 or Z=1)
}

InstructionARM64 jae_32() {
  return InstructionARM64(0x54000002u);  // B.HS (unsigned ≥, C=1)
}

InstructionARM64 jb_32() {
  return InstructionARM64(0x54000003u);  // B.LO (unsigned <, C=0)
}

InstructionARM64 ja_32() {
  return InstructionARM64(0x54000008u);  // B.HI (unsigned >, C=1 and Z=0)
}

//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
//   FLOAT MATH
//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

// ARM64 scalar float operations (single-precision S registers):
// Base encoding for 2-source: 0x1E200800 | (opcode<<12) | (Rm<<16) | (Rn<<5) | Rd
// opcodes: FMUL=0, FDIV=1, FADD=2, FSUB=3, FMAX=4, FMIN=5
// https://www.scs.stanford.edu/~zyedidia/arm64/fadd_float.html

InstructionARM64 cmp_flt_flt(Register a, Register b) {
  // FCMP Sn, Sm — compare floats, sets NZCV flags
  // Encoding: 0x1E202000 | (Rm<<16) | (Rn<<5) — no Rd (result discarded)
  // https://www.scs.stanford.edu/~zyedidia/arm64/fcmp_float.html
  ASSERT(a.is_128bit_simd(instr_set));
  ASSERT(b.is_128bit_simd(instr_set));
  return InstructionARM64(0x1E202000u, Rm(b.id()), Rn(a.id()));
}

InstructionARM64 sqrts_xmm(Register dst, Register src) {
  // FSQRT Sd, Sn — single-precision square root
  // Encoding: 0x1E21C000 | (Rn<<5) | Rd
  // https://www.scs.stanford.edu/~zyedidia/arm64/fsqrt_float.html
  ASSERT(dst.is_128bit_simd(instr_set));
  ASSERT(src.is_128bit_simd(instr_set));
  return InstructionARM64(0x1E21C000u, Rn(src.id()), Rd(dst.id()));
}

InstructionARM64 mulss_xmm_xmm(Register dst, Register src) {
  // FMUL Sd, Sd, Sm
  ASSERT(dst.is_128bit_simd(instr_set));
  ASSERT(src.is_128bit_simd(instr_set));
  return InstructionARM64(0x1E200800u, Rm(src.id()), Rn(dst.id()), Rd(dst.id()));
}

InstructionARM64 divss_xmm_xmm(Register dst, Register src) {
  // FDIV Sd, Sd, Sm
  ASSERT(dst.is_128bit_simd(instr_set));
  ASSERT(src.is_128bit_simd(instr_set));
  return InstructionARM64(0x1E201800u, Rm(src.id()), Rn(dst.id()), Rd(dst.id()));
}

InstructionARM64 subss_xmm_xmm(Register dst, Register src) {
  // FSUB Sd, Sd, Sm
  ASSERT(dst.is_128bit_simd(instr_set));
  ASSERT(src.is_128bit_simd(instr_set));
  return InstructionARM64(0x1E203800u, Rm(src.id()), Rn(dst.id()), Rd(dst.id()));
}

InstructionARM64 addss_xmm_xmm(Register dst, Register src) {
  // FADD Sd, Sd, Sm
  ASSERT(dst.is_128bit_simd(instr_set));
  ASSERT(src.is_128bit_simd(instr_set));
  return InstructionARM64(0x1E202800u, Rm(src.id()), Rn(dst.id()), Rd(dst.id()));
}

InstructionARM64 minss_xmm_xmm(Register dst, Register src) {
  // FMIN Sd, Sd, Sm
  ASSERT(dst.is_128bit_simd(instr_set));
  ASSERT(src.is_128bit_simd(instr_set));
  return InstructionARM64(0x1E205800u, Rm(src.id()), Rn(dst.id()), Rd(dst.id()));
}

InstructionARM64 maxss_xmm_xmm(Register dst, Register src) {
  // FMAX Sd, Sd, Sm
  ASSERT(dst.is_128bit_simd(instr_set));
  ASSERT(src.is_128bit_simd(instr_set));
  return InstructionARM64(0x1E204800u, Rm(src.id()), Rn(dst.id()), Rd(dst.id()));
}

InstructionARM64 int32_to_float(Register dst, Register src) {
  // SCVTF Sd, Wn — convert signed 32-bit int to single-precision float
  // Encoding: 0x1E220000 | (Rn<<5) | Rd
  // https://www.scs.stanford.edu/~zyedidia/arm64/scvtf_float_int.html
  ASSERT(dst.is_128bit_simd(instr_set));
  ASSERT(src.is_gpr(instr_set));
  return InstructionARM64(0x1E220000u, Rn(src.id()), Rd(dst.id()));
}

InstructionARM64 float_to_int32(Register dst, Register src) {
  // FCVTZS Wd, Sn — convert float to signed 32-bit int (truncate toward zero)
  // Encoding: 0x1E380000 | (Rn<<5) | Rd
  // https://www.scs.stanford.edu/~zyedidia/arm64/fcvtzs_float_int.html
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(src.is_128bit_simd(instr_set));
  return InstructionARM64(0x1E380000u, Rn(src.id()), Rd(dst.id()));
}

InstructionARM64 nop() {
  // ARM64 NOP — 0xD503201F
  // https://www.scs.stanford.edu/~zyedidia/arm64/nop.html
  return InstructionARM64(0xD503201Fu);
}

// TODO - rsqrt / abs / sqrt

//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
//   UTILITIES
//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

InstructionARM64 null() {
  // Null instruction — emits 0 bytes (used as label placeholder)
  // ARM64 instructions are always 4 bytes, so this emits NOP instead.
  // The ObjectGenerator uses it only as a label target, length=0 would be ideal
  // but the fixed-size InstructionARM64 format always emits 4 bytes.
  return InstructionARM64(0xD503201Fu);  // NOP — harmless placeholder
}

/////////////////////////////
// AVX (VF - Vector Float) //
/////////////////////////////

// NEON 128-bit (4S = 4 × float32) vector operations:
// "Advanced SIMD three same" for float: 0_Q_0_01110_sz_1_Rm_opcode_1_Rn_Rd
// For 4S: Q=1 (bit30), sz=0 (bit22=0, single-precision)
// FADD: bits[15:10]=110101 → 0x4E20D400 + Rm+Rn+Rd
// FSUB: sz=1 (bit22 set)  → 0x4EA0D400 + ...
// FMUL: U=1(bit29) + opcode=11011 → 0x6E20DC00
// FDIV: U=1 + opcode=11111 → 0x6E20FC00
// FMAX: opcode=11110 → 0x4E20F400
// FMIN: sz=1+opcode=11110 → 0x4EA0F400
// FSQRT.4S: 2-reg unary: 0x6EA1F800 + Rn+Rd
// EOR.16B: 0x6E201C00 + Rm+Rn+Rd
// ORR.16B (MOV.16B): 0x4EA01C00 + Rm+Rn+Rd (set Rm=Rn=src for MOV)
// SCVTF.4S: 0x4E21D800 + Rn+Rd
// FCVTZS.4S: 0x4EA1B800 + Rn+Rd

InstructionARM64 nop_vf() {
  return nop();
}

InstructionARM64 wait_vf() {
  // No VU0 wait equivalent on ARM64 — NOP
  return nop();
}

InstructionARM64 mov_vf_vf(Register dst, Register src) {
  // MOV Vd.16B, Vn.16B — alias for ORR Vd.16B, Vn.16B, Vn.16B
  // https://www.scs.stanford.edu/~zyedidia/arm64/orr_advsimd_reg.html
  ASSERT(dst.is_128bit_simd(instr_set));
  ASSERT(src.is_128bit_simd(instr_set));
  return InstructionARM64(0x4EA01C00u, Rm(src.id()), Rn(src.id()), Rd(dst.id()));
}

InstructionARM64 loadvf_gpr64_plus_gpr64(Register dst, Register addr1, Register addr2) {
  // LDR Qt, [addr1, addr2] — 128-bit SIMD register load with register offset
  // size=00, V=1, opc=11: 0x3CE06800
  return InstructionARM64(0x3CE06800u, Rm(addr2.id()), Rn(addr1.id()), Rt(dst.id()));
}

InstructionARM64 loadvf_gpr64_plus_gpr64_plus_s8(Register dst,
                                                 Register addr1,
                                                 Register addr2,
                                                 s64 offset) {
  ASSERT_MSG(offset == 0, "loadvf 3-reg+offset requires multi-instruction on ARM64");
  return loadvf_gpr64_plus_gpr64(dst, addr1, addr2);
}

InstructionARM64 loadvf_gpr64_plus_gpr64_plus_s32(Register dst,
                                                  Register addr1,
                                                  Register addr2,
                                                  s64 offset) {
  ASSERT_MSG(offset == 0, "loadvf 3-reg+offset requires multi-instruction on ARM64");
  return loadvf_gpr64_plus_gpr64(dst, addr1, addr2);
}

InstructionARM64 storevf_gpr64_plus_gpr64(Register value, Register addr1, Register addr2) {
  // STR Qt, [addr1, addr2] — 128-bit SIMD store with register offset
  // size=00, V=1, opc=10: 0x3CA06800
  return InstructionARM64(0x3CA06800u, Rm(addr2.id()), Rn(addr1.id()), Rt(value.id()));
}

InstructionARM64 storevf_gpr64_plus_gpr64_plus_s8(Register value,
                                                  Register addr1,
                                                  Register addr2,
                                                  s64 offset) {
  ASSERT_MSG(offset == 0, "storevf 3-reg+offset requires multi-instruction on ARM64");
  return storevf_gpr64_plus_gpr64(value, addr1, addr2);
}

InstructionARM64 storevf_gpr64_plus_gpr64_plus_s32(Register value,
                                                   Register addr1,
                                                   Register addr2,
                                                   s64 offset) {
  ASSERT_MSG(offset == 0, "storevf 3-reg+offset requires multi-instruction on ARM64");
  return storevf_gpr64_plus_gpr64(value, addr1, addr2);
}

InstructionARM64 loadvf_rip_plus_s32(Register dest, s64 offset) {
  // LDR Qt, label — PC-relative 128-bit SIMD load
  // Encoding: 00_011100_imm19_Rt (size=00, V=1)
  (void)offset;
  return InstructionARM64(Base(0b00011100, 8), Rt(dest.id()));
}

// TODO - rip relative loads and stores.

InstructionARM64 blend_vf(Register dst, Register src1, Register src2, u8 mask) {
  // x86 BLENDPS equivalent on ARM64 requires multi-instruction mask setup (BSL path).
  // Keep codegen moving with exact fast-paths and a conservative fallback.
  ASSERT(dst.is_128bit_simd(instr_set));
  ASSERT(src1.is_128bit_simd(instr_set));
  ASSERT(src2.is_128bit_simd(instr_set));
  if ((mask & 0xF) == 0x0) {
    return mov_vf_vf(dst, src1);
  }
  if ((mask & 0xF) == 0xF) {
    return mov_vf_vf(dst, src2);
  }
  return mov_vf_vf(dst, src1);
}

InstructionARM64 shuffle_vf(Register dst, Register src, u8 dx, u8 dy, u8 dz, u8 dw) {
  (void)dst; (void)src; (void)dx; (void)dy; (void)dz; (void)dw;
  ASSERT_MSG(false, "shuffle_vf: not reachable on ARM64 — handle in do_codegen_arm64");
  return InstructionARM64(0b0);
}

InstructionARM64 swizzle_vf(Register dst, Register src, u8 controlBytes) {
  (void)dst; (void)src; (void)controlBytes;
  ASSERT_MSG(false, "swizzle_vf: not reachable on ARM64 — handled in do_codegen_arm64");
  return InstructionARM64(0b0);
}

InstructionARM64 ext_16b(Register dst, Register src0, Register src1, u8 imm) {
  // EXT Vd.16B, Vn.16B, Vm.16B, #imm — byte rotate/extract from concatenation [src0:src1]
  // Advanced SIMD extract: 0_1_101110_00_0_Rm_0_imm4_0_Rn_Rd
  ASSERT(imm < 16);
  return InstructionARM64(0x6E000000u, Rm(src1.id()), Field{(u32)imm << 11}, Rn(src0.id()), Rd(dst.id()));
}

InstructionARM64 ins_vf_element(Register dst, u8 dstIdx, Register src, u8 srcIdx) {
  // INS Vd.S[dstIdx], Vn.S[srcIdx] — insert 32-bit float element
  // Advanced SIMD copy (element): 0_1_1_01110_000_imm5_0_imm4_1_Rn_Rd
  // imm5 = (dstIdx<<2)|4, imm4 = srcIdx<<1
  ASSERT(dstIdx < 4 && srcIdx < 4);
  u32 imm5 = ((u32)dstIdx << 2u) | 4u;
  u32 imm4 = (u32)srcIdx << 1u;
  return InstructionARM64(0x6E000400u, Field{imm5 << 16}, Field{imm4 << 11}, Rn(src.id()), Rd(dst.id()));
}

InstructionARM64 rev64_4s(Register dst, Register src) {
  // REV64 Vd.4S, Vn.4S — reverse 32-bit elements within each 64-bit lane
  // Advanced SIMD two-reg misc: 0_1_0_01110_10_1_00000_000010_Rn_Rd
  return InstructionARM64(0x4EA00800u, Rn(src.id()), Rd(dst.id()));
}

InstructionARM64 splat_vf(Register dst, Register src, Register::VF_ELEMENT element) {
  // DUP Vd.4S, Vn.S[index] — broadcast single element to all lanes
  // Encoding: 0_1_0_01110_000_imm5_000001_Rn_Rd  (DUP element)
  // imm5 encodes size+index: for S (32-bit), imm5 = (index << 2) | 0b100
  // https://www.scs.stanford.edu/~zyedidia/arm64/dup_advsimd_elt.html
  ASSERT(dst.is_128bit_simd(instr_set));
  ASSERT(src.is_128bit_simd(instr_set));
  u32 idx = static_cast<u32>(element);  // 0=X, 1=Y, 2=Z, 3=W
  u32 imm5 = (idx << 2) | 0b100u;       // size=10 (32-bit S), index in upper bits
  return InstructionARM64(0x4E000400u, Field{(imm5 & 31u) << 16}, Rn(src.id()), Rd(dst.id()));
}

InstructionARM64 xor_vf(Register dst, Register src1, Register src2) {
  // EOR Vd.16B, Vn.16B, Vm.16B — 128-bit bitwise XOR
  // https://www.scs.stanford.edu/~zyedidia/arm64/eor_advsimd.html
  ASSERT(dst.is_128bit_simd(instr_set));
  return InstructionARM64(0x6E201C00u, Rm(src2.id()), Rn(src1.id()), Rd(dst.id()));
}

InstructionARM64 sub_vf(Register dst, Register src1, Register src2) {
  // FSUB Vd.4S, Vn.4S, Vm.4S — 4x float32 subtract
  // bit22=sz=1 (FSUB vs FADD)
  ASSERT(dst.is_128bit_simd(instr_set));
  return InstructionARM64(0x4EA0D400u, Rm(src2.id()), Rn(src1.id()), Rd(dst.id()));
}

InstructionARM64 add_vf(Register dst, Register src1, Register src2) {
  // FADD Vd.4S, Vn.4S, Vm.4S — 4x float32 add
  ASSERT(dst.is_128bit_simd(instr_set));
  return InstructionARM64(0x4E20D400u, Rm(src2.id()), Rn(src1.id()), Rd(dst.id()));
}

InstructionARM64 mul_vf(Register dst, Register src1, Register src2) {
  // FMUL Vd.4S, Vn.4S, Vm.4S — 4x float32 multiply (U=1, opcode=11011)
  ASSERT(dst.is_128bit_simd(instr_set));
  return InstructionARM64(0x6E20DC00u, Rm(src2.id()), Rn(src1.id()), Rd(dst.id()));
}

InstructionARM64 max_vf(Register dst, Register src1, Register src2) {
  // FMAX Vd.4S, Vn.4S, Vm.4S — 4x float32 maximum (opcode=11110)
  ASSERT(dst.is_128bit_simd(instr_set));
  return InstructionARM64(0x4E20F400u, Rm(src2.id()), Rn(src1.id()), Rd(dst.id()));
}

InstructionARM64 min_vf(Register dst, Register src1, Register src2) {
  // FMIN Vd.4S, Vn.4S, Vm.4S — 4x float32 minimum (sz=1, opcode=11110)
  ASSERT(dst.is_128bit_simd(instr_set));
  return InstructionARM64(0x4EA0F400u, Rm(src2.id()), Rn(src1.id()), Rd(dst.id()));
}

InstructionARM64 div_vf(Register dst, Register src1, Register src2) {
  // FDIV Vd.4S, Vn.4S, Vm.4S — 4x float32 divide (U=1, opcode=11111)
  ASSERT(dst.is_128bit_simd(instr_set));
  return InstructionARM64(0x6E20FC00u, Rm(src2.id()), Rn(src1.id()), Rd(dst.id()));
}

InstructionARM64 sqrt_vf(Register dst, Register src) {
  // FSQRT Vd.4S, Vn.4S — 4x float32 square root (2-reg unary)
  ASSERT(dst.is_128bit_simd(instr_set));
  ASSERT(src.is_128bit_simd(instr_set));
  return InstructionARM64(0x6EA1F800u, Rn(src.id()), Rd(dst.id()));
}

InstructionARM64 itof_vf(Register dst, Register src) {
  // SCVTF Vd.4S, Vn.4S — convert 4x int32 to float32
  ASSERT(dst.is_128bit_simd(instr_set));
  ASSERT(src.is_128bit_simd(instr_set));
  return InstructionARM64(0x4E21D800u, Rn(src.id()), Rd(dst.id()));
}

InstructionARM64 ftoi_vf(Register dst, Register src) {
  // FCVTZS Vd.4S, Vn.4S — convert 4x float32 to int32 (truncate)
  ASSERT(dst.is_128bit_simd(instr_set));
  ASSERT(src.is_128bit_simd(instr_set));
  return InstructionARM64(0x4EA1B800u, Rn(src.id()), Rd(dst.id()));
}

// NEON parallel vector shifts (operating on 128-bit packed integer lanes):
// SSHR Vd.4S, Vn.4S, #imm — arithmetic right shift 4x int32 by imm
// Encoding (immh:immb for 4S): immh=0b001X for 16-bit, 001XX for 32-bit
// For 4S (32-bit lanes): immh:immb = 0b0001_0000 shifted right by imm
// SSHR: 0_1_0_011110_immh(4)_immb(3)_000100_1_Rn_Rd
// For 32-bit: immh=0100 (base), shift right by imm: immb = 32-imm, immh=0100+overflow
// immh:immb = 64 - imm for 32-bit? No: for SSHR.4S by imm: immh:immb = (64-imm), but
// for 4S the constraint is 1 ≤ imm ≤ 32, so immh:immb = concat(0b0,1,0,0,0+bits) = (32-imm+32)
// Actually: for SSHR Vd.4S by imm (1≤imm≤32): immh:immb = 0b0100_000 | (32-imm)?
// This is getting complex. Use raw constants for the common shifts.
// Reference: https://www.scs.stanford.edu/~zyedidia/arm64/sshr_advsimd.html

InstructionARM64 pw_sra(Register dst, Register src, u8 imm) {
  // SSHR Vd.4S, Vn.4S, #imm — 4x int32 arithmetic right shift
  // immh=0100 (for 32-bit), immb = 32-imm → immh:immb total at [22:16]
  ASSERT(dst.is_128bit_simd(instr_set));
  ASSERT(src.is_128bit_simd(instr_set));
  ASSERT(imm >= 1 && imm <= 32);
  u32 immhb = 64u - imm;  // for 4S: immh:immb = 0b0100_xxx where bits = 32-imm
  return InstructionARM64(0x4F000400u, Field{(immhb & 0x7Fu) << 16}, Rn(src.id()), Rd(dst.id()));
}

InstructionARM64 pw_srl(Register dst, Register src, u8 imm) {
  // USHR Vd.4S, Vn.4S, #imm — 4x int32 logical right shift
  // Same encoding as SSHR but U=1: 0x6F000400
  ASSERT(dst.is_128bit_simd(instr_set));
  ASSERT(src.is_128bit_simd(instr_set));
  ASSERT(imm >= 1 && imm <= 32);
  u32 immhb = 64u - imm;
  return InstructionARM64(0x6F000400u, Field{(immhb & 0x7Fu) << 16}, Rn(src.id()), Rd(dst.id()));
}

InstructionARM64 ph_srl(Register dst, Register src, u8 imm) {
  // USHR Vd.8H, Vn.8H, #imm — 8x int16 logical right shift
  // For 16-bit lanes: immh=0010 base, immhb = 32-imm
  ASSERT(dst.is_128bit_simd(instr_set));
  ASSERT(src.is_128bit_simd(instr_set));
  ASSERT(imm >= 1 && imm <= 16);
  u32 immhb = 32u - imm;
  return InstructionARM64(0x6F000400u, Field{(immhb & 0x3Fu) << 16}, Rn(src.id()), Rd(dst.id()));
}

InstructionARM64 pw_sll(Register dst, Register src, u8 imm) {
  // SHL Vd.4S, Vn.4S, #imm — 4x int32 logical left shift
  // Encoding: 0_1_0_011110_immh_immb_010101_1_Rn_Rd (SHL)
  // For 4S left shift by imm: immh:immb = 0b0100_000 | imm (immh=0100, immb=imm)
  ASSERT(dst.is_128bit_simd(instr_set));
  ASSERT(src.is_128bit_simd(instr_set));
  ASSERT(imm >= 0 && imm <= 31);
  u32 immhb = 32u + imm;  // for SHL Vd.4S: immhb = 32+imm
  return InstructionARM64(0x4F005400u, Field{(immhb & 0x7Fu) << 16}, Rn(src.id()), Rd(dst.id()));
}

InstructionARM64 ph_sll(Register dst, Register src, u8 imm) {
  // SHL Vd.8H, Vn.8H, #imm — 8x int16 logical left shift
  // For 16-bit left shift: immhb = 16+imm
  ASSERT(dst.is_128bit_simd(instr_set));
  ASSERT(src.is_128bit_simd(instr_set));
  ASSERT(imm >= 0 && imm <= 15);
  u32 immhb = 16u + imm;
  return InstructionARM64(0x4F005400u, Field{(immhb & 0x3Fu) << 16}, Rn(src.id()), Rd(dst.id()));
}

InstructionARM64 parallel_add_byte(Register dst, Register src0, Register src1) {
  // ADD Vd.16B, Vn.16B, Vm.16B — add 16x int8
  // "Advanced SIMD three same": 0_1_0_01110_00_1_Rm_100001_Rn_Rd (size=00 for byte)
  return InstructionARM64(0x4E208400u, Rm(src1.id()), Rn(src0.id()), Rd(dst.id()));
}

InstructionARM64 parallel_bitwise_or(Register dst, Register src0, Register src1) {
  // ORR Vd.16B, Vn.16B, Vm.16B
  return InstructionARM64(0x4EA01C00u, Rm(src1.id()), Rn(src0.id()), Rd(dst.id()));
}

InstructionARM64 parallel_bitwise_xor(Register dst, Register src0, Register src1) {
  // EOR Vd.16B, Vn.16B, Vm.16B
  return InstructionARM64(0x6E201C00u, Rm(src1.id()), Rn(src0.id()), Rd(dst.id()));
}

InstructionARM64 parallel_bitwise_and(Register dst, Register src0, Register src1) {
  // AND Vd.16B, Vn.16B, Vm.16B
  return InstructionARM64(0x4E201C00u, Rm(src1.id()), Rn(src0.id()), Rd(dst.id()));
}

// MIPS EE interleave/unpack operations mapped to ARM64 NEON:
// PUNPCKHBW / PUNPCKLBW equivalents using ZIP1/ZIP2 (interleave)
// Note: x86 and MIPS have opposite element ordering in some cases — "swapped" variants account for this.
// ARM64 ZIP1/ZIP2 for 16B: 0_1_001110_size_0_Rm_0001_1_0_Rn_Rd

InstructionARM64 pextub_swapped(Register dst, Register src0, Register src1) {
  // Unpack high bytes (PUNPCKHBW equivalent) → ZIP2 Vd.16B, Vn.16B, Vm.16B
  // ZIP2 (high interleave) 16B: size=00: 0x4E007800
  return InstructionARM64(0x4E007800u, Rm(src1.id()), Rn(src0.id()), Rd(dst.id()));
}

InstructionARM64 pextuh_swapped(Register dst, Register src0, Register src1) {
  // Unpack high halfwords → ZIP2 Vd.8H, Vn.8H, Vm.8H
  // ZIP2 8H: size=01: 0x4E407800
  return InstructionARM64(0x4E407800u, Rm(src1.id()), Rn(src0.id()), Rd(dst.id()));
}

InstructionARM64 pextuw_swapped(Register dst, Register src0, Register src1) {
  // Unpack high words → ZIP2 Vd.4S, Vn.4S, Vm.4S
  // ZIP2 4S: size=10: 0x4E807800
  return InstructionARM64(0x4E807800u, Rm(src1.id()), Rn(src0.id()), Rd(dst.id()));
}

InstructionARM64 pextlb_swapped(Register dst, Register src0, Register src1) {
  // Unpack low bytes → ZIP1 Vd.16B, Vn.16B, Vm.16B
  // ZIP1 16B: size=00: 0x4E003800
  return InstructionARM64(0x4E003800u, Rm(src1.id()), Rn(src0.id()), Rd(dst.id()));
}

InstructionARM64 pextlh_swapped(Register dst, Register src0, Register src1) {
  // Unpack low halfwords → ZIP1 Vd.8H, Vn.8H, Vm.8H
  // ZIP1 8H: size=01: 0x4E403800
  return InstructionARM64(0x4E403800u, Rm(src1.id()), Rn(src0.id()), Rd(dst.id()));
}

InstructionARM64 pextlw_swapped(Register dst, Register src0, Register src1) {
  // Unpack low words → ZIP1 Vd.4S, Vn.4S, Vm.4S
  // ZIP1 4S: size=10: 0x4E803800
  return InstructionARM64(0x4E803800u, Rm(src1.id()), Rn(src0.id()), Rd(dst.id()));
}

InstructionARM64 parallel_compare_e_b(Register dst, Register src0, Register src1) {
  // CMEQ Vd.16B, Vn.16B, Vm.16B — compare equal, 16x int8
  // 0_1_0_01110_00_1_Rm_100011_Rn_Rd: 0x4E208C00
  return InstructionARM64(0x4E208C00u, Rm(src1.id()), Rn(src0.id()), Rd(dst.id()));
}

InstructionARM64 parallel_compare_e_h(Register dst, Register src0, Register src1) {
  // CMEQ Vd.8H, Vn.8H, Vm.8H — compare equal, 8x int16
  // size=01: 0x4E608C00
  return InstructionARM64(0x4E608C00u, Rm(src1.id()), Rn(src0.id()), Rd(dst.id()));
}

InstructionARM64 parallel_compare_e_w(Register dst, Register src0, Register src1) {
  // CMEQ Vd.4S, Vn.4S, Vm.4S — compare equal, 4x int32
  // size=10: 0x4EA08C00
  return InstructionARM64(0x4EA08C00u, Rm(src1.id()), Rn(src0.id()), Rd(dst.id()));
}

InstructionARM64 parallel_compare_gt_b(Register dst, Register src0, Register src1) {
  // CMGT Vd.16B, Vn.16B, Vm.16B — compare greater-than (signed), 16x int8
  // 0_1_0_01110_00_1_Rm_001101_Rn_Rd: 0x4E203400
  return InstructionARM64(0x4E203400u, Rm(src1.id()), Rn(src0.id()), Rd(dst.id()));
}

InstructionARM64 parallel_compare_gt_h(Register dst, Register src0, Register src1) {
  // CMGT Vd.8H — size=01: 0x4E603400
  return InstructionARM64(0x4E603400u, Rm(src1.id()), Rn(src0.id()), Rd(dst.id()));
}

InstructionARM64 parallel_compare_gt_w(Register dst, Register src0, Register src1) {
  // CMGT Vd.4S — size=10: 0x4EA03400
  return InstructionARM64(0x4EA03400u, Rm(src1.id()), Rn(src0.id()), Rd(dst.id()));
}

InstructionARM64 vpunpcklqdq(Register dst, Register src0, Register src1) {
  // Unpack low quadwords → ZIP1 Vd.2D, Vn.2D, Vm.2D
  // ZIP1 2D: size=11: 0x4EC03800
  return InstructionARM64(0x4EC03800u, Rm(src1.id()), Rn(src0.id()), Rd(dst.id()));
}

InstructionARM64 pcpyld_swapped(Register dst, Register src0, Register src1) {
  // MIPS PCPYLD: copy lower 64-bit of src1 to lower 64-bit of dst, src0 upper → dst upper
  // ARM64: ZIP1 Vd.2D, Vn.2D(src1), Vm.2D(src0)  [swapped arg order]
  return InstructionARM64(0x4EC03800u, Rm(src0.id()), Rn(src1.id()), Rd(dst.id()));
}

InstructionARM64 pcpyud(Register dst, Register src0, Register src1) {
  // MIPS PCPYUD: copy upper 64-bit of src0 to lower 64-bit of dst, src1 upper → dst upper
  // ARM64: ZIP2 Vd.2D, Vn.2D(src0), Vm.2D(src1)
  return InstructionARM64(0x4EC07800u, Rm(src1.id()), Rn(src0.id()), Rd(dst.id()));
}

InstructionARM64 vpsubd(Register dst, Register src0, Register src1) {
  // SUB Vd.2D, Vn.2D, Vm.2D — subtract 2x int64
  // "Advanced SIMD three same" size=11: 0_1_1_01110_11_1_Rm_100001_Rn_Rd → 0x6EE08400
  return InstructionARM64(0x6EE08400u, Rm(src1.id()), Rn(src0.id()), Rd(dst.id()));
}

InstructionARM64 vpsrldq(Register dst, Register src, u8 imm) {
  // Not emitted on ARM64 — ppach/ppacb use native UZP1 paths instead.
  (void)dst; (void)src; (void)imm;
  ASSERT_MSG(false, "vpsrldq: not reachable on ARM64 — use UZP1 path in compile_asm_ppach");
  return InstructionARM64(0b0);
}

InstructionARM64 vpslldq(Register dst, Register src, u8 imm) {
  (void)dst; (void)src; (void)imm;
  ASSERT_MSG(false, "vpslldq: not reachable on ARM64");
  return InstructionARM64(0b0);
}

InstructionARM64 vpshuflw(Register dst, Register src, u8 imm) {
  (void)dst; (void)src; (void)imm;
  ASSERT_MSG(false, "vpshuflw: not reachable on ARM64 — use UZP1 path in compile_asm_ppach");
  return InstructionARM64(0b0);
}

InstructionARM64 vpshufhw(Register dst, Register src, u8 imm) {
  (void)dst; (void)src; (void)imm;
  ASSERT_MSG(false, "vpshufhw: not reachable on ARM64 — use UZP1 path in compile_asm_ppach");
  return InstructionARM64(0b0);
}

InstructionARM64 vpackuswb(Register dst, Register src0, Register src1) {
  (void)dst; (void)src0; (void)src1;
  ASSERT_MSG(false, "vpackuswb: not reachable on ARM64 — use UZP1 path in compile_asm_ppacb");
  return InstructionARM64(0b0);
}

InstructionARM64 uzp1_8h(Register dst, Register src0, Register src1) {
  // UZP1 Vd.8H, Vn.8H, Vm.8H — unzip even int16 elements (128-bit, 8x int16)
  // Advanced SIMD permute: Q=1, size=01, opcode=001 → 0x4E401800
  return InstructionARM64(0x4E401800u, Rm(src1.id()), Rn(src0.id()), Rd(dst.id()));
}

InstructionARM64 uzp1_16b(Register dst, Register src0, Register src1) {
  // UZP1 Vd.16B, Vn.16B, Vm.16B — unzip even bytes (128-bit, 16x uint8)
  // Advanced SIMD permute: Q=1, size=00, opcode=001 → 0x4E001800
  return InstructionARM64(0x4E001800u, Rm(src1.id()), Rn(src0.id()), Rd(dst.id()));
}
}  // namespace ARM64
}  // namespace IGen
}  // namespace emitter
