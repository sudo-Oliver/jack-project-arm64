#pragma once

/*!
 * @file MetalEye.h
 * The eyes on Metal.
 *
 * Working out which four quads an eye is made of is in EyeRendererCore, shared with the OpenGL
 * backend. What is here is the forty render targets and the draws into them.
 *
 * Each eye is rendered into and then sampled by merc in the same frame, so the draws go in the
 * offscreen command buffer -- see MetalRenderState.h.
 */

#include <memory>
#include <string>

#include "common/common_types.h"

#include "game/graphics/eye/EyeRendererCore.h"
#include "game/graphics/metal_renderer/MetalRenderState.h"

#ifdef __OBJC__

class MetalEyeRenderer : public MetalBucketRenderer, public EyeRendererCore {
 public:
  MetalEyeRenderer(const std::string& name, int my_id);
  ~MetalEyeRenderer() override;

  bool init(MetalRenderState* render_state) override;
  void render(DmaFollower& dma, MetalRenderState* render_state) override;
  void init_textures(TexturePool& pool, GameVersion version);

 protected:
  u64 backend_create_eye_texture(int slot) override;
  void backend_run_gpu(const std::vector<SingleEyeDraws>& draws) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
  MetalRenderState* m_current_render_state = nullptr;
};

#endif  // __OBJC__
