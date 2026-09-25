/*!
 * @file DepthCue.cpp
 * See DepthCue.h. The DMA walk and the quad building are in DepthCueCore.
 */

#include "DepthCue.h"

#include "third-party/imgui/imgui.h"

DepthCue::DepthCue(const std::string& name, int my_id) : BucketRenderer(name, my_id) {
  opengl_setup();
}

void DepthCue::opengl_setup() {
  // Gen texture for sampling the framebuffer
  glGenFramebuffers(1, &m_ogl.framebuffer_sample_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, m_ogl.framebuffer_sample_fbo);

  glGenTextures(1, &m_ogl.framebuffer_sample_tex);
  glBindTexture(GL_TEXTURE_2D, m_ogl.framebuffer_sample_tex);

  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         m_ogl.framebuffer_sample_tex, 0);

  ASSERT(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  // Gen framebuffer for depth-cue-base-page
  glGenFramebuffers(1, &m_ogl.fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, m_ogl.fbo);

  glGenTextures(1, &m_ogl.fbo_texture);
  glBindTexture(GL_TEXTURE_2D, m_ogl.fbo_texture);

  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ogl.fbo_texture, 0);

  ASSERT(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  // Gen vertex array for drawing to depth-cue-base-page
  glGenVertexArrays(1, &m_ogl.depth_cue_page_vao);
  glBindVertexArray(m_ogl.depth_cue_page_vao);
  glGenBuffers(1, &m_ogl.depth_cue_page_vertex_buffer);
  glBindBuffer(GL_ARRAY_BUFFER, m_ogl.depth_cue_page_vertex_buffer);

  glBufferData(GL_ARRAY_BUFFER, 4 * sizeof(SpriteVertex), nullptr, GL_STATIC_DRAW);

  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0,                                 // location 0 in the shader
                        2,                                 // 2 floats per vert
                        GL_FLOAT,                          // floats
                        GL_FALSE,                          // don't normalize, ignored
                        sizeof(SpriteVertex),              //
                        (void*)offsetof(SpriteVertex, xy)  // offset in array
  );
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1,                                 // location 1 in the shader
                        2,                                 // 2 floats per vert
                        GL_FLOAT,                          // floats
                        GL_FALSE,                          // don't normalize, ignored
                        sizeof(SpriteVertex),              //
                        (void*)offsetof(SpriteVertex, st)  // offset in array
  );

  // Gen vertex array for drawing to on-screen framebuffer
  glGenVertexArrays(1, &m_ogl.on_screen_vao);
  glBindVertexArray(m_ogl.on_screen_vao);
  glGenBuffers(1, &m_ogl.on_screen_vertex_buffer);
  glBindBuffer(GL_ARRAY_BUFFER, m_ogl.on_screen_vertex_buffer);

  glBufferData(GL_ARRAY_BUFFER, 4 * sizeof(SpriteVertex), nullptr, GL_STATIC_DRAW);

  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0,                                 // location 0 in the shader
                        2,                                 // 2 floats per vert
                        GL_FLOAT,                          // floats
                        GL_FALSE,                          // don't normalize, ignored
                        sizeof(SpriteVertex),              //
                        (void*)offsetof(SpriteVertex, xy)  // offset in array
  );
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1,                                 // location 1 in the shader
                        2,                                 // 2 floats per vert
                        GL_FLOAT,                          // floats
                        GL_FALSE,                          // don't normalize, ignored
                        sizeof(SpriteVertex),              //
                        (void*)offsetof(SpriteVertex, st)  // offset in array
  );

  // Done
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
}

void DepthCue::render(DmaFollower& dma,
                      SharedRenderState* render_state,
                      ScopedProfilerNode& prof) {
  m_current_render_state = render_state;
  m_current_prof = &prof;

  Context context;
  context.next_bucket = render_state->next_bucket;
  context.enabled = m_enabled;
  context.draw_region_w = render_state->draw_region_w;
  context.draw_region_h = render_state->draw_region_h;
  render_core(dma, context);

  m_current_render_state = nullptr;
  m_current_prof = nullptr;
}

void DepthCue::draw_debug_window() {
  draw_debug_window_core();
}

