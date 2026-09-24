#pragma once

/*!
 * @file Generic2Core.h
 * The generic renderer, with no graphics API in it.
 *
 * "Generic" is what the game falls back to for geometry that does not fit any of the specialised
 * renderers: lightning, warp effects, some HUD pieces, and whatever else a level throws at it.
 * Ten buckets use it. Reading its DMA, emulating the VU program that unpacks the fragments,
 * building the vertices and sorting the draws into buckets is about 1200 lines, none of it a
 * graphics API.
 *
 * What a backend provides is one method: issue the draws the sorting produced.
 */

#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "common/dma/dma_chain_read.h"
#include "common/dma/gs.h"
#include "common/math/Vector.h"
#include "common/versions/versions.h"

#include "game/graphics/opengl_renderer/AdgifHandler.h"

class Generic2Core {
 public:
  Generic2Core(u32 num_verts = 500000,
               u32 num_frags = 10000,
               u32 num_adgif = 10000,
               u32 num_buckets = 800);
  virtual ~Generic2Core();

  enum class Mode { NORMAL, LIGHTNING, WARP, PRIM };

  // Reads one bucket's data and draws it. `next_bucket` is where that data ends.
  void render_in_mode(DmaFollower& dma, GameVersion version, u32 next_bucket, Mode mode);

  void draw_debug_window();
  bool empty() { return m_empty; }

  struct Vertex {
    math::Vector<float, 3> xyz;
    math::Vector<u8, 4> rgba;
    math::Vector<float, 2> st;  // 16
    u8 tex_unit;
    u8 flags;
    u8 adc;
    u8 pad0;
    u32 pad1;
  };
  static_assert(sizeof(Vertex) == 32);

 protected:
  // Issue the draws. A backend walks m_buckets, which are already in the order they have to be
  // drawn in, and reads its state out of the members below.
  virtual void do_draws() = 0;

  void determine_draw_modes(bool enable_at, bool default_fog);
  void build_index_buffer();
  void link_adgifs_back_to_frags();
  void draws_to_buckets();
  void reset_buffers();
  void process_matrices();
  void process_dma_jak1(DmaFollower& dma, u32 next_bucket);
  void process_dma_lightning(DmaFollower& dma, u32 next_bucket);
  void process_dma_jak2(DmaFollower& dma, u32 next_bucket);
  void process_dma_prim(DmaFollower& dma, u32 next_bucket);
  void setup_draws(bool enable_at, bool default_fog);
  bool check_for_end_of_generic_data(DmaFollower& dma, u32 next_bucket);
  void final_vertex_update();
  bool handle_bucket_setup_dma(DmaFollower& dma, u32 next_bucket);


  struct {
    u32 stcycl;
  } m_dma_unpack;

  struct DrawingConfig {
    bool zmsk = false;
    // horizontal, vertical, depth, fog offsets.
    math::Vector4f hvdf_offset;
    float pfog0;             // scale factor for perspective divide
    float fog_min, fog_max;  // clamp for fog
    math::Vector3f proj_scale;
    float proj_mat_23, proj_mat_32;

    math::Vector3f hud_scale;
    float hud_mat_23, hud_mat_32, hud_mat_33;

    bool uses_hud = false;
    bool uses_full_matrix = false;
    math::Vector4f full_matrix[4];
  } m_drawing_config;

  struct GsState {
    DrawMode as_mode;
    u16 tbp;
    GsTest gs_test;
    GsTex0 gs_tex0;
    GsPrim gs_prim;
    GsAlpha gs_alpha;
    u8 tex_unit = 0;

    u8 vertex_flags = 0;
    void set_tcc_flag(bool value) { vertex_flags ^= (-(u8)value ^ vertex_flags) & 1; }
    void set_decal_flag(bool value) { vertex_flags ^= (-(u8)value ^ vertex_flags) & 2; }
    void set_fog_flag(bool value) { vertex_flags ^= (-(u8)value ^ vertex_flags) & 4; }

  } m_gs;

  static constexpr u32 FRAG_HEADER_SIZE = 16 * 7;
  struct Fragment {
    u8 header[FRAG_HEADER_SIZE];
    u32 adgif_idx = 0;
    u32 adgif_count = 0;

    u32 vtx_idx = 0;
    u32 vtx_count = 0;
    u8 mscal_addr = 0;
    bool uses_hud;
  };

  struct Adgif {
    AdGifData data;
    DrawMode mode;
    u32 tbp;
    u32 fix;
    u8 vtx_flags;
    u32 frag;
    u32 vtx_idx;
    u32 vtx_count;
    bool uses_hud;

    u32 next = -2;

    u64 key() const {
      u64 result = mode.as_int();
      result |= (((u64)tbp) << 32);
      result |= (((u64)fix) << 48);
      result |= (((u64)uses_hud ? 1ull : 0ull) << 62);
      return result;
    }
  };

  struct Bucket {
    DrawMode mode;
    u32 tbp;
    u32 start = UINT32_MAX;
    u32 last = UINT32_MAX;

    u32 idx_idx;
    u32 idx_count;

    u32 tri_count;  // just for debug
  };

  u32 handle_fragments_after_unpack_v4_32(const u8* data,
                                          u32 off,
                                          u32 first_unpack_bytes,
                                          u32 end_of_vif,
                                          Fragment* frag,
                                          bool loop);

  u32 m_next_free_frag = 0;
  std::vector<Fragment> m_fragments;
  u32 m_max_frags_seen = 0;

  u32 m_next_free_vert = 0;
  std::vector<Vertex> m_verts;
  u32 m_max_verts_seen = 0;

  u32 m_next_free_adgif = 0;
  std::vector<Adgif> m_adgifs;
  u32 m_max_adgifs_seen = 0;

  u32 m_next_free_bucket = 0;
  std::vector<Bucket> m_buckets;
  u32 m_max_buckets_seen = 0;

  u32 m_next_free_idx = 0;
  std::vector<u32> m_indices;
  u32 m_max_indices_seen = 0;

  Fragment& next_frag() {
    ASSERT(m_next_free_frag < m_fragments.size());
    return m_fragments[m_next_free_frag++];
  }

  Adgif& next_adgif() {
    ASSERT(m_next_free_adgif < m_adgifs.size());
    return m_adgifs[m_next_free_adgif++];
  }

  void alloc_vtx(int count) {
    m_next_free_vert += count;
    ASSERT(m_next_free_vert < m_verts.size());
  }

  static constexpr int ALPHA_MODE_COUNT = 7;
  bool m_alpha_draw_enable[ALPHA_MODE_COUNT] = {true, true, true, true, true, true, true};

  bool m_empty = false;
};
