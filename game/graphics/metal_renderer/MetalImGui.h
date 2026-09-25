#pragma once

/*!
 * @file MetalImGui.h
 * The debug GUI on Metal.
 *
 * The OpenGL backend uses ImGui's own imgui_impl_opengl3 backend. There is no equivalent Metal
 * backend vendored here, and vendoring one would tie this fork to a particular ImGui release, so
 * this is the backend: ImGui hands over a vertex buffer, an index buffer and a list of clipped
 * commands per frame, and drawing them is about as much work as reading the upstream file would
 * be.
 *
 * The platform half -- keyboard, mouse, window size -- is still ImGui's own imgui_impl_sdl3,
 * which both backends share.
 */

#include <memory>

#include "common/common_types.h"

#ifdef __OBJC__
#import <Metal/Metal.h>

struct ImDrawData;

class MetalImGui {
 public:
  MetalImGui();
  ~MetalImGui();

  // Builds the pipeline and uploads the font atlas. Call once the ImGui context and its fonts
  // exist, and once the shader library is compiled.
  bool init(id<MTLDevice> device, id<MTLLibrary> library, MTLPixelFormat color_format);

  // Draws one frame's draw data into an encoder whose target is `target_width` x `target_height`
  // pixels. Does nothing before init() has succeeded.
  void render(ImDrawData* draw_data,
              id<MTLRenderCommandEncoder> encoder,
              u32 target_width,
              u32 target_height);

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

#endif  // __OBJC__
