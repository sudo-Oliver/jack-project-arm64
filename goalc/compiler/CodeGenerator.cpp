/*!
 * @file CodeGenerator.cpp
 * Generate object files from a FileEnv using an emitter::ObjectGenerator.
 * Populates a DebugInfo.
 * Currently owns the logic for emitting the function prologues/epilogues and stack spill ops.
 */

#include "CodeGenerator.h"

#include <stdexcept>
#include <unordered_set>

#include "IR.h"

#include "goalc/debugger/DebugInfo.h"
#include "goalc/emitter/IGen.h"
#include "goalc/emitter/IGenARM64.h"

#include "fmt/format.h"

using namespace emitter;

CodeGenerator::CodeGenerator(FileEnv* env,
                             DebugInfo* debug_info,
                             GameVersion version,
                             InstructionSet instruction_set)
    : m_gen(version, instruction_set), m_fe(env), m_debug_info(debug_info) {}

namespace {
constexpr u32 kEeMainMemorySize = 0x08000000;

Instruction arm64_branch_hs_skip_one() {
  // B.HS +8: skip exactly one 4-byte ARM64 instruction.
  return InstructionARM64(0x54000042u);
}

void emit_arm64_mov_gpr64_u64(ObjectGenerator& gen,
                              FunctionRecord f_rec,
                              InstructionInfo::Kind kind,
                              Register dst,
                              u64 value) {
  bool emitted_movz = false;
  for (int hw = 0; hw < 4; ++hw) {
    const u64 chunk = (value >> (hw * 16)) & 0xffff;
    if (!chunk) {
      continue;
    }

    const u32 hw_field = static_cast<u32>(hw) << 21;
    const u32 imm_field = static_cast<u32>(chunk) << 5;
    const u32 base = emitted_movz ? 0xF2800000u : 0xD2800000u;
    gen.add_instr_no_ir(f_rec,
                        InstructionARM64(base, ARM64::Field{hw_field}, ARM64::Field{imm_field},
                                         ARM64::Rd(dst.id())),
                        kind);
    emitted_movz = true;
  }
}

void emit_arm64_mov_gpr64_u64(ObjectGenerator& gen, IR_Record i_rec, Register dst, u64 value) {
  bool emitted_movz = false;
  for (int hw = 0; hw < 4; ++hw) {
    const u64 chunk = (value >> (hw * 16)) & 0xffff;
    if (!chunk) {
      continue;
    }

    const u32 hw_field = static_cast<u32>(hw) << 21;
    const u32 imm_field = static_cast<u32>(chunk) << 5;
    const u32 base = emitted_movz ? 0xF2800000u : 0xD2800000u;
    gen.add_instr(InstructionARM64(base, ARM64::Field{hw_field}, ARM64::Field{imm_field},
                                   ARM64::Rd(dst.id())),
                  i_rec);
    emitted_movz = true;
  }
}

void emit_arm64_lr_relative_guard(ObjectGenerator& gen,
                                  FunctionRecord f_rec,
                                  InstructionInfo::Kind kind) {
  const auto lr = Register(ARM64_REG::X30);
  const auto scratch = Register(ARM64_REG::X16);
  emit_arm64_mov_gpr64_u64(gen, f_rec, kind, scratch, kEeMainMemorySize);
  gen.add_instr_no_ir(f_rec, IGen::cmp_gpr64_gpr64(gen, lr, scratch), kind);
  gen.add_instr_no_ir(f_rec, arm64_branch_hs_skip_one(), kind);
  gen.add_instr_no_ir(f_rec, IGen::add_gpr64_gpr64(gen, lr, gen.get_offset_reg()), kind);
}

void emit_arm64_lr_relative_guard(ObjectGenerator& gen, IR_Record i_rec) {
  const auto lr = Register(ARM64_REG::X30);
  const auto scratch = Register(ARM64_REG::X16);
  emit_arm64_mov_gpr64_u64(gen, i_rec, scratch, kEeMainMemorySize);
  gen.add_instr(IGen::cmp_gpr64_gpr64(gen, lr, scratch), i_rec);
  gen.add_instr(arm64_branch_hs_skip_one(), i_rec);
  gen.add_instr(IGen::add_gpr64_gpr64(gen, lr, gen.get_offset_reg()), i_rec);
}
}  // namespace

/*!
 * Generate an object file.
 */
