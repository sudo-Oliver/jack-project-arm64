/*!
 * @file DirectRenderer.cpp
 * See DirectRenderer.h.
 */

#include "DirectRenderer.h"

#include "common/log/log.h"

#include "game/graphics/opengl_renderer/AdgifHandler.h"

#include "third-party/imgui/imgui.h"

namespace {
constexpr PerGameVersion<int> game_height(448, 416, 416, 416);
}  // namespace

DirectRenderer::DirectRenderer(const std::string& name, int my_id, int batch_size)
    : BucketRenderer(name, my_id), DirectRendererCore(name, batch_size) {
  glGenBuffers(1, &m_ogl.vertex_buffer);
  glGenVertexArrays(1, &m_ogl.vao);
  glBindVertexArray(m_ogl.vao);
  glBindBuffer(GL_ARRAY_BUFFER, m_ogl.vertex_buffer);
  m_ogl.vertex_buffer_max_verts = batch_size * 3 * 2;
  m_ogl.vertex_buffer_bytes = m_ogl.vertex_buffer_max_verts * sizeof(Vertex);
  glBufferData(GL_ARRAY_BUFFER, m_ogl.vertex_buffer_bytes, nullptr,
               GL_STREAM_DRAW);  // todo stream?
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0,                             // location 0 in the shader
                        4,                             // 4 floats per vert (w unused)
                        GL_FLOAT,                      // floats
                        GL_TRUE,                       // normalized, ignored,
                        sizeof(Vertex),                //
                        (void*)offsetof(Vertex, xyzf)  // offset in array (why is this a pointer...)
  );

  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1,                             // location 0 in the shader
                        4,                             // 4 color components
                        GL_UNSIGNED_BYTE,              // floats
                        GL_TRUE,                       // normalized, ignored,
                        sizeof(Vertex),                //
                        (void*)offsetof(Vertex, rgba)  // offset in array (why is this a pointer...)
  );

  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2,                            // location 0 in the shader
                        3,                            // 3 floats per vert
                        GL_FLOAT,                     // floats
                        GL_FALSE,                     // normalized, ignored,
                        sizeof(Vertex),               //
                        (void*)offsetof(Vertex, stq)  // offset in array (why is this a pointer...)
  );

  glEnableVertexAttribArray(3);
  glVertexAttribIPointer(
      3,                                 // location 3 in the shader
      4,                                 // 3 floats per vert
      GL_UNSIGNED_BYTE,                  // floats
      sizeof(Vertex),                    //
      (void*)offsetof(Vertex, tex_unit)  // offset in array (why is this a pointer...)
  );

  glEnableVertexAttribArray(4);
  glVertexAttribIPointer(
      4,                               // location 4 in the shader
      1,                               // 3 floats per vert
      GL_UNSIGNED_BYTE,                // floats
      sizeof(Vertex),                  //
      (void*)offsetof(Vertex, use_uv)  // offset in array (why is this a pointer...)
  );

  glEnableVertexAttribArray(5);
  glVertexAttribPointer(5,                                // location 5 in the shader
                        4,                                // 4 floats per vert
                        GL_FLOAT,                         // floats
                        GL_FALSE,                         // normalized, ignored,
                        sizeof(Vertex),                   //
                        (void*)offsetof(Vertex, scissor)  // offset in array
  );
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
}

DirectRenderer::~DirectRenderer() {
  glDeleteBuffers(1, &m_ogl.vertex_buffer);
  glDeleteVertexArrays(1, &m_ogl.vao);
}

void DirectRenderer::init_shaders(ShaderLibrary& sl) {
  auto id = sl[ShaderId::DIRECT_BASIC_TEXTURED].id();
  m_uniforms.alpha_min = glGetUniformLocation(id, "alpha_min");
  m_uniforms.alpha_max = glGetUniformLocation(id, "alpha_max");
  m_uniforms.normal_shader_id = id;
}

