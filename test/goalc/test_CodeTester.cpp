/*!
 * @file test_CodeTester.cpp
 * Tests for the CodeTester, a tool for testing the emitter by emitting code and running it
 * from within the test application.
 *
 * These tests should just make sure the basic functionality of CodeTester works, and that it
 * can generate prologues/epilogues, and execute them without crashing.
 */

#include "goalc/emitter/CodeTester.h"
#include "goalc/emitter/IGen.h"
#include "goalc/emitter/IGenARM64.h"
#include "goalc/emitter/InstructionSet.h"
#include "goalc/emitter/Register.h"
#include "gtest/gtest.h"

using namespace emitter;

TEST(CodeTester, prologue_x86) {
  CodeTester tester;
  tester.init_code_buffer(256);
  tester.emit_push_all_gprs();
  // check we generate the right code for pushing all gpr's
  EXPECT_EQ(tester.dump_to_hex_string(),
            "50 51 52 53 54 55 56 57 41 50 41 51 41 52 41 53 41 54 41 55 41 56 41 57");
}

TEST(CodeTester, prologue_arm64) {
  CodeTester tester(emitter::InstructionSet::ARM64);
  tester.init_code_buffer(256);
  // tester.emit(IGen::push_gpr64(tester.generator(), ARM64_REG::X0));
  // EXPECT_EQ(tester.dump_to_hex_string(), "e0 8f 1f f8");
  tester.emit_push_all_gprs();
  // check we generate the right code for pushing all gpr's
  EXPECT_EQ(tester.dump_to_hex_string(),
            "e0 0f 1f f8 e1 0f 1f f8 e2 0f 1f f8 e3 0f 1f f8 e4 0f 1f f8 e5 0f 1f f8 e6 0f 1f f8 "
            "e7 0f 1f f8 e8 0f 1f f8 e9 0f 1f f8 ea 0f 1f f8 eb 0f 1f f8 ec 0f 1f f8 ed 0f 1f f8 "
            "ee 0f 1f f8 ef 0f 1f f8 f0 0f 1f f8 f1 0f 1f f8 f2 0f 1f f8 f3 0f 1f f8 f4 0f 1f f8 "
            "f5 0f 1f f8 f6 0f 1f f8 f7 0f 1f f8 f8 0f 1f f8 f9 0f 1f f8 fa 0f 1f f8 fb 0f 1f f8 "
            "fc 0f 1f f8 fd 0f 1f f8 fe 0f 1f f8");
}

TEST(CodeTester, epilogue_x86) {
  CodeTester tester;
  tester.init_code_buffer(256);
  tester.emit_pop_all_gprs();
  // check we generate the right code for popping all gpr's
  EXPECT_EQ(tester.dump_to_hex_string(),
            "41 5f 41 5e 41 5d 41 5c 41 5b 41 5a 41 59 41 58 5f 5e 5d 5c 5b 5a 59 58");
}

TEST(CodeTester, epilogue_arm64) {
  CodeTester tester(emitter::InstructionSet::ARM64);
  tester.init_code_buffer(256);
  tester.emit_pop_all_gprs();
  // check we generate the right code for popping all gpr's
  EXPECT_EQ(tester.dump_to_hex_string(),
            "fe 07 41 f8 fd 07 41 f8 fc 07 41 f8 fb 07 41 f8 fa 07 41 f8 f9 07 41 f8 f8 07 41 f8 "
            "f7 07 41 f8 f6 07 41 f8 f5 07 41 f8 f4 07 41 f8 f3 07 41 f8 f2 07 41 f8 f1 07 41 f8 "
            "f0 07 41 f8 ef 07 41 f8 ee 07 41 f8 ed 07 41 f8 ec 07 41 f8 eb 07 41 f8 ea 07 41 f8 "
            "e9 07 41 f8 e8 07 41 f8 e7 07 41 f8 e6 07 41 f8 e5 07 41 f8 e4 07 41 f8 e3 07 41 f8 "
            "e2 07 41 f8 e1 07 41 f8 e0 07 41 f8");
}

TEST(CodeTester, sub_gpr64_imm8_x86) {
  CodeTester tester;
  tester.init_code_buffer(256);
  for (int i = 0; i < 16; i++) {
    tester.emit(IGen::sub_gpr64_imm8s(tester.generator(), i, -1));
  }
  EXPECT_EQ(tester.dump_to_hex_string(true),
            "4883E8FF4883E9FF4883EAFF4883EBFF4883ECFF4883EDFF4883EEFF4883EFFF4983E8FF4983E9FF4983EA"
            "FF4983EBFF4983ECFF4983EDFF4983EEFF4983EFFF");
}

