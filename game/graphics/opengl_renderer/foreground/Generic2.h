#pragma once

/*!
 * @file Generic2.h
 * The OpenGL half of the generic renderer.
 *
 * Everything that reads the DMA and builds the draws is in Generic2Core, shared with the Metal
 * backend. What is here is the one method it leaves to a backend, plus the buffers and uniforms
 * that method needs.
 */

#include "game/graphics/generic/Generic2Core.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"

class Generic2 : public Generic2Core {
 public:
  Generic2(ShaderLibrary& shaders,
           u32 num_verts = 500000,
           u32 num_frags = 10000,
           u32 num_adgif = 10000,
           u32 num_buckets = 800);
  ~Generic2() override;

  void render_in_mode(DmaFollower& dma,
                      SharedRenderState* render_state,
                      ScopedProfilerNode& prof,
                      Mode mode);

 protected:
  void do_draws() override;

 private:
  void opengl_setup(ShaderLibrary& shaders);
  void opengl_cleanup();
  void opengl_bind_and_setup_proj(SharedRenderState* render_state);
  void setup_opengl_for_draw_mode(const DrawMode& draw_mode,
                                  u8 fix,
                                  SharedRenderState* render_state);
  void setup_opengl_tex(u16 unit,
                        u16 tbp,
                        bool filter,
                        bool clamp_s,
                        bool clamp_t,
                        SharedRenderState* render_state);
  void do_draws_for_alpha(SharedRenderState* render_state,
                          ScopedProfilerNode& prof,
                          DrawMode::AlphaBlend alpha,
                          bool hud);
  void do_hud_draws(SharedRenderState* render_state, ScopedProfilerNode& prof);

  // Only set for the length of a render_in_mode call.
  SharedRenderState* m_current_render_state = nullptr;
  ScopedProfilerNode* m_current_prof = nullptr;

  struct {
    GLuint vao;
    GLuint vertex_buffer;
    GLuint index_buffer;
    GLuint alpha_reject, color_mult, fog_color, scale, mat_23, mat_32, mat_33, fog_consts,
        hvdf_offset, use_full_matrix, full_matrix;
    GLuint gfx_hack_no_tex;
    GLuint warp_sample_mode;
  } m_ogl;
};
