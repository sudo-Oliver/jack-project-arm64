#pragma once

/*!
 * @file EyeRendererCore.h
 * The eyes, with no graphics API in it.
 *
 * Every character's eyes are drawn into a small texture of their own each frame, out of four
 * quads: a background, an iris, a pupil and a lid. Working out which quads those are, from the
 * sprites and scissor rectangles the game sends, is the same for every backend.
 *
 * What a backend provides is the forty render targets and the draws into them.
 */

#include <optional>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/dma/dma_chain_read.h"
#include "common/versions/versions.h"

#include "game/graphics/texture/TexturePool.h"

constexpr int EYE_BASE_BLOCK_JAK1 = 8160;
constexpr int EYE_BASE_BLOCK_JAK2 = 3968;
constexpr int EYE_BASE_BLOCK_JAK3 = 504;
constexpr int NUM_EYE_PAIRS = 20;
constexpr int SINGLE_EYE_SIZE = 32;

class EyeRendererCore {
 public:
  EyeRendererCore();
  virtual ~EyeRendererCore();

  void init_textures(TexturePool& texture_pool, GameVersion version);
  // Consume one bucket's DMA. `next_bucket` is where it ends.
  void render_core(DmaFollower& dma, TexturePool* texture_pool, GameVersion version,
                   u32 next_bucket);
  void handle_eye_dma2(DmaFollower& dma, TexturePool* texture_pool, GameVersion version);

  std::optional<u64> lookup_eye_texture(u8 eye_id);
  std::optional<u64> lookup_eye_texture_hash(u64 hash, bool lr);

  const std::string& debug_text() const { return m_debug; }
  float average_time_ms() const { return m_average_time_ms; }

  struct SpriteInfo {
    u8 a;
    u64 uv0;  // stores hashed name of merc-ctrl that reads this eye.
    u32 uv1[2];
    u32 xyz0[3];
    u32 xyz1[3];

    std::string print() const;
  };

  struct ScissorInfo {
    int x0, x1;
    int y0, y1;
    std::string print() const;
  };

  struct EyeDraw {
    SpriteInfo sprite;
    ScissorInfo scissor;
    std::string print() const;
  };


 protected:
  struct SingleEyeDraws {
    u64 fnv_name_hash = 0;
    int lr;
    int pair;
    bool using_64 = false;

    int tex_slot() const { return pair * 2 + lr; }
    EyeDraw iris;
    GpuTexture* iris_tex = nullptr;
    u64 iris_gl_tex = 0;

    EyeDraw pupil;
    GpuTexture* pupil_tex = nullptr;
    u64 pupil_gl_tex = 0;

    EyeDraw lid;
    GpuTexture* lid_tex = nullptr;
    u64 lid_gl_tex = 0;
  };

  // Make the texture for eye slot `slot` and return its handle. It is rendered into, so a backend
  // has to create it as a render target.
  virtual u64 backend_create_eye_texture(int slot) = 0;
  // Draw them.
  virtual void backend_run_gpu(const std::vector<SingleEyeDraws>& draws) = 0;

  std::string m_debug;
  float m_average_time_ms = 0;

  struct GpuEyeTex {
    GpuTexture* gpu_tex = nullptr;
    u32 tbp;
    u64 handle = 0;
    u64 fnv_name_hash = 0;
    bool lr = false;
  } m_gpu_eye_textures[NUM_EYE_PAIRS * 2];

  // xyst per vertex, 4 vertices per square, 4 draws per eye, 20 pairs of eyes, 2 eyes per pair.
  static constexpr int VTX_BUFFER_FLOATS = 4 * 4 * 4 * NUM_EYE_PAIRS * 2;
  float m_gpu_vertex_buffer[VTX_BUFFER_FLOATS];

  TexturePool* m_texture_pool = nullptr;


  std::vector<SingleEyeDraws> get_draws(DmaFollower& dma,
                                        TexturePool* texture_pool,
                                        GameVersion version);
};

// Fill four vertices for one quad. Shared, because both backends upload the same vertex buffer.
int add_draw_to_buffer_32(int idx,
                          const EyeRendererCore::EyeDraw& draw,
                          float* data,
                          int pair,
                          int lr);
int add_draw_to_buffer_64(int idx,
                          const EyeRendererCore::EyeDraw& draw,
                          float* data,
                          int pair,
                          int lr);
int add_clear_draw_to_buffer(int idx, float* data);
