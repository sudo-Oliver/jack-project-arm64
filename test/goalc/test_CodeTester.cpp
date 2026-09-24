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

TEST(CodeTester, ins_vf_element_arm64) {
  // INS Vd.S[d], Vn.S[s] for every lane pair, checked against the system assembler.
  // In imm5 the lowest set bit selects the element size, so a wrong shift silently aliases
  // lanes together and can even select D-sized elements.
  const char* expected[4][4] = {
      {"28 05 04 6e", "28 25 04 6e", "28 45 04 6e", "28 65 04 6e"},
      {"28 05 0c 6e", "28 25 0c 6e", "28 45 0c 6e", "28 65 0c 6e"},
      {"28 05 14 6e", "28 25 14 6e", "28 45 14 6e", "28 65 14 6e"},
      {"28 05 1c 6e", "28 25 1c 6e", "28 45 1c 6e", "28 65 1c 6e"}};
  for (u8 d = 0; d < 4; d++) {
    for (u8 sIdx = 0; sIdx < 4; sIdx++) {
      CodeTester tester(emitter::InstructionSet::ARM64);
      tester.init_code_buffer(32);
      tester.emit(IGen::ARM64::ins_vf_element(emitter::Register(emitter::XMM8), d,
                                              emitter::Register(emitter::XMM9), sIdx));
      EXPECT_EQ(tester.dump_to_hex_string(), expected[d][sIdx])
          << "dst lane " << (int)d << " src lane " << (int)sIdx;
    }
  }
}

TEST(CodeTester, umov_and_ins_gpr32_arm64) {
  const char* umov_expected[4] = {"28 3d 04 0e", "28 3d 0c 0e", "28 3d 14 0e", "28 3d 1c 0e"};
  const char* ins_expected[4] = {"28 1d 04 4e", "28 1d 0c 4e", "28 1d 14 4e", "28 1d 1c 4e"};
  for (u8 i = 0; i < 4; i++) {
    {
      CodeTester tester(emitter::InstructionSet::ARM64);
      tester.init_code_buffer(32);
      tester.emit(IGen::ARM64::umov_gpr32_vf_element(emitter::Register(emitter::X8),
                                                     emitter::Register(emitter::XMM9), i));
      EXPECT_EQ(tester.dump_to_hex_string(), umov_expected[i]) << "umov lane " << (int)i;
    }
    {
      CodeTester tester(emitter::InstructionSet::ARM64);
      tester.init_code_buffer(32);
      tester.emit(IGen::ARM64::ins_vf_element_from_gpr32(emitter::Register(emitter::XMM8), i,
                                                         emitter::Register(emitter::X9)));
      EXPECT_EQ(tester.dump_to_hex_string(), ins_expected[i]) << "ins lane " << (int)i;
    }
  }
}

#ifdef __aarch64__
// Executing check: a wrong lane index still produces floats, so verify the moved lane really is
// the requested one for every source/destination pair.
TEST(CodeTester, execute_ins_vf_element_arm64) {
  struct Vec {
    u32 v[4];
  };
  const Vec dst_val = {{0xD0, 0xD1, 0xD2, 0xD3}};
  const Vec src_val = {{0x50, 0x51, 0x52, 0x53}};
  for (u8 d = 0; d < 4; d++) {
    for (u8 sIdx = 0; sIdx < 4; sIdx++) {
      CodeTester tester(emitter::InstructionSet::ARM64);
      tester.init_code_buffer(64);
      emitter::Register q_dst(emitter::XMM8), q_src(emitter::XMM9);
      tester.emit(IGen::ARM64::load128_simd128_gpr64(q_dst, emitter::Register(emitter::X0)));
      tester.emit(IGen::ARM64::load128_simd128_gpr64(q_src, emitter::Register(emitter::X1)));
      tester.emit(IGen::ARM64::ins_vf_element(q_dst, d, q_src, sIdx));
      tester.emit(IGen::ARM64::store128_gpr64_simd128(emitter::Register(emitter::X2), q_dst));
      tester.emit_return();

      Vec in_dst = dst_val, in_src = src_val, out = {};
      tester.execute((u64)&in_dst, (u64)&in_src, (u64)&out, 0);
      for (int lane = 0; lane < 4; lane++) {
        u32 expected = (lane == d) ? src_val.v[sIdx] : dst_val.v[lane];
        EXPECT_EQ(out.v[lane], expected)
            << "d=" << (int)d << " s=" << (int)sIdx << " lane=" << lane;
      }
    }
  }
}
#endif

