/*!
 * @file DirectRendererCore.cpp
 * See DirectRendererCore.h. Moved here from the OpenGL renderer: the register handlers, the
 * primitive assembly and the texture-state bookkeeping are unchanged, and the render state and
 * profiler arguments they carried only so they could reach OpenGL are now a Context.
 */

#include "DirectRendererCore.h"

#include <cstring>

#include "common/log/log.h"
#include "common/util/Assert.h"

#include "game/graphics/opengl_renderer/AdgifHandler.h"

#include "fmt/format.h"

DirectRendererCore::ScissorState DirectRendererCore::m_scissor;

DirectRendererCore::DirectRendererCore(const std::string& name, int batch_size)
    : m_name(name), m_prim_buffer(batch_size) {}

DirectRendererCore::~DirectRendererCore() = default;

/*!
 * Consume one bucket's DMA. The data is VIF, with the GIF packets inside it.
 */
void DirectRendererCore::flush() {
  flush_draws();
}

void DirectRendererCore::consume_bucket_dma(DmaFollower& dma) {
  pre_render();
  // Coming in from a bucket, the state starts from nothing.
  reset_state();

  while (dma.current_tag_offset() != m_context.next_bucket && !dma.ended()) {
    auto data = dma.read_and_advance();
    if (data.size_bytes) {
      render_vif(data.vif0(), data.vif1(), data.data, data.size_bytes);
    }

    if (dma.current_tag_offset() == m_context.default_regs_buffer) {
      dma.read_and_advance();  // cnt
      dma.read_and_advance();  // ret
    }
  }

  flush();
  post_render();
}

void DirectRendererCore::reset_state() {
  m_test_state_needs_gl_update = true;
  m_test_state = TestState();

  m_blend_state_needs_gl_update = true;
  m_blend_state = BlendState();

  m_prim_gl_state_needs_gl_update = true;
  m_prim_gl_state = PrimGlState();

  for (int i = 0; i < TEXTURE_STATE_COUNT; ++i) {
    m_buffered_tex_state[i] = TextureState();
  }
  m_tex_state_from_reg = {};
  m_next_free_tex_state = 0;
  m_current_tex_state_idx = -1;

  m_prim_building = PrimBuildState();

  m_stats = {};
}

float u32_to_float(u32 in) {
  double x = (double)in / UINT32_MAX;
  return x;
}

float u32_to_sc(u32 in) {
  float flt = u32_to_float(in);
  return (flt - 0.5) * 16.0;
}

void DirectRendererCore::lookup_textures_again() {
  for (int i = 0; i < TEXTURE_STATE_COUNT; i++) {
    if (m_buffered_tex_state_currently_bound[i]) {
      auto& tex_state = m_buffered_tex_state[i];
      tex_state.used = true;
      backend_bind_texture(i);
      tex_state.used = false;
    }
  }
}

namespace {
/*!
 * If it's a direct, returns the qwc.
 * If it's ignorable (nop, flush), returns 0.
 * Otherwise, assert.
 */
u32 get_direct_qwc_or_nop(const VifCode& code) {
  switch (code.kind) {
    case VifCode::Kind::NOP:
    case VifCode::Kind::FLUSHA:
      return 0;
    case VifCode::Kind::DIRECT:
      if (code.immediate == 0) {
        return 65536;
      } else {
        return code.immediate;
      }
    default:
      ASSERT_MSG(false, fmt::format("expected direct, got {}", code.print()));
      return 0;
  }
}
}  // namespace

void DirectRendererCore::render_vif(u32 vif0, u32 vif1, const u8* data, u32 size) {
  // here we process VIF data. Basically we just go forward, looking for DIRECTs.
  // We skip stuff like flush and nops.

  // read the vif cmds at the front.
  u32 gif_qwc = get_direct_qwc_or_nop(VifCode(vif0));
  if (gif_qwc) {
    // we got a direct. expect the second thing to be a nop/similar.
    ASSERT(get_direct_qwc_or_nop(VifCode(vif1)) == 0);
  } else {
    gif_qwc = get_direct_qwc_or_nop(VifCode(vif1));
  }

  u32 offset_into_data = 0;
  while (offset_into_data < size) {
    if (gif_qwc) {
      if (offset_into_data & 0xf) {
        // not aligned. should get nops.
        u32 vif;
        memcpy(&vif, data + offset_into_data, 4);
        offset_into_data += 4;
        ASSERT(get_direct_qwc_or_nop(VifCode(vif)) == 0);
      } else {
        // aligned! do a gif transfer!
        render_gif(data + offset_into_data, gif_qwc * 16);
        offset_into_data += gif_qwc * 16;
      }
    } else {
      // we are reading VIF data.
      u32 vif;
      memcpy(&vif, data + offset_into_data, 4);
      offset_into_data += 4;
      gif_qwc = get_direct_qwc_or_nop(VifCode(vif));
    }
  }
}

