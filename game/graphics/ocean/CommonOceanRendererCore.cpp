/*!
 * @file CommonOceanRendererCore.cpp
 * See CommonOceanRendererCore.h. Moved here from the OpenGL renderer unchanged; only the two
 * flush methods stayed behind, because only they touch a graphics API.
 */

#include "CommonOceanRendererCore.h"

#include <algorithm>
#include <cstring>

#include "common/log/log.h"
#include "common/util/Assert.h"

CommonOceanRendererCore::CommonOceanRendererCore() {
  m_vertices.resize(4096 * 10);
  for (auto& buf : m_indices) {
    buf.resize(4096 * 10);
  }
}

CommonOceanRendererCore::~CommonOceanRendererCore() = default;

namespace {
void reverse_indices(u32* indices, u32 count) {
  if (count) {
    for (u32 a = 0, b = count - 1; a < b; a++, b--) {
      std::swap(indices[a], indices[b]);
    }
  }
}
}  // namespace

void CommonOceanRendererCore::flush_mid() {
  // The game draws some sections of ocean twice: a low-poly mesh with the ocean texture, the same
  // with the envmap, then a high-poly mesh with each, which overwrite them. Drawing all the ocean
  // textures together and then all the envmaps gives the same result if the envmap pass runs back
  // to front and zeroes the destination alpha as it goes, because the blend mode then discards
  // the low-poly version that follows.
  reverse_indices(m_indices[1].data(), m_next_free_index[1]);
  flush_mid_draws();
}

void CommonOceanRendererCore::init_for_near() {
  m_next_free_vertex = 0;
  for (auto& x : m_next_free_index) {
    x = 0;
  }
}

void CommonOceanRendererCore::kick_from_near(const u8* data) {
  bool eop = false;

  u32 offset = 0;
  while (!eop) {
    GifTag tag(data + offset);
    offset += 16;

    if (tag.nreg() == 3) {
      ASSERT(tag.pre());
      if (GsPrim(tag.prim()).kind() == GsPrim::Kind::TRI_STRIP) {
        handle_near_vertex_gif_data_strip(data, offset, tag.nloop());
      } else {
        handle_near_vertex_gif_data_fan(data, offset, tag.nloop());
      }
      offset += 16 * 3 * tag.nloop();
    } else if (tag.nreg() == 1) {
      handle_near_adgif(data, offset, tag.nloop());
      offset += 16 * 1 * tag.nloop();
    } else {
      ASSERT(false);
    }

    eop = tag.eop();
  }
}

void CommonOceanRendererCore::handle_near_vertex_gif_data_strip(const u8* data, u32 offset, u32 loop) {
  m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = UINT32_MAX;
  bool reset_last = false;
  for (u32 i = 0; i < loop; i++) {
    auto& dest_vert = m_vertices[m_next_free_vertex++];

    // stq
    memcpy(dest_vert.stq.data(), data + offset, 12);
    offset += 16;

    // rgba
    dest_vert.rgba[0] = data[offset];
    dest_vert.rgba[1] = data[offset + 4];
    dest_vert.rgba[2] = data[offset + 8];
    dest_vert.rgba[3] = data[offset + 12];
    offset += 16;

    // xyz
    u32 x = 0, y = 0;
    memcpy(&x, data + offset, 4);
    memcpy(&y, data + offset + 4, 4);

    u64 upper;
    memcpy(&upper, data + offset + 8, 8);
    u32 z = (upper >> 4) & 0xffffff;
    offset += 16;

    dest_vert.xyz[0] = (float)(x << 16) / (float)UINT32_MAX;
    dest_vert.xyz[1] = (float)(y << 16) / (float)UINT32_MAX;
    dest_vert.xyz[2] = (float)(z << 8) / (float)UINT32_MAX;

    u8 f = (upper >> 36);
    dest_vert.fog = f;

    auto vidx = m_next_free_vertex - 1;
    bool adc = upper & (1ull << 47);
    if (!adc) {
      m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = vidx;
      reset_last = false;
    } else {
      if (reset_last) {
        m_next_free_index[m_current_bucket] -= 3;
      }
      m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = UINT32_MAX;
      m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = vidx - 1;
      m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = vidx;
      reset_last = true;
    }
  }
}

void CommonOceanRendererCore::handle_near_vertex_gif_data_fan(const u8* data, u32 offset, u32 loop) {
  u32 ind_of_fan_start = UINT32_MAX;
  bool fan_running = false;
  // :regs0 (gif-reg-id st) :regs1 (gif-reg-id rgbaq) :regs2 (gif-reg-id xyzf2)
  for (u32 i = 0; i < loop; i++) {
    auto& dest_vert = m_vertices[m_next_free_vertex++];

    // stq
    memcpy(dest_vert.stq.data(), data + offset, 12);
    offset += 16;

    // rgba
    dest_vert.rgba[0] = data[offset];
    dest_vert.rgba[1] = data[offset + 4];
    dest_vert.rgba[2] = data[offset + 8];
    dest_vert.rgba[3] = data[offset + 12];
    offset += 16;

    // xyz
    u32 x = 0, y = 0;
    memcpy(&x, data + offset, 4);
    memcpy(&y, data + offset + 4, 4);

    u64 upper;
    memcpy(&upper, data + offset + 8, 8);
    u32 z = (upper >> 4) & 0xffffff;
    offset += 16;

    dest_vert.xyz[0] = (float)(x << 16) / (float)UINT32_MAX;
    dest_vert.xyz[1] = (float)(y << 16) / (float)UINT32_MAX;
    dest_vert.xyz[2] = (float)(z << 8) / (float)UINT32_MAX;

    u8 f = (upper >> 36);
    dest_vert.fog = f;

    auto vidx = m_next_free_vertex - 1;

    if (ind_of_fan_start == UINT32_MAX) {
      ind_of_fan_start = vidx;
    } else {
      if (fan_running) {
        // hack to draw fans with strips. this isn't efficient, but fans happen extremely rarely
        // (you basically have to put the camera intersecting the ocean and looking fwd)
        m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = UINT32_MAX;
        m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = vidx;
        m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = vidx - 1;
        m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = ind_of_fan_start;
      } else {
        fan_running = true;
      }
    }
  }
}

