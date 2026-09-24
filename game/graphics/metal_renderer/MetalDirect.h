#pragma once

/*!
 * @file MetalDirect.h
 * The Metal half of the direct renderer.
 *
 * The GS state machine is in DirectRendererCore, shared with the OpenGL backend. What is here is
 * the two methods a backend provides: applying that state and drawing, and binding a texture.
 *
 * Where OpenGL sets blend, depth and colour-mask state with calls between draws, Metal bakes
 * blend and the colour write mask into a pipeline state and the depth test into a depth-stencil
 * state. Both are built once per distinct state and cached, so a frame that flips between two
 * modes costs two lookups rather than two rebuilds.
 */

#include <memory>
#include <string>

#include "common/common_types.h"

#include "game/graphics/direct/DirectRendererCore.h"
#include "game/graphics/metal_renderer/MetalRenderState.h"

#ifdef __OBJC__

class MetalDirect : public DirectRendererCore {
 public:
  MetalDirect(const std::string& name, int batch_size);
  ~MetalDirect() override;

  bool init(MetalRenderState* render_state);

  // Each of these points the core at the frame's render state first, because the core does not
  // carry one.
  void consume_bucket_dma(DmaFollower& dma, MetalRenderState* render_state);
  void render_vif(u32 vif0,
                  u32 vif1,
                  const u8* data,
                  u32 size,
                  MetalRenderState* render_state);
  void render_gif(const u8* data, u32 size, MetalRenderState* render_state);
  void flush_pending(MetalRenderState* render_state);

  using DirectRendererCore::reset_state;
  using DirectRendererCore::set_mipmap;

  u32 last_frame_tris() const { return m_last_frame_tris; }
  void reset_tri_count() { m_last_frame_tris = 0; }

 protected:
  void flush_draws() override;
  void backend_bind_texture(int unit) override;

 private:
  void set_context(MetalRenderState* render_state);

  struct Impl;
  std::unique_ptr<Impl> m_impl;
  MetalRenderState* m_current_render_state = nullptr;
  u32 m_last_frame_tris = 0;
};

/*!
 * One bucket of plain GIF data: the debug draws and the subtitles.
 */
class MetalDirectBucketRenderer : public MetalBucketRenderer {
 public:
  MetalDirectBucketRenderer(const std::string& name, int my_id, int batch_size);

  bool init(MetalRenderState* render_state) override;
  void render(DmaFollower& dma, MetalRenderState* render_state) override;

  u32 last_frame_tris() const { return m_direct.last_frame_tris(); }

 private:
  MetalDirect m_direct;
};

#endif  // __OBJC__