TEST(CodeTester, sub_gpr64_imm8_arm64) {
  CodeTester tester(emitter::InstructionSet::ARM64);
  tester.init_code_buffer(256);
  for (int i = 0; i < 31; i++) {
    tester.emit(IGen::sub_gpr64_imm8s(tester.generator(), i, -1));
  }
  EXPECT_EQ(tester.dump_to_hex_string(true),
            "0004009121040091420400916304009184040091A5040091C6040091E704009108050091290500914A0500"
            "916B0500918C050091AD050091CE050091EF0500911006009131060091520600917306009194060091B506"
            "0091D6060091F706009118070091390700915A0700917B0700919C070091BD070091DE070091");
}

TEST(CodeTester, add_gpr64_imm8_x86) {
  CodeTester tester;
  tester.init_code_buffer(256);
  for (int i = 0; i < 16; i++) {
    tester.emit(IGen::add_gpr64_imm8s(tester.generator(), i, -1));
  }
  EXPECT_EQ(tester.dump_to_hex_string(true),
            "4883C0FF4883C1FF4883C2FF4883C3FF4883C4FF4883C5FF4883C6FF4883C7FF4983C0FF4983C1FF4983C2"
            "FF4983C3FF4983C4FF4983C5FF4983C6FF4983C7FF");
}

TEST(CodeTester, add_gpr64_imm8_arm64) {
  CodeTester tester(emitter::InstructionSet::ARM64);
  tester.init_code_buffer(256);
  for (int i = 0; i < 31; i++) {
    tester.emit(IGen::add_gpr64_imm8s(tester.generator(), i, -1));
  }
  EXPECT_EQ(tester.dump_to_hex_string(true),
            "000400D1210400D1420400D1630400D1840400D1A50400D1C60400D1E70400D1080500D1290500D14A0500"
            "D16B0500D18C0500D1AD0500D1CE0500D1EF0500D1100600D1310600D1520600D1730600D1940600D1B506"
            "00D1D60600D1F70600D1180700D1390700D15A0700D17B0700D19C0700D1BD0700D1DE0700D1");
}

TEST(CodeTester, simd_store_128_x86) {
  CodeTester tester;
  tester.init_code_buffer(256);
  //  movdqa [rbx], xmm3
  //  movdqa [r14], xmm3
  //  movdqa [rbx], xmm14
  //  movdqa [r14], xmm13
  tester.emit(IGen::store128_gpr64_simd128(tester.generator(), RBX, XMM3));
  tester.emit(IGen::store128_gpr64_simd128(tester.generator(), R14, XMM3));
  tester.emit(IGen::store128_gpr64_simd128(tester.generator(), RBX, XMM14));
  tester.emit(IGen::store128_gpr64_simd128(tester.generator(), R14, XMM13));
  EXPECT_EQ(tester.dump_to_hex_string(),
            "66 0f 7f 1b 66 41 0f 7f 1e 66 44 0f 7f 33 66 45 0f 7f 2e");

  tester.clear();
  tester.emit(IGen::store128_gpr64_simd128(tester.generator(), RSP, XMM1));
  EXPECT_EQ(tester.dump_to_hex_string(), "66 0f 7f 0c 24");  // requires SIB byte.

  tester.clear();
  tester.emit(IGen::store128_gpr64_simd128(tester.generator(), R12, XMM13));
  EXPECT_EQ(tester.dump_to_hex_string(), "66 45 0f 7f 2c 24");  // requires SIB byte and REX byte

  tester.clear();
  tester.emit(IGen::store128_gpr64_simd128(tester.generator(), RBP, XMM1));
  EXPECT_EQ(tester.dump_to_hex_string(), "66 0f 7f 4d 00");

  tester.clear();
  tester.emit(IGen::store128_gpr64_simd128(tester.generator(), RBP, XMM11));
  EXPECT_EQ(tester.dump_to_hex_string(), "66 44 0f 7f 5d 00");

  tester.clear();
  tester.emit(IGen::store128_gpr64_simd128(tester.generator(), R13, XMM2));
  EXPECT_EQ(tester.dump_to_hex_string(), "66 41 0f 7f 55 00");

  tester.clear();
  tester.emit(IGen::store128_gpr64_simd128(tester.generator(), R13, XMM12));
  EXPECT_EQ(tester.dump_to_hex_string(), "66 45 0f 7f 65 00");
}

TEST(CodeTester, simd_store_128_arm64) {
  CodeTester tester(emitter::InstructionSet::ARM64);
  tester.init_code_buffer(256);

  tester.emit(IGen::store128_gpr64_simd128(tester.generator(), X2, Q3));
  tester.emit(IGen::store128_gpr64_simd128(tester.generator(), X14, Q3));
  tester.emit(IGen::store128_gpr64_simd128(tester.generator(), X2, Q14));
  tester.emit(IGen::store128_gpr64_simd128(tester.generator(), X14, Q13));
  EXPECT_EQ(tester.dump_to_hex_string(), "43 00 80 3d c3 01 80 3d 4e 00 80 3d cd 01 80 3d");
}

