/*!
 * @file Sprite3_Distort.cpp
 * The OpenGL half of the sprite distorter. The vertex building is in Sprite3Core_Distort.cpp.
 */

#include "Sprite3.h"

#include "game/graphics/opengl_renderer/background/background_common.h"

void Sprite3::opengl_setup_distort() {
  // Create framebuffer to snapshot current render to a texture that can be bound for the distort
  // shader This will represent tex0 from the original GS data
  glGenFramebuffers(1, &m_distort_ogl.fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, m_distort_ogl.fbo);

  glGenTextures(1, &m_distort_ogl.fbo_texture);
  glBindTexture(GL_TEXTURE_2D, m_distort_ogl.fbo_texture);

  // GL_RGBA8, not GL_RGB: distort_draw_common resolves the render framebuffer into this texture
  // with glBlitFramebuffer, and at msaa != 1 that read is multisampled. A multisample resolve
  // requires the source and destination colour formats to match, so an RGB destination against
  // the RGBA8 render buffer is GL_INVALID_OPERATION: the blit is dropped and the shader samples a
  // stale texture.
  //
  // The alpha channel is load-bearing. The distort shader computes
  // out_color = color * texture(framebuffer_tex, ...) under
  // glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, ...), so the sampled alpha is the
  // blend weight; an RGB texture returns alpha = 1.0 by GL component expansion and silently pins
  // that weight even at msaa 1, where the blit does succeed.
  //
  // distort_setup_framebuffer_dims() reallocates this on resize and must use the same format --
  // fixing only one site reintroduces the fault on the first resize.
  //
  // Fix from https://github.com/nikolasburns/jak-arm64-macos (f0e09e0d9).
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_distort_ogl.fbo_width, m_distort_ogl.fbo_height, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, NULL);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  // Texture clamping here matches the GS init data for distort
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         m_distort_ogl.fbo_texture, 0);

  ASSERT(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  // Non-instancing
  // ----------------------
  glGenBuffers(1, &m_distort_ogl.vertex_buffer);
  glGenVertexArrays(1, &m_distort_ogl.vao);
  glBindVertexArray(m_distort_ogl.vao);
  glBindBuffer(GL_ARRAY_BUFFER, m_distort_ogl.vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, MAX_DISTORT_VERTS * sizeof(SpriteDistortVertex), nullptr,
               GL_DYNAMIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0,                                         // location 0 in the shader
                        3,                                         // 3 floats per vert
                        GL_FLOAT,                                  // floats
                        GL_FALSE,                                  // don't normalize, ignored
                        sizeof(SpriteDistortVertex),               //
                        (void*)offsetof(SpriteDistortVertex, xyz)  // offset in array
  );
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1,                                        // location 1 in the shader
                        2,                                        // 2 floats per vert
                        GL_FLOAT,                                 // floats
                        GL_FALSE,                                 // don't normalize, ignored
                        sizeof(SpriteDistortVertex),              //
                        (void*)offsetof(SpriteDistortVertex, st)  // offset in array
  );

  glGenBuffers(1, &m_distort_ogl.index_buffer);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_distort_ogl.index_buffer);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, MAX_DISTORT_INDS * sizeof(u32), nullptr, GL_DYNAMIC_DRAW);

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  glBindVertexArray(0);

  // Instancing
  // ----------------------
  glGenVertexArrays(1, &m_distort_instanced_ogl.vao);
  glBindVertexArray(m_distort_instanced_ogl.vao);

  glGenBuffers(1, &m_distort_instanced_ogl.vertex_buffer);
  glBindBuffer(GL_ARRAY_BUFFER, m_distort_instanced_ogl.vertex_buffer);

  glBufferData(GL_ARRAY_BUFFER,
               m_sprite_distorter_vertices_instanced.size() * sizeof(SpriteDistortVertex), nullptr,
               GL_STREAM_DRAW);

  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0,                                         // location 0 in the shader
                        3,                                         // 3 floats per vert
                        GL_FLOAT,                                  // floats
                        GL_FALSE,                                  // don't normalize, ignored
                        sizeof(SpriteDistortVertex),               //
                        (void*)offsetof(SpriteDistortVertex, xyz)  // offset in array
  );
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1,                                        // location 1 in the shader
                        2,                                        // 2 floats per vert
                        GL_FLOAT,                                 // floats
                        GL_FALSE,                                 // don't normalize, ignored
                        sizeof(SpriteDistortVertex),              //
                        (void*)offsetof(SpriteDistortVertex, st)  // offset in array
  );

  glGenBuffers(1, &m_distort_instanced_ogl.instance_buffer);
  glBindBuffer(GL_ARRAY_BUFFER, m_distort_instanced_ogl.instance_buffer);

  glBufferData(GL_ARRAY_BUFFER, MAX_DISTORT_SPRITES * sizeof(SpriteDistortInstanceData), nullptr,
               GL_DYNAMIC_DRAW);

  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2,                                  // location 2 in the shader
                        4,                                  // 4 floats per vert
                        GL_FLOAT,                           // floats
                        GL_FALSE,                           // normalized, ignored,
                        sizeof(SpriteDistortInstanceData),  //
                        (void*)offsetof(SpriteDistortInstanceData, x_y_z_s)  // offset in array
  );
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(3,                                  // location 3 in the shader
                        4,                                  // 4 floats per vert
                        GL_FLOAT,                           // floats
                        GL_FALSE,                           // normalized, ignored,
                        sizeof(SpriteDistortInstanceData),  //
                        (void*)offsetof(SpriteDistortInstanceData, sx_sy_sz_t)  // offset in array
  );

  glVertexAttribDivisor(2, 1);
  glVertexAttribDivisor(3, 1);

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
}

