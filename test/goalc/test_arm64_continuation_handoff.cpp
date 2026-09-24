/*!
 * Adapted from the ARM64 differential test suite in
 * https://github.com/nikolasburns/jak-arm64-macos (ISC licensed), which in turn builds on
 * https://github.com/DiMiTriFrog/jak2-macos-arm64. Their fork and this one implement the ARM64
 * backend independently, so these tests are imported as an external check on our emitter rather
 * than as a description of it: a failure here is a real difference that has to be explained, not
 * a test to be edited until it passes.
 */

// ARM64 continuation-handoff sentinels.
//
// WHY THIS FILE EXISTS
// --------------------
// Two shipped bugs in this port shared one shape: GOAL asm-func code hands a
// continuation ("when you're done, go HERE") to a jump target using the x86
// idiom "push the return address, then jump/return". Correct on x86, where
// `ret` pops the pushed address. Silently wrong on ARM64, where `ret` and `br`
// use x30 and ignore the stack entirely.
//
//   1. `(method deactivate process)` and `enter-state` used a raw
//      `(.push temp) (.ret)`. ARM64 `ret` branched to a stale x30, returning to
//      the caller and recursing until the 8 MB dram stack was exhausted.
//   2. `enter-state`'s trampoline `.push` was emitted from an
//      UNCONSTRAINED register, so it never hit the `src_reg == X8` case in
//      IR_AsmPush::do_codegen_arm64 and never became `mov x30, x8`. x30 was
//      therefore never loaded before `(.jr func)`.
//
// The compiler's whole defence against this class is ONE codegen decision:
//
//   IR_AsmPush::do_codegen_arm64 (goalc/compiler/IR.cpp) emits
//     `mov x30, x8`            when the push source is X8 (historical rax role)
//     `str reg,[sp,#-0x10]!`   otherwise.
//
// WHAT THESE TESTS ASSERT, AND WHY AT THIS LAYER
// ----------------------------------------------
// They drive IR_AsmPush directly and assert on the EMITTED BYTES. An earlier
// draft executed hand-written `mov x30, x8` sequences instead; that verified
// ARM64 hardware semantics (never in doubt) rather than the compiler's
// decision, and a mutation disabling the src_reg==X8 branch left it PASSING.
// Asserting on emitted bytes is what the mutation can actually kill. It also
// needs no execution, so it cannot hang.
//
// (Historical irony worth keeping: the return-linkage test hung on a
// return-linkage bug. Installing a continuation into x30 destroyed the test
// harness's own return address, and the continuation returned to itself.)
//
// MUTATION-VERIFIED: with `src_reg == X8` disabled in IR_AsmPush, both
// PushFromRaxRoleInstallsX30 and the X8 row of PushIdiomLoweringTable FAIL.
// Mutation procedure and rationale: see PORTING-NOTES.md and CASE-STUDIES.md.

#include <cstring>
#include <memory>
#include <vector>

#include "common/type_system/TypeSystem.h"

#include "goalc/compiler/IR.h"
#include "goalc/debugger/DebugInfo.h"
#include "goalc/emitter/CodeTester.h"
#include "goalc/emitter/IGen.h"
#include "goalc/emitter/IGenARM64.h"
#include "goalc/emitter/ObjectGenerator.h"
#include "test/goalc/arm64_test_compat.h"

#include "gtest/gtest.h"

using namespace emitter;

namespace {

// Same harness shape as test_arm64_ir_asm_basic.cpp: build one function, pin a
// virtual register to a chosen hardware register, run a single IR node's ARM64
// codegen, and read back the bytes it produced.
struct PushHarness {
  ObjectGenerator gen{GameVersion::Jak1, InstructionSet::ARM64};
  TypeSystem ts;
  FunctionDebugInfo dbg;
  FunctionRecord func;
  AllocationResult allocs;

  PushHarness() {
    ts.add_builtin_types(GameVersion::Jak1);
    dbg.name = "continuation-handoff-test";
    func = gen.add_function_to_seg(0, &dbg);
  }

  void assign(int id, Register reg, int n_ir) {
    std::vector<bool> live(n_ir, true);
    std::vector<Assignment> ass(n_ir);
    for (auto& a : ass) {
      a.kind = Assignment::Kind::REGISTER;
      a.reg = reg;
    }
    if (allocs.ass_as_ranges.size() <= size_t(id)) {
      std::vector<bool> dummy_live(1, true);
      std::vector<Assignment> dummy_ass(1);
      allocs.ass_as_ranges.resize(id + 1, AssignmentRange(0, dummy_live, dummy_ass));
    }
    allocs.ass_as_ranges[id] = AssignmentRange(0, live, ass);
  }

