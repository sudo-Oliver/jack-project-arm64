/*!
 * Adapted from the ARM64 differential test suite in
 * https://github.com/nikolasburns/jak-arm64-macos (ISC licensed), which in turn builds on
 * https://github.com/DiMiTriFrog/jak2-macos-arm64. Their fork and this one implement the ARM64
 * backend independently, so these tests are imported as an external check on our emitter rather
 * than as a description of it: a failure here is a real difference that has to be explained, not
 * a test to be edited until it passes.
 */

#include <cstring>
#include <memory>

#include "common/link_types.h"
#include "common/type_system/TypeSystem.h"

#include "goalc/compiler/IR.h"
#include "goalc/compiler/StaticObject.h"
#include "goalc/debugger/DebugInfo.h"
#include "goalc/emitter/CodeTester.h"
#include "goalc/emitter/IGen.h"
#include "goalc/emitter/IGenARM64.h"
#include "goalc/emitter/ObjectGenerator.h"

#include "test/goalc/arm64_test_compat.h"

#include "gtest/gtest.h"

using namespace emitter;
using namespace emitter::IGen;
using namespace emitter::IGen::ARM64;

namespace {

struct IRHarness {
  ObjectGenerator gen{GameVersion::Jak2, InstructionSet::ARM64};
  TypeSystem ts;
  FunctionDebugInfo dbg;
  FunctionRecord func;

  IRHarness() {
    ts.add_builtin_types(GameVersion::Jak2);
    dbg.name = "sym-test";
    func = gen.add_function_to_seg(0, &dbg);
  }

  void assign(int id, Register reg, int n_ir) {
    std::vector<bool> live(n_ir, true);
    std::vector<Assignment> ass(n_ir);
    for (auto& a : ass) {
      a.kind = Assignment::Kind::REGISTER;
      a.reg = reg;
    }
    if (allocs.ass_as_ranges.size() <= (size_t)id) {
      std::vector<bool> dummy_live(1, true);
      std::vector<Assignment> dummy_ass(1);
      allocs.ass_as_ranges.resize(id + 1, AssignmentRange(0, dummy_live, dummy_ass));
    }
    allocs.ass_as_ranges[id] = AssignmentRange(0, live, ass);
  }

  AllocationResult allocs;

  // Append a ret instruction (the epilogue role) before the static literals so
  // execution does not fall through into the data words.
  void finish() {
    IR_Record irr = gen.add_ir(func);
    gen.add_instr(Instruction(IGen::ARM64::ret()), irr);
  }

  std::vector<u8> generate() { return gen.generate_data_v3(&ts).segment_data.at(0); }
  const std::vector<u8>& link_table() { return m_link; }

  // Run the generated instructions (skip the type tag) with a trailing ret.
  // st (x21) and offset (x22) are set from x2/x3 so that x0/x1 stay free for
  // the generated code's value registers.
  u64 execute_instrs(const std::vector<u8>& data, size_t start, u64 st, u64 offset, u64 in0,
                     u64 in1) {
#ifdef __aarch64__
    CodeTester t(InstructionSet::ARM64);
    // ARM64 object generation page-aligns static data so code pages can be RX
    // while static data remains writable.
    t.init_code_buffer(0x10000);
    t.emit(IGen::mov_gpr64_gpr64(t.generator(), X21, X2));
    t.emit(IGen::mov_gpr64_gpr64(t.generator(), X22, X3));
    // copy the whole tail (instructions + trailing statics) so the patched
    // literal offsets remain valid.
    t.append_bytes(data.data() + start, (int)(data.size() - start));
    t.emit_return();
    return t.execute(in0, in1, st, offset);
#else
    return 0;
#endif
  }

  // Find the literal .quad: with one function and one static it is the last
  // 8 bytes of the segment data.
  size_t literal_offset(const std::vector<u8>& data) const { return data.size() - 8; }

 private:
  std::vector<u8> m_link;
};

std::unique_ptr<RegVal> make_reg(int id, RegClass cls, const TypeSpec& ts) {
  IRegister ireg;
  ireg.id = id;
  ireg.reg_class = cls;
  return std::make_unique<RegVal>(ireg, ts);
}

u32 read_word(const std::vector<u8>& data, size_t off) {
  u32 w;
  memcpy(&w, data.data() + off, 4);
  return w;
}

bool has_link_kind(const std::vector<u8>& link, u8 kind) {
  for (size_t i = 0; i < link.size(); i++) {
    if (link[i] == kind) {
      return true;
    }
  }
  return false;
}

}  // namespace

// REMOVED IN THIS FORK: LoadSymbolPointerNamed, SetSymbolValue, GetSymbolValue, StaticVarAddr,
// StaticVarLoadFloat.
//
// All five read and patch an 8-byte literal at the end of the segment: the source fork resolves a
// symbol by loading its offset from a literal pool. We do not have that pool. Our
// IR_LoadSymbolPointer emits MOVZ/MOVK with the offset patched into the instruction immediates by
// the linker (link_instruction_symbol_arm64_movw), then MOVSX + ADD -- four instructions, no
// load, and the sign-extension these tests were written to check is already explicit in ours
// (IR.cpp, IR_LoadSymbolPointer::do_codegen_arm64).
//
// Equivalents for the MOVZ/MOVK mechanism are still owed: they have to simulate the linker by
// patching the two instruction immediates before executing. Tracked in CODEX.md.

TEST(ARM64IRSymbols, LoadSymbolPointerFalse) {
  IRHarness h;
  IR_Record ir0 = h.gen.add_ir(h.func);
  auto dest = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
  h.assign(0, X0, 1);
  IR_LoadSymbolPointer ir(dest.get(), "#f");
  ir.do_codegen_arm64(&h.gen, h.allocs, ir0);
  auto data = h.generate();
  // mov x0, x21 (st)
  EXPECT_EQ(read_word(data, 4), 0xAA1503E0u);
}

TEST(ARM64IRSymbols, LoadSymbolPointerTrue) {
  IRHarness h;
  IR_Record ir0 = h.gen.add_ir(h.func);
  auto dest = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
  h.assign(0, X0, 1);
  IR_LoadSymbolPointer ir(dest.get(), "#t");
  ir.do_codegen_arm64(&h.gen, h.allocs, ir0);
  auto data = h.generate();
  // lea x0, [x21, #offset]: add x0, x21, #imm
  EXPECT_EQ((read_word(data, 4) >> 5) & 0x1F, 21u);
}






TEST(ARM64IRSymbols, LinkTableHasPointerKind) {
  // StaticVarAddr produces a LINK_PTR entry in the link table.
  IRHarness h;
  IR_Record ir0 = h.gen.add_ir(h.func);
  auto lit = h.gen.add_static_to_seg(0);
  h.gen.get_static_data(lit).assign(8, 0);
  auto target = h.gen.add_static_to_seg(0);
  h.gen.get_static_data(target).assign(4, 0);
  h.gen.link_static_pointer_to_data(lit, 0, target, 0);
  // Our ObjectGenerator emits no segment for a function with no instructions, so give it one.
  IR_Nop nop;
  nop.do_codegen_arm64(&h.gen, h.allocs, ir0);
  auto obj = h.gen.generate_data_v3(&h.ts);
  EXPECT_TRUE(has_link_kind(obj.link_tables.at(0), 5));
}
