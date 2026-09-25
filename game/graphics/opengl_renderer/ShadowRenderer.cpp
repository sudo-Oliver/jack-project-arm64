/*!
 * @file ShadowRenderer.cpp
 * See ShadowRenderer.h. The VU emulation is in ShadowRendererCore.
 */

#include "ShadowRenderer.h"

#include "third-party/imgui/imgui.h"

ShadowRenderer::ShadowRenderer(const std::string& name, int my_id) : BucketRenderer(name, my_id) {
  // create OpenGL objects
  glGenBuffers(1, &m_ogl.vertex_buffer);

  glGenBuffers(2, m_ogl.index_buffer);
  glGenVertexArrays(1, &m_ogl.vao);

  // set up the vertex array
  glBindVertexArray(m_ogl.vao);
  for (int i = 0; i < 2; i++) {
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ogl.index_buffer[i]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, MAX_INDICES * sizeof(u32), nullptr, GL_STREAM_DRAW);
  }
  glBindBuffer(GL_ARRAY_BUFFER, m_ogl.vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, MAX_VERTICES * sizeof(Vertex), nullptr, GL_STREAM_DRAW);

  // xyz
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0,                            // location 0 in the shader
                        3,                            // 3 floats per vert
                        GL_FLOAT,                     // floats
                        GL_TRUE,                      // normalized, ignored,
                        sizeof(Vertex),               //
                        (void*)offsetof(Vertex, xyz)  // offset in array
  );

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
}

ShadowRenderer::~ShadowRenderer() {
  glDeleteBuffers(1, &m_ogl.vertex_buffer);
  glDeleteBuffers(2, m_ogl.index_buffer);
  glDeleteVertexArrays(1, &m_ogl.vao);
}

void ShadowRenderer::draw_debug_window() {
  ImGui::Checkbox("Volume", &m_debug_draw_volume);
  ImGui::Text("Vert: %d, Front: %d, Back: %d\n", m_next_vertex, m_next_front_index,
              m_next_back_index);
}

void ShadowRenderer::render(DmaFollower& dma,
                            SharedRenderState* render_state,
                            ScopedProfilerNode& prof) {
  m_current_render_state = render_state;
  m_current_prof = &prof;

  Context context;
  context.next_bucket = render_state->next_bucket;
  context.enabled = m_enabled;
  render_core(dma, context);

  m_current_render_state = nullptr;
  m_current_prof = nullptr;
}

void ShadowRenderer::draw_volumes() {
  auto* render_state = m_current_render_state;
  auto& prof = *m_current_prof;
  // enable stencil!
  glEnable(GL_STENCIL_TEST);
  glStencilMask(0xFF);

  glBindVertexArray(m_ogl.vao);
  glBindBuffer(GL_ARRAY_BUFFER, m_ogl.vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, m_next_vertex * sizeof(Vertex), m_vertices, GL_STREAM_DRAW);

  glEnable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDepthFunc(GL_GEQUAL);

  render_state->shaders.at(ShaderId::SHADOW).activate();

  glDepthMask(GL_FALSE);  // no depth writes.
  if (m_debug_draw_volume) {
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
  } else {
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);  // no color writes.
  }

  // First pass.
  // here, we don't write depth or color.
  // but we increment stencil on depth fail.

  {
    glUniform4f(glGetUniformLocation(render_state->shaders[ShaderId::SHADOW].id(), "color_uniform"),
                0.0f, 128.0f / 256, 0.0f, 127.0f / 256);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ogl.index_buffer[0]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_next_front_index * sizeof(u32), m_front_indices,
                 GL_STREAM_DRAW);
    glStencilFunc(GL_ALWAYS, 0, 0);          // always pass stencil
    glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);  // increment on depth pass.
    glDrawElements(GL_TRIANGLES, (m_next_front_index - 6), GL_UNSIGNED_INT, nullptr);

    if (m_debug_draw_volume) {
      glDisable(GL_BLEND);
      glUniform4f(
          glGetUniformLocation(render_state->shaders[ShaderId::SHADOW].id(), "color_uniform"), 0.0f,
          0.0f, 0.0f, 0.5f);
      glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
      glDrawElements(GL_TRIANGLES, (m_next_front_index - 6), GL_UNSIGNED_INT, nullptr);
      glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
      glEnable(GL_BLEND);
    }
    prof.add_draw_call();
    prof.add_tri(m_next_back_index / 3);
  }

  {
    glUniform4f(glGetUniformLocation(render_state->shaders[ShaderId::SHADOW].id(), "color_uniform"),
                128.0f / 256, 0.0f, 0.0f, 130.0f / 256);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ogl.index_buffer[1]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_next_back_index * sizeof(u32), m_back_indices,
                 GL_STREAM_DRAW);
    // Second pass.
    // same settings, but decrement.
    glStencilFunc(GL_ALWAYS, 0, 0);
    glStencilOp(GL_KEEP, GL_KEEP, GL_DECR);  // decrement on depth pass.
    glDrawElements(GL_TRIANGLES, m_next_back_index, GL_UNSIGNED_INT, nullptr);
    if (m_debug_draw_volume) {
      glDisable(GL_BLEND);
      glUniform4f(
          glGetUniformLocation(render_state->shaders[ShaderId::SHADOW].id(), "color_uniform"), 0.0f,
          0.0f, 0.0f, 0.5f);
      glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
      glDrawElements(GL_TRIANGLES, (m_next_back_index - 0), GL_UNSIGNED_INT, nullptr);
      glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
      glEnable(GL_BLEND);
    }

    prof.add_draw_call();
    prof.add_tri(m_next_front_index / 3);
  }

  // finally, draw shadow.
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ogl.index_buffer[0]);
  glUniform4f(glGetUniformLocation(render_state->shaders[ShaderId::SHADOW].id(), "color_uniform"),
              m_color.x(), m_color.y(), m_color.z(), m_color.w());
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
  // glStencilFunc(GL_GREATER, 0, 0);
  glStencilFunc(GL_NOTEQUAL, 0, 0xFF);
  glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
  glDepthFunc(GL_ALWAYS);

  glEnable(GL_BLEND);
  glBlendEquation(GL_FUNC_ADD);
  glBlendFuncSeparate(GL_DST_COLOR, GL_ZERO, GL_ONE, GL_ZERO);
  glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, (void*)(sizeof(u32) * (m_next_front_index - 6)));
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  prof.add_draw_call();
  prof.add_tri(2);
  glDepthMask(GL_TRUE);

  glDisable(GL_STENCIL_TEST);
}
