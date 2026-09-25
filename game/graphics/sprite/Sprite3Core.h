#pragma once

/*!
 * @file Sprite3Core.h
 * The sprite renderer with no graphics API in it.
 *
 * The sprite bucket draws everything the game builds out of camera-facing quads: particles, the
 * HUD, the menus, the fake shadows, and the heat-haze distorters. Of that work, only the last
 * step -- uploading the vertices and issuing the draws -- is backend-specific. Reading the DMA,
 * emulating the VU program that turns a "sprite" into four vertices, bucketing the sprites by
 * texture and GS draw mode, and expanding the distorter's sine tables into triangle strips are
 * all identical for OpenGL and Metal, and subtle enough that a second copy would be a second set
 * of bugs.
 *
 * So all of that lives here, and a backend implements the handful of methods below that touch the
 * GPU. The data those methods read is protected rather than passed as arguments: the buffers are
 * large, the backends need the same ones, and a parameter list long enough to carry them all is
 * harder to keep honest than a shared member.
 */

#include <map>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/dma/dma_chain_read.h"
#include "common/dma/gs.h"
#include "common/math/Vector.h"
#include "common/versions/versions.h"

#include "game/graphics/sprite/sprite_common.h"

class Sprite3Core {
 public:
  Sprite3Core();
  virtual ~Sprite3Core();

  static constexpr int SPRITES_PER_CHUNK = 48;

  // How many sprites and distort sprites the buffers hold. A backend allocates its GPU buffers
  // from these, so they are public.
  static constexpr int MAX_SPRITES = 1920 * 12;
  // size of sprite-aux-list in GOAL code * SPRITE_MAX_AMOUNT_MULT
  static constexpr int MAX_DISTORT_SPRITES = 256 * 12;
  // max * ((verts_per_slice - 1) * max_slices + 1)
  static constexpr int MAX_DISTORT_VERTS = MAX_DISTORT_SPRITES * ((5 - 1) * 11 + 1);
  // max * ((verts_per_slice * max_slices) + 1)
  static constexpr int MAX_DISTORT_INDS = MAX_DISTORT_SPRITES * ((5 * 11) + 1);

  /*!
   * One sprite as the vertex shader wants it. Four of these, identical apart from info[2], make
   * one quad; the shader turns the quad corner index into a corner.
   */
  struct SpriteVertex3D {
    math::Vector4f xyz_sx;              // position + x scale
    math::Vector4f quat_sy;             // quaternion + y scale
    math::Vector4f rgba;                // color
    math::Vector<u16, 2> flags_matrix;  // flags + matrix... split
    math::Vector<u16, 4> info;
    math::Vector<u8, 4> pad;
  };
  static_assert(sizeof(SpriteVertex3D) == 64);

  struct SpriteDistortVertex {
    math::Vector3f xyz;
    math::Vector2f st;
  };

  struct SpriteDistortInstanceData {
    math::Vector4f x_y_z_s;     // position, S-texture coord
    math::Vector4f sx_sy_sz_t;  // scale, T-texture coord
  };

  /*!
   * The sprites that share one texture and one GS draw mode, and therefore one draw call.
   */
  struct Bucket {
    std::vector<u32> ids;
    u32 offset_in_idx_buffer = 0;
    u64 key = -1;

    // The texture page and the draw mode the key packs.
    u32 tbp() const { return key >> 32; }
    DrawMode mode() const {
      DrawMode m;
      m.as_int() = key & 0xffffffff;
      return m;
    }
  };

 protected:
  /*!
   * What the core needs from the frame that is not its own. A backend fills this in before
   * calling render_core().
   */
  struct Context {
    GameVersion version = GameVersion::Jak1;
    // Where this bucket's data ends.
    u32 next_bucket = 0;
    // 2D sprites are culled against the camera's frustum. Null disables culling, which is what
    // the OpenGL backend does when the level has no PC data.
    const math::Vector4f* camera_planes = nullptr;
    // The bucket's debug toggle. A disabled bucket still walks its DMA, it just draws nothing.
    bool enabled = true;
  };

  // Walks one bucket's DMA and issues the draws through the hooks below.
  void render_core(DmaFollower& dma, const Context& context);

  // --- what a backend implements -------------------------------------------------------------

  // The per-frame constants for the world (group 0 and 3D) sprites, from m_3d_matrix_data and
  // m_frame_data. Called once per frame, before any sprite is drawn.
  virtual void set_frame_constants() = 0;
  // The per-frame constants for the HUD sprites, from m_hud_matrix_data.
  virtual void set_hud_constants() = 0;
  // Upload m_vertices_3d (m_sprite_idx sprites, four vertices each) and m_index_buffer_data, then
  // draw every bucket in m_bucket_list. `double_draw` asks for the alpha-fail second pass, which
  // is what the HUD needs and the world sprites do not.
  virtual void flush_sprites_gpu(bool double_draw) = 0;

  // The distorters. `instanced` is what distort_wants_instancing() returned for this frame; the
  // non-instanced path draws m_sprite_distorter_vertices/indices, the instanced one draws
  // m_sprite_distorter_vertices_instanced once per entry in m_sprite_distorter_instances_by_res.
  virtual void distort_draw_gpu(bool instanced) = 0;
  // Whether to build the instanced form this frame. A backend that cannot instance returns false.
  virtual bool distort_wants_instancing() const { return true; }
  // The aspect ratio the instanced mesh was last built for. The core rebuilds the mesh when the
  // game sends a different one, and the backend has to re-upload it then, so it owns the value.
  virtual void distort_instanced_mesh_changed() {}

  // The direct renderer the sprite bucket shares with the rest of the backend, used for the GIF
  // data the game sends before the sprites (menus and the progress screen).
  virtual void direct_reset_state() = 0;
  virtual void direct_render_vif(u32 vif0, u32 vif1, const u8* data, u32 size) = 0;
  virtual void direct_flush() = 0;

