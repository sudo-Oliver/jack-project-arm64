#include "Generic2.h"

#include <cfloat>
#include <cmath>

namespace {
bool generic2_matrix_finite(const std::array<math::Vector4f, 4>& mat) {
  for (const auto& row : mat) {
    for (int i = 0; i < 4; i++) {
      if (!std::isfinite(row[i])) {
        return false;
      }
    }
  }
  return true;
}

bool generic2_matrix_nonzero(const std::array<math::Vector4f, 4>& mat) {
  for (const auto& row : mat) {
    for (int i = 0; i < 4; i++) {
      if (std::fabs(row[i]) > 1e-20f) {
        return true;
      }
    }
  }
  return false;
}

void generic2_fix_arm64_projection_layout(std::array<math::Vector4f, 4>& mat) {
  // ARM64 path can deliver the PS2 projection coefficient in m33. Generic2's shader expects
  // it in m23, with m33 zero, so normalize this exact projection shape before classification.
  if (generic2_matrix_finite(mat) && std::fabs(mat[2][3]) < 1e-20f &&
      std::fabs(mat[3][3]) > 1e-8f && std::fabs(mat[3][3]) < 1.f &&
      std::fabs(mat[3][2]) > 1000000.f && std::fabs(mat[3][2]) < 20000000.f) {
    mat[2][3] = mat[3][3];
    mat[3][3] = 0.f;
  }
}

bool generic2_valid_projection_matrix(const std::array<math::Vector4f, 4>& mat) {
  return generic2_matrix_finite(mat) && generic2_matrix_nonzero(mat) && mat[3][3] == 0.f &&
         (std::fabs(mat[0][0]) > 1e-20f || std::fabs(mat[1][1]) > 1e-20f ||
          std::fabs(mat[2][2]) > 1e-20f || std::fabs(mat[2][3]) > 1e-20f ||
          std::fabs(mat[3][2]) > 1e-20f);
}

bool generic2_usable_matrix(const std::array<math::Vector4f, 4>& mat) {
  return generic2_matrix_finite(mat) && generic2_matrix_nonzero(mat) &&
         (std::fabs(mat[0][0]) > 1e-20f || std::fabs(mat[1][1]) > 1e-20f ||
          std::fabs(mat[2][2]) > 1e-20f || std::fabs(mat[2][3]) > 1e-20f ||
          std::fabs(mat[3][2]) > 1e-20f || std::fabs(mat[3][3]) > 1e-20f);
}

bool generic2_valid_hud_matrix(const std::array<math::Vector4f, 4>& mat) {
  return generic2_matrix_finite(mat) && generic2_matrix_nonzero(mat) && mat[3][3] != 0.f;
}

float generic2_matrix_absmax(const std::array<math::Vector4f, 4>& mat) {
  float result = 0.f;
  for (const auto& row : mat) {
    for (int i = 0; i < 4; i++) {
      result = std::max(result, std::fabs(row[i]));
    }
  }
  return result;
}
}  // namespace

/*!
 * Main function to set up Generic2 draw lists.
 * This function figures out which vertices belong to which draw settings.
 */