void DepthCue::do_draw() {
  auto* render_state = m_current_render_state;
  auto& prof = *m_current_prof;

  // The core only rebuilds the quads and the target sizes when something changed.
  if (m_geometry_changed) {
    glBindTexture(GL_TEXTURE_2D, m_ogl.framebuffer_sample_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, m_fb_sample_width, m_fb_sample_height, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, NULL);

    glBindTexture(GL_TEXTURE_2D, m_ogl.fbo_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, m_fbo_width, m_fbo_height, 0, GL_RGB, GL_UNSIGNED_BYTE,
                 NULL);
    glBindTexture(GL_TEXTURE_2D, 0);

    glBindBuffer(GL_ARRAY_BUFFER, m_ogl.depth_cue_page_vertex_buffer);
    glBufferData(GL_ARRAY_BUFFER, m_depth_cue_page_vertices.size() * sizeof(SpriteVertex),
                 m_depth_cue_page_vertices.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, m_ogl.on_screen_vertex_buffer);
    glBufferData(GL_ARRAY_BUFFER, m_on_screen_vertices.size() * sizeof(SpriteVertex),
                 m_on_screen_vertices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
  }

  // Disable depth writing but keep test
  glEnable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);

  // Activate shader
  auto shader = &render_state->shaders[ShaderId::DEPTH_CUE];
  shader->activate();

  glUniform1i(glGetUniformLocation(shader->id(), "tex"), 0);

  // First, we need to copy the framebuffer into the framebuffer sample texture
  glBindFramebuffer(GL_READ_FRAMEBUFFER, render_state->render_fb);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_ogl.framebuffer_sample_fbo);

  glBlitFramebuffer(render_state->draw_offset_x,                                // srcX0
                    render_state->draw_offset_y,                                // srcY0
                    render_state->draw_offset_x + render_state->draw_region_w,  // srcX1
                    render_state->draw_offset_y + render_state->draw_region_h,  // srcY1
                    0,                                                          // dstX0
                    0,                                                          // dstY0
                    m_fb_sample_width,                                          // dstX1
                    m_fb_sample_height,                                         // dstY1
                    GL_COLOR_BUFFER_BIT,                                        // mask
                    GL_NEAREST                                                  // filter
  );

  glBindFramebuffer(GL_FRAMEBUFFER, render_state->render_fb);

  // Next, draw from the framebuffer sample texture to the depth-cue-base-page framebuffer
  {
    math::Vector4f colorf = page_color();
    glUniform4fv(glGetUniformLocation(shader->id(), "u_color"), 1, colorf.data());
    glUniform1f(glGetUniformLocation(shader->id(), "u_depth"), 1.0f);

    glBindFramebuffer(GL_FRAMEBUFFER, m_ogl.fbo);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_ogl.framebuffer_sample_tex);

    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ZERO);

    glViewport(0, 0, m_fbo_width, m_fbo_height);

    prof.add_draw_call();
    prof.add_tri(2 * TOTAL_DRAW_SLICES);

    glBindVertexArray(m_ogl.depth_cue_page_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6 * TOTAL_DRAW_SLICES);  // 6 verts per slice
  }

  // Finally, the contents of depth-cue-base-page need to be overlayed onto the on-screen
  // framebuffer
  {
    math::Vector4f colorf = screen_color();
    glUniform4fv(glGetUniformLocation(shader->id(), "u_color"), 1, colorf.data());
    glUniform1f(glGetUniformLocation(shader->id(), "u_depth"), screen_depth());

    glBindFramebuffer(GL_FRAMEBUFFER, render_state->render_fb);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_ogl.fbo_texture);

    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);

    glViewport(render_state->draw_offset_x, render_state->draw_offset_y,
               render_state->draw_region_w, render_state->draw_region_h);

    prof.add_draw_call();
    prof.add_tri(2 * TOTAL_DRAW_SLICES);

    glBindVertexArray(m_ogl.on_screen_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6 * TOTAL_DRAW_SLICES);  // 6 verts per slice
  }

  // Done
  glDepthMask(GL_TRUE);
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindVertexArray(0);
}