void DirectRenderer::draw_debug_window() {
  ImGui::Checkbox("Wireframe", &m_debug_state.wireframe);
  ImGui::SameLine();
  ImGui::Checkbox("No-texture", &m_debug_state.disable_texture);
  ImGui::SameLine();
  ImGui::Checkbox("red", &m_debug_state.red);
  ImGui::SameLine();
  ImGui::Checkbox("always", &m_debug_state.always_draw);
  ImGui::SameLine();
  ImGui::Checkbox("no mip", &m_debug_state.disable_mipmap);

  ImGui::Text("Triangles: %d", m_stats.triangles);
  ImGui::SameLine();
  ImGui::Text("Draws: %d", m_stats.draw_calls);

  ImGui::Text("Flush from state change:");
  ImGui::Text("  tex0: %d", m_stats.flush_from_tex_0);
  ImGui::Text("  tex1: %d", m_stats.flush_from_tex_1);
  ImGui::Text("  zbuf: %d", m_stats.flush_from_zbuf);
  ImGui::Text("  test: %d", m_stats.flush_from_test);
  ImGui::Text("  ta0: %d", m_stats.flush_from_ta0);
  ImGui::Text("  alph: %d", m_stats.flush_from_alpha);
  ImGui::Text("  clmp: %d", m_stats.flush_from_clamp);
  ImGui::Text("  prim: %d", m_stats.flush_from_prim);
  ImGui::Text("  texstate: %d", m_stats.flush_from_state_exhaust);
  ImGui::Text(" Total: %d/%d",
              m_stats.flush_from_prim + m_stats.flush_from_clamp + m_stats.flush_from_alpha +
                  m_stats.flush_from_test + m_stats.flush_from_zbuf + m_stats.flush_from_tex_1 +
                  m_stats.flush_from_tex_0 + m_stats.flush_from_state_exhaust,
              m_stats.draw_calls);
}

void DirectRenderer::flush_draws() {
  SharedRenderState* render_state = m_current_render_state;
  ScopedProfilerNode& prof = *m_current_prof;
  // update opengl state
  if (m_blend_state_needs_gl_update) {
    update_gl_blend();
    m_blend_state_needs_gl_update = false;
  }

  if (m_prim_gl_state_needs_gl_update) {
    update_gl_prim();
    m_prim_gl_state_needs_gl_update = false;
  }

  if (m_test_state_needs_gl_update) {
    update_gl_test();
    m_test_state_needs_gl_update = false;
  }

  for (int i = 0; i < TEXTURE_STATE_COUNT; i++) {
    auto& tex_state = m_buffered_tex_state[i];
    if (tex_state.used) {
      update_gl_texture(i);
      tex_state.used = false;
      m_buffered_tex_state_currently_bound[i] = true;
    } else {
      m_buffered_tex_state_currently_bound[i] = false;
    }
  }
  m_next_free_tex_state = 0;
  m_current_tex_state_idx = -1;

  // NOTE: sometimes we want to update the GL state without actually rendering anything, such as sky
  // textures, so we only return after we've updated the full state
  if (m_prim_buffer.vert_count == 0) {
    return;
  }

  if (m_debug_state.disable_texture) {
    // a bit of a hack, this forces the non-textured shader always.
    render_state->shaders[ShaderId::DIRECT_BASIC].activate();
    m_blend_state_needs_gl_update = true;
    m_prim_gl_state_needs_gl_update = true;
  }

  if (m_debug_state.red) {
    render_state->shaders[ShaderId::DEBUG_RED].activate();
    glDisable(GL_BLEND);
    m_prim_gl_state_needs_gl_update = true;
    m_blend_state_needs_gl_update = true;
  }

  // hacks
  if (m_debug_state.always_draw) {
    glDisable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
  }

  glBindVertexArray(m_ogl.vao);
  // render!
  // update buffers:
  glBindBuffer(GL_ARRAY_BUFFER, m_ogl.vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, m_prim_buffer.vert_count * sizeof(Vertex),
               m_prim_buffer.vertices.data(), GL_STREAM_DRAW);

  GLint current_shader;
  GLint viewport_size[4];
  glGetIntegerv(GL_CURRENT_PROGRAM, &current_shader);
  glGetIntegerv(GL_VIEWPORT, viewport_size);
  glUniform1i(glGetUniformLocation(current_shader, "scissor_enable"),
              m_scissor_enable && !m_offscreen_mode);
  glUniform4f(glGetUniformLocation(current_shader, "game_sizes"), 512.0f,
              game_height[render_state->version], viewport_size[2], viewport_size[3]);

  int draw_count = 0;
  int num_tris = 0;

  if (m_test_state_needs_double_draw && current_shader == m_uniforms.normal_shader_id) {
    // this batch thing is a hack to make the sky in jak 2 draw correctly.
    // This is the usual atest with FB_ONLY issue.
    // we should check what pcsx2 does
    int n_batch = m_prim_buffer.vert_count;
    if (n_batch > 50 && n_batch < 700 && (n_batch % 2) == 0) {
      n_batch = n_batch / 2;
    } else {
      // printf("not splitting batch %d\n", n_batch);
    }
    int offset = 0;
    while (offset < m_prim_buffer.vert_count) {
      glDepthMask(GL_TRUE);
      glUniform1f(m_uniforms.alpha_min, m_double_draw_aref);
      glUniform1f(m_uniforms.alpha_max, 10);
      glDrawArrays(GL_TRIANGLES, offset, n_batch);
      glDepthMask(GL_FALSE);
      glUniform1f(m_uniforms.alpha_min, -10);
      glUniform1f(m_uniforms.alpha_max, m_double_draw_aref);
      glDrawArrays(GL_TRIANGLES, offset, n_batch);
      offset += n_batch;
      draw_count += 2;
      num_tris += n_batch / 3;
    }
    m_test_state_needs_double_draw = false;
    m_test_state_needs_gl_update = true;
    m_prim_gl_state_needs_gl_update = true;
  } else {
    glDrawArrays(GL_TRIANGLES, 0, m_prim_buffer.vert_count);
    num_tris += m_prim_buffer.vert_count / 3;
    draw_count++;
  }

  if (m_debug_state.wireframe) {
    render_state->shaders[ShaderId::DEBUG_RED].activate();
    glDisable(GL_BLEND);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glDrawArrays(GL_TRIANGLES, 0, m_prim_buffer.vert_count);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    m_blend_state_needs_gl_update = true;
    m_prim_gl_state_needs_gl_update = true;
    draw_count++;
  }

  glActiveTexture(GL_TEXTURE0);
  glBindVertexArray(0);
  prof.add_tri(num_tris);
  prof.add_draw_call(draw_count);
  m_stats.triangles += num_tris;
  m_stats.draw_calls += draw_count;
  m_prim_buffer.vert_count = 0;
}