TEST(CodeTester, xmm_load_128_x86) {
  CodeTester tester;
  tester.init_code_buffer(256);

  tester.emit(IGen::load128_simd128_gpr64(tester.generator(), XMM3, RBX));
  tester.emit(IGen::load128_simd128_gpr64(tester.generator(), XMM3, R14));
  tester.emit(IGen::load128_simd128_gpr64(tester.generator(), XMM14, RBX));
  tester.emit(IGen::load128_simd128_gpr64(tester.generator(), XMM13, R14));
  EXPECT_EQ(tester.dump_to_hex_string(),
            "66 0f 6f 1b 66 41 0f 6f 1e 66 44 0f 6f 33 66 45 0f 6f 2e");

  tester.clear();
  tester.emit(IGen::load128_simd128_gpr64(tester.generator(), XMM1, RSP));
  EXPECT_EQ(tester.dump_to_hex_string(), "66 0f 6f 0c 24");  // requires SIB byte.

  tester.clear();
  tester.emit(IGen::load128_simd128_gpr64(tester.generator(), XMM13, R12));
  EXPECT_EQ(tester.dump_to_hex_string(), "66 45 0f 6f 2c 24");  // requires SIB byte and REX byte

  tester.clear();
  tester.emit(IGen::load128_simd128_gpr64(tester.generator(), XMM1, RBP));
  EXPECT_EQ(tester.dump_to_hex_string(), "66 0f 6f 4d 00");

  tester.clear();
  tester.emit(IGen::load128_simd128_gpr64(tester.generator(), XMM11, RBP));
  EXPECT_EQ(tester.dump_to_hex_string(), "66 44 0f 6f 5d 00");

  tester.clear();
  tester.emit(IGen::load128_simd128_gpr64(tester.generator(), XMM2, R13));
  EXPECT_EQ(tester.dump_to_hex_string(), "66 41 0f 6f 55 00");

  tester.clear();
  tester.emit(IGen::load128_simd128_gpr64(tester.generator(), XMM12, R13));
  EXPECT_EQ(tester.dump_to_hex_string(), "66 45 0f 6f 65 00");
}

TEST(CodeTester, xmm_load_128_arm64) {
  CodeTester tester(emitter::InstructionSet::ARM64);
  tester.init_code_buffer(256);

  tester.emit(IGen::load128_simd128_gpr64(tester.generator(), Q3, X1));
  tester.emit(IGen::load128_simd128_gpr64(tester.generator(), Q3, X14));
  tester.emit(IGen::load128_simd128_gpr64(tester.generator(), Q14, X1));
  tester.emit(IGen::load128_simd128_gpr64(tester.generator(), Q13, X14));
  EXPECT_EQ(tester.dump_to_hex_string(), "23 00 c0 3d c3 01 c0 3d 2e 00 c0 3d cd 01 c0 3d");
}

// ZIP1 Vd.2D, Vn.2D(src0), Vm.2D(src1): dst.lo=src0.lo, dst.hi=src1.lo — matching x86 VPUNPCKLQDQ.
// Registers use GOAL SIMD IDs (XMM0..XMM15 = ids 16..31 = ARM64 v16..v31).
TEST(CodeTester, pcpyld_swapped_arm64) {
  CodeTester tester(emitter::InstructionSet::ARM64);
  tester.init_code_buffer(256);
  // pcpyld_swapped(dst=XMM3=id19→Q3, src0=XMM13=id29→Q13, src1=XMM1=id17→Q1): ZIP1 V3.2D,V13.2D,V1.2D
  tester.emit(IGen::pcpyld_swapped(tester.generator(), XMM0 + 3, XMM0 + 13, XMM0 + 1));
  // pcpyld_swapped(dst=XMM8→Q8, src0=XMM1→Q1, src1=XMM9→Q9): ZIP1 V8.2D,V1.2D,V9.2D
  tester.emit(IGen::pcpyld_swapped(tester.generator(), XMM0 + 8, XMM0 + 1, XMM0 + 9));
  // pcpyld_swapped(dst=XMM1→Q1, src0=XMM8→Q8, src1=XMM8→Q8): ZIP1 V1.2D,V8.2D,V8.2D
  tester.emit(IGen::pcpyld_swapped(tester.generator(), XMM0 + 1, XMM0 + 8, XMM0 + 8));
  EXPECT_EQ(tester.dump_to_hex_string(), "a3 39 c1 4e 28 38 c9 4e 01 39 c8 4e");
}

