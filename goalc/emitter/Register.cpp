#include "Register.h"

#include <stdexcept>

namespace emitter {
RegisterInfo RegisterInfo::make_register_info() {
  RegisterInfo info;

#if defined(__aarch64__)
  // ARM64: register IDs 0-30 map directly to X0-X30.
  // X0      (0)  = arg0/return, temp  [≡ RAX/RDI overlap in id space]
  // X1-X7   (1-7)= arg1-arg7, temp
  // X8      (8)  = indirect result / scratch, temp
  // X9-X15  (9-15) = temp
  // X16-X18 (16-18) = temp (IP0/IP1/platform)
  // x19     (19) = callee-saved (GOAL uses as extra saved)
  // x20     (20) = pp   (special)
  // x21     (21) = st   (special)
  // x22     (22) = off  (special)
  // X23-X28 (23-28) = callee-saved, unused by GOAL
  // X29/X30 = FP/LR, do not allocate
  info.m_info[X0]  = {false, false, "x0"};   // arg0/ret, temp
  info.m_info[X1]  = {false, false, "x1"};   // arg1, temp
  info.m_info[X2]  = {false, false, "x2"};   // arg2, temp
  info.m_info[X3]  = {false, false, "x3"};   // arg3, temp
  info.m_info[X4]  = {false, false, "x4"};   // arg4, temp
  info.m_info[X5]  = {false, false, "x5"};   // arg5, temp
  info.m_info[X6]  = {false, false, "x6"};   // arg6, temp
  info.m_info[X7]  = {false, false, "x7"};   // arg7, temp
  info.m_info[X8]  = {false, false, "x8"};   // scratch/ret, temp
  info.m_info[X9]  = {false, false, "x9"};
  info.m_info[X10] = {false, false, "x10"};
  info.m_info[X11] = {false, false, "x11"};
  info.m_info[X12] = {false, false, "x12"};
  info.m_info[X13] = {false, false, "x13"};
  info.m_info[X14] = {false, false, "x14"};
  info.m_info[X15] = {false, false, "x15"};
  info.m_info[X16] = {false, false, "x16"};
  info.m_info[X17] = {false, false, "x17"};
  info.m_info[X18] = {false, false, "x18"};
  info.m_info[x19] = {true,  false, "x19"};  // callee-saved
  info.m_info[x20] = {false, true,  "x20"};  // pp (special)
  info.m_info[x21] = {false, true,  "x21"};  // st (special)
  info.m_info[x22] = {false, true,  "x22"};  // off (special)
  info.m_info[X23] = {true,  false, "x23"};  // callee-saved
  info.m_info[X24] = {true,  false, "x24"};  // callee-saved
  info.m_info[X25] = {true,  false, "x25"};  // callee-saved
  info.m_info[X26] = {true,  false, "x26"};  // callee-saved
  info.m_info[X27] = {true,  false, "x27"};  // callee-saved
  info.m_info[X28] = {true,  false, "x28"};  // callee-saved
  info.m_info[X29] = {false, true,  "x29"};  // FP (special, do not alloc)
  info.m_info[X30] = {false, true,  "x30"};  // LR (special, do not alloc)

  // SIMD: the regalloc uses XMM0-XMM15 ids (16-31) for vector regs.
  // On ARM64 these map to q0-q15.  Write SIMD entries FIRST so the GPR
  // entries for X16-X30 (which share the same index range 16-30) win.
  info.m_info[XMM0]  = {false, false, "q0"};   // index 16 — overwritten below by X16 GPR
  info.m_info[XMM1]  = {false, false, "q1"};   // 17
  info.m_info[XMM2]  = {false, false, "q2"};   // 18
  info.m_info[XMM3]  = {false, false, "q3"};   // 19
  info.m_info[XMM4]  = {false, false, "q4"};   // 20
  info.m_info[XMM5]  = {false, false, "q5"};   // 21
  info.m_info[XMM6]  = {false, false, "q6"};   // 22
  info.m_info[XMM7]  = {false, false, "q7"};   // 23
  info.m_info[XMM8]  = {true,  false, "q8"};   // 24
  info.m_info[XMM9]  = {true,  false, "q9"};   // 25
  info.m_info[XMM10] = {true,  false, "q10"};  // 26
  info.m_info[XMM11] = {true,  false, "q11"};  // 27
  info.m_info[XMM12] = {true,  false, "q12"};  // 28
  info.m_info[XMM13] = {true,  false, "q13"};  // 29
  info.m_info[XMM14] = {true,  false, "q14"};  // 30
  info.m_info[XMM15] = {true,  false, "q15"};  // 31 = SP — safe, SP never allocated

  // GPR entries (written after SIMD so they overwrite indices 16-30).
  // ARM64 ABI: args in x0-x7 (ids 0-7).
  info.m_gpr_arg_regs = std::array<Register, N_ARGS>(
      {X0, X1, X2, X3, X4, X5, X6, X7});
  // SIMD args: q1-q7 (skip q0 so it can be used for return).
  info.m_xmm_arg_regs =
      std::array<Register, N_ARGS>({XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7, XMM8});
  // Callee-saved GPRs available to GOAL (exclude specials x20/x21/x22 and FP/LR).
  info.m_saved_gprs = std::array<Register, N_SAVED_GPRS>({x19, X23, X24, X25, X26});
  info.m_saved_xmms =
      std::array<Register, N_SAVED_XMMS>({XMM8, XMM9, XMM10, XMM11, XMM12, XMM13, XMM14, XMM15});

  for (size_t i = 0; i < N_SAVED_GPRS; i++) {
    info.m_saved_all[i] = info.m_saved_gprs[i];
  }
  for (size_t i = 0; i < N_SAVED_XMMS; i++) {
    info.m_saved_all[i + N_SAVED_GPRS] = info.m_saved_xmms[i];
  }

  // Allocate temps first (x8-x18), then args (x0-x7), then saved (x19, x23-x28).
  info.m_gpr_alloc_order = {X8, X9, X10, X11, X12, X13, X14, X15,
                             X0, X1, X2,  X3,  X4,  X5,  X6,  X7, x19};
  info.m_xmm_alloc_order = {XMM0, XMM1, XMM2, XMM3,  XMM4,  XMM5,  XMM6,
                             XMM7, XMM8, XMM9, XMM10, XMM11, XMM12, XMM13};

  info.m_gpr_temp_only_alloc_order = {X8, X9, X10, X11, X12, X13, X14, X15,
                                      X0, X1, X2,  X3,  X4,  X5,  X6,  X7};
  info.m_xmm_temp_only_alloc_order = {XMM0, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7};

  info.m_gpr_spill_temp_alloc_order = {X8,  X9,  X10, X11, X12, X13,
                                       X14, X15, X0,  X1,  X2,  X3,
                                       X4,  X5,  X6,  X7,  x19};
  info.m_xmm_spill_temp_alloc_order = {XMM0, XMM1, XMM2,  XMM3,  XMM4,  XMM5,  XMM6,  XMM7,
                                       XMM8, XMM9, XMM10, XMM11, XMM12, XMM13, XMM14, XMM15};
#else
  info.m_info[RAX] = {false, false, "rax"};  // return, temp
  info.m_info[RCX] = {false, false, "rcx"};  // gpr arg 3, temp
  info.m_info[RDX] = {false, false, "rdx"};  // gpr arg 2, temp
  info.m_info[RBX] = {true, false, "rbx"};   // saved
  info.m_info[RSP] = {false, true, "rsp"};   // stack pointer
  info.m_info[RBP] = {true, false, "rbp"};   // saved
  info.m_info[RSI] = {false, false, "rsi"};  // gpr arg 1, temp
  info.m_info[RDI] = {false, false, "rdi"};  // gpr arg 0, temp

  info.m_info[R8] = {false, false, "r8"};   // gpr arg 4, temp
  info.m_info[R9] = {false, false, "r9"};   // gpr arg 5, temp
  info.m_info[R10] = {true, false, "r10"};  // gpr arg 6, saved
  info.m_info[R11] = {true, false, "r11"};  // gpr arg 7, saved
  info.m_info[R12] = {true, false, "r12"};  // saved
  info.m_info[R13] = {false, true, "r13"};  // pp
  info.m_info[R14] = {false, true, "r14"};  // st
  info.m_info[R15] = {false, true, "r15"};  // offset.

  info.m_info[XMM0] = {false, false, "xmm0"};
  info.m_info[XMM1] = {false, false, "xmm1"};
  info.m_info[XMM2] = {false, false, "xmm2"};
  info.m_info[XMM3] = {false, false, "xmm3"};
  info.m_info[XMM4] = {false, false, "xmm4"};
  info.m_info[XMM5] = {false, false, "xmm5"};
  info.m_info[XMM6] = {false, false, "xmm6"};
  info.m_info[XMM7] = {false, false, "xmm7"};
  info.m_info[XMM8] = {true, false, "xmm8"};
  info.m_info[XMM9] = {true, false, "xmm9"};
  info.m_info[XMM10] = {true, false, "xmm10"};
  info.m_info[XMM11] = {true, false, "xmm11"};
  info.m_info[XMM12] = {true, false, "xmm12"};
  info.m_info[XMM13] = {true, false, "xmm13"};
  info.m_info[XMM14] = {true, false, "xmm14"};
  info.m_info[XMM15] = {true, false, "xmm15"};

  info.m_gpr_arg_regs = std::array<Register, N_ARGS>({RDI, RSI, RDX, RCX, R8, R9, R10, R11});
  // skip xmm0 so it can be used for return.
  info.m_xmm_arg_regs =
      std::array<Register, N_ARGS>({XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7, XMM8});
  info.m_saved_gprs = std::array<Register, N_SAVED_GPRS>({RBX, RBP, R10, R11, R12});
  info.m_saved_xmms =
      std::array<Register, N_SAVED_XMMS>({XMM8, XMM9, XMM10, XMM11, XMM12, XMM13, XMM14, XMM15});

  for (size_t i = 0; i < N_SAVED_GPRS; i++) {
    info.m_saved_all[i] = info.m_saved_gprs[i];
  }
  for (size_t i = 0; i < N_SAVED_XMMS; i++) {
    info.m_saved_all[i + N_SAVED_GPRS] = info.m_saved_xmms[i];
  }

  // todo - experiment with better orders for allocation.
  info.m_gpr_alloc_order = {RAX, RCX, RDX, RBX, RBP, RSI, RDI, R8, R9, R10};  // arbitrary
  info.m_xmm_alloc_order = {XMM0, XMM1, XMM2, XMM3,  XMM4,  XMM5,  XMM6,
                            XMM7, XMM8, XMM9, XMM10, XMM11, XMM12, XMM13};

  // these should only be temp registers!
  info.m_gpr_temp_only_alloc_order = {RAX, RCX, RDX, RSI, RDI, R8, R9};
  info.m_xmm_temp_only_alloc_order = {XMM0, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7};

  info.m_gpr_spill_temp_alloc_order = {RAX, RCX, RDX, RBX, RBP, RSI,
                                       RDI, R8,  R9,  R10, R11, R12};  // arbitrary
  info.m_xmm_spill_temp_alloc_order = {XMM0, XMM1, XMM2,  XMM3,  XMM4,  XMM5,  XMM6,  XMM7,
                                       XMM8, XMM9, XMM10, XMM11, XMM12, XMM13, XMM14, XMM15};
#endif
  return info;
}

RegisterInfo gRegInfo = RegisterInfo::make_register_info();

bool RegisterInfo::is_xmm_arg_reg(Register r) const {
  for (const auto& xa : m_xmm_arg_regs) {
    if (xa.id() == r.id()) return true;
  }
  return false;
}

std::string to_string(HWRegKind kind) {
  switch (kind) {
    case HWRegKind::GPR:
      return "gpr";
    case HWRegKind::XMM:
      return "xmm";
    default:
      throw std::runtime_error("Unsupported HWRegKind");
  }
}

HWRegKind reg_class_to_hw(RegClass reg_class) {
  switch (reg_class) {
    case RegClass::VECTOR_FLOAT:
    case RegClass::FLOAT:
    case RegClass::INT_128:
      return HWRegKind::XMM;
    case RegClass::GPR_64:
      return HWRegKind::GPR;
    default:
      ASSERT(false);
      return HWRegKind::INVALID;
  }
}

std::string Register::print() const {
  return gRegInfo.get_info(*this).name;
}

}  // namespace emitter