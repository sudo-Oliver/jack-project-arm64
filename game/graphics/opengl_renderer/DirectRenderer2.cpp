#include "DirectRenderer2.h"

#include "common/log/log.h"
#include "common/util/Assert.h"

#include "third-party/imgui/imgui.h"

DirectRenderer2::DirectRenderer2(u32 max_verts,
                                 u32 max_inds,
                                 u32 max_draws,
                                 const std::string& name,
                                 bool use_ftoi_mod)
    : DirectRenderer2Core(max_verts, max_inds, max_draws, name, use_ftoi_mod) {
  // create OpenGL objects
  glGenBuffers(1, &m_ogl.vertex_buffer);
  glGenBuffers(1, &m_ogl.index_buffer);
  glGenVertexArrays(1, &m_ogl.vao);

  // set up the vertex array
  glBindVertexArray(m_ogl.vao);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ogl.index_buffer);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, max_inds * sizeof(u32), nullptr, GL_STREAM_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, m_ogl.vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, max_verts * sizeof(Vertex), nullptr, GL_STREAM_DRAW);

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
                        3,                            // 3 floats per vert
                        GL_FLOAT,                     // floats
                        GL_FALSE,                     // normalized, ignored
                        sizeof(Vertex),               //
                        (void*)offsetof(Vertex, stq)  // offset in array
  );

  // byte data
  glEnableVertexAttribArray(3);
  glVertexAttribIPointer(3,                                 // location 0 in the shader
                         4,                                 // 3 floats per vert
                         GL_UNSIGNED_BYTE,                  // u8's
                         sizeof(Vertex),                    //
                         (void*)offsetof(Vertex, tex_unit)  // offset in array
  );

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
}

DirectRenderer2::~DirectRenderer2() {
  glDeleteBuffers(1, &m_ogl.vertex_buffer);
  glDeleteBuffers(1, &m_ogl.index_buffer);
  glDeleteVertexArrays(1, &m_ogl.vao);
}

void DirectRenderer2::init_shaders(ShaderLibrary& shaders) {
  shaders[ShaderId::DIRECT2].activate();
  m_ogl.alpha_reject = glGetUniformLocation(shaders[ShaderId::DIRECT2].id(), "alpha_reject");
  m_ogl.color_mult = glGetUniformLocation(shaders[ShaderId::DIRECT2].id(), "color_mult");
  m_ogl.fog_color = glGetUniformLocation(shaders[ShaderId::DIRECT2].id(), "fog_color");
}

/*!
 * The core calls this when its buffers fill mid-packet. The render state and the profiler are
 * whatever the call that is currently running the core handed us.
 */
void DirectRenderer2::flush_draws() {
  ASSERT(m_current_render_state && m_current_prof);
  flush_pending(m_current_render_state, *m_current_prof);
}

/*!
 * Consume a GIF packet. Everything this does is in the core; we only hold the render state so the
 * flush it may trigger has somewhere to draw to.
 */
void DirectRenderer2::render_gif_data(const u8* data,
                                      SharedRenderState* render_state,
                                      ScopedProfilerNode& prof) {
  m_current_render_state = render_state;
  m_current_prof = &prof;
  DirectRenderer2Core::render_gif_data(data);
  m_current_render_state = nullptr;
  m_current_prof = nullptr;
}

void DirectRenderer2::flush_pending(SharedRenderState* render_state, ScopedProfilerNode& prof) {
  // skip, if we're empty.
  if (m_next_free_draw == 0) {
    reset_buffers();
    return;
  }

  // first, upload:
  Timer upload_timer;
  glBindVertexArray(m_ogl.vao);
  glBindBuffer(GL_ARRAY_BUFFER, m_ogl.vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, m_vertices.next_vertex * sizeof(Vertex), m_vertices.vertices.data(),
               GL_STREAM_DRAW);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ogl.index_buffer);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_vertices.next_index * sizeof(u32),
               m_vertices.indices.data(), GL_STREAM_DRAW);
  m_stats.upload_wait += upload_timer.getSeconds();
  m_stats.num_uploads++;
  m_stats.upload_bytes +=
      (m_vertices.next_vertex * sizeof(Vertex)) + (m_vertices.next_index * sizeof(u32));

  // initial OpenGL setup
  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);
  render_state->shaders[ShaderId::DIRECT2].activate();

  // draw call loop
  // draw_call_loop_simple(render_state, prof);
  draw_call_loop_grouped(render_state, prof);

  // done! reset.
  glBindVertexArray(0);

  reset_buffers();
}