std::vector<u8> CodeGenerator::run(const TypeSystem* ts) {
  std::unordered_set<std::string> function_names;

  // first, add each function to the ObjectGenerator (but don't add any data)
  for (auto& f : m_fe->functions()) {
    if (function_names.find(f->name()) == function_names.end()) {
      function_names.insert(f->name());
    } else {
      printf("Failed to codegen, there are two functions with internal names [%s]\n",
             f->name().c_str());
      throw std::runtime_error("Failed to codegen.");
    }
    auto rec =
        m_gen.add_function_to_seg(f->segment, &m_debug_info->add_function(f->name(), m_fe->name()));
    for (auto& x : f->code_source()) {
      rec.debug->code_sources.push_back(x.heap_obj);
    }
    for (auto& x : f->code()) {
      rec.debug->ir_strings.push_back(x->print());
    }
  }

  // next, add all static objects.
  for (auto& static_obj : m_fe->statics()) {
    static_obj->generate(&m_gen);
  }

  // next, add instructions to functions
  for (size_t i = 0; i < m_fe->functions().size(); i++) {
    do_function(m_fe->functions().at(i).get(), i);
  }

  // generate a v3 object.
  return m_gen.generate_data_v3(ts).to_vector();
}

void CodeGenerator::do_function(FunctionEnv* env, int f_idx) {
  if (env->is_asm_func) {
    if (m_gen.instr_set() == InstructionSet::X86) {
      do_asm_function_x86(env, f_idx, env->asm_func_saved_regs);
    } else if (m_gen.instr_set() == InstructionSet::ARM64) {
      do_asm_function_arm64(env, f_idx, env->asm_func_saved_regs);
    } else {
      throw std::runtime_error("CodeGenerator::do_function, instruction set not supported");
    }
  } else {
    if (m_gen.instr_set() == InstructionSet::X86) {
      do_goal_function_x86(env, f_idx);
    } else if (m_gen.instr_set() == InstructionSet::ARM64) {
      do_goal_function_arm64(env, f_idx);
    } else {
      throw std::runtime_error("CodeGenerator::do_function, instruction set not supported");
    }
  }
}

/*!
 * Add instructions to the function, specified by index.
 * Generates prologues / epilogues.
 */