void DirectRendererCore::render_gif(const u8* data, u32 size) {
  if (size != UINT32_MAX) {
    ASSERT(size >= 16);
  }

  if (m_blit_buf_state.expect == 5) {
    ASSERT(m_blit_buf_state.qwc * 16 == size);
    m_blit_buf_state.expect = 0;
    return;
  }

  bool eop = false;

  u32 offset = 0;
  while (!eop) {
    if (size != UINT32_MAX) {
      ASSERT(offset < size);
    }
    GifTag tag(data + offset);
    offset += 16;
    eop = tag.eop();

    // unpack registers.
    // faster to do it once outside of the nloop loop.
    GifTag::RegisterDescriptor reg_desc[16];
    u32 nreg = tag.nreg();
    for (u32 i = 0; i < nreg; i++) {
      reg_desc[i] = tag.reg(i);
    }

    auto format = tag.flg();
    if (format == GifTag::Format::PACKED) {
      if (tag.pre()) {
        handle_prim(tag.prim());
      }
      for (u32 loop = 0; loop < tag.nloop(); loop++) {
        for (u32 reg = 0; reg < nreg; reg++) {
          // fmt::print("{}\n", reg_descriptor_name(reg_desc[reg]));
          switch (reg_desc[reg]) {
            case GifTag::RegisterDescriptor::AD:
              handle_ad(data + offset);
              break;
            case GifTag::RegisterDescriptor::ST:
              handle_st_packed(data + offset);
              break;
            case GifTag::RegisterDescriptor::RGBAQ:
              handle_rgbaq_packed(data + offset);
              break;
            case GifTag::RegisterDescriptor::XYZF2:
              handle_xyzf2_packed(data + offset);
              break;
            case GifTag::RegisterDescriptor::XYZ2:
              handle_xyz2_packed(data + offset);
              break;
            case GifTag::RegisterDescriptor::PRIM:
              handle_prim_packed(data + offset);
              break;
            case GifTag::RegisterDescriptor::TEX0_1:
              handle_tex0_1_packed(data + offset);
              break;
            case GifTag::RegisterDescriptor::UV:
              handle_uv_packed(data + offset);
              break;
            default:
              ASSERT_MSG(false, fmt::format("Register {} is not supported in packed mode yet\n",
                                            reg_descriptor_name(reg_desc[reg])));
          }
          offset += 16;  // PACKED = quadwords
        }
      }
    } else if (format == GifTag::Format::REGLIST) {
      for (u32 loop = 0; loop < tag.nloop(); loop++) {
        for (u32 reg = 0; reg < nreg; reg++) {
          u64 register_data;
          memcpy(&register_data, data + offset, 8);
          // fmt::print("loop: {} reg: {} {}\n", loop, reg, reg_descriptor_name(reg_desc[reg]));
          switch (reg_desc[reg]) {
            case GifTag::RegisterDescriptor::PRIM:
              handle_prim(register_data);
              break;
            case GifTag::RegisterDescriptor::RGBAQ:
              handle_rgbaq(register_data);
              break;
            case GifTag::RegisterDescriptor::XYZF2:
              handle_xyzf2(register_data);
              break;
            default:
              ASSERT_MSG(false, fmt::format("Register {} is not supported in reglist mode yet\n",
                                            reg_descriptor_name(reg_desc[reg])));
          }
          offset += 8;  // PACKED = quadwords
        }
      }
    } else if (format == GifTag::Format::IMAGE) {
      ASSERT(m_blit_buf_state.expect == 4);
      m_blit_buf_state.expect++;

      // dont support non-eop image transfers yet
      ASSERT(eop);
      // in IMAGE mode this is the amount of 2x64-bit (1 qword) we will transfer
      m_blit_buf_state.qwc = tag.nloop();
    } else {
      ASSERT(false);  // format not packed or reglist or image
    }
  }

  if (size != UINT32_MAX) {
    if ((offset + 15) / 16 != size / 16) {
      ASSERT_MSG(false, fmt::format("DirectRenderer size failed in {}. expected: {}, got: {}",
                                    m_name, size, offset));
    }
  }

  //  fmt::print("{}\n", GifTag(data).print());
}

