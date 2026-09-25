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
 * The swaying instances -- the palm trees, the banners -- are a second pass with their own shader
 * and their own index buffer, because each instance is drawn with its own matrix rather than the
 * camera's. The maths that builds those matrices is shared with the OpenGL backend.
 *
 * Not yet done: the envmap second draw, the shiny pass, which needs the ETIE shader. The envmap
 * *base* draw is included, with the plain shader -- see the note in the implementation.
 */

#include <memory>
#include <string>

#include "game/graphics/background/TieWind.h"
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
  void draw_tree_wind(MetalRenderState* render_state,
                      const LevelData& level,
                      size_t tree_idx,
                      const tfrag3::TieTree& in_tree);

  void draw_tree_envmap(MetalRenderState* render_state,
                        const LevelData& level,
                        const tfrag3::TieTree& in_tree,
                        tfrag3::TieCategory category);

  // The wind state and the envmap tint the game sends once per frame, in this bucket's own DMA.
  TieWindWork m_wind_data{};
  bool m_has_wind_data = false;
  math::Vector4f m_envmap_color{1.f, 1.f, 1.f, 1.f};

  struct Impl;
  std::unique_ptr<Impl> m_impl;
  int m_level_id = 0;
  u32 m_last_frame_tris = 0;
};

#endif  // __OBJC__
