#pragma once

/*!
 * @file MetalDepthCue.h
 * The depth-cue bucket on Metal: the soft horizontal smear the PS2 lays over the whole frame.
 *
 * The DMA walk and the sixteen slice quads are in DepthCueCore, shared with the OpenGL backend.
 * What is here is the two targets and the two passes.
 *
 * This is the only renderer that needs its own render pass in the middle of the frame, so it is
 * the only caller of MetalRenderState::pause_and_snapshot() / resume_scene() rather than the
 * combined snapshot_scene(): the scratch page has to be filled between the two.
 */

#include <memory>
#include <string>

#include "common/common_types.h"

#include "game/graphics/depth_cue/DepthCueCore.h"
#include "game/graphics/metal_renderer/MetalRenderState.h"

#ifdef __OBJC__

class MetalDepthCue : public MetalBucketRenderer, public DepthCueCore {
 public:
  MetalDepthCue(const std::string& name, int my_id);
  ~MetalDepthCue() override;

  bool init(MetalRenderState* render_state) override;
  void render(DmaFollower& dma, MetalRenderState* render_state) override;

  u32 last_frame_tris() const { return m_last_frame_tris; }

 protected:
  void do_draw() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
  MetalRenderState* m_current_render_state = nullptr;
  u32 m_last_frame_tris = 0;
};

#endif  // __OBJC__