// These tests actually execute the code, you cannot execute arm64 code on x86 and vise versa
// so these tests have to be conditional based on the platform unfortunately.
TEST(CodeTester, execute_push_pop_simd_x86) {
  CodeTester tester;
  tester.init_code_buffer(512);
  tester.emit_push_all_simd();
  tester.emit_pop_all_simd();
  tester.emit_return();
  EXPECT_EQ(
      tester.dump_to_hex_string(),
      "48 83 ec 08 48 83 ec 10 66 0f 7f 04 24 48 83 ec 10 66 0f 7f 0c 24 48 83 ec 10 66 0f 7f 14 "
      "24 48 83 ec 10 66 0f 7f 1c 24 48 83 ec 10 66 0f 7f 24 24 48 83 ec 10 66 0f 7f 2c 24 48 83 "
      "ec 10 66 0f 7f 34 24 48 83 ec 10 66 0f 7f 3c 24 48 83 ec 10 66 44 0f 7f 04 24 48 83 ec 10 "
      "66 44 0f 7f 0c 24 48 83 ec 10 66 44 0f 7f 14 24 48 83 ec 10 66 44 0f 7f 1c 24 48 83 ec 10 "
      "66 44 0f 7f 24 24 48 83 ec 10 66 44 0f 7f 2c 24 48 83 ec 10 66 44 0f 7f 34 24 48 83 ec 10 "
      "66 44 0f 7f 3c 24 66 0f 6f 04 24 48 83 c4 10 66 0f 6f 0c 24 48 83 c4 10 66 0f 6f 14 24 48 "
      "83 c4 10 66 0f 6f 1c 24 48 83 c4 10 66 0f 6f 24 24 48 83 c4 10 66 0f 6f 2c 24 48 83 c4 10 "
      "66 0f 6f 34 24 48 83 c4 10 66 0f 6f 3c 24 48 83 c4 10 66 44 0f 6f 04 24 48 83 c4 10 66 44 "
      "0f 6f 0c 24 48 83 c4 10 66 44 0f 6f 14 24 48 83 c4 10 66 44 0f 6f 1c 24 48 83 c4 10 66 44 "
      "0f 6f 24 24 48 83 c4 10 66 44 0f 6f 2c 24 48 83 c4 10 66 44 0f 6f 34 24 48 83 c4 10 66 44 "
      "0f 6f 3c 24 48 83 c4 10 48 83 c4 08 c3");
#ifndef __aarch64__
  tester.execute();
#endif
}

TEST(CodeTester, execute_push_pop_simd_arm64) {
  CodeTester tester(emitter::InstructionSet::ARM64);
  tester.init_code_buffer(512);
  tester.emit_push_all_simd();
  tester.emit_pop_all_simd();
  tester.emit_return();
  EXPECT_EQ(
      tester.dump_to_hex_string(),
      "ff 43 00 d1 e0 03 80 3d ff 43 00 d1 e1 03 80 3d ff 43 00 d1 e2 03 80 3d ff 43 00 d1 e3 03 "
      "80 3d ff 43 00 d1 e4 03 80 3d ff 43 00 d1 e5 03 80 3d ff 43 00 d1 e6 03 80 3d ff 43 00 d1 "
      "e7 03 80 3d ff 43 00 d1 e8 03 80 3d ff 43 00 d1 e9 03 80 3d ff 43 00 d1 ea 03 80 3d ff 43 "
      "00 d1 eb 03 80 3d ff 43 00 d1 ec 03 80 3d ff 43 00 d1 ed 03 80 3d ff 43 00 d1 ee 03 80 3d "
      "ff 43 00 d1 ef 03 80 3d e0 03 c0 3d ff 43 00 91 e1 03 c0 3d ff 43 00 91 e2 03 c0 3d ff 43 "
      "00 91 e3 03 c0 3d ff 43 00 91 e4 03 c0 3d ff 43 00 91 e5 03 c0 3d ff 43 00 91 e6 03 c0 3d "
      "ff 43 00 91 e7 03 c0 3d ff 43 00 91 e8 03 c0 3d ff 43 00 91 e9 03 c0 3d ff 43 00 91 ea 03 "
      "c0 3d ff 43 00 91 eb 03 c0 3d ff 43 00 91 ec 03 c0 3d ff 43 00 91 ed 03 c0 3d ff 43 00 91 "
      "ee 03 c0 3d ff 43 00 91 ef 03 c0 3d ff 43 00 91 c0 03 5f d6");
#ifdef __aarch64__
  tester.execute();
#endif
}

