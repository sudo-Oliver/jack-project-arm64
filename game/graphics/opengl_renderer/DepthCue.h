#pragma once

/*!
 * @file DepthCue.h
 * The OpenGL half of the depth-cue renderer.
 *
 * The DMA walk and the quad building are in DepthCueCore, shared with the Metal backend. What is
 * here is the two render targets and the draws.
 */

#include "game/graphics/depth_cue/DepthCueCore.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"

class DepthCue : public BucketRenderer, public DepthCueCore {
 public:
  DepthCue(const std::string& name, int my_id);
  void render(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof) override;
  void draw_debug_window() override;

 protected:
  void do_draw() override;

 private:
  void opengl_setup();

  struct {
    // Framebuffer for depth-cue-base-page
    GLuint fbo;
    GLuint fbo_texture;

    // Vertex data for drawing to depth-cue-base-page
    GLuint depth_cue_page_vao;
    GLuint depth_cue_page_vertex_buffer;

    // Vertex data for drawing to on-screen framebuffer
    GLuint on_screen_vao;
    GLuint on_screen_vertex_buffer;

    // Texture to sample the framebuffer from
    GLuint framebuffer_sample_fbo;
    GLuint framebuffer_sample_tex;
  } m_ogl;

  SharedRenderState* m_current_render_state = nullptr;
  ScopedProfilerNode* m_current_prof = nullptr;
};