void Generic2::setup_draws(bool enable_at, bool default_fog) {
  if (m_next_free_frag == 0) {
    return;
  }
  m_gs = GsState();
  link_adgifs_back_to_frags();
  process_matrices();
  determine_draw_modes(enable_at, default_fog);
  draws_to_buckets();
  final_vertex_update();
  build_index_buffer();

  static u32 s_generic2_frame = 0;
  s_generic2_frame++;
  if (s_generic2_frame <= 20 || (s_generic2_frame % 300) == 0) {
    u32 empty_adgifs = 0;
    u32 invalid_mode_adgifs = 0;
    for (u32 i = 0; i < m_next_free_adgif; i++) {
      if (m_adgifs[i].vtx_count < 3) {
        empty_adgifs++;
      }
      if (m_adgifs[i].mode.is_invalid()) {
        invalid_mode_adgifs++;
      }
    }
    math::Vector3f min_xyz(FLT_MAX, FLT_MAX, FLT_MAX);
    math::Vector3f max_xyz(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    u32 finite_xyz = 0;
    for (u32 i = 0; i < m_next_free_vert; i++) {
      if (std::isfinite(m_verts[i].xyz.x()) && std::isfinite(m_verts[i].xyz.y()) &&
          std::isfinite(m_verts[i].xyz.z())) {
        finite_xyz++;
        for (int c = 0; c < 3; c++) {
          min_xyz[c] = std::min(min_xyz[c], m_verts[i].xyz[c]);
          max_xyz[c] = std::max(max_xyz[c], m_verts[i].xyz[c]);
        }
      }
    }
    u32 first_xyz_bits[3] = {};
    if (m_next_free_vert > 0) {
      memcpy(first_xyz_bits, m_verts[0].xyz.data(), sizeof(first_xyz_bits));
    }
    fmt::print("[Generic2] frame={} frags={} adgifs={} verts={} buckets={} indices={} "
               "empty-adgifs={} invalid-mode={} finite-xyz={} xyz0={:08x},{:08x},{:08x} "
               "xyz-min=({:.2f},{:.2f},{:.2f}) "
               "xyz-max=({:.2f},{:.2f},{:.2f}) proj=({:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f}) "
               "hud=({:.3f},{:.3f},{:.3f},{:.3f},{:.3f}) "
               "fog=({:.3f},{:.3f},{:.3f}) hvdf=({:.3f},{:.3f},{:.3f},{:.3f})\n",
               s_generic2_frame, m_next_free_frag, m_next_free_adgif, m_next_free_vert,
               m_next_free_bucket, m_next_free_idx, empty_adgifs, invalid_mode_adgifs,
               finite_xyz, first_xyz_bits[0], first_xyz_bits[1], first_xyz_bits[2], min_xyz.x(),
               min_xyz.y(), min_xyz.z(), max_xyz.x(), max_xyz.y(), max_xyz.z(),
               m_drawing_config.proj_scale.x(), m_drawing_config.proj_scale.y(),
               m_drawing_config.proj_scale.z(), m_drawing_config.proj_mat_23,
               m_drawing_config.proj_mat_32, m_drawing_config.proj_mat_33,
               m_drawing_config.hud_scale.x(), m_drawing_config.hud_scale.y(),
               m_drawing_config.hud_scale.z(), m_drawing_config.hud_mat_23,
               m_drawing_config.hud_mat_32, m_drawing_config.pfog0,
               m_drawing_config.fog_min, m_drawing_config.fog_max,
               m_drawing_config.hvdf_offset.x(), m_drawing_config.hvdf_offset.y(),
               m_drawing_config.hvdf_offset.z(), m_drawing_config.hvdf_offset.w());
  }
}

/*!
 * For each adgif, determine the draw mode.
 * There's a bunch of stuff in adgifs that don't really matter, and this filters out all that junk
 * They also do a bunch of tricks where some of the GS state is left over from the previous draw.
 *
 * For each adgif, it determines the "draw mode" which is used as a unique identifier for OpenGL
 * settings, the tbp (texture vram address), and the "vertex flags" that need to be set for each
 * vertex.  This information is used in later steps.
 */
void Generic2::determine_draw_modes(bool enable_at, bool default_fog) {
  // initialize draw mode
  DrawMode current_mode;
  current_mode.set_at(enable_at);
  current_mode.set_alpha_test(DrawMode::AlphaTest::GEQUAL);
  current_mode.set_aref(0x26);
  current_mode.set_alpha_fail(GsTest::AlphaFail::FB_ONLY);
  current_mode.set_zt(true);
  current_mode.set_depth_test(GsTest::ZTest::GEQUAL);
  current_mode.set_depth_write_enable(!m_drawing_config.zmsk);
  current_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_SRC_SRC_SRC);
  m_gs.set_fog_flag(default_fog);

  u32 tbp = -1;

  // these are copies of the state
  GsTex0 tex0;
  tex0.data = UINT64_MAX;

  // iterate over all adgifs
  for (u32 i = 0; i < m_next_free_adgif; i++) {
    auto& draw = m_adgifs[i];
    auto& ad = draw.data;
    auto& frag = m_fragments[draw.frag];
    draw.uses_hud = frag.uses_hud;

    // the header contains the giftag which might set fogging based on pre.
    GifTag tag(frag.header + (4 * 16));
    if (tag.pre()) {
      GsPrim prim(tag.prim());
      m_gs.set_fog_flag(prim.fge());
    }

    if ((u8)ad.tex0_addr != (u32)GsRegisterAddress::TEX0_1 ||
        (u8)ad.tex1_addr != (u32)GsRegisterAddress::TEX1_1 ||
        (u8)ad.mip_addr != (u32)GsRegisterAddress::MIPTBP1_1 ||
        (u8)ad.clamp_addr != (u32)GsRegisterAddress::CLAMP_1) {
      draw.vtx_count = 0;
      continue;
    }

    // ADGIF 0
    if (ad.tex0_data != tex0.data) {
      tex0.data = ad.tex0_data;
      GsTex0 reg(ad.tex0_data);
      tbp = reg.tbp0();
      // tbw
      if (reg.psm() == GsTex0::PSM::PSMT4HH) {
        tbp |= 0x8000;
      }
      // tw/th
      current_mode.set_tcc(reg.tcc());
      m_gs.set_tcc_flag(reg.tcc());
      bool decal = reg.tfx() == GsTex0::TextureFunction::DECAL;
      current_mode.set_decal(decal);
      m_gs.set_decal_flag(decal);
      ASSERT(reg.tfx() == GsTex0::TextureFunction::DECAL ||
             reg.tfx() == GsTex0::TextureFunction::MODULATE);
    }

    // ADGIF 1
    {
      GsTex1 reg(ad.tex1_data);
      current_mode.set_filt_enable(reg.mmag());
    }

    // ADGIF 2

    // ADGIF 3
    {
      bool clamp_s = ad.clamp_data & 0b001;
      bool clamp_t = ad.clamp_data & 0b100;
      current_mode.set_clamp_s_enable(clamp_s);
      current_mode.set_clamp_t_enable(clamp_t);
    }

    std::optional<u64> final_alpha;

    // ADGIF 4
    if ((u8)ad.alpha_addr == (u32)GsRegisterAddress::ALPHA_1) {
      final_alpha = ad.alpha_data;
    } else {
      ASSERT((u8)ad.alpha_addr == (u32)GsRegisterAddress::MIPTBP2_1);
    }

    u64 bonus_adgif_data[4];
    memcpy(bonus_adgif_data, frag.header + (5 * 16), 4 * sizeof(u64));

    std::optional<u64> final_test;
    if ((u8)bonus_adgif_data[1] == (u8)(GsRegisterAddress::ALPHA_1)) {
      ASSERT((u8)bonus_adgif_data[1] == (u8)(GsRegisterAddress::ALPHA_1));
      final_alpha = bonus_adgif_data[0];
      ASSERT((u8)bonus_adgif_data[3] == (u8)(GsRegisterAddress::TEST_1));
      final_test = bonus_adgif_data[2];
    } else {
      // ADGIF 5
      if ((u8)bonus_adgif_data[1] == (u8)(GsRegisterAddress::TEST_1)) {
        final_test = bonus_adgif_data[0];
      }

      // ADGIF 6
      if ((u8)bonus_adgif_data[3] == (u8)(GsRegisterAddress::ALPHA_1)) {
        final_alpha = bonus_adgif_data[2];
      } else {
        if ((u8)bonus_adgif_data[3] == (u8)(GsRegisterAddress::TEST_1)) {
          final_test = bonus_adgif_data[2];
        }
      }
    }

    if (final_alpha) {
      GsAlpha reg(*final_alpha);
      if (m_gs.gs_alpha != reg) {
        m_gs.gs_alpha = reg;
        auto a = reg.a_mode();
        auto b = reg.b_mode();
        auto c = reg.c_mode();
        auto d = reg.d_mode();
        if (a == GsAlpha::BlendMode::SOURCE && b == GsAlpha::BlendMode::DEST &&
            c == GsAlpha::BlendMode::SOURCE && d == GsAlpha::BlendMode::DEST) {
          current_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_SRC_DST);
        } else if (a == GsAlpha::BlendMode::SOURCE && b == GsAlpha::BlendMode::ZERO_OR_FIXED &&
                   c == GsAlpha::BlendMode::SOURCE && d == GsAlpha::BlendMode::DEST) {
          current_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_0_SRC_DST);
        } else if (a == GsAlpha::BlendMode::ZERO_OR_FIXED && b == GsAlpha::BlendMode::SOURCE &&
                   c == GsAlpha::BlendMode::SOURCE && d == GsAlpha::BlendMode::DEST) {
          current_mode.set_alpha_blend(DrawMode::AlphaBlend::ZERO_SRC_SRC_DST);
        } else if (a == GsAlpha::BlendMode::SOURCE && b == GsAlpha::BlendMode::DEST &&
                   c == GsAlpha::BlendMode::ZERO_OR_FIXED && d == GsAlpha::BlendMode::DEST) {
          current_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_FIX_DST);
        } else if (a == GsAlpha::BlendMode::SOURCE && b == GsAlpha::BlendMode::SOURCE &&
                   c == GsAlpha::BlendMode::SOURCE && d == GsAlpha::BlendMode::SOURCE) {
          current_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_SRC_SRC_SRC);
        } else if (a == GsAlpha::BlendMode::SOURCE && b == GsAlpha::BlendMode::ZERO_OR_FIXED &&
                   c == GsAlpha::BlendMode::DEST && d == GsAlpha::BlendMode::DEST) {
          current_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_0_DST_DST);
        } else if (a == GsAlpha::BlendMode::SOURCE && b == GsAlpha::BlendMode::ZERO_OR_FIXED &&
                   c == GsAlpha::BlendMode::ZERO_OR_FIXED && d == GsAlpha::BlendMode::DEST) {
          current_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_0_FIX_DST);
        } else {
          fmt::print("unsupported blend: a {} b {} c {} d {}\n", (int)a, (int)b, (int)c, (int)d);
          // ASSERT(false);
        }
      }
    }

    if (final_test) {
      GsTest reg(*final_test);
      current_mode.set_at(reg.alpha_test_enable());
      if (reg.alpha_test_enable()) {
        switch (reg.alpha_test()) {
          case GsTest::AlphaTest::NEVER:
            current_mode.set_alpha_test(DrawMode::AlphaTest::NEVER);
            break;
          case GsTest::AlphaTest::ALWAYS:
            current_mode.set_alpha_test(DrawMode::AlphaTest::ALWAYS);
            break;
          case GsTest::AlphaTest::GEQUAL:
            current_mode.set_alpha_test(DrawMode::AlphaTest::GEQUAL);
            break;
          default:
            ASSERT(false);
        }
      }

      current_mode.set_aref(reg.aref());
      current_mode.set_alpha_fail(reg.afail());
      current_mode.set_zt(reg.zte());
      current_mode.set_depth_test(reg.ztest());

      // detect a strange way of disabling z writes by light-trail.
      // we don't actually handle alpha_fail later on in Direct - it doesn't map well to modern
      // graphics and would require a draw call per primitive.
      if (current_mode.get_alpha_fail() == GsTest::AlphaFail::FB_ONLY &&
          current_mode.get_aref() == 0x80 &&
          current_mode.get_alpha_test() == DrawMode::AlphaTest::GEQUAL) {
        current_mode.set_alpha_test(DrawMode::AlphaTest::ALWAYS);
        current_mode.disable_depth_write();
      }

      // another way to disable z-writing:
      if (current_mode.get_alpha_test() == DrawMode::AlphaTest::NEVER &&
          current_mode.get_alpha_fail() == GsTest::AlphaFail::FB_ONLY) {
        current_mode.set_alpha_test(DrawMode::AlphaTest::ALWAYS);
        current_mode.disable_depth_write();
      }
    }

    m_adgifs[i].mode = current_mode;
    m_adgifs[i].vtx_flags = m_gs.vertex_flags;
    m_adgifs[i].tbp = tbp;
    m_adgifs[i].fix = m_gs.gs_alpha.fix();
  }
}