TEST(CodeTester, execute_push_pop_all_the_things_x86) {
  CodeTester tester;
  tester.init_code_buffer(512);
  tester.emit_push_all_simd();
  tester.emit_push_all_gprs();

  // ...
  tester.emit_pop_all_gprs();
  tester.emit_pop_all_simd();
  tester.emit_return();
  EXPECT_EQ(tester.dump_to_hex_string(),
            "48 83 ec 08 48 83 ec 10 66 0f 7f 04 24 48 83 ec 10 66 0f 7f 0c 24 48 83 ec 10 66 0f "
            "7f 14 24 48 83 ec 10 66 0f 7f 1c 24 48 83 ec 10 66 0f 7f 24 24 48 83 ec 10 66 0f 7f "
            "2c 24 48 83 ec 10 66 0f 7f 34 24 48 83 ec 10 66 0f 7f 3c 24 48 83 ec 10 66 44 0f 7f "
            "04 24 48 83 ec 10 66 44 0f 7f 0c 24 48 83 ec 10 66 44 0f 7f 14 24 48 83 ec 10 66 44 "
            "0f 7f 1c 24 48 83 ec 10 66 44 0f 7f 24 24 48 83 ec 10 66 44 0f 7f 2c 24 48 83 ec 10 "
            "66 44 0f 7f 34 24 48 83 ec 10 66 44 0f 7f 3c 24 50 51 52 53 54 55 56 57 41 50 41 51 "
            "41 52 41 53 41 54 41 55 41 56 41 57 41 5f 41 5e 41 5d 41 5c 41 5b 41 5a 41 59 41 58 "
            "5f 5e 5d 5c 5b 5a 59 58 66 0f 6f 04 24 48 83 c4 10 66 0f 6f 0c 24 48 83 c4 10 66 0f "
            "6f 14 24 48 83 c4 10 66 0f 6f 1c 24 48 83 c4 10 66 0f 6f 24 24 48 83 c4 10 66 0f 6f "
            "2c 24 48 83 c4 10 66 0f 6f 34 24 48 83 c4 10 66 0f 6f 3c 24 48 83 c4 10 66 44 0f 6f "
            "04 24 48 83 c4 10 66 44 0f 6f 0c 24 48 83 c4 10 66 44 0f 6f 14 24 48 83 c4 10 66 44 "
            "0f 6f 1c 24 48 83 c4 10 66 44 0f 6f 24 24 48 83 c4 10 66 44 0f 6f 2c 24 48 83 c4 10 "
            "66 44 0f 6f 34 24 48 83 c4 10 66 44 0f 6f 3c 24 48 83 c4 10 48 83 c4 08 c3");
#ifndef __aarch64__
  tester.execute();
#endif
}

TEST(CodeTester, execute_push_pop_all_the_things_arm64) {
  CodeTester tester(emitter::InstructionSet::ARM64);
  tester.init_code_buffer(512);
  tester.emit_push_all_simd();
  tester.emit_push_all_gprs();

  // ...
  tester.emit_pop_all_gprs();
  tester.emit_pop_all_simd();
  tester.emit_return();
  EXPECT_EQ(
      tester.dump_to_hex_string(),
      "ff 43 00 d1 e0 03 80 3d ff 43 00 d1 e1 03 80 3d ff 43 00 d1 e2 03 80 3d ff 43 00 d1 e3 03 "
      "80 3d ff 43 00 d1 e4 03 80 3d ff 43 00 d1 e5 03 80 3d ff 43 00 d1 e6 03 80 3d ff 43 00 d1 "
      "e7 03 80 3d ff 43 00 d1 e8 03 80 3d ff 43 00 d1 e9 03 80 3d ff 43 00 d1 ea 03 80 3d ff 43 "
      "00 d1 eb 03 80 3d ff 43 00 d1 ec 03 80 3d ff 43 00 d1 ed 03 80 3d ff 43 00 d1 ee 03 80 3d "
      "ff 43 00 d1 ef 03 80 3d e0 0f 1f f8 e1 0f 1f f8 e2 0f 1f f8 e3 0f 1f f8 e4 0f 1f f8 e5 0f "
      "1f f8 e6 0f 1f f8 e7 0f 1f f8 e8 0f 1f f8 e9 0f 1f f8 ea 0f 1f f8 eb 0f 1f f8 ec 0f 1f f8 "
      "ed 0f 1f f8 ee 0f 1f f8 ef 0f 1f f8 f0 0f 1f f8 f1 0f 1f f8 f2 0f 1f f8 f3 0f 1f f8 f4 0f "
      "1f f8 f5 0f 1f f8 f6 0f 1f f8 f7 0f 1f f8 f8 0f 1f f8 f9 0f 1f f8 fa 0f 1f f8 fb 0f 1f f8 "
      "fc 0f 1f f8 fd 0f 1f f8 fe 0f 1f f8 fe 07 41 f8 fd 07 41 f8 fc 07 41 f8 fb 07 41 f8 fa 07 "
      "41 f8 f9 07 41 f8 f8 07 41 f8 f7 07 41 f8 f6 07 41 f8 f5 07 41 f8 f4 07 41 f8 f3 07 41 f8 "
      "f2 07 41 f8 f1 07 41 f8 f0 07 41 f8 ef 07 41 f8 ee 07 41 f8 ed 07 41 f8 ec 07 41 f8 eb 07 "
      "41 f8 ea 07 41 f8 e9 07 41 f8 e8 07 41 f8 e7 07 41 f8 e6 07 41 f8 e5 07 41 f8 e4 07 41 f8 "
      "e3 07 41 f8 e2 07 41 f8 e1 07 41 f8 e0 07 41 f8 e0 03 c0 3d ff 43 00 91 e1 03 c0 3d ff 43 "
      "00 91 e2 03 c0 3d ff 43 00 91 e3 03 c0 3d ff 43 00 91 e4 03 c0 3d ff 43 00 91 e5 03 c0 3d "
      "ff 43 00 91 e6 03 c0 3d ff 43 00 91 e7 03 c0 3d ff 43 00 91 e8 03 c0 3d ff 43 00 91 e9 03 "
      "c0 3d ff 43 00 91 ea 03 c0 3d ff 43 00 91 eb 03 c0 3d ff 43 00 91 ec 03 c0 3d ff 43 00 91 "
      "ed 03 c0 3d ff 43 00 91 ee 03 c0 3d ff 43 00 91 ef 03 c0 3d ff 43 00 91 c0 03 5f d6");
#ifdef __aarch64__
  tester.execute();
#endif
}