void DirectRenderer::update_gl_prim() {
  SharedRenderState* render_state = m_current_render_state;
  // currently gouraud is handled in setup.
  const auto& state = m_prim_gl_state;
  if (state.texture_enable) {
    float alpha_min = 0.0;
    float alpha_max = 10;
    int greater = 0;
    if (m_test_state.alpha_test_enable) {
      switch (m_test_state.alpha_test) {
        case GsTest::AlphaTest::ALWAYS:
          break;
        case GsTest::AlphaTest::GEQUAL:
          alpha_min = m_test_state.aref / 128.f;
          m_double_draw_aref = alpha_min;
          greater = 0;
          break;
        case GsTest::AlphaTest::GREATER:
          alpha_min = (1 + m_test_state.aref) / 128.f;
          m_double_draw_aref = alpha_min;
          greater = 1;
          break;
        case GsTest::AlphaTest::NEVER:
          break;
        default:
          ASSERT_MSG(false, fmt::format("unknown alpha test: {}", (int)m_test_state.alpha_test));
      }
    }

    render_state->shaders[ShaderId::DIRECT_BASIC_TEXTURED].activate();
    glUniform1f(glGetUniformLocation(render_state->shaders[ShaderId::DIRECT_BASIC_TEXTURED].id(),
                                     "alpha_min"),
                alpha_min);
    glUniform1f(glGetUniformLocation(render_state->shaders[ShaderId::DIRECT_BASIC_TEXTURED].id(),
                                     "alpha_max"),
                alpha_max);
    glUniform1f(glGetUniformLocation(render_state->shaders[ShaderId::DIRECT_BASIC_TEXTURED].id(),
                                     "color_mult"),
                m_ogl.color_mult);
    glUniform1f(glGetUniformLocation(render_state->shaders[ShaderId::DIRECT_BASIC_TEXTURED].id(),
                                     "alpha_mult"),
                m_ogl.alpha_mult);
    glUniform4f(glGetUniformLocation(render_state->shaders[ShaderId::DIRECT_BASIC_TEXTURED].id(),
                                     "fog_color"),
                render_state->fog_color[0] / 255.f, render_state->fog_color[1] / 255.f,
                render_state->fog_color[2] / 255.f, render_state->fog_intensity / 255);
    glUniform1i(glGetUniformLocation(render_state->shaders[ShaderId::DIRECT_BASIC_TEXTURED].id(),
                                     "offscreen_mode"),
                m_offscreen_mode);
    glUniform1i(glGetUniformLocation(render_state->shaders[ShaderId::DIRECT_BASIC_TEXTURED].id(),
                                     "greater"),
                greater);
    glUniform1f(
        glGetUniformLocation(render_state->shaders[ShaderId::DIRECT_BASIC_TEXTURED].id(), "ta0"),
        state.ta0 / 255.f);

  } else {
    render_state->shaders[ShaderId::DIRECT_BASIC].activate();
  }

  if (state.aa_enable) {
    ASSERT(false);
  }
  if (state.ctxt) {
    ASSERT(false);
  }
  if (state.fix) {
    ASSERT(false);
  }
}

