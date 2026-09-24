#pragma once

/*!
 * @file Merc2Core.h
 * The merc2 renderer with no graphics API in it.
 *
 * merc2 is the foreground renderer: characters, collectables, and some water. Everything about it
 * that decides how a pixel looks -- reading the DMA the GOAL code generates, finding the model in
 * the loader, building the skinning matrices, running blerc, deciding which draws exist and in
 * what order -- is the same for every backend, and is about 1200 lines of it. So it lives here,
 * and a backend implements the handful of methods below that actually touch the GPU.
 *
 * Each "merc model" is one merc-ctrl in game, one per LOD of an art-group ("jak", "orb", an
 * enemy). A model is made of effects, which divide it into parts drawn with different settings
 * (environment mapping, for instance). Within an effect are fragments, which is how the data was
 * split to fit in VU1 memory; the PC renderer ignores fragments except when the game modifies
 * vertices, which it does per fragment.
 */

#include <memory>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/dma/dma_chain_read.h"
#include "common/math/Vector.h"
#include "common/versions/versions.h"

#include "game/graphics/opengl_renderer/loader/Loader.h"

struct MercDebugStats {
  int num_models = 0;
  int num_missing_models = 0;
  int num_chains = 0;
  int num_effects = 0;
  int num_predicted_draws = 0;
  int num_predicted_tris = 0;
  int num_bones_uploaded = 0;
  int num_lights = 0;
  int num_draw_flush = 0;

  int num_envmap_effects = 0;
  int num_envmap_tris = 0;

  int num_upload_bytes = 0;
  int num_uploads = 0;

  struct DrawDebug {
    DrawMode mode;
    int num_tris;
  };
  struct EffectDebug {
    bool envmap = false;
    DrawMode envmap_mode;
    std::vector<DrawDebug> draws;
  };
  struct ModelDebug {
    std::string name;
    std::string level;
    std::vector<EffectDebug> effects;
  };

  std::vector<ModelDebug> model_list;

  bool collect_debug_model_list = false;
};

class Merc2Core {
 public:
  Merc2Core();
  virtual ~Merc2Core();

  void draw_debug_window(MercDebugStats* stats);

  static constexpr int kMaxBlerc = 40;

 protected:
  // What the core needs from the frame that is not its own. A backend fills this in before
  // calling render_core().
  struct Context {
    Loader* loader = nullptr;
    GameVersion version = GameVersion::Jak1;
    // Where this bucket's data ends.
    u32 next_bucket = 0;
  };

  // Walks one bucket's DMA and issues the draws through the hooks below.
  void render_core(DmaFollower& dma, const Context& context, MercDebugStats* stats);

  enum MercDataMemory {
    LOW_MEMORY = 0,
    BUFFER_BASE = 442,
    // this negative offset is what broke jak graphics in Dobiestation for a long time.
    BUFFER_OFFSET = -442
  };

  struct LowMemory {
    u8 tri_strip_tag[16];
    u8 ad_gif_tag[16];
    math::Vector4f hvdf_offset;
    math::Vector4f perspective[4];
    math::Vector4f fog;
  } m_low_memory;
  static_assert(sizeof(LowMemory) == 0x80);

  struct VuLights {
    math::Vector3f direction0;
    u32 w0;  // 12
    math::Vector3f direction1;
    u32 w1;  // 28
    math::Vector3f direction2;
    u32 w2;  // 44
    math::Vector4f color0;
    math::Vector4f color1;
    math::Vector4f color2;
    math::Vector4f ambient;
  };

  struct MercMat {
    math::Vector4f tmat[4];
    math::Vector4f nmat[3];
  };

  struct ShaderMercMat {
    math::Vector4f tmat[4];
    math::Vector4f nmat[3];
    math::Vector4f pad;
    std::string to_string() const;
  };

  enum DrawFlags {
    IGNORE_ALPHA = 1,
    MOD_VTX = 2,
    NO_TEXTURE = 4,
  };

  struct Draw {
    u32 first_index;
    u32 index_count;
    DrawMode mode;
    s32 texture;
    u32 num_triangles;
    u16 first_bone;
    u16 light_idx;
    u8 flags;
    // Index into the backend's list of modifiable-vertex buffers, from alloc_mod_vtx_buffer().
    // Only meaningful with the MOD_VTX flag.
    u32 mod_vtx_buffer;
    u8 fade[4];
    // no strip hack for custom models
    u8 no_strip;
    u64 hash;
  };

  struct LevelDrawBucket {
    const LevelData* level = nullptr;
    std::vector<Draw> draws;
    std::vector<Draw> envmap_draws;
    u32 next_free_draw = 0;
    u32 next_free_envmap_draw = 0;