/*!
 * For each adgif, figure out the vertices that it belongs to, in the giant vertex buffer.
 */
void Generic2::link_adgifs_back_to_frags() {
  for (u32 i = 0; i < m_next_free_frag; i++) {
    auto& frag = m_fragments[i];
    for (u32 j = 0; j < frag.adgif_count; j++) {
      auto& ad = m_adgifs[frag.adgif_idx + j];
      ad.vtx_count = (ad.data.tex1_addr >> 32) & 0xfff;  // drop the eop flag
      ad.vtx_idx = frag.vtx_idx + ((ad.data.tex0_addr >> 32) & 0xffff) / 3;
      const auto frag_end = frag.vtx_count + frag.vtx_idx;
      if (ad.vtx_idx >= frag_end) {
        ad.vtx_idx = frag_end;
        ad.vtx_count = 0;
      } else if (ad.vtx_count + ad.vtx_idx > frag_end) {
        ad.vtx_count = frag_end - ad.vtx_idx;
      }
      ad.frag = i;
    }
  }
}

/*!
 * Build linked lists of adgifs that share the same settings.
 * TODO: also determine texture units per bucket here.
 */
void Generic2::draws_to_buckets() {
  std::unordered_map<u64, u32> draw_key_to_bucket;
  for (u32 i = 0; i < m_next_free_adgif; i++) {
    auto& ad = m_adgifs[i];
    if (ad.vtx_count < 3) {
      ad.next = UINT32_MAX;
      continue;
    }
    if (ad.uses_hud) {
      // put all hud draws in separate buckets.
      // there's some really weird messed up draws for the orbs that fly up to the corner when
      // breaking a crate on a zoomer.
      u32 bucket_idx = m_next_free_bucket++;
      ASSERT(bucket_idx < m_buckets.size());
      draw_key_to_bucket[ad.key()] = bucket_idx;
      auto& bucket = m_buckets[bucket_idx];
      bucket.tbp = ad.tbp;
      bucket.mode = ad.mode;
      bucket.start = i;
      bucket.last = i;
      ad.next = UINT32_MAX;
    } else {
      u64 key = ad.key();
      const auto& bucket_it = draw_key_to_bucket.find(key);
      if (bucket_it == draw_key_to_bucket.end()) {
        // new bucket!
        u32 bucket_idx = m_next_free_bucket++;
        ASSERT(bucket_idx < m_buckets.size());
        draw_key_to_bucket[key] = bucket_idx;
        auto& bucket = m_buckets[bucket_idx];
        bucket.tbp = ad.tbp;
        bucket.mode = ad.mode;
        bucket.start = i;
        bucket.last = i;
        ad.next = UINT32_MAX;
      } else {
        // existing bucket!
        auto& bucket = m_buckets[bucket_it->second];
        m_adgifs[bucket.last].next = i;
        ad.next = UINT32_MAX;
        bucket.last = i;
      }
    }
  }
}