void CommonOceanRendererCore::handle_near_adgif(const u8* data, u32 offset, u32 count) {
  u32 most_recent_tbp = 0;

  for (u32 i = 0; i < count; i++) {
    u64 value;
    GsRegisterAddress addr;
    memcpy(&value, data + offset + 16 * i, sizeof(u64));
    memcpy(&addr, data + offset + 16 * i + 8, sizeof(GsRegisterAddress));
    switch (addr) {
      case GsRegisterAddress::MIPTBP1_1:
        // ignore this, it's just mipmapping settings
        break;
      case GsRegisterAddress::TEX1_1: {
        GsTex1 reg(value);
        ASSERT(reg.mmag());
      } break;
      case GsRegisterAddress::CLAMP_1: {
        bool s = value & 0b001;
        bool t = value & 0b100;
        ASSERT(s == t);
        if (s) {
          m_current_bucket = VertexBucket::ENV_MAP;
        }
      } break;
      case GsRegisterAddress::TEX0_1: {
        GsTex0 reg(value);
        ASSERT(reg.tfx() == GsTex0::TextureFunction::MODULATE);
        if (!reg.tcc()) {
          m_current_bucket = VertexBucket::RGB_TEXTURE;
        }
        most_recent_tbp = reg.tbp0();
      } break;
      case GsRegisterAddress::ALPHA_1: {
        GsAlpha reg(value);
        if (reg.a_mode() == GsAlpha::BlendMode::SOURCE &&
            reg.b_mode() == GsAlpha::BlendMode::ZERO_OR_FIXED &&
            reg.c_mode() == GsAlpha::BlendMode::DEST && reg.d_mode() == GsAlpha::BlendMode::DEST) {
          m_current_bucket = VertexBucket::ENV_MAP;
        }
      } break;

      case GsRegisterAddress::FRAME_1: {
        u32 mask = value >> 32;
        if (mask) {
          m_current_bucket = VertexBucket::ALPHA;
        }
      } break;

      default:
        lg::debug("reg: {}", register_address_name(addr));
        break;
    }
  }

  if (m_current_bucket == VertexBucket::ENV_MAP) {
    m_envmap_tex = most_recent_tbp;
  }

  if (m_vertices.size() - 128 < m_next_free_vertex) {
    ASSERT(false);  // add more vertices.
  }
}


void CommonOceanRendererCore::kick_from_mid(const u8* data) {
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
      if (tag.nreg() == 1) {
        ASSERT(!tag.pre());
        ASSERT(tag.nloop() == 5);
        handle_mid_adgif(data, offset);
        offset += 5 * 16;
      } else {
        ASSERT(tag.nreg() == 3);
        ASSERT(tag.pre());
        m_current_bucket = GsPrim(tag.prim()).abe() ? 1 : 0;

        int count = tag.nloop();
        if (GsPrim(tag.prim()).kind() == GsPrim::Kind::TRI_STRIP) {
          handle_near_vertex_gif_data_strip(data, offset, tag.nloop());
        } else {
          handle_near_vertex_gif_data_fan(data, offset, tag.nloop());
        }
        offset += 3 * 16 * count;
        // todo handle.
      }
    } else {
      ASSERT(false);  // format not packed or reglist.
    }

    eop = tag.eop();
  }
}

void CommonOceanRendererCore::handle_mid_adgif(const u8* data, u32 offset) {
  u32 most_recent_tbp = 0;

  for (u32 i = 0; i < 5; i++) {
    u64 value;
    GsRegisterAddress addr;
    memcpy(&value, data + offset + 16 * i, sizeof(u64));
    memcpy(&addr, data + offset + 16 * i + 8, sizeof(GsRegisterAddress));
    switch (addr) {
      case GsRegisterAddress::MIPTBP1_1:
      case GsRegisterAddress::MIPTBP2_1:
        // ignore this, it's just mipmapping settings
        break;
      case GsRegisterAddress::TEX1_1: {
        GsTex1 reg(value);
        ASSERT(reg.mmag());
      } break;
      case GsRegisterAddress::CLAMP_1: {
        bool s = value & 0b001;
        bool t = value & 0b100;
        ASSERT(s == t);
      } break;
      case GsRegisterAddress::TEX0_1: {
        GsTex0 reg(value);
        ASSERT(reg.tfx() == GsTex0::TextureFunction::MODULATE);
        most_recent_tbp = reg.tbp0();
      } break;
      case GsRegisterAddress::ALPHA_1: {
      } break;

      default:
        lg::debug("reg: {}", register_address_name(addr));
        break;
    }
  }

  if (most_recent_tbp != OCEAN_TEX_TBP_JAK2) {
    m_envmap_tex = most_recent_tbp;
  }

  if (m_vertices.size() - 128 < m_next_free_vertex) {
    ASSERT(false);  // add more vertices.
  }
}

void CommonOceanRendererCore::init_for_mid() {
  m_next_free_vertex = 0;
  for (auto& x : m_next_free_index) {
    x = 0;
  }
}