TEST(CodeTester, execute_return_x86) {
  CodeTester tester;
  tester.init_code_buffer(256);
  // test creating a function which simply returns
  tester.emit_return();
  EXPECT_EQ(tester.dump_to_hex_string(), "c3");
  // and execute it!
#ifndef __aarch64__
  tester.execute();
#endif
}

TEST(CodeTester, execute_return_arm64) {
  CodeTester tester(emitter::InstructionSet::ARM64);
  tester.init_code_buffer(256);
  // test creating a function which simply returns
  tester.emit(IGen::add_gpr64_imm8s(tester.generator(), ARM64_REG::X0, 1));
  tester.emit(IGen::ret(tester.generator()));
  EXPECT_EQ(tester.dump_to_hex_string(), "00 04 00 91 c0 03 5f d6");
  // and execute it!
#ifdef __aarch64__
  tester.execute();
#endif
}

TEST(CodeTester, execute_push_pop_gprs_x86) {
  CodeTester tester;
  tester.init_code_buffer(256);
  // test we can push/pop gprs without crashing.
  tester.emit_push_all_gprs();
  tester.emit_pop_all_gprs();
  tester.emit_return();
  EXPECT_EQ(tester.dump_to_hex_string(),
            "50 51 52 53 54 55 56 57 41 50 41 51 41 52 41 53 41 54 41 55 41 56 41 57 41 5f 41 5e "
            "41 5d 41 5c 41 5b 41 5a 41 59 41 58 5f 5e 5d 5c 5b 5a 59 58 c3");
#ifndef __aarch64__
  tester.execute();
#endif
}

TEST(CodeTester, execute_push_pop_gprs_arm64) {
  CodeTester tester(emitter::InstructionSet::ARM64);
  tester.init_code_buffer(256);
  // test we can push/pop gprs without crashing.
  tester.emit_push_all_gprs();
  tester.emit_pop_all_gprs();
  tester.emit_return();
  EXPECT_EQ(tester.dump_to_hex_string(),
            "e0 0f 1f f8 e1 0f 1f f8 e2 0f 1f f8 e3 0f 1f f8 e4 0f 1f f8 e5 0f 1f f8 e6 0f 1f f8 "
            "e7 0f 1f f8 e8 0f 1f f8 e9 0f 1f f8 ea 0f 1f f8 eb 0f 1f f8 ec 0f 1f f8 ed 0f 1f f8 "
            "ee 0f 1f f8 ef 0f 1f f8 f0 0f 1f f8 f1 0f 1f f8 f2 0f 1f f8 f3 0f 1f f8 f4 0f 1f f8 "
            "f5 0f 1f f8 f6 0f 1f f8 f7 0f 1f f8 f8 0f 1f f8 f9 0f 1f f8 fa 0f 1f f8 fb 0f 1f f8 "
            "fc 0f 1f f8 fd 0f 1f f8 fe 0f 1f f8 fe 07 41 f8 fd 07 41 f8 fc 07 41 f8 fb 07 41 f8 "
            "fa 07 41 f8 f9 07 41 f8 f8 07 41 f8 f7 07 41 f8 f6 07 41 f8 f5 07 41 f8 f4 07 41 f8 "
            "f3 07 41 f8 f2 07 41 f8 f1 07 41 f8 f0 07 41 f8 ef 07 41 f8 ee 07 41 f8 ed 07 41 f8 "
            "ec 07 41 f8 eb 07 41 f8 ea 07 41 f8 e9 07 41 f8 e8 07 41 f8 e7 07 41 f8 e6 07 41 f8 "
            "e5 07 41 f8 e4 07 41 f8 e3 07 41 f8 e2 07 41 f8 e1 07 41 f8 e0 07 41 f8 c0 03 5f d6");
#ifdef __aarch64__
  tester.execute();
#endif
}