void DirectRenderer2::draw_call_loop_simple(SharedRenderState* render_state,
                                            ScopedProfilerNode& prof) {
  lg::debug("------------------------");
  for (u32 draw_idx = 0; draw_idx < m_next_free_draw; draw_idx++) {
    const auto& draw = m_draw_buffer[draw_idx];
    lg::debug("{}", draw.to_single_line_string());
    setup_opengl_for_draw_mode(draw, render_state);
    setup_opengl_tex(0, draw.tbp, draw.mode.get_filt_enable(), draw.mode.get_clamp_s_enable(),
                     draw.mode.get_clamp_t_enable(), render_state);
    void* offset = (void*)(draw.start_index * sizeof(u32));
    int end_idx;
    if (draw_idx == m_next_free_draw - 1) {
      end_idx = m_vertices.next_index;
    } else {
      end_idx = m_draw_buffer[draw_idx + 1].start_index;
    }
    glDrawElements(GL_TRIANGLE_STRIP, end_idx - draw.start_index, GL_UNSIGNED_INT, (void*)offset);
    prof.add_draw_call();
    prof.add_tri((end_idx - draw.start_index) - 2);
  }
}

void DirectRenderer2::draw_call_loop_grouped(SharedRenderState* render_state,
                                             ScopedProfilerNode& prof) {
  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);
  u32 draw_idx = 0;
  while (draw_idx < m_next_free_draw) {
    const auto& draw = m_draw_buffer[draw_idx];
    u32 end_of_draw_group = draw_idx;  // this is inclusive
    setup_opengl_for_draw_mode(draw, render_state);
    setup_opengl_tex(draw.tex_unit, draw.tbp, draw.mode.get_filt_enable(),
                     draw.mode.get_clamp_s_enable(), draw.mode.get_clamp_t_enable(), render_state);

    for (u32 draw_to_consider = draw_idx + 1; draw_to_consider < draw_idx + TEX_UNITS;
         draw_to_consider++) {
      if (draw_to_consider >= m_next_free_draw) {
        break;
      }
      const auto& next_draw = m_draw_buffer[draw_to_consider];
      if (next_draw.mode.as_int() != draw.mode.as_int()) {
        break;
      }
      if (next_draw.fix != draw.fix) {
        break;
      }
      m_stats.saved_draws++;
      end_of_draw_group++;
      setup_opengl_tex(next_draw.tex_unit, next_draw.tbp, next_draw.mode.get_filt_enable(),
                       next_draw.mode.get_clamp_s_enable(), next_draw.mode.get_clamp_t_enable(),
                       render_state);
    }

    u32 end_idx;
    if (end_of_draw_group == m_next_free_draw - 1) {
      end_idx = m_vertices.next_index;
    } else {
      end_idx = m_draw_buffer[end_of_draw_group + 1].start_index;
    }
    void* offset = (void*)(draw.start_index * sizeof(u32));
    // fmt::print("drawing {:4d} with abe {} tex {} {}", end_idx - draw.start_index,
    // (int)draw.mode.get_ab_enable(), end_of_draw_group - draw_idx, draw.to_single_line_string() );
    // fmt::print("{}\n", draw.mode.to_string());
    glDrawElements(GL_TRIANGLE_STRIP, end_idx - draw.start_index, GL_UNSIGNED_INT, (void*)offset);
    prof.add_draw_call();
    prof.add_tri((end_idx - draw.start_index) / 3);
    draw_idx = end_of_draw_group + 1;
  }
}

