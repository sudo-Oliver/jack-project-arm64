#pragma once

/*!
 * @file DirectRenderer2.h
 * The OpenGL half of the direct renderer.
 *
 * The GS state machine -- register handlers, primitive assembly, DrawMode bookkeeping -- is in
 * DirectRenderer2Core, shared with the Metal backend. What is left here is the part that is
 * actually OpenGL: the vertex array, the buffers, and turning a Draw's mode into GL calls.
 */

#include <vector>

#include "common/common_types.h"
#include "common/dma/gs.h"

#include "game/graphics/direct/DirectRenderer2Core.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"

class DirectRenderer2 : public DirectRenderer2Core {
 public:
  DirectRenderer2(u32 max_verts,
                  u32 max_inds,
                  u32 max_draws,
                  const std::string& name,
                  bool use_ftoi_mod);
  ~DirectRenderer2();

  void init_shaders(ShaderLibrary& shaders);
  void render_gif_data(const u8* data, SharedRenderState* render_state, ScopedProfilerNode& prof);
  void flush_pending(SharedRenderState* render_state, ScopedProfilerNode& prof);
  void draw_debug_window();

 protected:
  // The core calls this when its buffers are nearly full, which is why the render state and the
  // profiler are held as members: the core does not carry them.
  void flush_draws() override;

 private:
  void draw_call_loop_simple(SharedRenderState* render_state, ScopedProfilerNode& prof);
  void draw_call_loop_grouped(SharedRenderState* render_state, ScopedProfilerNode& prof);
  void setup_opengl_for_draw_mode(const Draw& draw, SharedRenderState* render_state);
  void setup_opengl_tex(u16 unit,
                        u16 tbp,
                        bool filter,
                        bool clamp_s,
                        bool clamp_t,
                        SharedRenderState* render_state);

  struct {
    GLuint vertex_buffer;
    GLuint index_buffer;
    GLuint vao;
    GLuint alpha_reject, color_mult, fog_color;
  } m_ogl;

  // Only set for the duration of a render_gif_data or flush_pending call.
  SharedRenderState* m_current_render_state = nullptr;
  ScopedProfilerNode* m_current_prof = nullptr;
};