void DirectRendererCore::handle_ad(const u8* data) {
  u64 value;
  GsRegisterAddress addr;
  memcpy(&value, data, sizeof(u64));
  memcpy(&addr, data + 8, sizeof(GsRegisterAddress));

  // fmt::print("{}\n", register_address_name(addr));
  switch (addr) {
    case GsRegisterAddress::ZBUF_1:
      handle_zbuf1(value);
      break;
    case GsRegisterAddress::TEST_1:
      handle_test1(value);
      break;
    case GsRegisterAddress::ALPHA_1:
      handle_alpha1(value);
      break;
    case GsRegisterAddress::PABE:
      handle_pabe(value);
      break;
    case GsRegisterAddress::CLAMP_1:
      handle_clamp1(value);
      break;
    case GsRegisterAddress::PRIM:
      handle_prim(value);
      break;

    case GsRegisterAddress::TEX1_1:
      handle_tex1_1(value);
      break;
    case GsRegisterAddress::TEXA:
      handle_texa(value);
      break;
    case GsRegisterAddress::TEXCLUT:
      // TODO
      // the only thing the direct renderer does with texture is font, which does no tricks with
      // CLUT. The texture upload process will do all of the lookups with the default CLUT.
      // So we'll just assume that the TEXCLUT is set properly and ignore this.
      break;
    case GsRegisterAddress::FOGCOL:
      // TODO
      break;
    case GsRegisterAddress::TEX0_1:
      handle_tex0_1(value);
      break;
    case GsRegisterAddress::MIPTBP1_1:
    case GsRegisterAddress::MIPTBP2_1:
      // TODO this has the address of different mip levels.
      break;
    case GsRegisterAddress::TEXFLUSH:
      break;
    case GsRegisterAddress::FRAME_1:
      handle_frame(value);
      break;
    case GsRegisterAddress::RGBAQ: {  // shadow scissor does this?
      m_prim_building.rgba_reg[0] = data[0];
      m_prim_building.rgba_reg[1] = data[1];
      m_prim_building.rgba_reg[2] = data[2];
      m_prim_building.rgba_reg[3] = data[3];
      memcpy(&m_prim_building.Q, data + 4, 4);
    } break;
    case GsRegisterAddress::SCISSOR_1:
      handle_scissor(value);
      break;
    case GsRegisterAddress::XYOFFSET_1:
      ASSERT(m_context.version >= GameVersion::Jak2);  // hardcoded jak 2 scissor vals in handle
      handle_xyoffset(value);
      break;
    case GsRegisterAddress::COLCLAMP:
      ASSERT(value == 1);
      break;
    case GsRegisterAddress::BITBLTBUF:
      ASSERT(false);
      handle_bitbltbuf(value);
      break;
    case GsRegisterAddress::TRXPOS:
      ASSERT(false);
      handle_trxpos(value);
      break;
    case GsRegisterAddress::TRXREG:
      ASSERT(false);
      handle_trxreg(value);
      break;
    case GsRegisterAddress::TRXDIR:
      ASSERT(false);
      handle_trxdir(value & 0x3);
      break;
    default:
      ASSERT_MSG(false, fmt::format("Address {} is not supported", register_address_name(addr)));
  }
}

void DirectRendererCore::handle_frame(u64) {}

void DirectRendererCore::handle_scissor(u64 val) {
  m_scissor.scax0 = (val >> 0) & 0x7ff;
  m_scissor.scax1 = (val >> 16) & 0x7ff;
  m_scissor.scay0 = (val >> 32) & 0x7ff;
  m_scissor.scay1 = (val >> 48) & 0x7ff;
  m_scissor_enable = true;
}

void DirectRendererCore::handle_xyoffset(u64 val) {
  GsXYOffset xyo(val);
  // :ofx #x7000 :ofy #x7300
  float scale = -65536;
  m_prim_buffer.x_off = scale * ((s32)xyo.ofx() - 0x7000) / float(UINT32_MAX);
  m_prim_buffer.y_off = scale * ((s32)xyo.ofy() - 0x7300) / float(UINT32_MAX);
}

void DirectRendererCore::handle_bitbltbuf(u64 val) {
  ASSERT(m_blit_buf_state.expect == 0);
  m_blit_buf_state.expect++;

  m_blit_buf_state.sbp = (val >> 0) & 0x3fff;
  m_blit_buf_state.sbw = (val >> 16) & 0x3f;
  m_blit_buf_state.spsm = (val >> 24) & 0x3f;
  m_blit_buf_state.dbp = (val >> 32) & 0x3fff;
  m_blit_buf_state.dbw = (val >> 48) & 0x3f;
  m_blit_buf_state.dpsm = (val >> 56) & 0x3f;
}

void DirectRendererCore::handle_trxpos(u64 val) {
  ASSERT(m_blit_buf_state.expect == 1);
  m_blit_buf_state.expect++;

  m_blit_buf_state.ssax = (val >> 0) & 0x7ff;
  m_blit_buf_state.ssay = (val >> 16) & 0x7ff;
  m_blit_buf_state.dsax = (val >> 32) & 0x7ff;
  m_blit_buf_state.dsay = (val >> 48) & 0x7ff;
  m_blit_buf_state.pixel_dir = (val >> 59) & 0x3;
}