void CodeGenerator::do_goal_function_x86(FunctionEnv* env, int f_idx) {
  bool use_new_xmms = true;
  auto* debug = &m_debug_info->function_by_name(env->name());

  auto f_rec = m_gen.get_existing_function_record(f_idx);
  // todo, extra alignment settings

  auto& ri = emitter::gRegInfo;
  const auto& allocs = env->alloc_result();

  // compute how much stack we will use
  int stack_offset = 0;

  // count how many xmm's we have to backup
  int n_xmm_backups = 0;
  for (auto& saved_reg : allocs.used_saved_regs) {
    if (saved_reg.is_xmm(m_gen.instr_set())) {
      n_xmm_backups++;
    }
  }

  // only for new xmms. if n == 0, we don't use this at all.
  int xmm_backup_stack_offset = 8 + XMM_SIZE * n_xmm_backups;

  if (use_new_xmms) {
    if (n_xmm_backups > 0) {
      // offset the stack
      stack_offset += xmm_backup_stack_offset;
      m_gen.add_instr_no_ir(f_rec, IGen::sub_gpr64_imm(m_gen, RSP, xmm_backup_stack_offset),
                            InstructionInfo::Kind::PROLOGUE);
      // back up xmms
      int i = 0;
      for (auto& saved_reg : allocs.used_saved_regs) {
        if (saved_reg.is_xmm(m_gen.instr_set())) {
          int offset = i * XMM_SIZE;
          m_gen.add_instr_no_ir(f_rec,
                                IGen::store128_xmm128_reg_offset(m_gen, RSP, saved_reg, offset),
                                InstructionInfo::Kind::PROLOGUE);
          i++;
        }
      }
    }
  } else {
    // back up xmms (currently not aligned)
    for (auto& saved_reg : allocs.used_saved_regs) {
      if (saved_reg.is_xmm(m_gen.instr_set())) {
        m_gen.add_instr_no_ir(f_rec, IGen::sub_gpr64_imm8s(m_gen, RSP, XMM_SIZE),
                              InstructionInfo::Kind::PROLOGUE);
        m_gen.add_instr_no_ir(f_rec, IGen::store128_gpr64_simd128(m_gen, RSP, saved_reg),
                              InstructionInfo::Kind::PROLOGUE);
        stack_offset += XMM_SIZE;
      }
    }
  }

  // back up gprs
  for (auto& saved_reg : allocs.used_saved_regs) {
    if (saved_reg.is_gpr(m_gen.instr_set())) {
      m_gen.add_instr_no_ir(f_rec, IGen::push_gpr64(m_gen, saved_reg),
                            InstructionInfo::Kind::PROLOGUE);
      stack_offset += GPR_SIZE;
    }
  }

  // do we include an extra push to get 8 more bytes to keep the stack aligned?
  bool bonus_push = false;

  // the offset to add directly to rsp for stack variables or spills (no push/pop)
  int manually_added_stack_offset =
      GPR_SIZE * (allocs.stack_slots_for_spills + allocs.stack_slots_for_vars);
  stack_offset += manually_added_stack_offset;

  // do we need to align or manually offset?
  if (manually_added_stack_offset || allocs.needs_aligned_stack_for_spills ||
      env->needs_aligned_stack()) {
    if (!(stack_offset & 15)) {
      if (manually_added_stack_offset) {
        // if we're already adding to rsp, just add 8 more.
        manually_added_stack_offset += 8;
      } else {
        // otherwise to an extra push, and remember so we can do an extra pop later on.
        bonus_push = true;
        m_gen.add_instr_no_ir(f_rec, IGen::push_gpr64(m_gen, ri.get_saved_gpr(0)),
                              InstructionInfo::Kind::PROLOGUE);
      }
      stack_offset += 8;
    }

    ASSERT(stack_offset & 15);

    // do manual stack offset.
    if (manually_added_stack_offset) {
      m_gen.add_instr_no_ir(f_rec, IGen::sub_gpr64_imm(m_gen, RSP, manually_added_stack_offset),
                            InstructionInfo::Kind::PROLOGUE);
    }
  }
  debug->stack_usage = stack_offset;

  // emit each IR into x86 instructions.
  for (int ir_idx = 0; ir_idx < int(env->code().size()); ir_idx++) {
    auto& ir = env->code().at(ir_idx);
    // start of IR
    auto i_rec = m_gen.add_ir(f_rec);

    // load anything off the stack that was spilled and is needed.
    auto& bonus = allocs.stack_ops.at(ir_idx);
    for (auto& op : bonus.ops) {
      if (op.load) {
        if (op.reg.is_gpr(m_gen.instr_set()) && op.reg_class == RegClass::GPR_64) {
          // todo, s8 or 0 offset if possible?
          m_gen.add_instr(IGen::load64_gpr64_plus_s32(
                              m_gen, op.reg, allocs.get_slot_for_spill(op.slot) * GPR_SIZE, RSP),
                          i_rec);
        } else if (op.reg.is_xmm(m_gen.instr_set()) && op.reg_class == RegClass::FLOAT) {
          // load xmm32 off of the stack
          m_gen.add_instr(IGen::load_reg_offset_xmm32(
                              m_gen, op.reg, RSP, allocs.get_slot_for_spill(op.slot) * GPR_SIZE),
                          i_rec);
        } else if (op.reg.is_xmm(m_gen.instr_set()) &&
                   (op.reg_class == RegClass::VECTOR_FLOAT || op.reg_class == RegClass::INT_128)) {
          m_gen.add_instr(IGen::load128_xmm128_reg_offset(
                              m_gen, op.reg, RSP, allocs.get_slot_for_spill(op.slot) * GPR_SIZE),
                          i_rec);
        } else {
          ASSERT(false);
        }
      }
      if (op.store_before) {
        if (op.reg.is_gpr(m_gen.instr_set()) && op.reg_class == RegClass::GPR_64) {
          m_gen.add_instr(IGen::store64_gpr64_plus_s32(
                              m_gen, RSP, allocs.get_slot_for_spill(op.slot) * GPR_SIZE, op.reg),
                          i_rec);
        } else if (op.reg.is_xmm(m_gen.instr_set()) && op.reg_class == RegClass::FLOAT) {
          m_gen.add_instr(IGen::store_reg_offset_xmm32(
                              m_gen, RSP, op.reg, allocs.get_slot_for_spill(op.slot) * GPR_SIZE),
                          i_rec);
        } else if (op.reg.is_xmm(m_gen.instr_set()) &&
                   (op.reg_class == RegClass::VECTOR_FLOAT || op.reg_class == RegClass::INT_128)) {
          m_gen.add_instr(IGen::store128_xmm128_reg_offset(
                              m_gen, RSP, op.reg, allocs.get_slot_for_spill(op.slot) * GPR_SIZE),
                          i_rec);
        } else {
          ASSERT(false);
        }
      }
    }

    // do the actual op
    ir->do_codegen_x86(&m_gen, allocs, i_rec);

    // store things back on the stack if needed.
    for (auto& op : bonus.ops) {
      if (op.store && !op.store_before) {
        if (op.reg.is_gpr(m_gen.instr_set()) && op.reg_class == RegClass::GPR_64) {
          // todo, s8 or 0 offset if possible?
          m_gen.add_instr(IGen::store64_gpr64_plus_s32(
                              m_gen, RSP, allocs.get_slot_for_spill(op.slot) * GPR_SIZE, op.reg),
                          i_rec);
        } else if (op.reg.is_xmm(m_gen.instr_set()) && op.reg_class == RegClass::FLOAT) {
          // store xmm32 on the stack
          m_gen.add_instr(IGen::store_reg_offset_xmm32(
                              m_gen, RSP, op.reg, allocs.get_slot_for_spill(op.slot) * GPR_SIZE),
                          i_rec);
        } else if (op.reg.is_xmm(m_gen.instr_set()) &&
                   (op.reg_class == RegClass::VECTOR_FLOAT || op.reg_class == RegClass::INT_128)) {
          m_gen.add_instr(IGen::store128_xmm128_reg_offset(
                              m_gen, RSP, op.reg, allocs.get_slot_for_spill(op.slot) * GPR_SIZE),
                          i_rec);
        } else {
          ASSERT(false);
        }
      }
    }
  }  // end IR loop

  // EPILOGUE
  if (manually_added_stack_offset || allocs.needs_aligned_stack_for_spills ||
      env->needs_aligned_stack()) {
    if (manually_added_stack_offset) {
      m_gen.add_instr_no_ir(f_rec, IGen::add_gpr64_imm(m_gen, RSP, manually_added_stack_offset),
                            InstructionInfo::Kind::EPILOGUE);
    }

    if (bonus_push) {
      ASSERT(!manually_added_stack_offset);
      m_gen.add_instr_no_ir(f_rec, IGen::pop_gpr64(m_gen, ri.get_saved_gpr(0)),
                            InstructionInfo::Kind::EPILOGUE);
    }
  }

  for (int i = int(allocs.used_saved_regs.size()); i-- > 0;) {
    auto& saved_reg = allocs.used_saved_regs.at(i);
    if (saved_reg.is_gpr(m_gen.instr_set())) {
      m_gen.add_instr_no_ir(f_rec, IGen::pop_gpr64(m_gen, saved_reg),
                            InstructionInfo::Kind::EPILOGUE);
    }
  }

  if (use_new_xmms) {
    if (n_xmm_backups > 0) {
      int j = n_xmm_backups;
      for (int i = int(allocs.used_saved_regs.size()); i-- > 0;) {
        auto& saved_reg = allocs.used_saved_regs.at(i);
        if (saved_reg.is_xmm(m_gen.instr_set())) {
          j--;
          int offset = j * XMM_SIZE;
          m_gen.add_instr_no_ir(f_rec,
                                IGen::load128_xmm128_reg_offset(m_gen, saved_reg, RSP, offset),
                                InstructionInfo::Kind::EPILOGUE);
        }
      }
      ASSERT(j == 0);
      m_gen.add_instr_no_ir(f_rec, IGen::add_gpr64_imm(m_gen, RSP, xmm_backup_stack_offset),
                            InstructionInfo::Kind::EPILOGUE);
    }
  } else {
    for (int i = int(allocs.used_saved_regs.size()); i-- > 0;) {
      auto& saved_reg = allocs.used_saved_regs.at(i);
      if (saved_reg.is_xmm(m_gen.instr_set())) {
        m_gen.add_instr_no_ir(f_rec, IGen::load128_simd128_gpr64(m_gen, saved_reg, RSP),
                              InstructionInfo::Kind::EPILOGUE);
        m_gen.add_instr_no_ir(f_rec, IGen::add_gpr64_imm8s(m_gen, RSP, XMM_SIZE),
                              InstructionInfo::Kind::EPILOGUE);
      }
    }
  }

  m_gen.add_instr_no_ir(f_rec, IGen::ret(m_gen), InstructionInfo::Kind::EPILOGUE);
}

