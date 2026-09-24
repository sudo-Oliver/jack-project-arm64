/*!
 * @file CommonOceanRenderer.cpp
 * See CommonOceanRenderer.h.
 */

#include "CommonOceanRenderer.h"

#include "common/log/log.h"

CommonOceanRenderer::CommonOceanRenderer() {
  // create OpenGL objects
  glGenBuffers(1, &m_ogl.vertex_buffer);
  glGenBuffers(NUM_BUCKETS, m_ogl.index_buffer);
  glGenVertexArrays(1, &m_ogl.vao);

  // set up the vertex array
  glBindVertexArray(m_ogl.vao);
  for (int i = 0; i < NUM_BUCKETS; i++) {
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ogl.index_buffer[i]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_indices[i].size() * sizeof(u32), nullptr,
                 GL_STREAM_DRAW);
  }
  glBindBuffer(GL_ARRAY_BUFFER, m_ogl.vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, m_vertices.size() * sizeof(Vertex), nullptr, GL_STREAM_DRAW);

  // xyz
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0,                            // location 0 in the shader
                        3,                            // 3 floats per vert
                        GL_FLOAT,                     // floats
                        GL_TRUE,                      // normalized, ignored,
                        sizeof(Vertex),               //
                        (void*)offsetof(Vertex, xyz)  // offset in array
  );

  // rgba
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1,                             // location 1 in the shader
                        4,                             // 4 color components
                        GL_UNSIGNED_BYTE,              // u8
                        GL_TRUE,                       // normalized (255 becomes 1)
                        sizeof(Vertex),                //
                        (void*)offsetof(Vertex, rgba)  //
  );

  // stq
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2,                            // location 2 in the shader
                        3,                            // 2 floats per vert
                        GL_FLOAT,                     // floats
                        GL_FALSE,                     // normalized, ignored
                        sizeof(Vertex),               //
                        (void*)offsetof(Vertex, stq)  // offset in array
  );

  // byte data
  glEnableVertexAttribArray(3);
  glVertexAttribIPointer(3,                            // location 3 in the shader
                         4,                            //
                         GL_UNSIGNED_BYTE,             // u8's
                         sizeof(Vertex),               //
                         (void*)offsetof(Vertex, fog)  // offset in array
  );

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
}

CommonOceanRenderer::~CommonOceanRenderer() {
  glDeleteBuffers(1, &m_ogl.vertex_buffer);
  glDeleteBuffers(3, m_ogl.index_buffer);
  glDeleteVertexArrays(1, &m_ogl.vao);
}

void CommonOceanRenderer::flush_near(SharedRenderState* render_state, ScopedProfilerNode& prof) {
  m_current_render_state = render_state;
  m_current_prof = &prof;
  flush_near_draws();
  m_current_render_state = nullptr;
  m_current_prof = nullptr;
}