void DirectRendererCore::handle_trxreg(u64 val) {
  ASSERT(m_blit_buf_state.expect == 2);
  m_blit_buf_state.expect++;

  m_blit_buf_state.width = (val >> 0) & 0xfff;
  m_blit_buf_state.height = (val >> 32) & 0xfff;
}

void DirectRendererCore::handle_trxdir(u64 dir) {
  ASSERT(m_blit_buf_state.expect == 3);
  m_blit_buf_state.expect++;

  auto get_tex_func = [this](const std::string& name, u16 tbp) {
    auto result = m_context.texture_pool->lookup(tbp);
    if (!result) {
      fmt::print("{} tbp {} not found\n", name, tbp);
    } else {
      fmt::print("{} tbp {} found\n", name, tbp);
    }
    return result;
  };
  fmt::print("GS TEXTURE COPY --\n");
  fmt::print("src w/psm: {}/{} dst w/psm: {}/{}\n", m_blit_buf_state.sbw, m_blit_buf_state.spsm,
             m_blit_buf_state.dbw, m_blit_buf_state.dpsm);
  switch (dir) {
    case 0: {  // host->local
      fmt::print("-- FROM EE\n");
      auto dst_tex = get_tex_func("dst", m_blit_buf_state.dbp);
      (void)dst_tex;
      // ASSERT_MSG(false, "nyi trxdir host->local");
    } break;
    case 1: {  // local->host
      fmt::print("-- FROM GS\n");
      auto src_tex = get_tex_func("src", m_blit_buf_state.sbp);
      (void)src_tex;
      // ASSERT_MSG(false, "nyi trxdir local->host");
    } break;
    case 2: {  // local->local
      fmt::print("-- GS <-> GS\n");
      auto src_tex = get_tex_func("src", m_blit_buf_state.sbp);
      auto dst_tex = get_tex_func("dst", m_blit_buf_state.dbp);
      (void)src_tex;
      (void)dst_tex;
    } break;
    case 3:  // disable
      fmt::print("-- HUH???\n");
      ASSERT_MSG(false, "nyi trxdir disable");
      break;
  }
}

void DirectRendererCore::handle_tex1_1(u64 val) {
  GsTex1 reg(val);
  // for now, we aren't going to handle mipmapping. I don't think it's used with direct.
  //   ASSERT(reg.mxl() == 0);
  // if that's true, we can ignore LCM, MTBA, L, K

  bool want_tex_filt = reg.mmag();
  if (want_tex_filt != m_tex_state_from_reg.enable_tex_filt) {
    m_tex_state_from_reg.enable_tex_filt = want_tex_filt;
    // we changed the state_from_reg, we no longer know if it points to a texture state.
    m_current_tex_state_idx = -1;
  }

  // MMAG/MMIN specify texture filtering. For now, assume always linear
  //  ASSERT(reg.mmag() == true);
  //  if (!(reg.mmin() == 1 || reg.mmin() == 4)) {  // with mipmap off, both of these are linear
  //                                                //    lg::error("unsupported mmin");
  //  }
}

void DirectRendererCore::handle_tex0_1_packed(const u8* data) {
  u64 val;
  memcpy(&val, data, sizeof(u64));
  handle_tex0_1(val);
}

void DirectRendererCore::handle_tex0_1(u64 val) {
  GsTex0 reg(val);
  // update tbp
  if (m_tex_state_from_reg.current_register != reg) {
    m_tex_state_from_reg.texture_base_ptr = reg.tbp0();
    m_tex_state_from_reg.using_mt4hh = reg.psm() == GsTex0::PSM::PSMT4HH;
    m_tex_state_from_reg.current_register = reg;
    m_tex_state_from_reg.tcc = reg.tcc();
    m_tex_state_from_reg.decal = reg.tfx() == GsTex0::TextureFunction::DECAL;
    ASSERT(reg.tfx() == GsTex0::TextureFunction::DECAL ||
           reg.tfx() == GsTex0::TextureFunction::MODULATE);

    // we changed the state_from_reg, we no longer know if it points to a texture state.
    m_current_tex_state_idx = -1;
  }

  // tbw: assume they got it right
  // psm: assume they got it right
  // tw: assume they got it right
  // th: assume they got it right

  // MERC hack
  // ASSERT(reg.tfx() == GsTex0::TextureFunction::MODULATE);

  // cbp: assume they got it right
  // cpsm: assume they got it right
  // csm: assume they got it right
}

void DirectRendererCore::handle_st_packed(const u8* data) {
  memcpy(&m_prim_building.st_reg.x(), data + 0, 4);
  memcpy(&m_prim_building.st_reg.y(), data + 4, 4);
  memcpy(&m_prim_building.Q, data + 8, 4);
}

