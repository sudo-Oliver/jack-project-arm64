#pragma once

/*!
 * @file MetalTFragment.h
 * The tfrag3 bucket on Metal: the world geometry, and the first renderer ported from the OpenGL
 * backend.
 *
 * It reuses everything that is not OpenGL: the .fr3 level data from the shared Loader, the
 * time-of-day colour interpolation from background_common, the camera matrix make_new_cam_mat
 * builds, the culling, and the meaning of a GS draw mode. What is new here is the pipeline state,
 * the per-frame index list and the draw loop.
 *
 * Not yet done, deliberately: the lower-detail geometry levels, the near variants, and fog. See
 * CODEX.md.
 */

#include <memory>
#include <vector>

#include "common/common_types.h"

#include "game/graphics/metal_renderer/MetalRenderState.h"

#ifdef __OBJC__

class MetalTFragment : public MetalBucketRenderer {
 public:
  // `tree_kinds` is which tfrag tree kinds this bucket owns, exactly as the OpenGL bucket table
  // assigns them; `level_id` is the level slot, which selects the occlusion string.
  MetalTFragment(const std::string& name,
                 int my_id,
                 std::vector<tfrag3::TFragmentTreeKind> tree_kinds,
                 int level_id);
  ~MetalTFragment() override;

  bool init(MetalRenderState* render_state) override;
  void render(DmaFollower& dma, MetalRenderState* render_state) override;

  // Triangles drawn on the last frame, for the log line that says the renderer is doing work.
  u32 last_frame_tris() const { return m_last_frame_tris; }

 private:
  void draw_level(MetalRenderState* render_state, const LevelData& level);

  struct Impl;
  std::unique_ptr<Impl> m_impl;
  std::vector<tfrag3::TFragmentTreeKind> m_tree_kinds;
  int m_level_id = 0;
  u32 m_last_frame_tris = 0;
};

#endif  // __OBJC__