void CodeGenerator::do_goal_function_arm64(FunctionEnv* env, int f_idx) {
  auto* debug = &m_debug_info->function_by_name(env->name());
  auto f_rec = m_gen.get_existing_function_record(f_idx);
  const auto& allocs = env->alloc_result();

  int stack_offset = 0;

  // ARM64: BLR clobbers x30 (LR). Any non-leaf function must save LR before its first call
  // and restore it before ret, otherwise ret returns to the last BLR site instead of the caller.
  // x86 uses call/ret which push/pop the return address via the stack — no LR register exists.
  bool needs_lr_save = std::any_of(env->code().begin(), env->code().end(),
                                   [](const std::unique_ptr<IR>& ir) {
                                     return dynamic_cast<const IR_FunctionCall*>(ir.get()) !=
                                            nullptr;
                                   });
  auto lr_reg = emitter::Register(emitter::X30);
  if (needs_lr_save) {
    m_gen.add_instr_no_ir(f_rec, IGen::push_gpr64(m_gen, lr_reg), InstructionInfo::Kind::PROLOGUE);
    stack_offset += 16;
  }

  // Collect callee-saved SIMD (XMM/Q) registers, then save in pairs via STP.
  std::vector<emitter::Register> saved_xmm_regs;
  for (auto& saved_reg : allocs.used_saved_regs) {
    if (saved_reg.is_xmm(m_gen.instr_set())) {
      saved_xmm_regs.push_back(saved_reg);
    }
  }
  // Emit STP pairs (most efficient), then single STR for odd remainder.
  int xi = 0;
  for (; xi + 1 < (int)saved_xmm_regs.size(); xi += 2) {
    m_gen.add_instr_no_ir(
        f_rec, IGen::ARM64::stp_xmm128_pair(saved_xmm_regs[xi], saved_xmm_regs[xi + 1]),
        InstructionInfo::Kind::PROLOGUE);
    stack_offset += 32;
  }
  if (xi < (int)saved_xmm_regs.size()) {
    m_gen.add_instr_no_ir(f_rec, IGen::ARM64::push_xmm128(saved_xmm_regs[xi]),
                          InstructionInfo::Kind::PROLOGUE);
    stack_offset += 16;
  }

  // Save callee-saved GPRs. push_gpr64 does STR Xn, [SP, #-16]! — always 16-byte aligned.
  for (auto& saved_reg : allocs.used_saved_regs) {
    if (saved_reg.is_gpr(m_gen.instr_set())) {
      m_gen.add_instr_no_ir(f_rec, IGen::push_gpr64(m_gen, saved_reg),
                            InstructionInfo::Kind::PROLOGUE);
      stack_offset += 16;  // each ARM64 push reserves 16 bytes, not 8
    }
  }

  // Spill/var slots: 8 bytes each, but total must be 16-byte aligned on ARM64.
  int manually_added_stack_offset =
      GPR_SIZE * (allocs.stack_slots_for_spills + allocs.stack_slots_for_vars);
  manually_added_stack_offset = (manually_added_stack_offset + 15) & ~15;
  stack_offset += manually_added_stack_offset;

  if (manually_added_stack_offset || allocs.needs_aligned_stack_for_spills ||
      env->needs_aligned_stack()) {
    if (manually_added_stack_offset) {
      m_gen.add_instr_no_ir(f_rec, IGen::sub_gpr64_imm(m_gen, SP, manually_added_stack_offset),
                            InstructionInfo::Kind::PROLOGUE);
    }
  }
  debug->stack_usage = stack_offset;

  std::vector<bool> arm64_pop_lr_before_jump(env->code().size(), false);
  for (int i = 0; i < int(env->code().size()); i++) {
    if (!dynamic_cast<IR_JumpReg*>(env->code().at(i).get())) {
      continue;
    }
    for (int j = std::max(0, i - 3); j < i; j++) {
      if (dynamic_cast<IR_AsmPush*>(env->code().at(j).get())) {
        arm64_pop_lr_before_jump.at(i) = true;
        break;
      }
    }
  }

  for (int ir_idx = 0; ir_idx < int(env->code().size()); ir_idx++) {
    auto& ir = env->code().at(ir_idx);
    auto i_rec = m_gen.add_ir(f_rec);

    auto& bonus = allocs.stack_ops.at(ir_idx);
    for (auto& op : bonus.ops) {
      if (op.load) {
        if (op.reg.is_gpr(m_gen.instr_set()) && op.reg_class == RegClass::GPR_64) {
          m_gen.add_instr(IGen::load64_gpr64_plus_s32(
                              m_gen, op.reg, allocs.get_slot_for_spill(op.slot) * GPR_SIZE, SP),
                          i_rec);
        } else if (op.reg.is_xmm(m_gen.instr_set()) && op.reg_class == RegClass::FLOAT) {
          m_gen.add_instr(IGen::load_reg_offset_xmm32(
                              m_gen, op.reg, SP, allocs.get_slot_for_spill(op.slot) * GPR_SIZE),
                          i_rec);
        } else if (op.reg.is_xmm(m_gen.instr_set()) &&
                   (op.reg_class == RegClass::VECTOR_FLOAT || op.reg_class == RegClass::INT_128)) {
          s64 spill_off = allocs.get_slot_for_spill(op.slot) * GPR_SIZE;
          if (spill_off % 16 == 0) {
            m_gen.add_instr(IGen::load128_xmm128_reg_offset(m_gen, op.reg, SP, spill_off), i_rec);
          } else {
            // STR Qt requires 16-byte aligned offset; use LDUR (unscaled, byte-granular).
            m_gen.add_instr(IGen::ARM64::ldur_xmm128(op.reg, SP, spill_off), i_rec);
          }
        } else {
          ASSERT(false);
        }
      }
      // store_before: function-arg spills that must be saved before the first IR executes
      // (the first IR may itself be a call that clobbers the arg register).
      if (op.store_before) {
        if (op.reg.is_gpr(m_gen.instr_set()) && op.reg_class == RegClass::GPR_64) {
          m_gen.add_instr(IGen::store64_gpr64_plus_s32(
                              m_gen, SP, allocs.get_slot_for_spill(op.slot) * GPR_SIZE, op.reg),
                          i_rec);
        } else if (op.reg.is_xmm(m_gen.instr_set()) && op.reg_class == RegClass::FLOAT) {
          m_gen.add_instr(IGen::store_reg_offset_xmm32(
                              m_gen, SP, op.reg, allocs.get_slot_for_spill(op.slot) * GPR_SIZE),
                          i_rec);
        } else if (op.reg.is_xmm(m_gen.instr_set()) &&
                   (op.reg_class == RegClass::VECTOR_FLOAT || op.reg_class == RegClass::INT_128)) {
          s64 spill_off = allocs.get_slot_for_spill(op.slot) * GPR_SIZE;
          if (spill_off % 16 == 0) {
            m_gen.add_instr(IGen::store128_xmm128_reg_offset(m_gen, SP, op.reg, spill_off), i_rec);
          } else {
            m_gen.add_instr(IGen::ARM64::stur_xmm128(SP, op.reg, spill_off), i_rec);
          }
        } else {
          ASSERT(false);
        }
      }
    }

    // Inline asm in normal GOAL functions sometimes emulates x86 tail-call trampolines:
    //   push return-trampoline; jr target
    // ARM64 RET uses LR instead of popping the stack, so consume the pushed trampoline here.
    if (arm64_pop_lr_before_jump.at(ir_idx)) {
      m_gen.add_instr(IGen::pop_gpr64(m_gen, lr_reg), i_rec);
    }

    ir->do_codegen_arm64(&m_gen, allocs, i_rec);

    for (auto& op : bonus.ops) {
      if (op.store && !op.store_before) {
        if (op.reg.is_gpr(m_gen.instr_set()) && op.reg_class == RegClass::GPR_64) {
          m_gen.add_instr(IGen::store64_gpr64_plus_s32(
                              m_gen, SP, allocs.get_slot_for_spill(op.slot) * GPR_SIZE, op.reg),
                          i_rec);
        } else if (op.reg.is_xmm(m_gen.instr_set()) && op.reg_class == RegClass::FLOAT) {
          m_gen.add_instr(IGen::store_reg_offset_xmm32(
                              m_gen, SP, op.reg, allocs.get_slot_for_spill(op.slot) * GPR_SIZE),
                          i_rec);
        } else if (op.reg.is_xmm(m_gen.instr_set()) &&
                   (op.reg_class == RegClass::VECTOR_FLOAT || op.reg_class == RegClass::INT_128)) {
          s64 spill_off = allocs.get_slot_for_spill(op.slot) * GPR_SIZE;
          if (spill_off % 16 == 0) {
            m_gen.add_instr(IGen::store128_xmm128_reg_offset(m_gen, SP, op.reg, spill_off), i_rec);
          } else {
            // STR Qt requires 16-byte aligned offset; use STUR (unscaled, byte-granular).
            m_gen.add_instr(IGen::ARM64::stur_xmm128(SP, op.reg, spill_off), i_rec);
          }
        } else {
          ASSERT(false);
        }
      }
    }
  }

  // EPILOGUE
  if (manually_added_stack_offset || allocs.needs_aligned_stack_for_spills ||
      env->needs_aligned_stack()) {
    if (manually_added_stack_offset) {
      m_gen.add_instr_no_ir(f_rec, IGen::add_gpr64_imm(m_gen, SP, manually_added_stack_offset),
                            InstructionInfo::Kind::EPILOGUE);
    }
  }

  for (int i = int(allocs.used_saved_regs.size()); i-- > 0;) {
    auto& saved_reg = allocs.used_saved_regs.at(i);
    if (saved_reg.is_gpr(m_gen.instr_set())) {
      m_gen.add_instr_no_ir(f_rec, IGen::pop_gpr64(m_gen, saved_reg),
                            InstructionInfo::Kind::EPILOGUE);
    }
  }

  // Restore SIMD regs: reverse of prologue save order.
  // Prologue saved: pairs [0,1],[2,3],... then odd single at end (on top of stack).
  {
    int xmm_n = (int)saved_xmm_regs.size();
    int pair_end = xmm_n & ~1;  // highest even index (number of regs in pairs)
    // If odd, the last reg was pushed individually — restore it first (it's on top).
    if (xmm_n & 1) {
      m_gen.add_instr_no_ir(f_rec, IGen::ARM64::pop_xmm128(saved_xmm_regs[xmm_n - 1]),
                            InstructionInfo::Kind::EPILOGUE);
    }
    // Restore pairs in reverse order.
    for (int i = pair_end - 2; i >= 0; i -= 2) {
      m_gen.add_instr_no_ir(
          f_rec, IGen::ARM64::ldp_xmm128_pair(saved_xmm_regs[i], saved_xmm_regs[i + 1]),
          InstructionInfo::Kind::EPILOGUE);
    }
  }

  if (needs_lr_save) {
    m_gen.add_instr_no_ir(f_rec, IGen::pop_gpr64(m_gen, lr_reg), InstructionInfo::Kind::EPILOGUE);
  }

  emit_arm64_lr_relative_guard(m_gen, f_rec, InstructionInfo::Kind::EPILOGUE);
  m_gen.add_instr_no_ir(f_rec, IGen::ret(m_gen), InstructionInfo::Kind::EPILOGUE);
}