void DirectRenderer2::setup_opengl_for_draw_mode(const Draw& draw,
                                                 SharedRenderState* render_state) {
  // compute alpha_reject:
  float alpha_reject = 0.f;
  if (draw.mode.get_at_enable()) {
    switch (draw.mode.get_alpha_test()) {
      case DrawMode::AlphaTest::ALWAYS:
        break;
      case DrawMode::AlphaTest::GEQUAL:
        alpha_reject = draw.mode.get_aref() / 128.f;
        break;
      case DrawMode::AlphaTest::NEVER:
        break;
      default:
        ASSERT_MSG(false, fmt::format("unknown alpha test: {}", (int)draw.mode.get_alpha_test()));
    }
  }

  // setup blending and color mult
  float color_mult = 1.f;
  if (!draw.mode.get_ab_enable()) {
    glDisable(GL_BLEND);
  } else {
    glEnable(GL_BLEND);
    glBlendColor(1, 1, 1, 1);
    if (draw.mode.get_alpha_blend() == DrawMode::AlphaBlend::SRC_DST_SRC_DST) {
      // (Cs - Cd) * As + Cd
      // Cs * As  + (1 - As) * Cd
      // s, d
      glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
      glBlendEquation(GL_FUNC_ADD);
    } else if (draw.mode.get_alpha_blend() == DrawMode::AlphaBlend::SRC_0_SRC_DST) {
      // (Cs - 0) * As + Cd
      // Cs * As + (1) * Cd
      // s, d
      ASSERT(draw.fix == 0);
      glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
      glBlendEquation(GL_FUNC_ADD);
    } else if (draw.mode.get_alpha_blend() == DrawMode::AlphaBlend::ZERO_SRC_SRC_DST) {
      // (0 - Cs) * As + Cd
      // Cd - Cs * As
      // s, d
      glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
      glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
    } else if (draw.mode.get_alpha_blend() == DrawMode::AlphaBlend::SRC_DST_FIX_DST) {
      // (Cs - Cd) * fix + Cd
      // Cs * fix + (1 - fx) * Cd
      glBlendFuncSeparate(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA, GL_ONE, GL_ZERO);
      glBlendColor(0, 0, 0, draw.fix / 127.f);
      glBlendEquation(GL_FUNC_ADD);
    } else if (draw.mode.get_alpha_blend() == DrawMode::AlphaBlend::SRC_SRC_SRC_SRC) {
      // this is very weird...
      // Cs
      glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
      glBlendEquation(GL_FUNC_ADD);
    } else if (draw.mode.get_alpha_blend() == DrawMode::AlphaBlend::SRC_0_DST_DST) {
      // (Cs - 0) * Ad + Cd
      glBlendFuncSeparate(GL_DST_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
      glBlendEquation(GL_FUNC_ADD);
      color_mult = 0.5;
    } else {
      ASSERT(false);
    }
  }

  // setup ztest
  if (draw.mode.get_zt_enable()) {
    glEnable(GL_DEPTH_TEST);
    switch (draw.mode.get_depth_test()) {
      case GsTest::ZTest::NEVER:
        glDepthFunc(GL_NEVER);
        break;
      case GsTest::ZTest::ALWAYS:
        glDepthFunc(GL_ALWAYS);
        break;
      case GsTest::ZTest::GEQUAL:
        glDepthFunc(GL_GEQUAL);
        break;
      case GsTest::ZTest::GREATER:
        glDepthFunc(GL_GREATER);
        break;
      default:
        ASSERT(false);
    }
  } else {
    // you aren't supposed to turn off z test enable, the GS had some bugs
    ASSERT(false);
  }

  if (draw.mode.get_depth_write_enable()) {
    glDepthMask(GL_TRUE);
  } else {
    glDepthMask(GL_FALSE);
  }

  if (draw.tbp == UINT16_MAX) {
    // not using a texture
    ASSERT(false);
    render_state->shaders[ShaderId::DIRECT_BASIC].activate();
  } else {
    // yes using a texture
    render_state->shaders[ShaderId::DIRECT2].activate();
    glUniform1f(m_ogl.alpha_reject, alpha_reject);
    glUniform1f(m_ogl.color_mult, color_mult);
    glUniform4f(m_ogl.fog_color, render_state->fog_color[0] / 255.f,
                render_state->fog_color[1] / 255.f, render_state->fog_color[2] / 255.f,
                render_state->fog_intensity / 255);
  }
}

void DirectRenderer2::setup_opengl_tex(u16 unit,
                                       u16 tbp,
                                       bool filter,
                                       bool clamp_s,
                                       bool clamp_t,
                                       SharedRenderState* render_state) {
  // look up the texture
  std::optional<u64> tex;
  u32 tbp_to_lookup = tbp & 0x7fff;
  bool use_mt4hh = tbp & 0x8000;

  if (use_mt4hh) {
    tex = render_state->texture_pool->lookup_mt4hh(tbp_to_lookup);
  } else {
    tex = render_state->texture_pool->lookup(tbp_to_lookup);
  }

  if (!tex) {
    lg::warn("Failed to find texture at {}, using random (direct2: {})", tbp_to_lookup, m_name);
    tex = render_state->texture_pool->get_placeholder_texture();
  }

  glActiveTexture(GL_TEXTURE0 + unit);
  glBindTexture(GL_TEXTURE_2D, *tex);
  if (clamp_s) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  } else {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  }

  if (clamp_t) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  } else {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  }

  if (filter) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    m_debug.disable_mip ? GL_LINEAR : GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  } else {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  }
}

void DirectRenderer2::draw_debug_window() {
  ImGui::Text("Uploads: %d", m_stats.num_uploads);
  ImGui::Text("Upload time: %.3f ms", m_stats.upload_wait * 1000);
  ImGui::Text("Upload size: %d bytes", m_stats.upload_bytes);
  ImGui::Text("Flush due to full: %d times", m_stats.flush_due_to_full);
}