// Verify add/sub/mov_gpr64_gpr64 use extended-register form when dst=SP (id=31),
// preventing the shifted-register Rd=31→XZR bug that made all SP arithmetic a NOP.
TEST(CodeTester, add_sub_mov_sp_arm64) {
  using namespace emitter;
  using emitter::ARM64_REG;
  CodeTester tester(InstructionSet::ARM64);
  tester.init_code_buffer(64);
  // ADD SP, SP, X0, UXTX #0  → 0x8B2063FF → ff 63 20 8b
  tester.emit(IGen::ARM64::add_gpr64_gpr64(Register(ARM64_REG::SP), Register(ARM64_REG::X0)));
  // SUB SP, SP, X0, UXTX #0  → 0xCB2063FF → ff 63 20 cb
  tester.emit(IGen::ARM64::sub_gpr64_gpr64(Register(ARM64_REG::SP), Register(ARM64_REG::X0)));
  // MOV SP, X0 = ADD SP, X0, #0  → 0x9100001F → 1f 00 00 91
  tester.emit(IGen::ARM64::mov_gpr64_gpr64(Register(ARM64_REG::SP), Register(ARM64_REG::X0)));
  // MOV X0, SP = ADD X0, SP, #0  → 0x910003E0 → e0 03 00 91
  tester.emit(IGen::ARM64::mov_gpr64_gpr64(Register(ARM64_REG::X0), Register(ARM64_REG::SP)));
  EXPECT_EQ(tester.dump_to_hex_string(), "ff 63 20 8b ff 63 20 cb 1f 00 00 91 e0 03 00 91");
}

// Tests for new pre/post-indexed SIMD push/pop instructions (push_xmm128, pop_xmm128)
// and STP/LDP pair variants (stp_xmm128_pair, ldp_xmm128_pair).
// These are the canonical callee-saved Q-register save/restore path for ARM64 GOAL functions.
TEST(CodeTester, push_xmm128_arm64) {
  CodeTester tester(emitter::InstructionSet::ARM64);
  tester.init_code_buffer(64);
  // STR Q8, [SP, #-16]! — XMM8 (id=24) maps to physical Q8 via qreg()
  tester.emit(IGen::ARM64::push_xmm128(emitter::Register(emitter::XMM8)));
  // STR Q9, [SP, #-16]! — XMM9 (id=25) maps to physical Q9
  tester.emit(IGen::ARM64::push_xmm128(emitter::Register(emitter::XMM9)));
  // LDR Q9, [SP], #16
  tester.emit(IGen::ARM64::pop_xmm128(emitter::Register(emitter::XMM9)));
  // LDR Q8, [SP], #16
  tester.emit(IGen::ARM64::pop_xmm128(emitter::Register(emitter::XMM8)));
  // Expected encodings (little-endian):
  //   push Q8:  0x3C9F0FE8 -> e8 0f 9f 3c
  //   push Q9:  0x3C9F0FE9 -> e9 0f 9f 3c
  //   pop  Q9:  0x3CC107E9 -> e9 07 c1 3c
  //   pop  Q8:  0x3CC107E8 -> e8 07 c1 3c
  EXPECT_EQ(tester.dump_to_hex_string(), "e8 0f 9f 3c e9 0f 9f 3c e9 07 c1 3c e8 07 c1 3c");
}

// 3-register path: STP(r0,r1) then STR(r2); epilogue: LDR(r2) then LDP(r0,r1).
TEST(CodeTester, push_pop_xmm128_three_regs_arm64) {
  CodeTester tester(emitter::InstructionSet::ARM64);
  tester.init_code_buffer(64);
  // Prologue: pair then single
  tester.emit(IGen::ARM64::stp_xmm128_pair(emitter::Register(emitter::XMM8),
                                            emitter::Register(emitter::XMM9)));
  tester.emit(IGen::ARM64::push_xmm128(emitter::Register(emitter::XMM10)));
  // Epilogue (reverse): pop single, then pair
  tester.emit(IGen::ARM64::pop_xmm128(emitter::Register(emitter::XMM10)));
  tester.emit(IGen::ARM64::ldp_xmm128_pair(emitter::Register(emitter::XMM8),
                                            emitter::Register(emitter::XMM9)));
  // Expected (little-endian, qreg: XMM8→Q8, XMM9→Q9, XMM10→Q10):
  //   stp Q8,Q9  [SP-32]!: 0xADBF27E8 -> e8 27 bf ad
  //   str Q10    [SP-16]!: 0x3C9F0FEA -> ea 0f 9f 3c
  //   ldr Q10    [SP],#16: 0x3CC107EA -> ea 07 c1 3c
  //   ldp Q8,Q9  [SP],#32: 0xACC127E8 -> e8 27 c1 ac
  EXPECT_EQ(tester.dump_to_hex_string(), "e8 27 bf ad ea 0f 9f 3c ea 07 c1 3c e8 27 c1 ac");
}

