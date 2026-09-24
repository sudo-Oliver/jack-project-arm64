/*!
 * @file DirectRenderer2Core.cpp
 * See DirectRenderer2Core.h. Moved here from the OpenGL renderer unchanged, except that the
 * flush is now a virtual call and the render-state and profiler arguments it only carried for
 * that call are gone.
 */

#include "DirectRenderer2Core.h"

#include "common/log/log.h"
#include "common/util/Assert.h"

#include "common/dma/dma.h"

#include "fmt/format.h"

DirectRenderer2Core::DirectRenderer2Core(u32 max_verts,
                                         u32 max_inds,
                                         u32 max_draws,
                                         const std::string& name,
                                         bool use_ftoi_mod)
    : m_name(name), m_use_ftoi_mod(use_ftoi_mod) {
  // The CPU-side buffers. Their sizes are the contract with the backend: a backend allocates its
  // GPU buffers to match, and the flush uploads however much of them this packet filled.
  m_vertices.vertices.resize(max_verts);
  m_vertices.indices.resize(max_inds);
  m_draw_buffer.resize(max_draws);
}

DirectRenderer2Core::~DirectRenderer2Core() = default;

void DirectRenderer2Core::reset_buffers() {
  m_next_free_draw = 0;
  m_vertices.next_index = 0;
  m_vertices.next_vertex = 0;
  m_state.next_vertex_starts_strip = true;
  m_current_state_has_open_draw = false;
}


void DirectRenderer2Core::reset_state() {
  m_state = {};
  m_stats = {};
  if (m_next_free_draw || m_vertices.next_vertex || m_vertices.next_index) {
    lg::warn("[{}] Call to reset_state while there was pending draw data!", m_name);
  }
  reset_buffers();
}


std::string DirectRenderer2Core::Vertex::print() const {
  return fmt::format("{} {} {}\n", xyz.to_string_aligned(), stq.to_string_aligned(), rgba[0]);
}

std::string DirectRenderer2Core::Draw::to_string() const {
  std::string result;
  result += mode.to_string();
  result += fmt::format("TBP: 0x{:x}\n", tbp);
  result += fmt::format("fix: 0x{:x}\n", fix);
  return result;
}

std::string DirectRenderer2Core::Draw::to_single_line_string() const {
  return fmt::format("mode 0x{:8x} tbp 0x{:4x} fix 0x{:2x}\n", mode.as_int(), tbp, fix);
}


namespace {

/*!
 * If it is a DIRECT, return the quadword count it carries. If it is ignorable (nop, flush),
 * return 0.
 */
u32 get_direct_qwc_or_nop(const VifCode& code) {
  switch (code.kind) {
    case VifCode::Kind::NOP:
    case VifCode::Kind::FLUSHA:
      return 0;
    case VifCode::Kind::DIRECT:
      return code.immediate == 0 ? 65536 : code.immediate;
    default:
      ASSERT_MSG(false, fmt::format("expected direct, got {}", code.print()));
      return 0;
  }
}

}  // namespace

void DirectRenderer2Core::render_vif_data(u32 vif0, u32 vif1, const u8* data, u32 size) {
  // Walk forward looking for DIRECTs, skipping the nops and flushes between them.
  u32 gif_qwc = get_direct_qwc_or_nop(VifCode(vif0));
  if (gif_qwc) {
    ASSERT(get_direct_qwc_or_nop(VifCode(vif1)) == 0);
  } else {
    gif_qwc = get_direct_qwc_or_nop(VifCode(vif1));
  }

  u32 offset_into_data = 0;
  while (offset_into_data < size) {
    if (gif_qwc) {
      if (offset_into_data & 0xf) {
        // Not quadword-aligned: what follows has to be a nop.
        u32 vif;
        memcpy(&vif, data + offset_into_data, 4);
        offset_into_data += 4;
        ASSERT(get_direct_qwc_or_nop(VifCode(vif)) == 0);
      } else {
        render_gif_data(data + offset_into_data);
        offset_into_data += gif_qwc * 16;
      }
    } else {
      u32 vif;
      memcpy(&vif, data + offset_into_data, 4);
      offset_into_data += 4;
      gif_qwc = get_direct_qwc_or_nop(VifCode(vif));
    }
  }
}

