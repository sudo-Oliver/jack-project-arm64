#pragma once

/*!
 * @file MetalDirect2.h
 * The Metal half of the direct renderer.
 *
 * Everything that turns a GIF packet into vertices and draws is in DirectRenderer2Core, shared
 * with the OpenGL backend. What is here is the one method the core leaves to a backend --
 * uploading the buffers and issuing the draws -- plus the state objects a GS draw mode maps to.
 *
 * The draw-mode mapping is not MetalDrawStateCache's: the direct renderer reads a few mode bits
 * differently from the background renderers (its own alpha reject, a colour multiplier for one
 * blend mode, a per-draw FIX value), exactly as DirectRenderer2 differs from
 * setup_opengl_from_draw_mode. Keeping the two tables apart is what keeps each of them matching
 * its OpenGL sibling.
 */

#include <memory>
#include <string>

#include "common/common_types.h"

#include "game/graphics/direct/DirectRenderer2Core.h"
#include "game/graphics/metal_renderer/MetalRenderState.h"

#ifdef __OBJC__

class MetalDirect2 : public DirectRenderer2Core {
 public:
  MetalDirect2(u32 max_verts,
               u32 max_inds,
               u32 max_draws,
               const std::string& name,
               bool use_ftoi_mod);
  ~MetalDirect2() override;

  // Builds the pipeline states. Returns false if the shader is missing from the library.
  bool init(MetalRenderState* render_state);

  // Both hold the render state for the duration of the call, because the core's flush does not
  // carry one.
  void render_gif_data(const u8* data, MetalRenderState* render_state);
  void render_vif_data(u32 vif0,
                       u32 vif1,
                       const u8* data,
                       u32 size,
                       MetalRenderState* render_state);
  void flush_pending(MetalRenderState* render_state);

  u32 last_frame_tris() const { return m_last_frame_tris; }
  void reset_tri_count() { m_last_frame_tris = 0; }

 protected:
  void flush_draws() override;

 private:
  void draw_call_loop_grouped();
  // Returns false if this draw's texture could not be found, in which case it is skipped.
  bool bind_texture(u8 unit, u16 tbp, DrawMode mode);

  struct Impl;
  std::unique_ptr<Impl> m_impl;
  MetalRenderState* m_current_render_state = nullptr;
  u32 m_last_frame_tris = 0;
};

/*!
 * The bucket form: reads a bucket's DMA and feeds the GIF packets in it to a MetalDirect2.
 *
 * Jak 1's OpenGL table puts the older DirectRenderer on these three buckets. They carry nothing
 * but plain GIF packets -- no scissor changes, no buffer-to-buffer blits -- which is the subset
 * DirectRenderer2 covers, so the two agree on everything these buckets contain.
 */
class MetalDirectBucketRenderer : public MetalBucketRenderer {
 public:
  MetalDirectBucketRenderer(const std::string& name, int my_id, u32 batch_size);
  ~MetalDirectBucketRenderer() override;

  bool init(MetalRenderState* render_state) override;
  void render(DmaFollower& dma, MetalRenderState* render_state) override;

  u32 last_frame_tris() const { return m_direct.last_frame_tris(); }

 private:
  MetalDirect2 m_direct;
};

#endif  // __OBJC__