    void reset() {
      level = nullptr;
      next_free_draw = 0;
      next_free_envmap_draw = 0;
    }
  };

  // ---------------------------------------------------------------------------------------------
  // What a backend implements. Everything above is shared; these five are the GPU.
  // ---------------------------------------------------------------------------------------------

  // The perspective matrix, hvdf offset and fog constants for this frame, read out of the setup
  // DMA. They change once per frame, not per draw.
  virtual void backend_set_low_memory(const LowMemory& low_memory) = 0;

  // Make sure buffer `index` exists and is big enough for one effect's modifiable vertices. The
  // core numbers them from zero and restarts at each flush, so a backend keeps them in a vector
  // and creates one only the first time an index is reached.
  virtual void backend_ensure_mod_vtx_buffer(u32 index, const LevelData* level) = 0;
  virtual void backend_upload_mod_vtx(u32 buffer,
                                      const tfrag3::MercVertex* data,
                                      size_t num_vertices) = 0;

  // The skinning matrices for every draw about to be flushed, as raw vectors.
  virtual void backend_upload_bones(const math::Vector4f* data, u32 num_vectors) = 0;

  // Draw one level's worth of draws. `envmap` picks the emerc shader over merc2; `set_fade` goes
  // with it, and is what the envmap pass fades with.
  virtual void backend_do_draws(const Draw* draws,
                                const LevelData* level,
                                u32 num_draws,
                                bool envmap,
                                bool set_fade,
                                MercDebugStats* stats) = 0;

  // How many bone vectors a draw's first_bone must be a multiple of. OpenGL's
  // GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT and Metal's buffer-offset alignment both constrain this.
  // A backend sets it in its constructor; 1 means no constraint.
  u32 m_bone_buffer_alignment = 1;

  static constexpr int MAX_SKEL_BONES = 128;
  static constexpr int BONE_VECTORS_PER_BONE = 7;
  static constexpr int MAX_SHADER_BONE_VECTORS = 1024 * 32;
  static constexpr int MAX_LEVELS = 3;
  static constexpr int MAX_DRAWS_PER_LEVEL = 2048 * 2;
  static constexpr int MAX_ENVMAP_DRAWS_PER_LEVEL = MAX_DRAWS_PER_LEVEL;
  static constexpr int MAX_LIGHTS = 1024;
  static constexpr int kMaxEffect = 64;

  VuLights m_lights_buffer[MAX_LIGHTS];
  math::Vector4f m_shader_bone_vector_buffer[MAX_SHADER_BONE_VECTORS];

 private:
  struct DrawArgs {
    LevelDrawBucket* lev_bucket;
    const u8* fade;
    bool jak1_water_mode;
    bool ignore_alpha;
    bool disable_fog;
    bool no_texture;
    u64 hash;
    u32 lights;
    u32 first_bone;
  };

  void handle_pc_model(const DmaTransfer& setup, MercDebugStats* stats);
  void handle_setup_dma(DmaFollower& dma);
  void handle_all_dma(DmaFollower& dma, MercDebugStats* stats);
  void handle_merc_chain(DmaFollower& dma, MercDebugStats* stats);
  void flush_draw_buckets(MercDebugStats* stats);

  u32 alloc_lights(const VuLights& lights);
  u32 alloc_bones(int count, ShaderMercMat* data);
  Draw* alloc_normal_draw(const tfrag3::MercDraw& mdraw, const DrawArgs& args);
  Draw* try_alloc_envmap_draw(const tfrag3::MercDraw& mdraw,
                              const DrawMode& envmap_mode,
                              u32 envmap_texture,
                              const DrawArgs& args);

  void model_mod_draws(int num_effects,
                       const tfrag3::MercModel* model,
                       const LevelData* lev,
                       const u8* input_data,
                       const DmaTransfer& setup,
                       u32* mod_vtx_buffers,
                       MercDebugStats* stats);
  void model_mod_blerc_draws(int num_effects,
                             const tfrag3::MercModel* model,
                             const LevelData* lev,
                             u32* mod_vtx_buffers,
                             const float* blerc_weights,
                             MercDebugStats* stats);

  Context m_context;

  bool m_effect_debug_mask[kMaxEffect];

  static constexpr int MAX_MOD_VTX = UINT16_MAX;
  std::vector<tfrag3::MercVertex> m_mod_vtx_temp;

  struct UnpackTempVtx {
    float pos[4];
    float nrm[4];
    float uv[2];
  };
  std::vector<UnpackTempVtx> m_mod_vtx_unpack_temp;

  std::vector<LevelDrawBucket> m_level_draw_buckets;
  u32 m_next_free_level_bucket = 0;
  u32 m_next_free_bone_vector = 0;
  u32 m_next_free_light = 0;
  u32 m_next_mod_vtx_buffer = 0;
};
