/*!
 * @file Sprite3Core.cpp
 * See Sprite3Core.h.
 *
 * This is the sprite renderer's DMA walk and VU emulation, moved out of the OpenGL renderer
 * unchanged apart from the five places that touched the GPU, which are now virtual.
 */

#include "Sprite3Core.h"

#include "common/log/log.h"

#include "game/graphics/opengl_renderer/background/background_common.h"
#include "game/graphics/opengl_renderer/dma_helpers.h"

#include "fmt/format.h"

namespace {

/*!
 * Does the next DMA transfer look like it could be the start of a 2D group?
 */
bool looks_like_2d_chunk_start(const DmaFollower& dma) {
  return dma.current_tag().qwc == 1 && dma.current_tag().kind == DmaTag::Kind::CNT;
}

/*!
 * Read the header. Asserts if it's bad.
 * Returns the number of sprites.
 * Advances 1 dma transfer
 */
u32 process_sprite_chunk_header(DmaFollower& dma) {
  auto transfer = dma.read_and_advance();
  // note that flg = true, this should use double buffering
  bool ok = verify_unpack_with_stcycl(transfer, VifCode::Kind::UNPACK_V4_32, 4, 4, 1,
                                      SpriteDataMem::Header, false, true);
  ASSERT(ok);
  u32 header[4];
  memcpy(header, transfer.data, 16);
  ASSERT(header[0] <= Sprite3Core::SPRITES_PER_CHUNK);
  return header[0];
}

}  // namespace

Sprite3Core::Sprite3Core() {
  m_vertices_3d.resize(MAX_SPRITES * 4);
  m_index_buffer_data.resize(MAX_SPRITES * 5);

  m_sprite_distorter_vertices.resize(MAX_DISTORT_VERTS);
  m_sprite_distorter_indices.resize(MAX_DISTORT_INDS);
  m_sprite_distorter_frame_data.resize(MAX_DISTORT_SPRITES);

  int distort_max_sprite_slices = 0;
  for (int i = 3; i < 12; i++) {
    // For each 'resolution', there can be that many slices
    distort_max_sprite_slices += i;
  }
  m_sprite_distorter_vertices_instanced.resize(distort_max_sprite_slices * 5);
  for (int i = 3; i < 12; i++) {
    m_sprite_distorter_instances_by_res[i].resize(MAX_DISTORT_SPRITES);
  }

  m_default_mode.disable_depth_write();
  m_default_mode.set_depth_test(GsTest::ZTest::GEQUAL);
  m_default_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_SRC_DST);
  m_default_mode.set_aref(38);
  m_default_mode.set_alpha_test(DrawMode::AlphaTest::GEQUAL);
  m_default_mode.set_alpha_fail(GsTest::AlphaFail::FB_ONLY);
  m_default_mode.set_at(true);
  m_default_mode.set_zt(true);
  m_default_mode.set_ab(true);

  m_current_mode = m_default_mode;
}

Sprite3Core::~Sprite3Core() = default;

void Sprite3Core::render_core(DmaFollower& dma, const Context& context) {
  switch (context.version) {
    case GameVersion::Jak1:
      render_jak1(dma, context);
      break;
    case GameVersion::Jak2:
    case GameVersion::Jak3:
    case GameVersion::JakX:
      render_jak2(dma, context);
      break;
    default:
      ASSERT_NOT_REACHED();
  }
}

/*!
 * Handle DMA data that does the per-frame setup.
 * This should get the dma chain immediately after the call to sprite-draw-distorters.
 * It ends right before the sprite-add-matrix-data for the 3d's
 */