void Sprite3::distort_instanced_mesh_changed() {
  m_distort_instanced_ogl.vertex_data_changed = true;
}

void Sprite3::distort_draw_gpu(bool instanced) {
  distort_draw(instanced);
}

/*!
 * Draws each distort sprite.
 */
void Sprite3::distort_draw(bool instanced) {
  auto* render_state = m_current_render_state;
  auto& prof = *m_current_prof;

  // First, make sure the distort framebuffer is the correct size
  distort_setup_framebuffer_dims();

  if (m_distort_stats.total_tris == 0) {
    // No distort sprites to draw, we can end early
    return;
  }

  // Do common distort drawing logic
  distort_draw_common();

  // Set up shader
  auto shader = &render_state->shaders[instanced ? ShaderId::SPRITE_DISTORT_INSTANCED
                                                 : ShaderId::SPRITE_DISTORT];
  shader->activate();

  Vector4f colorf = Vector4f(m_sprite_distorter_sine_tables.color.x() / 255.0f,
                             m_sprite_distorter_sine_tables.color.y() / 255.0f,
                             m_sprite_distorter_sine_tables.color.z() / 255.0f,
                             m_sprite_distorter_sine_tables.color.w() / 255.0f);
  glUniform4fv(glGetUniformLocation(shader->id(), "u_color"), 1, colorf.data());

  if (!instanced) {
    // Bind vertex array
    glBindVertexArray(m_distort_ogl.vao);

    // Enable prim restart, we need this to break up the triangle strips
    glEnable(GL_PRIMITIVE_RESTART);
    glPrimitiveRestartIndex(UINT32_MAX);

    // Upload vertex data
    glBindBuffer(GL_ARRAY_BUFFER, m_distort_ogl.vertex_buffer);
    glBufferData(GL_ARRAY_BUFFER, m_sprite_distorter_vertices.size() * sizeof(SpriteDistortVertex),
                 m_sprite_distorter_vertices.data(), GL_DYNAMIC_DRAW);

    // Upload element data
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_distort_ogl.index_buffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_sprite_distorter_indices.size() * sizeof(u32),
                 m_sprite_distorter_indices.data(), GL_DYNAMIC_DRAW);

    // Draw
    prof.add_draw_call();
    prof.add_tri(m_distort_stats.total_tris);

    glDrawElements(GL_TRIANGLE_STRIP, m_sprite_distorter_indices.size(), GL_UNSIGNED_INT,
                   (void*)0);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    return;
  }

  // Bind vertex array
  glBindVertexArray(m_distort_instanced_ogl.vao);

  // Upload vertex data (if it changed)
  if (m_distort_instanced_ogl.vertex_data_changed) {
    m_distort_instanced_ogl.vertex_data_changed = false;

    glBindBuffer(GL_ARRAY_BUFFER, m_distort_instanced_ogl.vertex_buffer);
    glBufferData(GL_ARRAY_BUFFER,
                 m_sprite_distorter_vertices_instanced.size() * sizeof(SpriteDistortVertex),
                 m_sprite_distorter_vertices_instanced.data(), GL_STREAM_DRAW);
  }

  // Draw each resolution group
  glBindBuffer(GL_ARRAY_BUFFER, m_distort_instanced_ogl.instance_buffer);
  prof.add_tri(m_distort_stats.total_tris);

  int vert_offset = 0;
  for (int res = 3; res < 12; res++) {
    auto& instances = m_sprite_distorter_instances_by_res[res];
    int num_verts = res * 5;

    if (instances.size() > 0) {
      // Upload instance data
      glBufferData(GL_ARRAY_BUFFER, instances.size() * sizeof(SpriteDistortInstanceData),
                   instances.data(), GL_DYNAMIC_DRAW);

      // Draw
      prof.add_draw_call();

      glDrawArraysInstanced(GL_TRIANGLE_STRIP, vert_offset, num_verts, instances.size());
    }

    vert_offset += num_verts;
  }

  // Done
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
}

