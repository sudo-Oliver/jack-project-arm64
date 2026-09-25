#pragma once

/*!
 * @file MetalGeneric2.h
 * The generic renderer on Metal: the fallback path for geometry that fits no specialised
 * renderer -- lightning, warps, eco effects, parts of the HUD. Ten buckets use it.
 *
 * The DMA walk, the VU emulation and the sorting are in Generic2Core, shared with the OpenGL
 * backend. What is here is the one method the core leaves to a backend: issue the draws.
 *
 * Two Metal-specific notes:
 *
 * - Metal has no primitive restart, so each bucket's triangle strips are expanded into triangles
 *   while the index buffer is filled. Nothing here culls faces, so the winding the expansion
 *   produces does not matter.
 *
 * - The pipeline and depth-stencil states are cached here rather than in MetalDrawStateCache,
 *   because generic reads a DrawMode slightly differently from the background renderers: it takes
 *   the depth-write bit literally where background_common also lets a NEVER alpha test disable
 *   depth writes, and its SRC_DST_FIX_DST blend uses the draw's own `fix` value as the blend
 *   constant rather than a fixed 0.5. Sharing the cache would mean sharing those differences.
 */

#include <memory>
#include <string>

#include "common/common_types.h"

#include "game/graphics/generic/Generic2Core.h"
#include "game/graphics/metal_renderer/MetalRenderState.h"

#ifdef __OBJC__

class MetalGeneric2 : public Generic2Core {
 public:
  MetalGeneric2();
  ~MetalGeneric2() override;

  bool init(MetalRenderState* render_state);
  void render_in_mode(DmaFollower& dma, MetalRenderState* render_state, Mode mode);

  u32 last_frame_tris() const { return m_last_frame_tris; }
  void reset_tri_count() { m_last_frame_tris = 0; }

 protected:
  void do_draws() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
  MetalRenderState* m_current_render_state = nullptr;
  u32 m_last_frame_tris = 0;
};

/*!
 * One of the ten generic buckets. They all share one MetalGeneric2, the way the OpenGL table
 * shares one Generic2.
 */
class MetalGeneric2BucketRenderer : public MetalBucketRenderer {
 public:
  MetalGeneric2BucketRenderer(const std::string& name,
                              int my_id,
                              std::shared_ptr<MetalGeneric2> renderer,
                              MetalGeneric2::Mode mode);

  bool init(MetalRenderState* render_state) override;
  void render(DmaFollower& dma, MetalRenderState* render_state) override;

 private:
  std::shared_ptr<MetalGeneric2> m_generic;
  MetalGeneric2::Mode m_mode;
};

#endif  // __OBJC__