/*!
 * Extract the matrix. They are exactly a perspective projection and they are all the same.
 * I don't think this will hold for TIE...
 */
void Generic2::process_matrices() {
  // first, we need to find the projection matrix.
  // most of the time, it's first. If you have the hud open, there may be a few others.
  bool found_proj_matrix = false;
  bool projection_from_fallback = false;
  u32 projection_frag_idx = UINT32_MAX;
  std::array<math::Vector4f, 4> projection_matrix, hud_matrix;
  for (u32 i = 0; i < m_next_free_frag; i++) {
    if (m_drawing_config.uses_full_matrix) {
      memcpy(&m_drawing_config.full_matrix, m_fragments[i].header, 64);
      break;
    }
    std::array<math::Vector4f, 4> candidate;
    memcpy(&candidate, m_fragments[i].header, 64);
    generic2_fix_arm64_projection_layout(candidate);
    if (generic2_valid_projection_matrix(candidate)) {
      // got it.
      projection_matrix = candidate;
      found_proj_matrix = true;
      projection_frag_idx = i;
      break;
    }
  }

  if (!found_proj_matrix) {
    for (u32 i = 0; i < m_next_free_frag; i++) {
      std::array<math::Vector4f, 4> candidate;
      memcpy(&candidate, m_fragments[i].header, 64);
      generic2_fix_arm64_projection_layout(candidate);
      if (generic2_usable_matrix(candidate)) {
        projection_matrix = candidate;
        found_proj_matrix = true;
        projection_from_fallback = true;
        projection_frag_idx = i;
        break;
      }
    }
    if (!found_proj_matrix) {
      for (auto& row : projection_matrix) {
        row.fill(0);
      }
    }
  }

  // mark as hud/proj
  bool found_hud_matrix = false;
  for (u32 i = 0; i < m_next_free_frag; i++) {
    std::array<math::Vector4f, 4> candidate;
    memcpy(&candidate, m_fragments[i].header, 64);
    generic2_fix_arm64_projection_layout(candidate);
    if (generic2_valid_projection_matrix(candidate) ||
        (projection_from_fallback && generic2_usable_matrix(candidate))) {
      m_fragments[i].uses_hud = false;
    } else {
      m_fragments[i].uses_hud = true;
      if (!found_hud_matrix && generic2_valid_hud_matrix(candidate)) {
        found_hud_matrix = true;
        hud_matrix = candidate;
      }
    }
  }

  m_drawing_config.proj_scale[0] = projection_matrix[0][0];
  m_drawing_config.proj_scale[1] = projection_matrix[1][1];
  m_drawing_config.proj_scale[2] = projection_matrix[2][2];
  m_drawing_config.proj_mat_23 = projection_matrix[2][3];
  m_drawing_config.proj_mat_32 = projection_matrix[3][2];
  m_drawing_config.proj_mat_33 = projection_matrix[3][3];

  if (found_hud_matrix) {
    m_drawing_config.hud_scale[0] = hud_matrix[0][0];
    m_drawing_config.hud_scale[1] = hud_matrix[1][1];
    m_drawing_config.hud_scale[2] = hud_matrix[2][2];
    m_drawing_config.hud_mat_23 = hud_matrix[2][3];
    m_drawing_config.hud_mat_32 = hud_matrix[3][2];
    m_drawing_config.hud_mat_33 = hud_matrix[3][3];
  }

  m_drawing_config.uses_hud = found_hud_matrix;

  static u32 s_matrix_debug_count = 0;
  const bool bad_projection =
      found_proj_matrix && (std::fabs(m_drawing_config.proj_mat_32) > 100000.f ||
                            !std::isfinite(m_drawing_config.proj_mat_32));
  if (s_matrix_debug_count < 48 &&
      (!found_proj_matrix || !found_hud_matrix || projection_from_fallback || bad_projection)) {
    s_matrix_debug_count++;
    float first[16] = {};
    if (m_next_free_frag > 0) {
      memcpy(first, m_fragments[0].header, sizeof(first));
    }
    fmt::print("[Generic2:matrix] n={} frags={} found-proj={} found-hud={} "
               "fallback={} bad-proj={} proj-frag={} "
               "first-diag=({:.3f},{:.3f},{:.3f},{:.3f}) first-row0=({:.3f},{:.3f},{:.3f},{:.3f})\n",
               s_matrix_debug_count, m_next_free_frag, found_proj_matrix, found_hud_matrix,
               projection_from_fallback, bad_projection, projection_frag_idx, first[0], first[5],
               first[10], first[15], first[0], first[1], first[2], first[3]);
    const u32 dump_count = std::min<u32>(m_next_free_frag, 8);
    for (u32 i = 0; i < dump_count; i++) {
      std::array<math::Vector4f, 4> candidate;
      memcpy(&candidate, m_fragments[i].header, 64);
      generic2_fix_arm64_projection_layout(candidate);
      fmt::print("[Generic2:matrix-cand] dbg={} frag={} proj={} usable={} hud={} absmax={:.3f} "
                 "diag=({:.3f},{:.3f},{:.3f},{:.3f}) m23={:.3f} m32={:.3f} m33={:.3f} "
                 "r0=({:.3f},{:.3f},{:.3f},{:.3f}) r3=({:.3f},{:.3f},{:.3f},{:.3f})\n",
                 s_matrix_debug_count, i, generic2_valid_projection_matrix(candidate),
                 generic2_usable_matrix(candidate), generic2_valid_hud_matrix(candidate),
                 generic2_matrix_absmax(candidate), candidate[0][0], candidate[1][1],
                 candidate[2][2], candidate[3][3], candidate[2][3], candidate[3][2],
                 candidate[3][3], candidate[0][0], candidate[0][1], candidate[0][2],
                 candidate[0][3], candidate[3][0], candidate[3][1], candidate[3][2],
                 candidate[3][3]);
    }
  }
}

