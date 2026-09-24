#pragma once

/*!
 * @file MetalTFragment.h
 * The tfrag3 bucket on Metal: the world geometry, and the first renderer ported from the OpenGL
 * backend.
 *
 * It reuses everything that is not OpenGL: the .fr3 level data from the shared Loader, the
 * time-of-day colour interpolation from background_common, and the camera matrix that
 * make_new_cam_mat builds. What is new here is the pipeline state, the per-tree index buffer and
 * the draw loop.
 *
 * Not yet done, deliberately: visibility culling, the per-draw alpha and decal modes, and
 * multidraw. This draws every tree in full, which is correct but does more work than the OpenGL
 * renderer does. See CODEX.md.
 */

#include <memory>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"

#include "game/graphics/opengl_renderer/background/background_common.h"
#include "game/graphics/opengl_renderer/loader/common.h"

#ifdef __OBJC__
#import <Metal/Metal.h>
#endif

class MetalTFragment {
 public:
  MetalTFragment();
  ~MetalTFragment();

  MetalTFragment(const MetalTFragment&) = delete;
  MetalTFragment& operator=(const MetalTFragment&) = delete;

#ifdef __OBJC__
  // Builds the pipeline state from the already-compiled shader library. Returns false and logs
  // why if the library has no tfrag3 entry points.
  bool init(id<MTLDevice> device, id<MTLLibrary> library, MTLPixelFormat color_format,
            MTLPixelFormat depth_format);

  // Draws every tfrag tree of `level`. `camera` and `occlusion` come straight from the DMA the
  // game sent; `occlusion` may be null, which leaves frustum culling as the only culling.
  void render(id<MTLRenderCommandEncoder> encoder,
              const LevelData& level,
              const GoalBackgroundCameraData& camera,
              const u8* occlusion);
#endif

  // Triangles drawn on the last frame, for the log line that says the renderer is doing work.
  u32 last_frame_tris() const { return m_last_frame_tris; }

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
  u32 m_last_frame_tris = 0;
};
