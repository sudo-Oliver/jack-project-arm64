#pragma once

/*!
 * @file SkyBlendCPU.h
 * Blends the sky and cloud textures together on the CPU, with no graphics API in it.
 *
 * The sky texture system blends sky textures from different levels and times of day into the two
 * textures the sky geometry is drawn with. The blend itself is SIMD over bytes, and the result is
 * handed to the TexturePool, both of which are shared. Only the two textures it owns touch a
 * backend, and those go through gpu::.
 */

#include "common/dma/dma_chain_read.h"

#include "game/graphics/opengl_renderer/SkyBlendCommon.h"
#include "game/graphics/texture/TexturePool.h"

class SkyBlendCPU {
 public:
  SkyBlendCPU();
  ~SkyBlendCPU();

  SkyBlendStats do_sky_blends(DmaFollower& dma, TexturePool* texture_pool);
  void init_textures(TexturePool& tex_pool, GameVersion version);

 private:
  static constexpr int m_sizes[2] = {32, 64};
  std::vector<u8> m_texture_data[2];

  struct TexInfo {
    u64 handle = 0;
    u32 tbp = 0;
    GpuTexture* tex = nullptr;
  } m_textures[2];
};