  std::vector<u8> generate() { return gen.generate_data_v3(&ts).segment_data.at(0); }
};

std::unique_ptr<RegVal> make_reg(int id, const TypeSpec& ts) {
  IRegister ireg;
  ireg.id = id;
  ireg.reg_class = RegClass::GPR_64;
  return std::make_unique<RegVal>(ireg, ts);
}

// Emit exactly one IR_AsmPush whose source is pinned to `src`, and return the
// 4-byte instruction word it produced.
u32 push_word_for(Register src) {
  PushHarness h;
  auto reg = make_reg(0, TypeSpec("int"));
  h.assign(0, src, 1);
  IR_Record ir = h.gen.add_ir(h.func);
  IR_AsmPush push(true, reg.get());
  push.do_codegen_arm64(&h.gen, h.allocs, ir);
  auto data = h.generate();
  // generate_data_v3 prefixes the segment with a 4-byte header word.
  EXPECT_GE(data.size(), 8u);
  u32 word = 0;
  std::memcpy(&word, data.data() + 4, sizeof(word));
  return word;
}

// `mov x30, x8` is ORR X30, XZR, X8 == 0xAA0803FE.
constexpr u32 kMovX30X8 = 0xAA0803FE;

// `str <Xt>, [sp, #-0x10]!` (pre-index, imm9 = -16), Rn = SP = 31.
// Instructions differ only in Rt, bits [4:0].
constexpr u32 kStrPreIndexBase = 0xF81F0FE0;
u32 str_pre_index_for(Register reg) {
  return kStrPreIndexBase | (u32(reg.id()) & 0x1F);
}

}  // namespace


// The complement: a push from any NON-rax register must stay a real stack push
// and must NOT install x30. This is what made the trampoline bug possible (an
// unconstrained `temp` landed in x4), so it is pinned as intended behaviour
// rather than left implicit.
// REMOVED IN THIS FORK: PushFromRaxRoleInstallsX30, PushIdiomLoweringTable.
//
// Both assert that IR_AsmPush with an X8 (rax-role) source lowers to `mov x30, <src>`, because on
// ARM64 `ret` reads x30 and ignores the stack. The source fork solves the continuation handoff in
// the lowering, automatically and at every site.
//
// We solve it in the GOAL source instead: each site that relies on "push an address, then ret"
// carries an `#if ARM64_PORT (.mov lr temp)` next to the `(.push temp)` -- see gkernel.gc:500,
// gkernel.gc:1650, gkernel.gc:2040 and gstate.gc:463. So our IR_AsmPush deliberately stays a real
// stack push, and the two tests describe a lowering we do not have.
//
// Their approach is the more robust of the two: ours is only correct as long as every site is
// guarded, and a new unguarded one fails silently. The remaining `(.ret)` sites without such a
// guard -- return-from-thread (gkernel.gc:452), return-from-thread-dead (gkernel.gc:482),
// thread-suspend (gkernel.gc:643) and new catch-frame (gkernel.gc:1579) -- reach `ret` with x30
// set by the caller or by asm_funcs_arm64.s rather than by a push. That holds today; it is not
// enforced anywhere. Tracked in CODEX.md.
//
// PushFromOtherRegisterStaysAStackPush and RetTransfersToX30NotTheStack below still apply and
// are kept.

TEST(Arm64ContinuationHandoff, PushFromOtherRegisterStaysAStackPush) {
  for (Register src : {X4, X9, X19}) {
    const u32 word = push_word_for(src);
    EXPECT_EQ(str_pre_index_for(src), word)
        << "IR_AsmPush from x" << src.id() << " should emit `str x" << src.id()
        << ", [sp, #-0x10]!` (got 0x" << std::hex << word << ")";
    EXPECT_NE(kMovX30X8, word)
        << "IR_AsmPush from x" << src.id()
        << " unexpectedly installed x30; only the rax/X8 role may do that.";
  }
}


#if defined(__aarch64__)
// ARM64 HARDWARE SEMANTICS -- NOT compiler coverage.
//
// Documents the platform property that makes the x86 push/ret idiom unsafe:
// `ret` transfers to x30, so installing a value there redirects the return.
// Deliberately labelled -- a mutation of our emitter does NOT and SHOULD NOT
// affect it. Do not mistake a green result here for evidence that the lowering
// works; that is what PushFromRaxRoleInstallsX30 is for.
TEST(Arm64HardwareSemantics, RetTransfersToX30NotTheStack) {
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(256);
  // Preserve the harness return address, install arg0 in x30, read it back,
  // then restore. (Failing to preserve it is what made an earlier draft spin.)
  t.emit(IGen::mov_gpr64_gpr64(t.generator(), X19, X30));
  t.emit(IGen::mov_gpr64_gpr64(t.generator(), X30, X0));
  t.emit(IGen::mov_gpr64_gpr64(t.generator(), X0, X30));
  t.emit(IGen::mov_gpr64_gpr64(t.generator(), X30, X19));
  t.emit_return();
  EXPECT_EQ(0x5AFEC0DE5AFEC0DEull, t.execute(0x5AFEC0DE5AFEC0DEull, 0, 0, 0));
}
#endif  // __aarch64__