void CodeGenerator::do_asm_function_x86(FunctionEnv* env, int f_idx, bool allow_saved_regs) {
  auto f_rec = m_gen.get_existing_function_record(f_idx);
  const auto& allocs = env->alloc_result();

  if (!allow_saved_regs && !allocs.used_saved_regs.empty()) {
    std::string err = fmt::format(
        "ASM Function {}'s coloring using the following callee-saved registers: ", env->name());
    for (auto& x : allocs.used_saved_regs) {
      err += x.print();
      err += " ";
    }
    err.pop_back();
    err.push_back('.');
    throw std::runtime_error(err);
  }

  if (allocs.stack_slots_for_spills) {
    throw std::runtime_error("ASM Function has used the stack for spills.");
  }

  if (allocs.stack_slots_for_vars) {
    throw std::runtime_error("ASM Function has variables on the stack.");
  }

  // emit each IR into x86 instructions.
  for (int ir_idx = 0; ir_idx < int(env->code().size()); ir_idx++) {
    auto& ir = env->code().at(ir_idx);
    // start of IR
    auto i_rec = m_gen.add_ir(f_rec);

    // Make sure we aren't automatically accessing the stack.
    if (!allocs.stack_ops.at(ir_idx).ops.empty()) {
      throw std::runtime_error("ASM Function used a bonus op.");
    }

    // do the actual op
    ir->do_codegen_x86(&m_gen, allocs, i_rec);
  }
}

