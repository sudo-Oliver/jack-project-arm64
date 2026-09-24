#pragma once

/*!
 * @file MetalTie3.h
 * The tie buckets on Metal: the props that make up most of what a level looks like -- cliffs,
 * rocks, huts, fences, bridges.
 *
 * Tie shares more with tfrag3 than it looks: the same PreloadedVertex, the same StripDraw with vis
 * groups, the same BVH, and for the plain categories the same shader. The instancing is already
 * baked into the unpacked vertices by the loader. So this renderer is MetalTFragment with the
 * geometry taken from the tie buffers and the draws taken from a category range.
 *
 * Not yet done: the envmap second draw (the shiny pass, which needs the ETIE shader) and the wind
 * instances. The envmap *base* draw is included, with the plain shader -- see the note in the
 * implementation.
 */

#include <memory>
#include <string>

#include "game/graphics/metal_renderer/MetalRenderState.h"

#ifdef __OBJC__

class MetalTie3 : public MetalBucketRenderer {
 public:
  MetalTie3(const std::string& name, int my_id, int level_id);
  ~MetalTie3() override;

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