void Sprite3Core::handle_sprite_frame_setup(DmaFollower& dma, GameVersion version) {
  // first is some direct data
  auto direct_data = dma.read_and_advance();
  ASSERT(direct_data.size_bytes == 3 * 16);
  memcpy(m_sprite_direct_setup, direct_data.data, 3 * 16);
  ASSERT(m_sprite_direct_setup[0] == 0x2000000000008001);
  ASSERT(m_sprite_direct_setup[1] == 0xEEEEEEEEEEEEEEEE);
  ASSERT(m_sprite_direct_setup[2] == 0x000000000005126B);
  ASSERT(m_sprite_direct_setup[3] == 0x0000000000000047);
  ASSERT(m_sprite_direct_setup[4] == 0x0000000000000005);
  ASSERT(m_sprite_direct_setup[5] == 0x0000000000000008);

  // next would be the program, but it's 0 size on the PC and isn't sent.

  // next is the "frame data"
  switch (version) {
    case GameVersion::Jak1: {
      auto frame_data = dma.read_and_advance();
      ASSERT(frame_data.size_bytes == (int)sizeof(SpriteFrameDataJak1));  // very cool
      ASSERT(frame_data.vifcode0().kind == VifCode::Kind::STCYCL);
      VifCodeStcycl frame_data_stcycl(frame_data.vifcode0());
      ASSERT(frame_data_stcycl.cl == 4);
      ASSERT(frame_data_stcycl.wl == 4);
      ASSERT(frame_data.vifcode1().kind == VifCode::Kind::UNPACK_V4_32);
      VifCodeUnpack frame_data_unpack(frame_data.vifcode1());
      ASSERT(frame_data_unpack.addr_qw == SpriteDataMem::FrameData);
      ASSERT(frame_data_unpack.use_tops_flag == false);
      SpriteFrameDataJak1 jak1_data;
      memcpy(&jak1_data, frame_data.data, sizeof(SpriteFrameDataJak1));
      m_frame_data.from_jak1(jak1_data);
    } break;
    case GameVersion::Jak2:
    case GameVersion::Jak3:
    case GameVersion::JakX: {
      auto frame_data = dma.read_and_advance();
      ASSERT(frame_data.size_bytes == (int)sizeof(SpriteFrameData));  // very cool
      ASSERT(frame_data.vifcode0().kind == VifCode::Kind::STCYCL);
      VifCodeStcycl frame_data_stcycl(frame_data.vifcode0());
      ASSERT(frame_data_stcycl.cl == 4);
      ASSERT(frame_data_stcycl.wl == 4);
      ASSERT(frame_data.vifcode1().kind == VifCode::Kind::UNPACK_V4_32);
      VifCodeUnpack frame_data_unpack(frame_data.vifcode1());
      ASSERT(frame_data_unpack.addr_qw == SpriteDataMem::FrameData);
      ASSERT(frame_data_unpack.use_tops_flag == false);
      memcpy(&m_frame_data, frame_data.data, sizeof(SpriteFrameData));
    } break;
    default:
      ASSERT_NOT_REACHED();
  }

  // next, a MSCALF.
  auto mscalf = dma.read_and_advance();
  ASSERT(mscalf.size_bytes == 0);
  ASSERT(mscalf.vifcode0().kind == VifCode::Kind::MSCALF);
  ASSERT(mscalf.vifcode0().immediate == SpriteProgMem::Init);
  ASSERT(mscalf.vifcode1().kind == VifCode::Kind::FLUSHE);

  // next base and offset
  auto base_offset = dma.read_and_advance();
  ASSERT(base_offset.size_bytes == 0);
  ASSERT(base_offset.vifcode0().kind == VifCode::Kind::BASE);
  ASSERT(base_offset.vifcode0().immediate == SpriteDataMem::Buffer0);
  ASSERT(base_offset.vifcode1().kind == VifCode::Kind::OFFSET);
  ASSERT(base_offset.vifcode1().immediate == SpriteDataMem::Buffer1);
}

void Sprite3Core::render_3d(DmaFollower& dma) {
  // one time matrix data
  auto matrix_data = dma.read_and_advance();
  ASSERT(matrix_data.size_bytes == sizeof(Sprite3DMatrixData));

  bool unpack_ok = verify_unpack_with_stcycl(matrix_data, VifCode::Kind::UNPACK_V4_32, 4, 4, 5,
                                             SpriteDataMem::Matrix, false, false);
  ASSERT(unpack_ok);
  static_assert(sizeof(m_3d_matrix_data) == 5 * 16);
  memcpy(&m_3d_matrix_data, matrix_data.data, sizeof(m_3d_matrix_data));
}

