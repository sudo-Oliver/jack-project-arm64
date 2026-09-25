#pragma once

/*!
 * @file ShadowRenderer.h
 * The OpenGL half of the shadow renderer.
 *
 * The VU emulation that builds the shadow volumes is in ShadowRendererCore, shared with the Metal
 * backend. What is here is the stencil work.
 */

#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/shadow/ShadowRendererCore.h"

class ShadowRenderer : public BucketRenderer, public ShadowRendererCore {
 public:
  ShadowRenderer(const std::string& name, int my_id);
  ~ShadowRenderer() override;
  void render(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof) override;
  void draw_debug_window() override;

 protected:
  void draw_volumes() override;

 private:
  struct {
    // index is front, back
    GLuint vertex_buffer, index_buffer[2], vao;
  } m_ogl;

  bool m_debug_draw_volume = false;

  SharedRenderState* m_current_render_state = nullptr;
  ScopedProfilerNode* m_current_prof = nullptr;
};
