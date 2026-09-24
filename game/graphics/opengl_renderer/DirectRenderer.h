#pragma once

/*!
 * @file DirectRenderer.h
 * The OpenGL half of the direct renderer.
 *
 * The GS state machine is in DirectRendererCore, shared with the Metal backend. What is here is
 * the two methods the core leaves to a backend -- applying that state and drawing, and binding a
 * texture -- plus the vertex array they need.
 *
 * It can be used as a BucketRenderer, or as a subcomponent of another renderer.
 */

#include <vector>

#include "game/graphics/direct/DirectRendererCore.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/pipelines/opengl.h"

class DirectRenderer : public BucketRenderer, public DirectRendererCore {
 public:
  DirectRenderer(const std::string& name, int my_id, int batch_size);
  ~DirectRenderer();

  void init_shaders(ShaderLibrary& sl) override;
  void draw_debug_window() override;
  void render(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof) override;

  // The wrappers the other renderers call. Each one fills in the core's context first, because
  // the core does not carry a render state.
  void render_vif(u32 vif0,
                  u32 vif1,
                  const u8* data,
                  u32 size,
                  SharedRenderState* render_state,
                  ScopedProfilerNode& prof);
  void render_gif(const u8* data,
                  u32 size,
                  SharedRenderState* render_state,
                  ScopedProfilerNode& prof);
  void flush_pending(SharedRenderState* render_state, ScopedProfilerNode& prof);
  void lookup_textures_again(SharedRenderState* render_state);
  void reinitialize_gl_state() { mark_state_dirty(); }

  using DirectRendererCore::hack_disable_blend;
  using DirectRendererCore::reset_state;
  using DirectRendererCore::set_mipmap;

 protected:
  void flush_draws() override;
  void backend_bind_texture(int unit) override;

  void update_gl_prim();
  void update_gl_blend();
  void update_gl_test();
  void update_gl_texture(int unit);

  // Set for the duration of any call that can reach the core, because the core's flush needs
  // them and does not carry them.
  void set_context(SharedRenderState* render_state);
  SharedRenderState* m_current_render_state = nullptr;
  ScopedProfilerNode* m_current_prof = nullptr;

  bool m_offscreen_mode = false;

  struct {
    GLuint vertex_buffer;
    GLuint vao;
    u32 vertex_buffer_bytes = 0;
    u32 vertex_buffer_max_verts = 0;
    float color_mult = 1.0;
    float alpha_mult = 1.0;
  } m_ogl;

  struct {
    GLint alpha_min, alpha_max;
    GLint normal_shader_id = -1;
  } m_uniforms;
};