void Sprite3Core::render_2d_group0(DmaFollower& dma, const Context& context) {
  set_frame_constants();

  u16 last_prog = -1;

  while (looks_like_2d_chunk_start(dma)) {
    m_debug_stats.blocks_2d_grp0++;
    // 4 packets per chunk

    // first is the header
    u32 sprite_count = process_sprite_chunk_header(dma);
    m_debug_stats.count_2d_grp0 += sprite_count;

    // second is the vector data
    u32 expected_vec_size = sizeof(SpriteVecData2d) * sprite_count;
    auto vec_data = dma.read_and_advance();
    ASSERT(expected_vec_size <= sizeof(m_vec_data_2d));
    unpack_to_no_stcycl(&m_vec_data_2d, vec_data, VifCode::Kind::UNPACK_V4_32, expected_vec_size,
                        SpriteDataMem::Vector, false, true);

    // third is the adgif data
    u32 expected_adgif_size = sizeof(AdGifData) * sprite_count;
    auto adgif_data = dma.read_and_advance();
    ASSERT(expected_adgif_size <= sizeof(m_adgif));
    unpack_to_no_stcycl(&m_adgif, adgif_data, VifCode::Kind::UNPACK_V4_32, expected_adgif_size,
                        SpriteDataMem::Adgif, false, true);

    // fourth is the actual run!!!!!
    auto run = dma.read_and_advance();
    ASSERT(run.vifcode0().kind == VifCode::Kind::NOP);
    ASSERT(run.vifcode1().kind == VifCode::Kind::MSCAL);

    if (context.enabled) {
      if (run.vifcode1().immediate != last_prog) {
        // one-time setups and flushing
        flush_sprites(false);
      }

      if (run.vifcode1().immediate == SpriteProgMem::Sprites2dGrp0) {
        if (m_2d_enable) {
          do_block_common(SpriteMode::Mode2D, sprite_count, context);
        }
      } else {
        if (m_3d_enable) {
          do_block_common(SpriteMode::Mode3D, sprite_count, context);
        }
      }
      last_prog = run.vifcode1().immediate;
    }
  }
}

/*!
 * Run the pre-sprite directrenderer.
 */
bool Sprite3Core::render_direct(DmaFollower& dma, const Context& context) {
  direct_reset_state();
  while (dma.current_tag().qwc != 7 && dma.current_tag_offset() != context.next_bucket) {
    auto direct_data = dma.read_and_advance();
    direct_render_vif(direct_data.vif0(), direct_data.vif1(), direct_data.data,
                      direct_data.size_bytes);
  }
  direct_flush();

  // if sprites are off, after all the directrenderer dma, there is nothing left and we must exit
  if (dma.current_tag_offset() == context.next_bucket) {
    return true;
  }
  return false;
}

void Sprite3Core::render_fake_shadow(DmaFollower& dma) {
  // TODO
  // nop + flushe
  auto nop_flushe = dma.read_and_advance();
  ASSERT(nop_flushe.vifcode0().kind == VifCode::Kind::NOP);
  ASSERT(nop_flushe.vifcode1().kind == VifCode::Kind::FLUSHE);
}

/*!
 * Handle DMA data for group1 2d's (HUD)
 */