#ifdef __aarch64__
// Execute the mov+INS sequence that IR_BlendVF::do_codegen_arm64 builds, for every mask and every
// aliasing of dst with src1/src2, and check each lane comes from the source the mask selects.
TEST(CodeTester, execute_blend_vf_arm64) {
  struct Vec {
    u32 v[4];
  };
  const Vec src1_val = {{0x1111'0000, 0x1111'0001, 0x1111'0002, 0x1111'0003}};
  const Vec src2_val = {{0x2222'0000, 0x2222'0001, 0x2222'0002, 0x2222'0003}};

  // dst_alias: 0 = distinct, 1 = dst aliases src1, 2 = dst aliases src2.
  for (int dst_alias = 0; dst_alias < 3; dst_alias++) {
    for (u8 mask = 0; mask < 16; mask++) {
      emitter::Register q_src1(emitter::XMM9), q_src2(emitter::XMM10);
      emitter::Register q_dst = dst_alias == 1   ? q_src1
                                : dst_alias == 2 ? q_src2
                                                 : emitter::Register(emitter::XMM11);

      CodeTester tester(emitter::InstructionSet::ARM64);
      tester.init_code_buffer(256);
      // x0 = &src1, x1 = &src2, x2 = &out
      tester.emit(IGen::ARM64::load128_simd128_gpr64(q_src1, emitter::Register(emitter::X0)));
      tester.emit(IGen::ARM64::load128_simd128_gpr64(q_src2, emitter::Register(emitter::X1)));

      if (mask == 0x0 || mask == 0xF) {
        tester.emit(IGen::ARM64::blend_vf(q_dst, q_src1, q_src2, mask));
      } else {
        bool seed_from_src2 = (q_dst == q_src2);
        if (!seed_from_src2 && q_dst != q_src1) {
          tester.emit(IGen::ARM64::mov_vf_vf(q_dst, q_src1));
        }
        for (u8 lane = 0; lane < 4; lane++) {
          bool lane_from_src2 = (mask >> lane) & 1;
          if (lane_from_src2 == seed_from_src2) {
            continue;
          }
          tester.emit(
              IGen::ARM64::ins_vf_element(q_dst, lane, lane_from_src2 ? q_src2 : q_src1, lane));
        }
      }
      tester.emit(IGen::ARM64::store128_gpr64_simd128(emitter::Register(emitter::X2), q_dst));
      tester.emit_return();

      Vec in1 = src1_val, in2 = src2_val, out = {};
      tester.execute((u64)&in1, (u64)&in2, (u64)&out, 0);

      for (int lane = 0; lane < 4; lane++) {
        u32 expected = ((mask >> lane) & 1) ? src2_val.v[lane] : src1_val.v[lane];
        EXPECT_EQ(out.v[lane], expected)
            << "dst_alias=" << dst_alias << " mask=" << (int)mask << " lane=" << lane;
      }
    }
  }
}
#endif

TEST(CodeTester, splat_vf_arm64) {
  // DUP Vd.4S, Vn.S[lane] for X/Y/Z/W, checked against the system assembler's encodings.
  const char* expected[4] = {"28 05 04 4e", "28 05 0c 4e", "28 05 14 4e", "28 05 1c 4e"};
  const emitter::Register::VF_ELEMENT elements[4] = {
      emitter::Register::VF_ELEMENT::X, emitter::Register::VF_ELEMENT::Y,
      emitter::Register::VF_ELEMENT::Z, emitter::Register::VF_ELEMENT::W};
  for (int i = 0; i < 4; i++) {
    CodeTester tester(emitter::InstructionSet::ARM64);
    tester.init_code_buffer(32);
    tester.emit(IGen::ARM64::splat_vf(emitter::Register(emitter::XMM8),
                                      emitter::Register(emitter::XMM9), elements[i]));
    EXPECT_EQ(tester.dump_to_hex_string(), expected[i]) << "element " << i;
  }
}

#ifdef __aarch64__
// Broadcasting the wrong lane is silently plausible (you still get floats), so execute the splat
// and check every output lane really holds the requested input lane.
TEST(CodeTester, execute_splat_vf_arm64) {
  struct Vec {
    u32 v[4];
  };
  const Vec src_val = {{0xAAAA'0000, 0xBBBB'1111, 0xCCCC'2222, 0xDDDD'3333}};
  const emitter::Register::VF_ELEMENT elements[4] = {
      emitter::Register::VF_ELEMENT::X, emitter::Register::VF_ELEMENT::Y,
      emitter::Register::VF_ELEMENT::Z, emitter::Register::VF_ELEMENT::W};

  for (int lane = 0; lane < 4; lane++) {
    CodeTester tester(emitter::InstructionSet::ARM64);
    tester.init_code_buffer(64);
    emitter::Register q_src(emitter::XMM9), q_dst(emitter::XMM8);
    tester.emit(IGen::ARM64::load128_simd128_gpr64(q_src, emitter::Register(emitter::X0)));
    tester.emit(IGen::ARM64::splat_vf(q_dst, q_src, elements[lane]));
    tester.emit(IGen::ARM64::store128_gpr64_simd128(emitter::Register(emitter::X1), q_dst));
    tester.emit_return();

    Vec in = src_val, out = {};
    tester.execute((u64)&in, (u64)&out, 0, 0);
    for (int i = 0; i < 4; i++) {
      EXPECT_EQ(out.v[i], src_val.v[lane]) << "splat lane " << lane << " output " << i;
    }
  }
}
#endif

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