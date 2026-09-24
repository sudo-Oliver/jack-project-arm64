#pragma once

/*!
 * @file MetalOcean.h
 * The ocean on Metal: the water's texture, the vertices both ocean renderers produce, and the two
 * buckets that drive them.
 *
 * Everything that decides what the water looks like -- the VU1 programs, the vertex sorting, the
 * DMA -- is in game/graphics/ocean, shared with the OpenGL backend. What is here is the render
 * targets, the pipelines and the draws.
 *
 * The water texture is rendered into and then sampled by the same frame, so it is built in the
 * offscreen command buffer (see MetalRenderState.h) rather than inside the frame's render pass.
 */

#include <memory>
#include <string>

#include "common/common_types.h"

#include "game/graphics/metal_renderer/MetalDirect.h"
#include "game/graphics/metal_renderer/MetalRenderState.h"
#include "game/graphics/ocean/CommonOceanRendererCore.h"
#include "game/graphics/ocean/OceanMidCore.h"
#include "game/graphics/ocean/OceanNearCore.h"
#include "game/graphics/ocean/OceanTextureCore.h"

#ifdef __OBJC__

/*!
 * The vertex path's Metal half: uploads what the kicks built and draws each bucket.
 */
class MetalCommonOceanRenderer : public CommonOceanRendererCore {
 public:
  MetalCommonOceanRenderer();
  ~MetalCommonOceanRenderer() override;

  bool init(MetalRenderState* render_state);
  void set_frame_context(MetalRenderState* render_state) { m_current_render_state = render_state; }

  u32 last_frame_tris() const { return m_last_frame_tris; }
  void reset_tri_count() { m_last_frame_tris = 0; }

 protected:
  void flush_near_draws() override;
  void flush_mid_draws() override;

 private:
  void draw_buckets(const int* buckets, int count, bool near);

  struct Impl;
  std::unique_ptr<Impl> m_impl;
  MetalRenderState* m_current_render_state = nullptr;
  u32 m_last_frame_tris = 0;
};

/*!
 * The water texture's Metal half: a render target, the grid draw, and the mip chain.
 */
class MetalOceanTexture : public OceanTextureCore {
 public:
  explicit MetalOceanTexture(bool generate_mipmaps);
  ~MetalOceanTexture() override;

  bool init(MetalRenderState* render_state);
  void init_textures(TexturePool& pool, GameVersion version);
  void set_frame_context(MetalRenderState* render_state) { m_current_render_state = render_state; }

 protected:
  void backend_begin_pass(bool to_temp) override;
  void backend_end_pass() override;
  void backend_flush() override;
  void backend_make_texture_with_mipmaps() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
  MetalRenderState* m_current_render_state = nullptr;
};

/*!
 * The first ocean bucket: the water texture, ocean-far, and ocean-mid.
 */
class MetalOceanMidAndFar : public MetalBucketRenderer {
 public:
  MetalOceanMidAndFar(const std::string& name, int my_id);

  bool init(MetalRenderState* render_state) override;
  void render(DmaFollower& dma, MetalRenderState* render_state) override;
  void init_textures(TexturePool& pool, GameVersion version);

  u32 last_frame_tris() const { return m_common.last_frame_tris() + m_direct.last_frame_tris(); }

 private:
  void handle_ocean_far(DmaFollower& dma, MetalRenderState* render_state);
  void handle_ocean_mid(DmaFollower& dma, MetalRenderState* render_state);

  MetalDirect m_direct;
  MetalOceanTexture m_texture_renderer;
  MetalCommonOceanRenderer m_common;
  OceanMidCore m_mid_renderer;
};

/*!
 * The near ocean bucket.
 */
class MetalOceanNear : public MetalBucketRenderer {
 public:
  MetalOceanNear(const std::string& name, int my_id);

  bool init(MetalRenderState* render_state) override;
  void render(DmaFollower& dma, MetalRenderState* render_state) override;
  void init_textures(TexturePool& pool, GameVersion version);

  u32 last_frame_tris() const { return m_common.last_frame_tris(); }

 private:
  MetalOceanTexture m_texture_renderer;
  MetalCommonOceanRenderer m_common;
  OceanNearCore m_core;
};

#endif  // __OBJC__