void DirectRendererCore::handle_uv_packed(const u8* data) {
  u32 u, v;
  memcpy(&u, data, 4);
  memcpy(&v, data + 4, 4);
  m_prim_building.st_reg.x() = u;
  m_prim_building.st_reg.y() = v;
  m_prim_building.Q = 1;
}

void DirectRendererCore::handle_rgbaq_packed(const u8* data) {
  // TODO update Q from st.
  m_prim_building.rgba_reg[0] = data[0];
  m_prim_building.rgba_reg[1] = data[4];
  m_prim_building.rgba_reg[2] = data[8];
  m_prim_building.rgba_reg[3] = data[12];
}

void DirectRendererCore::handle_xyzf2_packed(const u8* data) {
  u32 x, y;
  memcpy(&x, data, 4);
  memcpy(&y, data + 4, 4);

  u64 upper;
  memcpy(&upper, data + 8, 8);
  u32 z = (upper >> 4) & 0xffffff;

  u8 f = (upper >> 36);
  bool adc = upper & (1ull << 47);
  handle_xyzf2_common(x << 16, y << 16, z, f, !adc);
}

void DirectRendererCore::handle_xyz2_packed(const u8* data) {
  u32 x, y;
  memcpy(&x, data, 4);
  memcpy(&y, data + 4, 4);

  u64 upper;
  memcpy(&upper, data + 8, 8);
  u32 z = upper;

  bool adc = upper & (1ull << 47);
  handle_xyzf2_common(x << 16, y << 16, z, 0, !adc);
}

PerGameVersion<u32> normal_zbp = {448, 304, 304, 304};
void DirectRendererCore::handle_zbuf1(u64 val) {
  // note: we can basically ignore this. There's a single z buffer that's always configured the same
  // way - 24-bit, at offset 448.
  GsZbuf x(val);
  ASSERT(x.psm() == TextureFormat::PSMZ24);
  ASSERT(x.zbp() == normal_zbp[m_context.version]);

  bool write = !x.zmsk();
  //  ASSERT(write);
  if (write != m_test_state.depth_writes) {
    m_stats.flush_from_zbuf++;
    flush();
    m_test_state_needs_gl_update = true;
    m_prim_gl_state_needs_gl_update = true;
    m_test_state.depth_writes = write;
  }
}

void DirectRendererCore::handle_test1(u64 val) {
  GsTest reg(val);
  if (reg.alpha_test_enable()) {
    // ASSERT(reg.alpha_test() == GsTest::AlphaTest::ALWAYS);
  }
  ASSERT(!reg.date());
  if (m_test_state.current_register != reg) {
    m_stats.flush_from_test++;
    flush();
    m_test_state.from_register(reg);
    m_test_state_needs_gl_update = true;
    m_prim_gl_state_needs_gl_update = true;
  }
}
void DirectRendererCore::handle_texa(u64 val) {
  GsTexa reg(val);

  // rgba16 isn't used so this doesn't matter?
  // but they use sane defaults anyway
  // ASSERT(reg.ta0() == 0); TODO
  if (m_prim_gl_state.ta0 != reg.ta0()) {
    m_stats.flush_from_ta0++;
    flush();
    m_prim_gl_state.ta0 = reg.ta0();
    m_test_state_needs_gl_update = true;
    m_prim_gl_state_needs_gl_update = true;
    m_blend_state_needs_gl_update = true;
  }
  ASSERT(reg.ta1() == 0x80);  // note: check rgba16_to_rgba32 if this changes.

  ASSERT(reg.aem() == false);
}
void DirectRendererCore::handle_alpha1(u64 val) {
  GsAlpha reg(val);
  if (m_blend_state.current_register != reg) {
    m_stats.flush_from_alpha++;
    flush();
    m_blend_state.from_register(reg);
    m_blend_state_needs_gl_update = true;
  }
}

void DirectRendererCore::handle_pabe(u64 val) {
  ASSERT(val == 0);  // not really sure how to handle this yet.
}

void DirectRendererCore::handle_clamp1(u64 val) {
  if (!(val == 0b101 || val == 0 || val == 1 || val == 0b100)) {
    //    fmt::print("clamp: 0x{:x}\n", val);
    //    ASSERT(false);
  }

  if (m_tex_state_from_reg.m_clamp_state.current_register != val) {
    m_current_tex_state_idx = -1;
    m_tex_state_from_reg.m_clamp_state.current_register = val;
    m_tex_state_from_reg.m_clamp_state.clamp_s = val & 0b001;
    m_tex_state_from_reg.m_clamp_state.clamp_t = val & 0b100;
  }
}

void DirectRendererCore::handle_prim_packed(const u8* data) {
  u64 val;
  memcpy(&val, data, sizeof(u64));
  handle_prim(val);
}