void CommonOceanRenderer::flush_near_draws() {
  SharedRenderState* render_state = m_current_render_state;
  ScopedProfilerNode& prof = *m_current_prof;
  glBindVertexArray(m_ogl.vao);
  glBindBuffer(GL_ARRAY_BUFFER, m_ogl.vertex_buffer);
  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);
  glBufferData(GL_ARRAY_BUFFER, m_next_free_vertex * sizeof(Vertex), m_vertices.data(),
               GL_STREAM_DRAW);
  render_state->shaders[ShaderId::OCEAN_COMMON].activate();
  glUniform4f(glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_COMMON].id(), "fog_color"),
              render_state->fog_color[0] / 255.f, render_state->fog_color[1] / 255.f,
              render_state->fog_color[2] / 255.f, render_state->fog_intensity / 255);

  glDepthMask(GL_FALSE);
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_BLEND);
  glDepthFunc(GL_GEQUAL);

  for (int bucket = 0; bucket < 3; bucket++) {
    switch (bucket) {
      case 0: {
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
        glBlendEquation(GL_FUNC_ADD);
        auto tbp =
            render_state->version == GameVersion::Jak1 ? OCEAN_TEX_TBP_JAK1 : OCEAN_TEX_TBP_JAK2;
        auto tex = render_state->texture_pool->lookup(tbp);
        if (!tex) {
          tex = render_state->texture_pool->get_placeholder_texture();
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, *tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glUniform1i(
            glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_COMMON].id(), "tex_T0"), 0);
        glUniform1i(
            glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_COMMON].id(), "bucket"), 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      }

      break;
      case 1:
        glBlendFuncSeparate(GL_ZERO, GL_ONE, GL_ONE, GL_ZERO);
        glUniform1f(
            glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_COMMON].id(), "alpha_mult"),
            1.f);
        glUniform1i(
            glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_COMMON].id(), "bucket"), 1);
        break;
      case 2:
        auto tex = render_state->texture_pool->lookup(m_envmap_tex);
        if (!tex) {
          tex = render_state->texture_pool->get_placeholder_texture();
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, *tex);

        glBlendFuncSeparate(GL_DST_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
        glBlendEquation(GL_FUNC_ADD);
        glUniform1i(
            glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_COMMON].id(), "bucket"), 2);
        break;
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ogl.index_buffer[bucket]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_next_free_index[bucket] * sizeof(u32),
                 m_indices[bucket].data(), GL_STREAM_DRAW);
    glDrawElements(GL_TRIANGLE_STRIP, m_next_free_index[bucket], GL_UNSIGNED_INT, nullptr);
    prof.add_draw_call();
    prof.add_tri(m_next_free_index[bucket]);
  }
}


void CommonOceanRenderer::flush_mid(SharedRenderState* render_state, ScopedProfilerNode& prof) {
  m_current_render_state = render_state;
  m_current_prof = &prof;
  CommonOceanRendererCore::flush_mid();
  m_current_render_state = nullptr;
  m_current_prof = nullptr;
}

void CommonOceanRenderer::flush_mid_draws() {
  SharedRenderState* render_state = m_current_render_state;
  ScopedProfilerNode& prof = *m_current_prof;
  glBindVertexArray(m_ogl.vao);
  glBindBuffer(GL_ARRAY_BUFFER, m_ogl.vertex_buffer);
  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);
  glBufferData(GL_ARRAY_BUFFER, m_next_free_vertex * sizeof(Vertex), m_vertices.data(),
               GL_STREAM_DRAW);
  render_state->shaders[ShaderId::OCEAN_COMMON].activate();
  glUniform4f(glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_COMMON].id(), "fog_color"),
              render_state->fog_color[0] / 255.f, render_state->fog_color[1] / 255.f,
              render_state->fog_color[2] / 255.f, render_state->fog_intensity / 255);

  glDepthMask(GL_TRUE);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_ALWAYS);
  glDisable(GL_BLEND);

  for (int bucket = 0; bucket < 2; bucket++) {
    switch (bucket) {
      case 0: {
        auto tbp =
            render_state->version == GameVersion::Jak1 ? OCEAN_TEX_TBP_JAK1 : OCEAN_TEX_TBP_JAK2;
        auto tex = render_state->texture_pool->lookup(tbp);
        if (!tex) {
          tex = render_state->texture_pool->get_placeholder_texture();
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, *tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glUniform1i(
            glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_COMMON].id(), "tex_T0"), 0);
        glUniform1i(
            glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_COMMON].id(), "bucket"), 3);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      }

      break;
      case 1:
        glEnable(GL_BLEND);
        auto tex = render_state->texture_pool->lookup(m_envmap_tex);
        if (!tex) {
          tex = render_state->texture_pool->get_placeholder_texture();
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, *tex);

        glBlendFuncSeparate(GL_DST_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
        glBlendEquation(GL_FUNC_ADD);
        glUniform1i(
            glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_COMMON].id(), "bucket"), 4);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        break;
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ogl.index_buffer[bucket]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_next_free_index[bucket] * sizeof(u32),
                 m_indices[bucket].data(), GL_STREAM_DRAW);
    glDrawElements(GL_TRIANGLE_STRIP, m_next_free_index[bucket], GL_UNSIGNED_INT, nullptr);
    prof.add_draw_call();
    prof.add_tri(m_next_free_index[bucket]);
  }
}