void DirectRenderer2Core::render_gif_data(const u8* data) {
  bool eop = false;

  u32 offset = 0;
  while (!eop) {
    GifTag tag(data + offset);
    offset += 16;

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
              if (m_use_ftoi_mod) {
                handle_xyzf2_mod_packed(data + offset);
              } else {
                handle_xyzf2_packed(data + offset);
              }
              break;
            case GifTag::RegisterDescriptor::PRIM:
              ASSERT(false);  // handle_prim_packed(data + offset, render_state, prof);
              break;
            case GifTag::RegisterDescriptor::TEX0_1:
              ASSERT(false);  // handle_tex0_1_packed(data + offset);
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
              ASSERT(false);  // handle_prim(register_data, render_state, prof);
              break;
            case GifTag::RegisterDescriptor::RGBAQ:
              ASSERT(false);  // handle_rgbaq(register_data);
              break;
            case GifTag::RegisterDescriptor::XYZF2:
              ASSERT(false);  // handle_xyzf2(register_data, render_state, prof);
              break;
            default:
              ASSERT_MSG(false, fmt::format("Register {} is not supported in reglist mode yet\n",
                                            reg_descriptor_name(reg_desc[reg])));
          }
          offset += 8;  // PACKED = quadwords
        }
      }
    } else {
      ASSERT(false);  // format not packed or reglist.
    }

    eop = tag.eop();
  }
}

void DirectRenderer2Core::handle_ad(const u8* data) {
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
      // ASSERT(false);  // handle_pabe(value);
      ASSERT(value == 0);
      break;
    case GsRegisterAddress::CLAMP_1:
      handle_clamp1(value);
      break;
    case GsRegisterAddress::PRIM:
      ASSERT(false);  // handle_prim(value, render_state, prof);
      break;

    case GsRegisterAddress::TEX1_1:
      handle_tex1_1(value);
      break;
    case GsRegisterAddress::TEXA: {
      GsTexa reg(value);

      // rgba16 isn't used so this doesn't matter?
      // but they use sane defaults anyway
      ASSERT(reg.ta0() == 0);
      ASSERT(reg.ta1() == 0x80);  // note: check rgba16_to_rgba32 if this changes.

      ASSERT(reg.aem() == false);
    } break;
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
    default:
      ASSERT_MSG(false, fmt::format("Address {} is not supported", register_address_name(addr)));
  }
}

void DirectRenderer2Core::handle_test1(u64 val) {
  GsTest reg(val);
  ASSERT(!reg.date());  // datm doesn't matter
  if (m_state.gs_test != reg) {
    m_current_state_has_open_draw = false;
    m_state.gs_test = reg;
    m_state.as_mode.set_at(reg.alpha_test_enable());
    if (reg.alpha_test_enable()) {
      switch (reg.alpha_test()) {
        case GsTest::AlphaTest::NEVER:
          m_state.as_mode.set_alpha_test(DrawMode::AlphaTest::NEVER);
          break;
        case GsTest::AlphaTest::ALWAYS:
          m_state.as_mode.set_alpha_test(DrawMode::AlphaTest::ALWAYS);
          break;
        case GsTest::AlphaTest::GEQUAL:
          m_state.as_mode.set_alpha_test(DrawMode::AlphaTest::GEQUAL);
          break;
        default:
          ASSERT(false);
      }
    }

    m_state.as_mode.set_aref(reg.aref());
    m_state.as_mode.set_alpha_fail(reg.afail());
    m_state.as_mode.set_zt(reg.zte());
    m_state.as_mode.set_depth_test(reg.ztest());
  }
}

void DirectRenderer2Core::handle_zbuf1(u64 val) {
  GsZbuf x(val);
  ASSERT(x.psm() == TextureFormat::PSMZ24);
  ASSERT(x.zbp() == 448);
  bool write = !x.zmsk();
  if (write != m_state.as_mode.get_depth_write_enable()) {
    m_current_state_has_open_draw = false;
    m_state.as_mode.set_depth_write_enable(write);
  }
}

void DirectRenderer2Core::handle_tex0_1(u64 val) {
  GsTex0 reg(val);
  if (m_state.gs_tex0 != reg) {
    m_current_state_has_open_draw = false;
    m_state.gs_tex0 = reg;
    m_state.tbp = reg.tbp0();
    // tbw
    if (reg.psm() == GsTex0::PSM::PSMT4HH) {
      m_state.tbp |= 0x8000;
    }
    // tw/th
    m_state.as_mode.set_tcc(reg.tcc());
    m_state.set_tcc_flag(reg.tcc());
    bool decal = reg.tfx() == GsTex0::TextureFunction::DECAL;
    m_state.as_mode.set_decal(decal);
    m_state.set_decal_flag(decal);
    ASSERT(reg.tfx() == GsTex0::TextureFunction::DECAL ||
           reg.tfx() == GsTex0::TextureFunction::MODULATE);
  }
}