void DirectRendererCore::handle_prim(u64 val) {
  if (m_prim_building.tri_strip_startup) {
    m_prim_building.tri_strip_startup = 0;
    m_prim_building.building_idx = 0;
  } else {
    if (m_prim_building.building_idx > 0) {
      ASSERT(false);  // shouldn't leave any half-finished prims
    }
  }
  // need to flush any in progress prims to the buffer.

  GsPrim prim(val);
  if (m_prim_gl_state.current_register != prim || m_blend_state.alpha_blend_enable != prim.abe()) {
    m_stats.flush_from_prim++;
    flush();
    m_prim_gl_state.from_register(prim);
    m_blend_state.alpha_blend_enable = prim.abe();
    m_prim_gl_state_needs_gl_update = true;
    m_blend_state_needs_gl_update = true;
  }

  m_prim_building.kind = prim.kind();
}

void DirectRendererCore::handle_rgbaq(u64 val) {
  ASSERT((val >> 32) == 0);  // q = 0
  memcpy(m_prim_building.rgba_reg.data(), &val, 4);
}

int DirectRendererCore::get_texture_unit_for_current_reg() {
  if (m_current_tex_state_idx != -1) {
    return m_current_tex_state_idx;
  }

  if (m_next_free_tex_state >= TEXTURE_STATE_COUNT) {
    m_stats.flush_from_state_exhaust++;
    flush();
    return get_texture_unit_for_current_reg();
  } else {
    ASSERT(!m_buffered_tex_state[m_next_free_tex_state].used);
    m_buffered_tex_state[m_next_free_tex_state] = m_tex_state_from_reg;
    m_buffered_tex_state[m_next_free_tex_state].used = true;
    m_current_tex_state_idx = m_next_free_tex_state++;
    return m_current_tex_state_idx;
  }
}

