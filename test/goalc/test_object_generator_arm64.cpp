/*!
 * Covers the ARM64-only split between a segment's executable functions and its writable static
 * data. Apple Silicon refuses to map a page both writable and executable, and GOAL writes to its
 * static objects while running, so the runtime protects only the function pages as RX. That is
 * only correct if the object file guarantees two things: the static data starts on a fresh page,
 * and the boundary is recorded where the runtime can find it.
 *
 * These assertions are about the bytes the compiler hands the runtime, so they are checked
 * against a generated object file rather than against an expected instruction encoding.
 */

#include <cstring>
#include <vector>

#include "common/jit_memory.h"
#include "common/link_types.h"
#include "common/type_system/TypeSystem.h"

#include "goalc/debugger/DebugInfo.h"
#include "goalc/emitter/IGen.h"
#include "goalc/emitter/IGenARM64.h"
#include "goalc/emitter/ObjectGenerator.h"

#include "gtest/gtest.h"

using namespace emitter;

namespace {

struct SizeOffset {
  u32 offset, size;
};

// The header generate_header_v3 writes: "GOAL", the GOAL version as two u16s, the object file
// version, the segment count, then the link and code tables.
constexpr size_t kTableOffset = 4 + 2 + 2 + 4 + 4;

struct ParsedHeader {
  u32 object_file_version = 0;
  u32 segment_count = 0;
  SizeOffset link_seg[N_SEG] = {};
  SizeOffset code_seg[N_SEG] = {};
};

ParsedHeader parse_header(const std::vector<u8>& header) {
  ParsedHeader out;
  EXPECT_GE(header.size(), kTableOffset + sizeof(SizeOffset) * N_SEG * 2);
  EXPECT_EQ(std::memcmp(header.data(), "GOAL", 4), 0);
  size_t at = 8;
  std::memcpy(&out.object_file_version, header.data() + at, sizeof(u32));
  at += sizeof(u32);
  std::memcpy(&out.segment_count, header.data() + at, sizeof(u32));
  at += sizeof(u32);
  EXPECT_EQ(out.object_file_version, 3u);
  EXPECT_EQ(out.segment_count, (u32)N_SEG);
  std::memcpy(out.link_seg, header.data() + at, sizeof(SizeOffset) * N_SEG);
  at += sizeof(SizeOffset) * N_SEG;
  std::memcpy(out.code_seg, header.data() + at, sizeof(SizeOffset) * N_SEG);
  return out;
}

// One function followed by two static objects, in segment 0.
ObjectFileData build_object(InstructionSet instr_set, TypeSystem* ts) {
  ObjectGenerator gen{GameVersion::Jak1, instr_set};
  static FunctionDebugInfo dbg;
  dbg = FunctionDebugInfo();
  dbg.name = "static-split-test";

  auto func = gen.add_function_to_seg(0, &dbg);
  auto ir = gen.add_ir(func);
  if (instr_set == InstructionSet::ARM64) {
    gen.add_instr(Instruction(IGen::ARM64::ret()), ir);
  } else {
    gen.add_instr(IGen::ret(gen), ir);
  }

  auto first = gen.add_static_to_seg(0, 16);
  gen.get_static_data(first).resize(24, 0xab);
  auto second = gen.add_static_to_seg(0, 16);
  gen.get_static_data(second).resize(8, 0xcd);

  return gen.generate_data_v3(ts);
}

}  // namespace

TEST(ObjectGeneratorArm64, StaticDataStartsOnItsOwnPage) {
  TypeSystem ts;
  ts.add_builtin_types(GameVersion::Jak1);
  auto data = build_object(InstructionSet::ARM64, &ts);
  auto header = parse_header(data.header);

  const u32 encoded = header.link_seg[0].size;
  ASSERT_TRUE(encoded & LINK_ARM64_EXECUTABLE_SIZE_FLAG)
      << "ARM64 objects must carry the executable-size boundary";
  const u32 boundary = encoded & ~LINK_ARM64_EXECUTABLE_SIZE_FLAG;

  // The runtime mprotects [segment start, boundary) as RX, so the boundary must be a page
  // multiple or it would take the first static object's page with it.
  const size_t page = jit_memory::page_size();
  EXPECT_EQ(boundary % page, 0u) << "boundary 0x" << std::hex << boundary << " is not page aligned";

  // The boundary has to sit inside the segment, and leave room for the static data.
  EXPECT_GT(boundary, 0u);
  EXPECT_LT(boundary, header.code_seg[0].size);
  EXPECT_EQ(data.segment_data.at(0).size(), header.code_seg[0].size);
}

TEST(ObjectGeneratorArm64, BoundaryMatchesTheFirstStaticObject) {
  TypeSystem ts;
  ts.add_builtin_types(GameVersion::Jak1);
  auto data = build_object(InstructionSet::ARM64, &ts);
  auto header = parse_header(data.header);
  const u32 boundary = header.link_seg[0].size & ~LINK_ARM64_EXECUTABLE_SIZE_FLAG;

  // The first static object was filled with 0xab, so it has to begin exactly at the boundary.
  const auto& seg = data.segment_data.at(0);
  ASSERT_LT(boundary, seg.size());
  EXPECT_EQ(seg.at(boundary), 0xab);

  // And the bytes just below it are padding, not static data.
  EXPECT_EQ(seg.at(boundary - 1), 0x00);
}

TEST(ObjectGeneratorArm64, X86ObjectsKeepTheirOriginalHeader) {
  TypeSystem ts;
  ts.add_builtin_types(GameVersion::Jak1);
  auto data = build_object(InstructionSet::X86, &ts);
  auto header = parse_header(data.header);

  for (int seg = 0; seg < N_SEG; seg++) {
    EXPECT_FALSE(header.link_seg[seg].size & LINK_ARM64_EXECUTABLE_SIZE_FLAG)
        << "segment " << seg << " must keep the link size an x86 runtime expects";
    EXPECT_EQ(header.link_seg[seg].size, data.link_tables.at(seg).size());
  }

  // Without the ARM64 split, static data follows the code at its natural alignment.
  EXPECT_LT(data.segment_data.at(0).size(), jit_memory::page_size());
}
