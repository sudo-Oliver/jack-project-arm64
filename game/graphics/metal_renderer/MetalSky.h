#pragma once

/*!
 * @file MetalSky.h
 * The sky on Metal: the two textures the sky is drawn with, and the geometry that draws it.
 *
 * Three buckets. MetalSkyBlend builds the sky and cloud textures by adding several source
 * textures together, one per level and time of day, into render targets. MetalSkyBlendHandler
 * runs that and then the transparent tfrag trees that share its bucket. MetalSkyRenderer draws
 * the sky geometry itself, which arrives as plain GIF packets.
 *
 * The blend renders into a texture that a later bucket samples, so it cannot go in the frame's
 * render pass. It goes in the offscreen command buffer instead -- see MetalRenderState.h.
 */

#include <memory>
#include <string>

#include "common/common_types.h"

#include "game/graphics/metal_renderer/MetalDirect.h"
#include "game/graphics/metal_renderer/MetalRenderState.h"
#include "game/graphics/metal_renderer/MetalTFragment.h"
#include "game/graphics/opengl_renderer/SkyBlendCommon.h"

#ifdef __OBJC__

/*!
 * Builds the sky and cloud textures. The OpenGL sibling is SkyBlendGPU.
 */
class MetalSkyBlend {
 public:
  MetalSkyBlend();
  ~MetalSkyBlend();

  bool init(MetalRenderState* render_state);
  // Registers the two textures with the pool, at the VRAM addresses the sky geometry samples.
  void init_textures(TexturePool& tex_pool, GameVersion version);

  SkyBlendStats do_sky_blends(DmaFollower& dma, MetalRenderState* render_state);

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

/*!
 * The sky-blend bucket: the blends, then the transparent tfrag trees that share the bucket.
 */
class MetalSkyBlendHandler : public MetalBucketRenderer {
 public:
  MetalSkyBlendHandler(const std::string& name,
                       int my_id,
                       int level_id,
                       std::shared_ptr<MetalSkyBlend> blender);

  bool init(MetalRenderState* render_state) override;
  void render(DmaFollower& dma, MetalRenderState* render_state) override;

  u32 last_frame_tris() const { return m_tfrag.last_frame_tris(); }

 private:
  std::shared_ptr<MetalSkyBlend> m_blender;
  MetalTFragment m_tfrag;
  SkyBlendStats m_stats;
};

/*!
 * The sky geometry: GIF packets through the direct renderer, same as the OpenGL SkyRenderer.
 * It has to be the older direct renderer: the sky sends REGLIST packets, which the newer one
 * does not handle.
 */
class MetalSkyRenderer : public MetalBucketRenderer {
 public:
  MetalSkyRenderer(const std::string& name, int my_id);

  bool init(MetalRenderState* render_state) override;
  void render(DmaFollower& dma, MetalRenderState* render_state) override;

  u32 last_frame_tris() const { return m_direct.last_frame_tris(); }

 private:
  MetalDirect m_direct;
};

#endif  // __OBJC__