void DirectRendererCore::handle_xyzf2_common(u32 x, u32 y, u32 z, u8 f, bool advance) {
  if (m_prim_buffer.is_full()) {
    lg::warn("Buffer wrapped in {} ({} verts, {} bytes)", m_name, m_prim_buffer.max_verts,
             m_prim_buffer.vert_count * sizeof(Vertex));
    flush();
  }

  m_prim_building.building_stq.at(m_prim_building.building_idx) = math::Vector<float, 3>(
      m_prim_building.st_reg.x(), m_prim_building.st_reg.y(), m_prim_building.Q);
  m_prim_building.building_rgba.at(m_prim_building.building_idx) = m_prim_building.rgba_reg;
  m_prim_building.building_vert.at(m_prim_building.building_idx) = math::Vector<u32, 4>{x, y, z, f};

  m_prim_building.building_idx++;

  int tex_unit = get_texture_unit_for_current_reg();
  bool tcc = m_buffered_tex_state[tex_unit].tcc;
  bool decal = m_buffered_tex_state[tex_unit].decal;
  bool fge = m_prim_gl_state.fogging_enable;
  bool use_uv = m_prim_gl_state.use_uv;

  math::Vector<float, 4> scissor(m_scissor.scax0, m_scissor.scax1, m_scissor.scay0,
                                 m_scissor.scay1);

  switch (m_prim_building.kind) {
    case GsPrim::Kind::SPRITE: {
      if (m_prim_building.building_idx == 2) {
        // build triangles from the sprite.
        auto& corner1_vert = m_prim_building.building_vert[0];
        auto& corner1_rgba = m_prim_building.building_rgba[0];
        auto& corner2_vert = m_prim_building.building_vert[1];
        auto& corner2_rgba = m_prim_building.building_rgba[1];
        auto& corner1_stq = m_prim_building.building_stq[0];
        auto& corner2_stq = m_prim_building.building_stq[1];

        // should use most recent vertex z.
        math::Vector<u32, 4> corner3_vert{corner1_vert[0], corner2_vert[1], corner2_vert[2], 0};
        math::Vector<u32, 4> corner4_vert{corner2_vert[0], corner1_vert[1], corner2_vert[2], 0};
        math::Vector<float, 3> corner3_stq{corner1_stq[0], corner2_stq[1], corner2_stq[2]};
        math::Vector<float, 3> corner4_stq{corner2_stq[0], corner1_stq[1], corner2_stq[2]};

        if (m_prim_gl_state.gouraud_enable) {
          // I'm not really sure what the GS does here.
          ASSERT(false);
        }
        auto& corner3_rgba = corner2_rgba;
        auto& corner4_rgba = corner2_rgba;

        m_prim_buffer.push(corner1_rgba, corner1_vert, corner1_stq, scissor, 0, tcc, decal, fge,
                           use_uv);
        m_prim_buffer.push(corner3_rgba, corner3_vert, corner3_stq, scissor, 0, tcc, decal, fge,
                           use_uv);
        m_prim_buffer.push(corner2_rgba, corner2_vert, corner2_stq, scissor, 0, tcc, decal, fge,
                           use_uv);
        m_prim_buffer.push(corner2_rgba, corner2_vert, corner2_stq, scissor, 0, tcc, decal, fge,
                           use_uv);
        m_prim_buffer.push(corner4_rgba, corner4_vert, corner4_stq, scissor, 0, tcc, decal, fge,
                           use_uv);
        m_prim_buffer.push(corner1_rgba, corner1_vert, corner1_stq, scissor, 0, tcc, decal, fge,
                           use_uv);
        m_prim_building.building_idx = 0;
      }
    } break;
    case GsPrim::Kind::TRI_STRIP: {
      if (m_prim_building.building_idx == 3) {
        m_prim_building.building_idx = 0;
      }

      if (m_prim_building.tri_strip_startup < 3) {
        m_prim_building.tri_strip_startup++;
      }
      if (m_prim_building.tri_strip_startup >= 3) {
        if (advance) {
          for (int i = 0; i < 3; i++) {
            m_prim_buffer.push(m_prim_building.building_rgba[i], m_prim_building.building_vert[i],
                               m_prim_building.building_stq[i], scissor, tex_unit, tcc, decal, fge,
                               use_uv);
          }
        }
      }

    } break;

    case GsPrim::Kind::TRI:
      if (m_prim_building.building_idx == 3) {
        m_prim_building.building_idx = 0;
        for (int i = 0; i < 3; i++) {
          m_prim_buffer.push(m_prim_building.building_rgba[i], m_prim_building.building_vert[i],
                             m_prim_building.building_stq[i], scissor, tex_unit, tcc, decal, fge,
                             use_uv);
        }
      }
      break;

    case GsPrim::Kind::TRI_FAN: {
      if (m_prim_building.tri_strip_startup < 2) {
        m_prim_building.tri_strip_startup++;
      } else {
        if (m_prim_building.building_idx == 2) {
          // nothing.
        } else if (m_prim_building.building_idx == 3) {
          m_prim_building.building_idx = 1;
        }
        for (int i = 0; i < 3; i++) {
          m_prim_buffer.push(m_prim_building.building_rgba[i], m_prim_building.building_vert[i],
                             m_prim_building.building_stq[i], scissor, tex_unit, tcc, decal, fge,
                             use_uv);
        }
      }
    } break;

    case GsPrim::Kind::LINE: {
      if (m_prim_building.building_idx == 2) {
        math::Vector<double, 3> pt0 = m_prim_building.building_vert[0].xyz().cast<double>();
        math::Vector<double, 3> pt1 = m_prim_building.building_vert[1].xyz().cast<double>();
        auto normal = (pt1 - pt0).normalized().cross(math::Vector<double, 3>{0, 0, 1});

        double line_width = (1 << 19);
        //        debug_print_vtx(m_prim_building.building_vert[0]);
        //        debug_print_vtx(m_prim_building.building_vert[1]);

        math::Vector<double, 3> a = pt0 + normal * line_width;
        math::Vector<double, 3> b = pt1 + normal * line_width;
        math::Vector<double, 3> c = pt0 - normal * line_width;
        math::Vector<double, 3> d = pt1 - normal * line_width;
        math::Vector<u32, 4> ai{a.x(), a.y(), a.z(), 0};
        math::Vector<u32, 4> bi{b.x(), b.y(), b.z(), 0};
        math::Vector<u32, 4> ci{c.x(), c.y(), c.z(), 0};
        math::Vector<u32, 4> di{d.x(), d.y(), d.z(), 0};

        // ACB:
        m_prim_buffer.push(m_prim_building.building_rgba[0], ai, {}, scissor, 0, false, false,
                           false, false);
        m_prim_buffer.push(m_prim_building.building_rgba[0], ci, {}, scissor, 0, false, false,
                           false, false);
        m_prim_buffer.push(m_prim_building.building_rgba[1], bi, {}, scissor, 0, false, false,
                           false, false);
        // b c d
        m_prim_buffer.push(m_prim_building.building_rgba[1], bi, {}, scissor, 0, false, false,
                           false, false);
        m_prim_buffer.push(m_prim_building.building_rgba[0], ci, {}, scissor, 0, false, false,
                           false, false);
        m_prim_buffer.push(m_prim_building.building_rgba[1], di, {}, scissor, 0, false, false,
                           false, false);
        //

        m_prim_building.building_idx = 0;
      }
    } break;

    case GsPrim::Kind::LINE_STRIP: {
      if (m_prim_building.building_idx == 2) {
        m_prim_building.building_idx = 0;
      }

      if (m_prim_building.tri_strip_startup < 2) {
        m_prim_building.tri_strip_startup++;
      }
      if (m_prim_building.tri_strip_startup >= 2) {
        if (advance) {
          math::Vector<double, 3> pt0 = m_prim_building.building_vert[0].xyz().cast<double>();
          math::Vector<double, 3> pt1 = m_prim_building.building_vert[1].xyz().cast<double>();
          auto normal = (pt1 - pt0).normalized().cross(math::Vector<double, 3>{0, 0, 1});

          double line_width = (1 << 19);
          //        debug_print_vtx(m_prim_building.building_vert[0]);
          //        debug_print_vtx(m_prim_building.building_vert[1]);

          math::Vector<double, 3> a = pt0 + normal * line_width;
          math::Vector<double, 3> b = pt1 + normal * line_width;
          math::Vector<double, 3> c = pt0 - normal * line_width;
          math::Vector<double, 3> d = pt1 - normal * line_width;
          math::Vector<u32, 4> ai{a.x(), a.y(), a.z(), 0};
          math::Vector<u32, 4> bi{b.x(), b.y(), b.z(), 0};
          math::Vector<u32, 4> ci{c.x(), c.y(), c.z(), 0};
          math::Vector<u32, 4> di{d.x(), d.y(), d.z(), 0};

          // ACB:
          m_prim_buffer.push(m_prim_building.building_rgba[0], ai, {}, scissor, 0, false, false,
                             false, false);
          m_prim_buffer.push(m_prim_building.building_rgba[0], ci, {}, scissor, 0, false, false,
                             false, false);
          m_prim_buffer.push(m_prim_building.building_rgba[1], bi, {}, scissor, 0, false, false,
                             false, false);
          // b c d
          m_prim_buffer.push(m_prim_building.building_rgba[1], bi, {}, scissor, 0, false, false,
                             false, false);
          m_prim_buffer.push(m_prim_building.building_rgba[0], ci, {}, scissor, 0, false, false,
                             false, false);
          m_prim_buffer.push(m_prim_building.building_rgba[1], di, {}, scissor, 0, false, false,
                             false, false);
          //
        }
      }
    } break;

    default:
      ASSERT_MSG(false, fmt::format("prim type {} is unsupported in {}.", (int)m_prim_building.kind,
                                    m_name));
  }
}

