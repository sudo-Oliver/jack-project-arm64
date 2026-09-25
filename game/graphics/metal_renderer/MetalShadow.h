#pragma once

/*!
 * @file MetalShadow.h
 * The shadow bucket on Metal: the character shadows, drawn as stencil shadow volumes.
 *
 * The VU emulation that builds the volumes is in ShadowRendererCore, shared with the OpenGL
 * backend. What is here is the three passes: count the front faces into the stencil buffer, count
 * the back faces out of it, and shade the pixels where the two disagree.
 *
 * That needs a stencil buffer, which is why the frame's depth attachment is
 * MTLPixelFormatDepth32Float_Stencil8 (see metal.mm). On an Apple GPU that costs nothing extra:
 * depth and stencil share one tile allocation, and neither is ever stored to memory.
 */

#include <memory>
#include <string>

#include "common/common_types.h"

#include "game/graphics/metal_renderer/MetalRenderState.h"
#include "game/graphics/shadow/ShadowRendererCore.h"

#ifdef __OBJC__

class MetalShadow : public MetalBucketRenderer, public ShadowRendererCore {
 public:
  MetalShadow(const std::string& name, int my_id);
  ~MetalShadow() override;

  bool init(MetalRenderState* render_state) override;
  void render(DmaFollower& dma, MetalRenderState* render_state) override;

  u32 last_frame_tris() const { return m_last_frame_tris; }

 protected:
  void draw_volumes() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
  MetalRenderState* m_current_render_state = nullptr;
  u32 m_last_frame_tris = 0;
};

#endif  // __OBJC__