TEST(CodeTester, stp_ldp_xmm128_pair_arm64) {
  CodeTester tester(emitter::InstructionSet::ARM64);
  tester.init_code_buffer(64);
  // STP Q8, Q9, [SP, #-32]! — XMM8(id=24)→Q8, XMM9(id=25)→Q9 via qreg()
  tester.emit(IGen::ARM64::stp_xmm128_pair(emitter::Register(emitter::XMM8),
                                            emitter::Register(emitter::XMM9)));
  // LDP Q8, Q9, [SP], #32
  tester.emit(IGen::ARM64::ldp_xmm128_pair(emitter::Register(emitter::XMM8),
                                            emitter::Register(emitter::XMM9)));
  // Expected encodings (little-endian):
  //   stp Q8,Q9: 0xADBF27E8 -> e8 27 bf ad
  //   ldp Q8,Q9: 0xACC127E8 -> e8 27 c1 ac
  EXPECT_EQ(tester.dump_to_hex_string(), "e8 27 bf ad e8 27 c1 ac");
}

// Regression tests: verify qreg() maps GOAL XMM IDs (16-31) → physical Q0-Q15.
// Before the fix, all SIMD helpers encoded Q16-Q31 (ARM64 caller-saved) by using
// reg.id() directly instead of reg.id()-16.

TEST(CodeTester, fmov_gpr_xmm_arm64) {
  CodeTester tester(emitter::InstructionSet::ARM64);
  tester.init_code_buffer(64);
  // FMOV X0, D8  (movq_gpr64_xmm64: dst=X0, src=XMM8=id24→Q8/D8)
  // 0x9E660000 | Rn(8)<<5 | Rd(0) = 0x9E660100
  tester.emit(IGen::ARM64::movq_gpr64_xmm64(emitter::Register(emitter::X0),
                                             emitter::Register(emitter::XMM8)));
  // FMOV D8, X1  (movq_xmm64_gpr64: dst=XMM8=id24→Q8/D8, src=X1)
  // 0x9E670000 | Rn(1)<<5 | Rd(8) = 0x9E670028
  tester.emit(IGen::ARM64::movq_xmm64_gpr64(emitter::Register(emitter::XMM8),
                                             emitter::Register(emitter::X1)));
  EXPECT_EQ(tester.dump_to_hex_string(), "00 01 66 9e 28 00 67 9e");
}

TEST(CodeTester, scalar_float_math_arm64) {
  CodeTester tester(emitter::InstructionSet::ARM64);
  tester.init_code_buffer(64);
  // FMUL S8, S8, S9  (mulss: XMM8→Q8, XMM9→Q9)
  // 0x1E200800 | Rm(9)<<16 | Rn(8)<<5 | Rd(8) = 0x1E290908
  tester.emit(IGen::ARM64::mulss_xmm_xmm(emitter::Register(emitter::XMM8),
                                          emitter::Register(emitter::XMM9)));
  // FADD S8, S8, S9
  // 0x1E202800 | Rm(9)<<16 | Rn(8)<<5 | Rd(8) = 0x1E292908
  tester.emit(IGen::ARM64::addss_xmm_xmm(emitter::Register(emitter::XMM8),
                                          emitter::Register(emitter::XMM9)));
  // FSUB S8, S8, S9: 0x1E203800 | Rm(9)<<16 | Rn(8)<<5 | Rd(8) = 0x1E293908
  tester.emit(IGen::ARM64::subss_xmm_xmm(emitter::Register(emitter::XMM8),
                                          emitter::Register(emitter::XMM9)));
  EXPECT_EQ(tester.dump_to_hex_string(), "08 09 29 1e 08 29 29 1e 08 39 29 1e");
}

TEST(CodeTester, mov_vf_vf_arm64) {
  CodeTester tester(emitter::InstructionSet::ARM64);
  tester.init_code_buffer(32);
  // MOV V8.16B, V9.16B  (mov_vf_vf: XMM8→Q8=dst, XMM9→Q9=src)
  // ORR Vd.16B, Vn, Vm  = 0x4EA01C00 | Rm(9)<<16 | Rn(9)<<5 | Rd(8) = 0x4EA91D28
  tester.emit(IGen::ARM64::mov_vf_vf(emitter::Register(emitter::XMM8),
                                     emitter::Register(emitter::XMM9)));
  EXPECT_EQ(tester.dump_to_hex_string(), "28 1d a9 4e");
}

TEST(CodeTester, add_vf_arm64) {
  CodeTester tester(emitter::InstructionSet::ARM64);
  tester.init_code_buffer(32);
  // FADD V8.4S, V9.4S, V10.4S  (add_vf: dst=XMM8→Q8, src1=XMM9→Q9, src2=XMM10→Q10)
  // 0x4E20D400 | Rm(10)<<16 | Rn(9)<<5 | Rd(8) = 0x4E2AD528
  tester.emit(IGen::ARM64::add_vf(emitter::Register(emitter::XMM8),
                                   emitter::Register(emitter::XMM9),
                                   emitter::Register(emitter::XMM10)));
  EXPECT_EQ(tester.dump_to_hex_string(), "28 d5 2a 4e");
}