void DirectRenderer2Core::handle_tex1_1(u64 val) {
  GsTex1 reg(val);
  if (reg.mmag() != m_state.as_mode.get_filt_enable()) {
    m_current_state_has_open_draw = false;
    m_state.as_mode.set_filt_enable(reg.mmag());
  }
}

void DirectRenderer2Core::handle_clamp1(u64 val) {
  bool clamp_s = val & 0b001;
  bool clamp_t = val & 0b100;

  if ((clamp_s != m_state.as_mode.get_clamp_s_enable()) ||
      (clamp_t != m_state.as_mode.get_clamp_t_enable())) {
    m_current_state_has_open_draw = false;
    m_state.as_mode.set_clamp_s_enable(clamp_s);
    m_state.as_mode.set_clamp_t_enable(clamp_t);
  }
}

void DirectRenderer2Core::handle_prim(u64 val) {
  m_state.next_vertex_starts_strip = true;
  GsPrim reg(val);
  if (reg != m_state.gs_prim) {
    m_current_state_has_open_draw = false;
    ASSERT(reg.kind() == GsPrim::Kind::TRI_STRIP);
    ASSERT(reg.gouraud());
    if (!reg.tme()) {
      ASSERT(false);  // todo, might need this
    }
    m_state.as_mode.set_fog(reg.fge());
    m_state.set_fog_flag(reg.fge());
    m_state.as_mode.set_ab(reg.abe());
    ASSERT(!reg.aa1());
    ASSERT(!reg.fst());
    ASSERT(!reg.ctxt());
    ASSERT(!reg.fix());
  }
}

void DirectRenderer2Core::handle_st_packed(const u8* data) {
  memcpy(&m_state.s, data + 0, 4);
  memcpy(&m_state.t, data + 4, 4);
  memcpy(&m_state.Q, data + 8, 4);
}

void DirectRenderer2Core::handle_rgbaq_packed(const u8* data) {
  m_state.rgba[0] = data[0];
  m_state.rgba[1] = data[4];
  m_state.rgba[2] = data[8];
  m_state.rgba[3] = data[12];
}

void DirectRenderer2Core::handle_xyzf2_packed(const u8* data) {
  if (m_vertices.close_to_full()) {
    m_stats.flush_due_to_full++;
    flush_draws();
  }

  u32 x, y;
  memcpy(&x, data, 4);
  memcpy(&y, data + 4, 4);

  u64 upper;
  memcpy(&upper, data + 8, 8);
  u32 z = (upper >> 4) & 0xffffff;

  u8 f = (upper >> 36);
  bool adc = !(upper & (1ull << 47));

  if (m_state.next_vertex_starts_strip) {
    m_state.next_vertex_starts_strip = false;
    m_vertices.indices[m_vertices.next_index++] = UINT32_MAX;
  }

  // push the vertex
  auto& vert = m_vertices.vertices[m_vertices.next_vertex++];
  auto vidx = m_vertices.next_vertex - 1;
  if (adc) {
    m_vertices.indices[m_vertices.next_index++] = vidx;
  } else {
    m_vertices.indices[m_vertices.next_index++] = UINT32_MAX;
    m_vertices.indices[m_vertices.next_index++] = vidx - 1;
    m_vertices.indices[m_vertices.next_index++] = vidx;
  }

  if (!m_current_state_has_open_draw) {
    m_current_state_has_open_draw = true;
    if (m_next_free_draw >= m_draw_buffer.size()) {
      ASSERT(false);
    }
    // pick a texture unit to use
    u8 tex_unit = 0;
    if (m_next_free_draw > 0) {
      tex_unit = (m_draw_buffer[m_next_free_draw - 1].tex_unit + 1) % TEX_UNITS;
    }
    auto& draw = m_draw_buffer[m_next_free_draw++];
    draw.mode = m_state.as_mode;
    draw.start_index = m_vertices.next_index;
    draw.tbp = m_state.tbp;
    draw.fix = m_state.gs_alpha.fix();
    // associate this draw with this texture unit.
    draw.tex_unit = tex_unit;
    m_state.tex_unit = tex_unit;
  }

  vert.xyz[0] = x;
  vert.xyz[1] = y;
  vert.xyz[2] = z;
  vert.rgba = m_state.rgba;
  vert.stq = math::Vector<float, 3>(m_state.s, m_state.t, m_state.Q);
  vert.tex_unit = m_state.tex_unit;
  vert.fog = f;
  vert.flags = m_state.vertex_flags;
}