/*!
 * After all bucketing/draw modes have been determined, fill out the flag fields of all vertices.
 * TODO: fill out texture units
 */
void Generic2::final_vertex_update() {
  for (u32 i = 0; i < m_next_free_adgif; i++) {
    auto& ad = m_adgifs[i];
    for (u32 j = 0; j < ad.vtx_count; j++) {
      m_verts[ad.vtx_idx + j].flags = ad.vtx_flags;
    }
  }
}

/*!
 * Build the index buffer.
 */
void Generic2::build_index_buffer() {
  for (u32 bucket_idx = 0; bucket_idx < m_next_free_bucket; bucket_idx++) {
    auto& bucket = m_buckets[bucket_idx];
    bucket.tri_count = 0;
    bucket.idx_idx = m_next_free_idx;

    u32 adgif_idx = bucket.start;
    while (adgif_idx != UINT32_MAX) {
      auto& adgif = m_adgifs[adgif_idx];
      if (adgif.vtx_count < 3) {
        adgif_idx = adgif.next;
        continue;
      }
      m_indices[m_next_free_idx++] = UINT32_MAX;
      for (u32 vidx = adgif.vtx_idx; vidx < adgif.vtx_idx + adgif.vtx_count; vidx++) {
        auto& vtx = m_verts[vidx];
        if (vtx.adc) {
          m_indices[m_next_free_idx++] = vidx;
          bucket.tri_count++;
        } else {
          m_indices[m_next_free_idx++] = UINT32_MAX;
          m_indices[m_next_free_idx++] = vidx - 1;
          m_indices[m_next_free_idx++] = vidx;
        }
      }
      bucket.tri_count -= 2;
      adgif_idx = adgif.next;
    }

    bucket.idx_count = m_next_free_idx - bucket.idx_idx;
  }
}