void Sprite3Core::render_2d_group1(DmaFollower& dma, const Context& context) {
  // one time matrix data upload
  auto mat_upload = dma.read_and_advance();
  bool mat_ok = verify_unpack_with_stcycl(mat_upload, VifCode::Kind::UNPACK_V4_32, 4, 4, 80,
                                          SpriteDataMem::Matrix, false, false);
  ASSERT(mat_ok);
  ASSERT(mat_upload.size_bytes == sizeof(m_hud_matrix_data));
  memcpy(&m_hud_matrix_data, mat_upload.data, sizeof(m_hud_matrix_data));

  set_hud_constants();

  // loop through chunks.
  while (looks_like_2d_chunk_start(dma)) {
    m_debug_stats.blocks_2d_grp1++;
    // 4 packets per chunk

    // first is the header
    u32 sprite_count = process_sprite_chunk_header(dma);
    m_debug_stats.count_2d_grp1 += sprite_count;

    // second is the vector data
    u32 expected_vec_size = sizeof(SpriteVecData2d) * sprite_count;
    auto vec_data = dma.read_and_advance();
    ASSERT(expected_vec_size <= sizeof(m_vec_data_2d));
    unpack_to_no_stcycl(&m_vec_data_2d, vec_data, VifCode::Kind::UNPACK_V4_32, expected_vec_size,
                        SpriteDataMem::Vector, false, true);

    // third is the adgif data
    u32 expected_adgif_size = sizeof(AdGifData) * sprite_count;
    auto adgif_data = dma.read_and_advance();
    ASSERT(expected_adgif_size <= sizeof(m_adgif));
    unpack_to_no_stcycl(&m_adgif, adgif_data, VifCode::Kind::UNPACK_V4_32, expected_adgif_size,
                        SpriteDataMem::Adgif, false, true);

    // fourth is the actual run!!!!!
    auto run = dma.read_and_advance();
    ASSERT(run.vifcode0().kind == VifCode::Kind::NOP);
    ASSERT(run.vifcode1().kind == VifCode::Kind::MSCAL);

    switch (context.version) {
      case GameVersion::Jak1:
        ASSERT(run.vifcode1().immediate == SpriteProgMem::Sprites2dHud_Jak1);
        break;
      case GameVersion::Jak2:
        ASSERT(run.vifcode1().immediate == SpriteProgMem::Sprites2dHud_Jak2);
        break;
      case GameVersion::Jak3:
      case GameVersion::JakX:
        ASSERT_EQ_IMM(run.vifcode1().immediate, (int)SpriteProgMem::Sprites2dHud_Jak3);
        break;
      default:
        ASSERT_NOT_REACHED();
    }
    if (context.enabled && m_2d_enable) {
      do_block_common(SpriteMode::ModeHUD, sprite_count, context);
    }
  }
}

void Sprite3Core::render_jak2(DmaFollower& dma, const Context& context) {
  m_debug_stats = {};
  auto data0 = dma.read_and_advance();
  ASSERT(data0.vif0() == 0 || data0.vifcode0().kind == VifCode::Kind::MARK);
  ASSERT(data0.vif1() == 0 || data0.vifcode1().kind == VifCode::Kind::NOP);
  ASSERT(data0.size_bytes == 0);

  if (dma.current_tag_offset() == context.next_bucket) {
    return;
  }

  // Before anything else, some directrenderer DMA might have been sent
  if (render_direct(dma, context)) {
    return;
  }

  // First is the distorter
  render_distorter(dma, context);

  // next, the normal sprite stuff
  handle_sprite_frame_setup(dma, context.version);

  // 3d sprites
  render_3d(dma);

  // 2d draw
  render_2d_group0(dma, context);
  flush_sprites(false);

  // shadow draw
  render_fake_shadow(dma);

  // 2d draw (HUD)
  render_2d_group1(dma, context);
  flush_sprites(true);
  {
    auto nop_flushe = dma.read_and_advance();
    ASSERT(nop_flushe.vifcode0().kind == VifCode::Kind::NOP);
    ASSERT(nop_flushe.vifcode1().kind == VifCode::Kind::FLUSHE);
  }

  // The glow renderer's DMA follows. A backend that does not draw it must still consume it;
  // the default hook does exactly that.
  glow_dma_and_draw(dma);

  while (dma.current_tag_offset() != context.next_bucket) {
    dma.read_and_advance();
  }
}