void DirectRenderer2Core::handle_xyzf2_mod_packed(const u8* data) {
  if (m_vertices.close_to_full()) {
    m_stats.flush_due_to_full++;
    flush_draws();
  }

  float x;
  float y;
  memcpy(&x, data, 4);
  memcpy(&y, data + 4, 4);

  u64 upper;
  memcpy(&upper, data + 8, 8);
  float z;
  memcpy(&z, &upper, 4);

  u8 f = (upper >> 36);
  bool adc = !(upper & (1ull << 47));

  if (m_state.next_vertex_starts_strip) {
    m_state.next_vertex_starts_strip = false;
    m_vertices.indices[m_vertices.next_index++] = UINT32_MAX;
  }

  // push the vertex
  auto& vert = m_vertices.vertices[m_vertices.next_vertex++];

  auto vidx = m_vertices.next_vertex - 1;
  if (adc) {
    m_vertices.indices[m_vertices.next_index++] = vidx;
  } else {
    m_vertices.indices[m_vertices.next_index++] = UINT32_MAX;
    m_vertices.indices[m_vertices.next_index++] = vidx - 1;
    m_vertices.indices[m_vertices.next_index++] = vidx;
  }

  if (!m_current_state_has_open_draw) {
    m_current_state_has_open_draw = true;
    if (m_next_free_draw >= m_draw_buffer.size()) {
      ASSERT(false);
    }
    // pick a texture unit to use
    u8 tex_unit = 0;
    if (m_next_free_draw > 0) {
      tex_unit = (m_draw_buffer[m_next_free_draw - 1].tex_unit + 1) % TEX_UNITS;
    }
    auto& draw = m_draw_buffer[m_next_free_draw++];
    draw.mode = m_state.as_mode;
    draw.start_index = m_vertices.next_index;
    draw.tbp = m_state.tbp;
    draw.fix = m_state.gs_alpha.fix();
    // associate this draw with this texture unit.
    draw.tex_unit = tex_unit;
    m_state.tex_unit = tex_unit;
  }

  // todo move to shader or something.
  vert.xyz[0] = x * 16.f;
  vert.xyz[1] = y * 16.f;
  vert.xyz[2] = z;
  vert.rgba = m_state.rgba;
  vert.stq = math::Vector<float, 3>(m_state.s, m_state.t, m_state.Q);
  vert.tex_unit = m_state.tex_unit;
  vert.fog = f;
  vert.flags = m_state.vertex_flags;
}

void DirectRenderer2Core::handle_alpha1(u64 val) {
  GsAlpha reg(val);
  if (m_state.gs_alpha != reg) {
    m_state.gs_alpha = reg;
    m_current_state_has_open_draw = false;
    auto a = reg.a_mode();
    auto b = reg.b_mode();
    auto c = reg.c_mode();
    auto d = reg.d_mode();
    if (a == GsAlpha::BlendMode::SOURCE && b == GsAlpha::BlendMode::DEST &&
        c == GsAlpha::BlendMode::SOURCE && d == GsAlpha::BlendMode::DEST) {
      m_state.as_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_SRC_DST);
    } else if (a == GsAlpha::BlendMode::SOURCE && b == GsAlpha::BlendMode::ZERO_OR_FIXED &&
               c == GsAlpha::BlendMode::SOURCE && d == GsAlpha::BlendMode::DEST) {
      m_state.as_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_0_SRC_DST);
    } else if (a == GsAlpha::BlendMode::ZERO_OR_FIXED && b == GsAlpha::BlendMode::SOURCE &&
               c == GsAlpha::BlendMode::SOURCE && d == GsAlpha::BlendMode::DEST) {
      m_state.as_mode.set_alpha_blend(DrawMode::AlphaBlend::ZERO_SRC_SRC_DST);
    } else if (a == GsAlpha::BlendMode::SOURCE && b == GsAlpha::BlendMode::DEST &&
               c == GsAlpha::BlendMode::ZERO_OR_FIXED && d == GsAlpha::BlendMode::DEST) {
      m_state.as_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_FIX_DST);
    } else if (a == GsAlpha::BlendMode::SOURCE && b == GsAlpha::BlendMode::SOURCE &&
               c == GsAlpha::BlendMode::SOURCE && d == GsAlpha::BlendMode::SOURCE) {
      m_state.as_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_SRC_SRC_SRC);
    } else if (a == GsAlpha::BlendMode::SOURCE && b == GsAlpha::BlendMode::ZERO_OR_FIXED &&
               c == GsAlpha::BlendMode::DEST && d == GsAlpha::BlendMode::DEST) {
      m_state.as_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_0_DST_DST);
    } else {
      // unsupported blend: a 0 b 2 c 2 d 1
      // lg::error("unsupported blend: a {} b {} c {} d {}", (int)a, (int)b, (int)c, (int)d);
      //      ASSERT(false);
    }
  }
}

