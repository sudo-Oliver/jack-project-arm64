#pragma once

/*!
 * @file MetalShrub.h
 * The shrub buckets on Metal: the vegetation and the small scattered props.
 *
 * Shrub has no visibility structure in the .fr3 (the OpenGL renderer draws every static draw), so
 * this is the same shape as MetalTFragment without the culling step.
 */

#include <memory>
#include <string>

#include "common/common_types.h"

#include "game/graphics/metal_renderer/MetalRenderState.h"

#ifdef __OBJC__

class MetalShrub : public MetalBucketRenderer {
 public:
  MetalShrub(const std::string& name, int my_id, int level_id);
  ~MetalShrub() override;

  bool init(MetalRenderState* render_state) override;
  void render(DmaFollower& dma, MetalRenderState* render_state) override;

  u32 last_frame_tris() const { return m_last_frame_tris; }

 private:
  void draw_level(MetalRenderState* render_state, const LevelData& level);

  struct Impl;
  std::unique_ptr<Impl> m_impl;
  int m_level_id = 0;
  u32 m_last_frame_tris = 0;
};

#endif  // __OBJC__