void Sprite3Core::render_jak1(DmaFollower& dma, const Context& context) {
  m_debug_stats = {};
  // First thing should be a NEXT with two nops. this is a jump from buckets to sprite data
  auto data0 = dma.read_and_advance();
  ASSERT(data0.vif1() == 0);
  ASSERT(data0.vif0() == 0);
  ASSERT(data0.size_bytes == 0);

  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    // sprite renderer didn't run, let's just get out of here.
    for (int i = 0; i < 4; i++) {
      dma.read_and_advance();
    }
    ASSERT(dma.current_tag_offset() == context.next_bucket);
    return;
  }

  // Before anything else, some directrenderer DMA might have been sent
  if (render_direct(dma, context)) {
    return;
  }

  // First is the distorter
  render_distorter(dma, context);

  // next, sprite frame setup.
  handle_sprite_frame_setup(dma, context.version);

  // 3d sprites
  render_3d(dma);

  // 2d draw
  render_2d_group0(dma, context);
  flush_sprites(false);

  // shadow draw
  render_fake_shadow(dma);

  // 2d draw (HUD)
  render_2d_group1(dma, context);
  flush_sprites(true);

  while (dma.current_tag_offset() != context.next_bucket) {
    dma.read_and_advance();
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Render (for real)

void Sprite3Core::flush_sprites(bool double_draw) {
  // two passes through the buckets. first to build the index buffer
  u32 idx_offset = 0;
  for (const auto bucket : m_bucket_list) {
    memcpy(&m_index_buffer_data[idx_offset], bucket->ids.data(), bucket->ids.size() * sizeof(u32));
    bucket->offset_in_idx_buffer = idx_offset;
    idx_offset += bucket->ids.size();
  }
  m_index_buffer_used = idx_offset;

  // then the backend uploads and draws them
  flush_sprites_gpu(double_draw);

  m_sprite_buckets.clear();
  m_bucket_list.clear();
  m_last_bucket_key = UINT64_MAX;
  m_last_bucket = nullptr;
  m_sprite_idx = 0;
  m_index_buffer_used = 0;
}

void Sprite3Core::handle_tex0(u64 val) {
  GsTex0 reg(val);

  // update tbp
  m_current_tbp = reg.tbp0();
  m_current_mode.set_tcc(reg.tcc());

  // tbw: assume they got it right
  // psm: assume they got it right
  // tw: assume they got it right
  // th: assume they got it right

  ASSERT(reg.tfx() == GsTex0::TextureFunction::MODULATE);
  ASSERT(reg.psm() != GsTex0::PSM::PSMT4HH);

  // cbp: assume they got it right
  // cpsm: assume they got it right
  // csm: assume they got it right
}

void Sprite3Core::handle_tex1(u64 val) {
  GsTex1 reg(val);
  m_current_mode.set_filt_enable(reg.mmag());
}

void Sprite3Core::handle_zbuf(u64 val) {
  // note: we can basically ignore this. There's a single z buffer that's always configured the same
  // way - 24-bit, at offset 448.
  GsZbuf x(val);
  ASSERT(x.psm() == TextureFormat::PSMZ24);
  ASSERT(x.zbp() == 448 || x.zbp() == 304);  // 304 for jak 2.

  m_current_mode.set_depth_write_enable(!x.zmsk());
}

void Sprite3Core::handle_clamp(u64 val) {
  if (!(val == 0b101 || val == 0 || val == 1 || val == 0b100)) {
    ASSERT_MSG(false, fmt::format("clamp: 0x{:x}", val));
  }

  m_current_mode.set_clamp_s_enable(val & 0b001);
  m_current_mode.set_clamp_t_enable(val & 0b100);
}

void Sprite3Core::update_mode_from_alpha1(u64 val, DrawMode& mode) {
  GsAlpha reg(val);
  if (reg.a_mode() == GsAlpha::BlendMode::SOURCE && reg.b_mode() == GsAlpha::BlendMode::DEST &&
      reg.c_mode() == GsAlpha::BlendMode::SOURCE && reg.d_mode() == GsAlpha::BlendMode::DEST) {
    // (Cs - Cd) * As + Cd
    // Cs * As  + (1 - As) * Cd
    mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_SRC_DST);

  } else if (reg.a_mode() == GsAlpha::BlendMode::SOURCE &&
             reg.b_mode() == GsAlpha::BlendMode::ZERO_OR_FIXED &&
             reg.c_mode() == GsAlpha::BlendMode::SOURCE &&
             reg.d_mode() == GsAlpha::BlendMode::DEST) {
    // (Cs - 0) * As + Cd
    // Cs * As + (1) * CD
    mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_0_SRC_DST);
  } else if (reg.a_mode() == GsAlpha::BlendMode::SOURCE &&
             reg.b_mode() == GsAlpha::BlendMode::ZERO_OR_FIXED &&
             reg.c_mode() == GsAlpha::BlendMode::ZERO_OR_FIXED &&
             reg.d_mode() == GsAlpha::BlendMode::DEST) {
    ASSERT(reg.fix() == 128);
    // Cv = (Cs - 0) * FIX + Cd
    // if fix = 128, it works out to 1.0
    mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_0_FIX_DST);
    // src plus dest
  } else if (reg.a_mode() == GsAlpha::BlendMode::SOURCE &&
             reg.b_mode() == GsAlpha::BlendMode::DEST &&
             reg.c_mode() == GsAlpha::BlendMode::ZERO_OR_FIXED &&
             reg.d_mode() == GsAlpha::BlendMode::DEST) {
    // Cv = (Cs - Cd) * FIX + Cd
    ASSERT(reg.fix() == 64);
    mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_FIX_DST);
  } else if (reg.a_mode() == GsAlpha::BlendMode::ZERO_OR_FIXED &&
             reg.b_mode() == GsAlpha::BlendMode::SOURCE &&
             reg.c_mode() == GsAlpha::BlendMode::SOURCE &&
             reg.d_mode() == GsAlpha::BlendMode::DEST) {
    // (0 - Cs) * As + Cd
    // Cd - Cs * As
    // s, d
    mode.set_alpha_blend(DrawMode::AlphaBlend::ZERO_SRC_SRC_DST);
  }

  else {
    lg::error("unsupported blend: a {} b {} c {} d {}", (int)reg.a_mode(), (int)reg.b_mode(),
              (int)reg.c_mode(), (int)reg.d_mode());
    mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_SRC_DST);
    ASSERT(false);
  }
}

