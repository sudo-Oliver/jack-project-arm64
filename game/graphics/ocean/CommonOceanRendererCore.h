#pragma once

/*!
 * @file CommonOceanRendererCore.h
 * The part of the ocean that turns GIF data into vertices, with no graphics API in it.
 *
 * Both ocean renderers -- the near one and the mid one -- receive GIF packets built by the VU
 * emulation and sort their vertices into buckets by what they are drawn with: the ocean texture,
 * an alpha-only pass, and the environment map. That sorting, and the strip and fan assembly it
 * does on the way, is the same for every backend.
 *
 * What is left to a backend is issuing the draws, which is what the two flush methods below are.
 */

#include <vector>

#include "common/common_types.h"
#include "common/dma/gs.h"
#include "common/math/Vector.h"
#include "common/versions/versions.h"

#include "game/graphics/texture/TexturePool.h"

// Where the ocean texture lives in PS2 VRAM.
constexpr int OCEAN_TEX_TBP_JAK1 = 8160;
constexpr int OCEAN_TEX_TBP_JAK2 = 672;

class CommonOceanRendererCore {
 public:
  CommonOceanRendererCore();
  virtual ~CommonOceanRendererCore();

  void init_for_near();
  void kick_from_near(const u8* data);
  void flush_near() { flush_near_draws(); }

  void init_for_mid();
  void kick_from_mid(const u8* data);

  // The mid ocean's envmap pass has to be drawn back to front. That reordering is data work, so
  // it happens here and a backend's flush_mid_draws can assume it is done.
  void flush_mid();

 protected:
  // Draw what the kicks built. The backends implement these.
  virtual void flush_near_draws() = 0;
  virtual void flush_mid_draws() = 0;

  void handle_near_vertex_gif_data_fan(const u8* data, u32 offset, u32 loop);
  void handle_near_vertex_gif_data_strip(const u8* data, u32 offset, u32 loop);
  void handle_near_adgif(const u8* data, u32 offset, u32 count);
  void handle_mid_adgif(const u8* data, u32 offset);

  enum VertexBucket {
    RGB_TEXTURE = 0,
    ALPHA = 1,
    ENV_MAP = 2,
  };
  u32 m_current_bucket = VertexBucket::RGB_TEXTURE;

  struct Vertex {
    math::Vector<float, 3> xyz;
    math::Vector<u8, 4> rgba;
    math::Vector<float, 3> stq;
    u8 fog;
    u8 pad[3];
  };
  static_assert(sizeof(Vertex) == 32);

  static constexpr int NUM_BUCKETS = 3;

  std::vector<Vertex> m_vertices;
  u32 m_next_free_vertex = 0;

  std::vector<u32> m_indices[NUM_BUCKETS];
  u32 m_next_free_index[NUM_BUCKETS] = {0};

  // Which PS2 VRAM address the environment map is at this frame.
  u32 m_envmap_tex = 0;
};