void DirectRendererCore::handle_xyzf2(u64 val) {
  u32 x = val & 0xffff;
  u32 y = (val >> 16) & 0xffff;
  u32 z = (val >> 32) & 0xffffff;
  u32 f = (val >> 56) & 0xff;

  handle_xyzf2_common(x << 16, y << 16, z, f, true);
}

void DirectRendererCore::TestState::from_register(GsTest reg) {
  current_register = reg;
  alpha_test_enable = reg.alpha_test_enable();
  if (alpha_test_enable) {
    alpha_test = reg.alpha_test();
    aref = reg.aref();
    afail = reg.afail();
  }

  date = reg.date();
  if (date) {
    datm = reg.datm();
  }

  zte = reg.zte();
  ztst = reg.ztest();
}

void DirectRendererCore::BlendState::from_register(GsAlpha reg) {
  current_register = reg;
  a = reg.a_mode();
  b = reg.b_mode();
  c = reg.c_mode();
  d = reg.d_mode();
  fix = reg.fix();
}

void DirectRendererCore::PrimGlState::from_register(GsPrim reg) {
  current_register = reg;
  gouraud_enable = reg.gouraud();
  texture_enable = reg.tme();
  fogging_enable = reg.fge();
  aa_enable = reg.aa1();
  use_uv = reg.fst();
  ctxt = reg.ctxt();
  fix = reg.fix();
}

DirectRendererCore::PrimitiveBuffer::PrimitiveBuffer(int max_triangles) {
  vertices.resize(max_triangles * 3);
  max_verts = max_triangles * 3;
}

void DirectRendererCore::PrimitiveBuffer::push(const math::Vector<u8, 4>& rgba,
                                           const math::Vector<u32, 4>& vert,
                                           const math::Vector<float, 3>& stq,
                                           const math::Vector<float, 4>& scissor,
                                           int unit,
                                           bool tcc,
                                           bool decal,
                                           bool fog_enable,
                                           bool use_uv) {
  auto& v = vertices[vert_count];
  v.rgba = rgba;
  v.xyzf[0] = (float)vert[0] / (float)UINT32_MAX;
  v.xyzf[0] += x_off;
  v.xyzf[1] = (float)vert[1] / (float)UINT32_MAX;
  v.xyzf[1] += y_off;
  v.xyzf[2] = (float)vert[2] / (float)0xffffff;
  v.xyzf[3] = (float)vert[3];
  v.stq = stq;
  v.tex_unit = unit;
  v.tcc = tcc;
  v.decal = decal;
  v.fog_enable = fog_enable;
  v.use_uv = use_uv;
  v.scissor = scissor;
  vert_count++;
}