void Sprite3Core::handle_alpha(u64 val) {
  update_mode_from_alpha1(val, m_current_mode);
}

void Sprite3Core::do_block_common(SpriteMode mode, u32 count, const Context& context) {
  m_current_mode = m_default_mode;
  for (u32 sprite_idx = 0; sprite_idx < count; sprite_idx++) {
    if (m_sprite_idx == MAX_SPRITES) {
      flush_sprites(mode == ModeHUD);
    }

    if (mode == Mode2D && context.camera_planes && m_enable_culling) {
      // we can skip sprites that are out of view
      // it's probably possible to do this for 3D as well.
      auto bsphere = m_vec_data_2d[sprite_idx].xyz_sx;
      bsphere.w() = std::max(bsphere.w(), m_vec_data_2d[sprite_idx].sy());
      if (bsphere.w() == 0 || !sphere_in_view_ref(bsphere, context.camera_planes)) {
        continue;
      }
    }

    if (context.version > GameVersion::Jak1) {
      // glow code sets the matrix to -1,
      // jak 2 adds:
      // ibltz vi08, L4
      // which is set from ilw.y vi08, 1(vi02)
      if (m_vec_data_2d[sprite_idx].matrix() == -1) {
        continue;
      }
    }

    auto& adgif = m_adgif[sprite_idx];
    handle_tex0(adgif.tex0_data);
    handle_tex1(adgif.tex1_data);
    if (GsRegisterAddress(adgif.clamp_addr) == GsRegisterAddress::ZBUF_1) {
      handle_zbuf(adgif.clamp_data);
    } else {
      handle_clamp(adgif.clamp_data);
    }
    handle_alpha(adgif.alpha_data);

    u64 key = (((u64)m_current_tbp) << 32) | m_current_mode.as_int();
    Bucket* bucket;
    if (key == m_last_bucket_key) {
      bucket = m_last_bucket;
    } else {
      auto it = m_sprite_buckets.find(key);
      if (it == m_sprite_buckets.end()) {
        bucket = &m_sprite_buckets[key];
        bucket->key = key;
        m_bucket_list.push_back(bucket);
      } else {
        bucket = &it->second;
      }
    }
    u32 start_vtx_id = m_sprite_idx * 4;
    bucket->ids.push_back(start_vtx_id);
    bucket->ids.push_back(start_vtx_id + 1);
    bucket->ids.push_back(start_vtx_id + 2);
    bucket->ids.push_back(start_vtx_id + 3);
    bucket->ids.push_back(UINT32_MAX);

    auto& vert1 = m_vertices_3d.at(start_vtx_id + 0);

    if (context.version == GameVersion::Jak3 || context.version == GameVersion::JakX) {
      auto flag = m_vec_data_2d[sprite_idx].flag();

      if ((flag & 0x10) || (flag & 0x20)) {
        // these flags mean we need to swap vertex order around - not yet implemented since it's too
        // hard to get right without this code running.
        // ASSERT_NOT_REACHED();
      }
    }

    vert1.xyz_sx = m_vec_data_2d[sprite_idx].xyz_sx;
    vert1.quat_sy = m_vec_data_2d[sprite_idx].flag_rot_sy;
    // ftoi'd in the original game, and I believe the VIF would discard the upper bits on pack
    vert1.rgba = m_vec_data_2d[sprite_idx].rgba;
    vert1.rgba.x() = (int)vert1.rgba.x() & 0xff;
    vert1.rgba.y() = (int)vert1.rgba.y() & 0xff;
    vert1.rgba.z() = (int)vert1.rgba.z() & 0xff;
    vert1.rgba.w() = (int)vert1.rgba.w() & 0xff;
    vert1.rgba /= 255;
    vert1.flags_matrix[0] = m_vec_data_2d[sprite_idx].flag();
    vert1.flags_matrix[1] = m_vec_data_2d[sprite_idx].matrix();
    vert1.info[0] = 0;  // hack
    vert1.info[1] = m_current_mode.get_tcc_enable();
    vert1.info[2] = 0;
    vert1.info[3] = mode;

    m_vertices_3d.at(start_vtx_id + 1) = vert1;
    m_vertices_3d.at(start_vtx_id + 2) = vert1;
    m_vertices_3d.at(start_vtx_id + 3) = vert1;

    m_vertices_3d.at(start_vtx_id + 1).info[2] = 1;
    m_vertices_3d.at(start_vtx_id + 2).info[2] = 3;
    m_vertices_3d.at(start_vtx_id + 3).info[2] = 2;

    // note that PC swaps the last two vertices
    if (context.version == GameVersion::Jak3) {
      auto flag = m_vec_data_2d[sprite_idx].flag();
      switch (flag & 0x30) {
        case 0x10:
          // FLAG 16: 1, 0, 3, 2
          m_vertices_3d.at(start_vtx_id + 0).info[2] = 0;
          m_vertices_3d.at(start_vtx_id + 1).info[2] = 1;
          m_vertices_3d.at(start_vtx_id + 2).info[2] = 3;
          m_vertices_3d.at(start_vtx_id + 3).info[2] = 2;
          break;
        case 0x20:
          // FLAG 32: 3, 2, 1, 0
          m_vertices_3d.at(start_vtx_id + 0).info[2] = 3;
          m_vertices_3d.at(start_vtx_id + 1).info[2] = 2;
          m_vertices_3d.at(start_vtx_id + 2).info[2] = 0;
          m_vertices_3d.at(start_vtx_id + 3).info[2] = 1;
          break;
        case 0x30:
          // 2, 3, 0, 1
          m_vertices_3d.at(start_vtx_id + 0).info[2] = 2;
          m_vertices_3d.at(start_vtx_id + 1).info[2] = 3;
          m_vertices_3d.at(start_vtx_id + 2).info[2] = 1;
          m_vertices_3d.at(start_vtx_id + 3).info[2] = 0;
          break;
      }
    }

    ++m_sprite_idx;
  }
}
