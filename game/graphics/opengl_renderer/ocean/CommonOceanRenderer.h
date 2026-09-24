#pragma once

/*!
 * @file CommonOceanRenderer.h
 * The OpenGL half of the ocean's vertex path.
 *
 * Turning the GIF data into buckets of vertices is in CommonOceanRendererCore, shared with the
 * Metal backend. What is here is the two flushes -- the draws themselves -- and the buffers.
 */

#include "game/graphics/ocean/CommonOceanRendererCore.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"

class CommonOceanRenderer : public CommonOceanRendererCore {
 public:
  CommonOceanRenderer();
  ~CommonOceanRenderer() override;

  void flush_near(SharedRenderState* render_state, ScopedProfilerNode& prof);
  void flush_mid(SharedRenderState* render_state, ScopedProfilerNode& prof);

  // Held for the length of a call into the core, which does not carry a render state.
  void set_frame_context(SharedRenderState* render_state, ScopedProfilerNode* prof) {
    m_current_render_state = render_state;
    m_current_prof = prof;
  }

 protected:
  void flush_near_draws() override;
  void flush_mid_draws() override;

 private:
  SharedRenderState* m_current_render_state = nullptr;
  ScopedProfilerNode* m_current_prof = nullptr;

  struct {
    GLuint vertex_buffer, index_buffer[NUM_BUCKETS], vao;
  } m_ogl;
};