void CodeGenerator::do_asm_function_arm64(FunctionEnv* env, int f_idx, bool allow_saved_regs) {
  auto f_rec = m_gen.get_existing_function_record(f_idx);
  const auto& allocs = env->alloc_result();
  (void)allow_saved_regs;

  if (allocs.stack_slots_for_spills) {
    fmt::print("[ARM64 ASM] Function {} needs {} stack spill slots\n", env->name(),
               allocs.stack_slots_for_spills);
    for (int ir_idx = 0; ir_idx < int(env->code().size()); ir_idx++) {
      fmt::print("  IR[{}]: {}\n", ir_idx, env->code().at(ir_idx)->print());
    }
    throw std::runtime_error(
        fmt::format("ASM Function {} has used the stack for spills.", env->name()));
  }

  if (allocs.stack_slots_for_vars) {
    throw std::runtime_error("ASM Function has variables on the stack.");
  }

  // Match x86 asm-func behavior: do not synthesize callee-saved register
  // prologues/epilogues here. Kernel asm functions switch stacks manually and
  // save/restore the GOAL context themselves; compiler-inserted pushes/pops can
  // restore x22/off from the wrong stack after a context switch.

  // ARM64-specific: detect kernel context-switching asm function patterns.
  // On x86, CALL pushes the return address to the stack and RET pops it.
  // On ARM64, BLR stores the return address in X30 (LR) and RET = BR X30.
  // The GOAL kernel switches between kernel/process stacks in asm functions
  // (reset-and-call, thread-resume, thread-suspend, return-from-thread), and these
  // rely on the x86 CALL/RET stack discipline. We emulate it on ARM64 by:
  //   1. Pushing X30 at function entry where the x86 CALL would have pushed the return addr.
  //   2. Loading [SP] into X30 before .jr so the process function's BR X30 lands on
  //      return-from-thread (the trampoline reset-and-call pushed to the process stack).
  //   3. Popping X30 before .ret in functions that restore the kernel stack, so BR X30
  //      returns to the original kernel call site.
  bool arm64_has_jump_reg = false;        // has .jr — reset-and-call / thread-resume / set-to-run-bootstrap
  bool arm64_has_kernel_sp_load = false;  // has .load-sym sp *kernel-sp* — thread-suspend / return-from-thread
  bool arm64_has_kernel_sp_store = false; // has set! *kernel-sp* — reset-and-call / thread-resume (NOT set-to-run-bootstrap)
  bool arm64_has_early_pop = false;       // .pop before *kernel-sp* load — thread-suspend
  bool arm64_has_push_near_jr = false;    // .push immediately before .jr — reset-and-call
  bool arm64_has_push_before_jr = false;  // any .push before .jr — set-to-run-bootstrap

  for (int i = 0; i < (int)env->code().size(); i++) {
    auto& cur_ir = env->code().at(i);
    if (dynamic_cast<IR_JumpReg*>(cur_ir.get())) {
      arm64_has_jump_reg = true;
      // Check if there's a .push in the last few IR ops. reset-and-call pushes the
      // return-from-thread trampoline just before .jr; thread-resume only has earlier
      // saved-register pushes and must not pop those as LR.
      for (int j = std::max(0, i - 3); j < i; j++) {
        if (dynamic_cast<IR_AsmPush*>(env->code().at(j).get())) {
          arm64_has_push_near_jr = true;
          break;
        }
      }
      // set-to-run-bootstrap pushes return-from-thread-dead earlier, before argument
      // moves. It does not store *kernel-sp*, unlike thread-resume/reset-and-call.
      for (int j = 0; j < i; j++) {
        if (dynamic_cast<IR_AsmPush*>(env->code().at(j).get())) {
          arm64_has_push_before_jr = true;
          break;
        }
      }
    }
    if (!arm64_has_kernel_sp_load && dynamic_cast<IR_AsmPop*>(cur_ir.get())) {
      arm64_has_early_pop = true;
    }
    if (dynamic_cast<IR_GetSymbolValueAsm*>(cur_ir.get())) {
      if (cur_ir->print().find("*kernel-sp*") != std::string::npos) {
        arm64_has_kernel_sp_load = true;
      }
    }
    if (dynamic_cast<IR_SetSymbolValue*>(cur_ir.get())) {
      if (cur_ir->print().find("*kernel-sp*") != std::string::npos) {
        arm64_has_kernel_sp_store = true;
      }
    }
  }

  // Push X30 at function entry for:
  //   - reset-and-call / thread-resume (has .jr AND stores *kernel-sp*): so the kernel
  //     return addr ends up on the kernel stack for thread-suspend to pop later.
  //     set-to-run-bootstrap also has .jr but does NOT store *kernel-sp*, so it is
  //     excluded — it is entered via BR (tail-jump), not BLR, making X30 stale.
  //   - thread-suspend (early .pop + *kernel-sp* load): so the .pop temp instruction
  //     reads the process return address that BLR stored in X30.
  //   - throw-dispatch (early .pop, NO *kernel-sp* load): x86 replaces the return address
  //     on the stack with the catch handler's address before .ret, so BLR must push X30
  //     first for .pop to consume, then .push puts the catch handler, then BR X30 jumps there.
  const bool arm64_push_lr_at_start =
      (arm64_has_jump_reg && arm64_has_kernel_sp_store) ||
      arm64_has_early_pop;
  // Pop X30 just before .jr so the process function's eventual BR X30 jumps to the
  // return trampoline pushed onto the process stack. This must consume the stack slot:
  // x86 RET pops the return address, while ARM64 RET reads LR and leaves SP unchanged.
  const bool arm64_pop_lr_before_jr =
      arm64_has_push_near_jr ||
      (arm64_has_jump_reg && arm64_has_push_before_jr && !arm64_has_kernel_sp_store);
  // Pop X30 before .ret in functions that restore the kernel stack so that
  // BR X30 (= .ret) jumps back to the original kernel call site.
  // Also applies to throw-dispatch which replaces the return address via .pop/.push.
  const bool arm64_pop_lr_before_ret = arm64_has_kernel_sp_load || arm64_has_early_pop;

  // Prologue: push X30 (ARM64-specific) before any auto-saved callee regs.
  if (arm64_push_lr_at_start) {
    m_gen.add_instr_no_ir(f_rec, IGen::push_gpr64(m_gen, Register(ARM64_REG::X30)),
                          InstructionInfo::Kind::PROLOGUE);
  }

  for (int ir_idx = 0; ir_idx < int(env->code().size()); ir_idx++) {
    auto& ir = env->code().at(ir_idx);
    auto i_rec = m_gen.add_ir(f_rec);
    if (!allocs.stack_ops.at(ir_idx).ops.empty()) {
      throw std::runtime_error("ASM Function used a bonus op.");
    }

    // ARM64-specific: before .jr (BR), pop the trampoline address into X30 so that when the
    // process function returns via BR X30, it lands on return-from-thread/return-from-thread-dead.
    if (arm64_pop_lr_before_jr && dynamic_cast<IR_JumpReg*>(ir.get())) {
      m_gen.add_instr(IGen::pop_gpr64(m_gen, Register(ARM64_REG::X30)), i_rec);
    }

    // ARM64-specific: pop X30 before .ret so BR X30 returns to the kernel call site
    // that was pushed to the kernel stack at function entry (by arm64_push_lr_at_start above).
    if (arm64_pop_lr_before_ret && dynamic_cast<IR_AsmRet*>(ir.get())) {
      m_gen.add_instr(IGen::pop_gpr64(m_gen, Register(ARM64_REG::X30)), i_rec);
    }

    if (dynamic_cast<IR_AsmRet*>(ir.get())) {
      emit_arm64_lr_relative_guard(m_gen, i_rec);
    }

    ir->do_codegen_arm64(&m_gen, allocs, i_rec);
  }
}
