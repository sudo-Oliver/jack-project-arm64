/*!
 * @file EyeRenderer.cpp
 * See EyeRenderer.h.
 */

#include "EyeRenderer.h"

#include "third-party/imgui/imgui.h"

EyeRenderer::EyeRenderer(const std::string& name, int id) : BucketRenderer(name, id) {}

EyeRenderer::~EyeRenderer() {
  if (m_gl_ready) {
    glDeleteVertexArrays(1, &m_vao);
    glDeleteBuffers(1, &m_gl_vertex_buffer);
  }
}

/*!
 * One render target per eye. They are 128x128 rather than the GS's 32x32, which is what makes the
 * eyes look right at a modern resolution.
 */
u64 EyeRenderer::backend_create_eye_texture(int slot) {
  if (m_fbs.empty()) {
    m_fbs.reserve(NUM_EYE_PAIRS * 2);
    for (int i = 0; i < NUM_EYE_PAIRS * 2; i++) {
      m_fbs.emplace_back(128, 128, GL_UNSIGNED_INT_8_8_8_8_REV);
    }
  }
  return m_fbs[slot].texture();
}

void EyeRenderer::init_textures(TexturePool& texture_pool, GameVersion version) {
  EyeRendererCore::init_textures(texture_pool, version);

  // The vertex buffer the draws come out of.
  glGenVertexArrays(1, &m_vao);
  glBindVertexArray(m_vao);
  glGenBuffers(1, &m_gl_vertex_buffer);
  glBindBuffer(GL_ARRAY_BUFFER, m_gl_vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, VTX_BUFFER_FLOATS * sizeof(float), nullptr, GL_STREAM_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 4, GL_FLOAT, GL_TRUE, sizeof(float) * 4, (void*)0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
  m_gl_ready = true;
}

void EyeRenderer::render(DmaFollower& dma,
                         SharedRenderState* render_state,
                         ScopedProfilerNode& prof) {
  (void)prof;
  if (!m_enabled) {
    while (dma.current_tag_offset() != render_state->next_bucket) {
      dma.read_and_advance();
    }
    return;
  }
  m_current_render_state = render_state;
  render_core(dma, render_state->texture_pool.get(), render_state->version,
              render_state->next_bucket);
  m_current_render_state = nullptr;
}

void EyeRenderer::handle_eye_dma2(DmaFollower& dma,
                                  SharedRenderState* render_state,
                                  ScopedProfilerNode& prof) {
  (void)prof;
  m_current_render_state = render_state;
  EyeRendererCore::handle_eye_dma2(dma, render_state->texture_pool.get(), render_state->version);
  m_current_render_state = nullptr;
}

void EyeRenderer::draw_debug_window() {
  ImGui::Text("Time: %.3f ms\n", average_time_ms());
  ImGui::Text("Debug:\n%s", debug_text().c_str());
}

void EyeRenderer::backend_run_gpu(const std::vector<SingleEyeDraws>& draws) {
  SharedRenderState* render_state = m_current_render_state;
  if (draws.empty()) {
    return;
  }

  glBindVertexArray(m_vao);
  glBindBuffer(GL_ARRAY_BUFFER, m_gl_vertex_buffer);

  // the first thing we'll do is prepare the vertices
  int buffer_idx = 0;
  for (const auto& draw : draws) {
    buffer_idx = add_clear_draw_to_buffer(buffer_idx, m_gpu_vertex_buffer);
    if (draw.using_64) {
      buffer_idx =
          add_draw_to_buffer_64(buffer_idx, draw.iris, m_gpu_vertex_buffer, draw.pair, draw.lr);
      buffer_idx =
          add_draw_to_buffer_64(buffer_idx, draw.pupil, m_gpu_vertex_buffer, draw.pair, draw.lr);
      buffer_idx =
          add_draw_to_buffer_64(buffer_idx, draw.lid, m_gpu_vertex_buffer, draw.pair, draw.lr);
    } else {
      buffer_idx =
          add_draw_to_buffer_32(buffer_idx, draw.iris, m_gpu_vertex_buffer, draw.pair, draw.lr);
      buffer_idx =
          add_draw_to_buffer_32(buffer_idx, draw.pupil, m_gpu_vertex_buffer, draw.pair, draw.lr);
      buffer_idx =
          add_draw_to_buffer_32(buffer_idx, draw.lid, m_gpu_vertex_buffer, draw.pair, draw.lr);
    }
  }
  ASSERT(buffer_idx <= VTX_BUFFER_FLOATS);
  int check = buffer_idx;

  // maybe buffer sub data.
  glBufferData(GL_ARRAY_BUFFER, buffer_idx * sizeof(float), m_gpu_vertex_buffer, GL_STREAM_DRAW);

  FramebufferTexturePairContext ctxt(m_fbs[draws.front().tex_slot()]);

  // set up common opengl state
  glDisable(GL_DEPTH_TEST);
  render_state->shaders[ShaderId::EYE].activate();
  glUniform1i(glGetUniformLocation(render_state->shaders[ShaderId::EYE].id(), "tex_T0"), 0);
  glActiveTexture(GL_TEXTURE0);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  buffer_idx = 0;
  for (size_t draw_idx = 0; draw_idx < draws.size(); draw_idx++) {
    const auto& draw = draws[draw_idx];
    auto& out_tex = m_gpu_eye_textures[draw.tex_slot()];
    out_tex.fnv_name_hash = draw.fnv_name_hash;
    out_tex.lr = draw.lr;

    // clear: not really needed, but we do it to help debugging in case all the textures are missing
    float clear[4] = {1.0, 0, 0, 0};
    glClearBufferfv(GL_COLOR, 0, clear);

    // background
    if (draw.iris_tex) {
      glDisable(GL_BLEND);
      glBindTexture(GL_TEXTURE_2D, draw.iris_gl_tex);
      glDrawArrays(GL_TRIANGLE_STRIP, buffer_idx / 4, 4);
    }
    buffer_idx += 4 * 4;

    // iris
    if (draw.iris_tex) {
      // set alpha
      // set Z
      // set texture
      glDisable(GL_BLEND);
      glBindTexture(GL_TEXTURE_2D, draw.iris_gl_tex);
      glDrawArrays(GL_TRIANGLE_STRIP, buffer_idx / 4, 4);
    }
    buffer_idx += 4 * 4;

    if (draw.pupil_tex) {
      glEnable(GL_BLEND);
      glBlendEquation(GL_FUNC_ADD);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      glBindTexture(GL_TEXTURE_2D, draw.pupil_gl_tex);
      glDrawArrays(GL_TRIANGLE_STRIP, buffer_idx / 4, 4);
    }
    buffer_idx += 4 * 4;

    if (draw.lid_tex) {
      glDisable(GL_BLEND);
      glBindTexture(GL_TEXTURE_2D, draw.lid_gl_tex);
      glDrawArrays(GL_TRIANGLE_STRIP, buffer_idx / 4, 4);
    }
    buffer_idx += 4 * 4;

    // finally, give to "vram"
    render_state->texture_pool->move_existing_to_vram(out_tex.gpu_tex, out_tex.tbp);

    if (draw_idx != draws.size() - 1) {
      ctxt.switch_to(m_fbs[draws[draw_idx + 1].tex_slot()]);
    }
  }

  ASSERT(check == buffer_idx);

  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