void DirectRenderer::update_gl_texture(int unit) {
  SharedRenderState* render_state = m_current_render_state;
  std::optional<u64> tex;
  auto& state = m_buffered_tex_state[unit];
  if (!state.used) {
    // nothing used this state, don't bother binding the texture.
    return;
  }
  if (state.using_mt4hh) {
    tex = render_state->texture_pool->lookup_mt4hh(state.texture_base_ptr);
  } else {
    tex = render_state->texture_pool->lookup(state.texture_base_ptr);
  }

  if (!tex) {
    lg::warn("Failed to find texture at {}, using random (direct: {})", state.texture_base_ptr,
             name_and_id());
    tex = render_state->texture_pool->get_placeholder_texture();
  }
  ASSERT(tex);

  glActiveTexture(GL_TEXTURE20 + unit);
  glBindTexture(GL_TEXTURE_2D, *tex);
  // Note: CLAMP and CLAMP_TO_EDGE are different...
  if (state.m_clamp_state.clamp_s) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  } else {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  }

  if (state.m_clamp_state.clamp_t) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  } else {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  }

  if (state.enable_tex_filt) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    m_debug_state.disable_mipmap ? GL_LINEAR : GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  } else {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  }
}

void DirectRenderer::update_gl_blend() {
  const auto& state = m_blend_state;
  m_ogl.color_mult = 1.f;
  m_ogl.alpha_mult = 1.f;
  m_prim_gl_state_needs_gl_update = true;
  if (!state.alpha_blend_enable) {
    glDisable(GL_BLEND);
  } else {
    glEnable(GL_BLEND);
    glBlendColor(1, 1, 1, 1);

    if (state.a == GsAlpha::BlendMode::SOURCE && state.b == GsAlpha::BlendMode::DEST &&
        state.c == GsAlpha::BlendMode::SOURCE && state.d == GsAlpha::BlendMode::DEST) {
      // (Cs - Cd) * As + Cd
      // Cs * As  + (1 - As) * Cd
      // s, d
      // glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
      glBlendEquation(GL_FUNC_ADD);

    } else if (state.a == GsAlpha::BlendMode::SOURCE &&
               state.b == GsAlpha::BlendMode::ZERO_OR_FIXED &&
               state.c == GsAlpha::BlendMode::SOURCE && state.d == GsAlpha::BlendMode::DEST) {
      // (Cs - 0) * As + Cd
      // Cs * As + (1) * Cd
      // s, d
      ASSERT(state.fix == 0);
      glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
      glBlendEquation(GL_FUNC_ADD);
    } else if (state.a == GsAlpha::BlendMode::ZERO_OR_FIXED &&
               state.b == GsAlpha::BlendMode::SOURCE && state.c == GsAlpha::BlendMode::SOURCE &&
               state.d == GsAlpha::BlendMode::DEST) {
      // (0 - Cs) * As + Cd
      // Cd - Cs * As
      // s, d
      glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
      glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
    } else if (state.a == GsAlpha::BlendMode::SOURCE && state.b == GsAlpha::BlendMode::DEST &&
               state.c == GsAlpha::BlendMode::ZERO_OR_FIXED &&
               state.d == GsAlpha::BlendMode::DEST) {
      // (Cs - Cd) * fix + Cd
      // Cs * fix + (1 - fx) * Cd
      glBlendFuncSeparate(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA, GL_ONE, GL_ZERO);
      glBlendColor(0, 0, 0, state.fix / 127.f);
      glBlendEquation(GL_FUNC_ADD);
    } else if (state.a == GsAlpha::BlendMode::SOURCE && state.b == GsAlpha::BlendMode::SOURCE &&
               state.c == GsAlpha::BlendMode::SOURCE && state.d == GsAlpha::BlendMode::SOURCE) {
      // trick to disable alpha blending.
      glDisable(GL_BLEND);
    } else if (state.a == GsAlpha::BlendMode::SOURCE &&
               state.b == GsAlpha::BlendMode::ZERO_OR_FIXED &&
               state.c == GsAlpha::BlendMode::DEST && state.d == GsAlpha::BlendMode::DEST) {
      // (Cs - 0) * Ad + Cd
      glBlendFuncSeparate(GL_DST_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
      glBlendEquation(GL_FUNC_ADD);
      m_ogl.color_mult = 0.5;
    } else {
      // unsupported blend: a 0 b 2 c 2 d 1
      lg::error("unsupported blend (direct): a {} b {} c {} d {}", (int)state.a, (int)state.b,
                (int)state.c, (int)state.d);
      //      ASSERT(false);
    }
  }
}

void DirectRenderer::update_gl_test() {
  const auto& state = m_test_state;

  if (state.zte) {
    glEnable(GL_DEPTH_TEST);
    switch (state.ztst) {
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

  if (state.date) {
    ASSERT(false);
  }

  bool alpha_trick_to_disable = m_test_state.alpha_test_enable &&
                                m_test_state.alpha_test == GsTest::AlphaTest::NEVER &&
                                m_test_state.afail == GsTest::AlphaFail::FB_ONLY;

  if (m_test_state.afail == GsTest::AlphaFail::FB_ONLY ||
      m_test_state.afail == GsTest::AlphaFail::RGB_ONLY) {
    m_test_state_needs_double_draw = true;
  } else {
    m_test_state_needs_double_draw = false;
  }

  if (state.depth_writes && !alpha_trick_to_disable) {
    glDepthMask(GL_TRUE);
  } else {
    glDepthMask(GL_FALSE);
  }

  if (state.write_rgb) {
    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  } else {
    glColorMaski(0, GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
  }
}
/*!
 * Point the core at this frame's render state. Everything it needs that is not its own comes
 * through here.
 */
void DirectRenderer::set_context(SharedRenderState* render_state) {
  m_current_render_state = render_state;
  Context ctx;
  ctx.texture_pool = render_state->texture_pool.get();
  ctx.version = render_state->version;
  ctx.next_bucket = render_state->next_bucket;
  ctx.default_regs_buffer = render_state->default_regs_buffer;
  ctx.fog_color = render_state->fog_color;
  ctx.fog_intensity = render_state->fog_intensity;
  m_context = ctx;
}

void DirectRenderer::render(DmaFollower& dma,
                            SharedRenderState* render_state,
                            ScopedProfilerNode& prof) {
  set_context(render_state);
  m_current_prof = &prof;
  if (m_enabled) {
    consume_bucket_dma(dma);
  } else {
    while (dma.current_tag_offset() != render_state->next_bucket && !dma.ended()) {
      dma.read_and_advance();
    }
  }
  m_current_prof = nullptr;
  glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

void DirectRenderer::render_vif(u32 vif0,
                                u32 vif1,
                                const u8* data,
                                u32 size,
                                SharedRenderState* render_state,
                                ScopedProfilerNode& prof) {
  set_context(render_state);
  m_current_prof = &prof;
  DirectRendererCore::render_vif(vif0, vif1, data, size);
  m_current_prof = nullptr;
}

void DirectRenderer::render_gif(const u8* data,
                                u32 size,
                                SharedRenderState* render_state,
                                ScopedProfilerNode& prof) {
  set_context(render_state);
  m_current_prof = &prof;
  DirectRendererCore::render_gif(data, size);
  m_current_prof = nullptr;
}

void DirectRenderer::flush_pending(SharedRenderState* render_state, ScopedProfilerNode& prof) {
  set_context(render_state);
  m_current_prof = &prof;
  flush();
  m_current_prof = nullptr;
}

void DirectRenderer::lookup_textures_again(SharedRenderState* render_state) {
  set_context(render_state);
  DirectRendererCore::lookup_textures_again();
}

void DirectRenderer::backend_bind_texture(int unit) {
  update_gl_texture(unit);
}