void Sprite3::distort_draw_common() {
  auto* render_state = m_current_render_state;

  // The distort effect needs to read the current framebuffer, so copy what's been rendered so far
  // to a texture that we can then pass to the shader
  glBindFramebuffer(GL_READ_FRAMEBUFFER, render_state->render_fb);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_distort_ogl.fbo);

  glBlitFramebuffer(0,                          // srcX0
                    0,                          // srcY0
                    render_state->render_fb_w,  // srcX1
                    render_state->render_fb_h,  // srcY1
                    0,                          // dstX0
                    0,                          // dstY0
                    m_distort_ogl.fbo_width,    // dstX1
                    m_distort_ogl.fbo_height,   // dstY1
                    GL_COLOR_BUFFER_BIT,        // mask
                    GL_NEAREST                  // filter
  );

  glBindFramebuffer(GL_FRAMEBUFFER, render_state->render_fb);

  // Set up OpenGL state
  m_current_mode.set_depth_write_enable(!m_sprite_distorter_setup.zbuf.zmsk());  // zbuf
  glBindTexture(GL_TEXTURE_2D, m_distort_ogl.fbo_texture);                       // tex0
  m_current_mode.set_filt_enable(m_sprite_distorter_setup.tex1.mmag());          // tex1
  update_mode_from_alpha1(m_sprite_distorter_setup.alpha.data, m_current_mode);  // alpha1
  // note: clamp and miptbp are skipped since that is set up ahead of time with the distort
  // framebuffer texture

  setup_opengl_from_draw_mode(m_current_mode, GL_TEXTURE0, false);
}

void Sprite3::distort_setup_framebuffer_dims() {
  auto* render_state = m_current_render_state;

  // Distort framebuffer must be the same dimensions as the default window framebuffer
  if (m_distort_ogl.fbo_width != render_state->render_fb_w ||
      m_distort_ogl.fbo_height != render_state->render_fb_h) {
    m_distort_ogl.fbo_width = render_state->render_fb_w;
    m_distort_ogl.fbo_height = render_state->render_fb_h;

    glBindTexture(GL_TEXTURE_2D, m_distort_ogl.fbo_texture);

    // Must match opengl_setup_distort()'s format exactly -- see the comment there.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_distort_ogl.fbo_width, m_distort_ogl.fbo_height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    glBindTexture(GL_TEXTURE_2D, 0);
  }
}