  // The glow renderer (jak 2 and later only): lens flares and the light bloom around them. Its
  // DMA sits between the HUD sprites and the end of the bucket. A backend that does not draw it
  // leaves this alone -- the core's trailing loop then consumes the data.
  virtual void glow_dma_and_draw(DmaFollower& /*dma*/) {}

  // Called once per draw, so a backend can count triangles the way its siblings do.
  virtual void add_tri_count(u32 /*tris*/) {}

  // --- state the backends read ---------------------------------------------------------------

  enum SpriteMode { Mode2D = 1, ModeHUD = 2, Mode3D = 3 };

  SpriteFrameData m_frame_data;  // qwa: 980
  Sprite3DMatrixData m_3d_matrix_data;
  SpriteHudMatrixData m_hud_matrix_data;

  std::vector<SpriteVertex3D> m_vertices_3d;
  std::vector<u32> m_index_buffer_data;
  std::vector<Bucket*> m_bucket_list;
  u64 m_sprite_idx = 0;
  // How many indices flush_sprites_gpu() should upload from m_index_buffer_data.
  u32 m_index_buffer_used = 0;

  // The distorter's state.
  struct SpriteDistorterSetup {
    GifTag gif_tag;
    GsZbuf zbuf;
    u64 zbuf_addr;
    GsTex0 tex0;
    u64 tex0_addr;
    GsTex1 tex1;
    u64 tex1_addr;
    u64 miptbp;
    u64 miptbp_addr;
    u64 clamp;
    u64 clamp_addr;
    GsAlpha alpha;
    u64 alpha_addr;
  };
  static_assert(sizeof(SpriteDistorterSetup) == (7 * 16));

  struct SpriteDistorterSineTables {
    Vector4f entry[128];
    math::Vector<u32, 4> ientry[9];
    GifTag gs_gif_tag;
    math::Vector<u32, 4> color;
  };
  static_assert(sizeof(SpriteDistorterSineTables) == (0x8b * 16));

  struct SpriteDistortFrameData {
    math::Vector3f xyz;  // position
    float num_255;       // always 255.0
    math::Vector2f st;   // texture coords
    float num_1;         // always 1.0
    u32 flag;            // 'resolution' of the sprite
    Vector4f rgba;       // ? (doesn't seem to be color)
  };
  static_assert(sizeof(SpriteDistortFrameData) == 16 * 3);

  SpriteDistorterSetup m_sprite_distorter_setup;  // direct data
  math::Vector4f m_sprite_distorter_sine_tables_aspect;
  SpriteDistorterSineTables m_sprite_distorter_sine_tables;
  std::vector<SpriteDistortFrameData> m_sprite_distorter_frame_data;
  std::vector<SpriteDistortVertex> m_sprite_distorter_vertices;
  std::vector<u32> m_sprite_distorter_indices;
  std::vector<SpriteDistortVertex> m_sprite_distorter_vertices_instanced;
  std::map<int, std::vector<SpriteDistortInstanceData>> m_sprite_distorter_instances_by_res;
  // The aspect the instanced mesh in m_sprite_distorter_vertices_instanced was built for.
  float m_distort_instanced_last_aspect_x = -1.f;
  float m_distort_instanced_last_aspect_y = -1.f;

  struct {
    int total_sprites = 0;
    int total_tris = 0;
  } m_distort_stats;

  struct DebugStats {
    int blocks_2d_grp0 = 0;
    int count_2d_grp0 = 0;
    int blocks_2d_grp1 = 0;
    int count_2d_grp1 = 0;
  } m_debug_stats;

  // The mode the distorter's draw runs under, built from its GS setup. A backend reads this in
  // distort_draw_gpu().
  DrawMode m_current_mode, m_default_mode;

  bool m_enable_distort_instancing = true;
  bool m_enable_culling = true;
  bool m_2d_enable = true;
  bool m_3d_enable = true;
  bool m_distort_enable = true;

  // Turns a GS ALPHA_1 register into the DrawMode's blend setting. Shared because the distorter
  // needs it too, and because getting one of the five cases wrong is invisible until someone
  // looks at that one effect.
  static void update_mode_from_alpha1(u64 val, DrawMode& mode);

 private:
  void render_jak1(DmaFollower& dma, const Context& context);
  void render_jak2(DmaFollower& dma, const Context& context);

  bool render_direct(DmaFollower& dma, const Context& context);
  void render_distorter(DmaFollower& dma, const Context& context);
  void distort_dma(GameVersion version, DmaFollower& dma);
  void distort_setup();
  void distort_setup_instanced();
  void handle_sprite_frame_setup(DmaFollower& dma, GameVersion version);
  void render_3d(DmaFollower& dma);
  void render_2d_group0(DmaFollower& dma, const Context& context);
  void render_fake_shadow(DmaFollower& dma);
  void render_2d_group1(DmaFollower& dma, const Context& context);
  void do_block_common(SpriteMode mode, u32 count, const Context& context);
  void flush_sprites(bool double_draw);

  void handle_tex0(u64 val);
  void handle_tex1(u64 val);
  void handle_zbuf(u64 val);
  void handle_clamp(u64 val);
  void handle_alpha(u64 val);

  u64 m_sprite_direct_setup[3 * 16 / 8];
  SpriteVecData2d m_vec_data_2d[SPRITES_PER_CHUNK];
  AdGifData m_adgif[SPRITES_PER_CHUNK];

  u32 m_current_tbp = 0;
  std::map<u64, Bucket> m_sprite_buckets;
  u64 m_last_bucket_key = UINT64_MAX;
  Bucket* m_last_bucket = nullptr;
};
