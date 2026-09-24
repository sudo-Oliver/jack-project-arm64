/*!
 * @file OceanTexture.cpp
 * See OceanTexture.h.
 */

#include "OceanTexture.h"

#include "game/graphics/ocean/CommonOceanRendererCore.h"
#include "game/graphics/opengl_renderer/AdgifHandler.h"

#include "third-party/imgui/imgui.h"

OceanTexture::OceanTexture(bool generate_mipmaps)
    : OceanTextureCore(generate_mipmaps),
      m_result_texture(TEX0_SIZE,
                       TEX0_SIZE,
                       GL_UNSIGNED_INT_8_8_8_8_REV,
                       m_generate_mipmaps ? NUM_MIPS : 1),
      m_temp_texture(TEX0_SIZE, TEX0_SIZE, GL_UNSIGNED_INT_8_8_8_8_REV) {
  init_gl();

  // initialize the mipmap drawing
  glGenVertexArrays(1, &m_mipmap.vao);
  glBindVertexArray(m_mipmap.vao);
  glGenBuffers(1, &m_mipmap.vtx_buffer);
  glBindBuffer(GL_ARRAY_BUFFER, m_mipmap.vtx_buffer);
  std::vector<MipMap::Vertex> vertices = {
      {-1, -1, 0, 0}, {-1, 1, 0, 1}, {1, -1, 1, 0}, {1, 1, 1, 1}};
  glBufferData(GL_ARRAY_BUFFER, sizeof(MipMap::Vertex) * 4, vertices.data(), GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(
      0,                                  // location 0 in the shader
      2,                                  // 4 color components
      GL_FLOAT,                           // floats
      GL_FALSE,                           // normalized, ignored,
      sizeof(MipMap::Vertex),             //
      (void*)offsetof(MipMap::Vertex, x)  // offset in array (why is this a pointer...)
  );
  glVertexAttribPointer(
      1,                                  // location 0 in the shader
      2,                                  // 4 color components
      GL_FLOAT,                           // floats
      GL_FALSE,                           // normalized, ignored,
      sizeof(MipMap::Vertex),             //
      (void*)offsetof(MipMap::Vertex, s)  // offset in array (why is this a pointer...)
  );
  glBindVertexArray(0);
}

OceanTexture::~OceanTexture() = default;

void OceanTexture::handle_ocean_texture_jak1(DmaFollower& dma,
                                             SharedRenderState* render_state,
                                             ScopedProfilerNode& prof) {
  m_current_render_state = render_state;
  m_current_prof = &prof;
  OceanTextureCore::handle_ocean_texture_jak1(dma, render_state->texture_pool.get());
  m_current_render_state = nullptr;
  m_current_prof = nullptr;
}

void OceanTexture::handle_ocean_texture_jak2(DmaFollower& dma,
                                             SharedRenderState* render_state,
                                             ScopedProfilerNode& prof) {
  m_current_render_state = render_state;
  m_current_prof = &prof;
  OceanTextureCore::handle_ocean_texture_jak2(dma, render_state->texture_pool.get());
  m_current_render_state = nullptr;
  m_current_prof = nullptr;
}

void OceanTexture::backend_begin_pass(bool to_temp) {
  m_pass_ctxt.emplace(to_temp ? m_temp_texture : m_result_texture);
}

void OceanTexture::backend_end_pass() {
  m_pass_ctxt.reset();
}

void OceanTexture::init_textures(TexturePool& pool, GameVersion version) {
  TextureInput in;
  in.gpu_texture = m_result_texture.texture();
  in.w = TEX0_SIZE;
  in.h = TEX0_SIZE;
  in.debug_page_name = "PC-OCEAN";
  in.debug_name = fmt::format("pc-ocean-mip-{}", m_generate_mipmaps);
  in.id = pool.allocate_pc_port_texture(version);
  switch (version) {
    case GameVersion::Jak1:
      m_tex0_gpu = pool.give_texture_and_load_to_vram(in, OCEAN_TEX_TBP_JAK1);
      break;
    case GameVersion::Jak2:
    case GameVersion::Jak3:
    case GameVersion::JakX:
      m_tex0_gpu = pool.give_texture_and_load_to_vram(in, OCEAN_TEX_TBP_JAK2);
      break;
  }
}

void OceanTexture::draw_debug_window() {
  if (m_tex0_gpu) {
    ImGui::Image((ImTextureID)(intptr_t)m_tex0_gpu->gpu_textures.at(0).gl,
                 ImVec2(m_tex0_gpu->w, m_tex0_gpu->h));
  }
}

void OceanTexture::backend_make_texture_with_mipmaps() {
  SharedRenderState* render_state = m_current_render_state;
  ScopedProfilerNode& prof = *m_current_prof;
  glBindVertexArray(m_mipmap.vao);
  render_state->shaders[ShaderId::OCEAN_TEXTURE_MIPMAP].activate();
  glUniform1f(glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_TEXTURE_MIPMAP].id(),
                                   "alpha_intensity"),
              1.0);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_temp_texture.texture());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glUniform1i(
      glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_TEXTURE_MIPMAP].id(), "tex_T0"),
      0);
  glBindBuffer(GL_ARRAY_BUFFER, m_mipmap.vtx_buffer);

  for (int i = 0; i < NUM_MIPS; i++) {
    FramebufferTexturePairContext ctxt(m_result_texture, i);
    glUniform1f(glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_TEXTURE_MIPMAP].id(),
                                     "alpha_intensity"),
                std::max(0.f, 1.f - 0.51f * i));
    glUniform1f(
        glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_TEXTURE_MIPMAP].id(), "scale"),
        1.f / (1 << i));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    prof.add_draw_call();
    prof.add_tri(2);
  }
  glBindVertexArray(0);
}

void OceanTexture::backend_flush() {
  SharedRenderState* render_state = m_current_render_state;
  ScopedProfilerNode& prof = *m_current_prof;
  ASSERT(m_pc.vtx_idx == 2112);
  glBindVertexArray(m_ogl.vao);
  glBindBuffer(GL_ARRAY_BUFFER, m_ogl.dynamic_vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * NUM_VERTS, m_pc.vertex_dynamic.data(),
               GL_DYNAMIC_DRAW);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ogl.gl_index_buffer);

  render_state->shaders[ShaderId::OCEAN_TEXTURE].activate();

  GsTex0 tex0(m_envmap_adgif.tex0_data);
  auto lookup = render_state->texture_pool->lookup(tex0.tbp0());
  if (!lookup) {
    lookup = render_state->texture_pool->get_placeholder_texture();
  }
  // no decal
  // yes tcc
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, *lookup);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  glUniform1i(glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_TEXTURE].id(), "tex_T0"),
              0);

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  // glDrawArrays(GL_TRIANGLE_STRIP, 0, NUM_VERTS);
  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);
  glDrawElements(GL_TRIANGLE_STRIP, m_pc.index_buffer.size(), GL_UNSIGNED_INT, (void*)0);
  prof.add_draw_call();
  prof.add_tri(NUM_STRIPS * NUM_STRIPS * 2);

  glBindVertexArray(0);
}


void OceanTexture::init_gl() {
  glGenVertexArrays(1, &m_ogl.vao);
  glBindVertexArray(m_ogl.vao);

  glGenBuffers(1, &m_ogl.gl_index_buffer);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ogl.gl_index_buffer);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(u32) * m_pc.index_buffer.size(),
               m_pc.index_buffer.data(), GL_STATIC_DRAW);

  glGenBuffers(1, &m_ogl.static_vertex_buffer);
  glBindBuffer(GL_ARRAY_BUFFER, m_ogl.static_vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, sizeof(math::Vector2f) * NUM_VERTS, m_pc.vertex_positions.data(),
               GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glEnableVertexAttribArray(2);

  glVertexAttribPointer(0,         // location 0 in the shader
                        2,         // 3 floats per vert
                        GL_FLOAT,  // floats
                        GL_TRUE,   // normalized, ignored,
                        0,         // tightly packed
                        0

  );

  glGenBuffers(1, &m_ogl.dynamic_vertex_buffer);
  glBindBuffer(GL_ARRAY_BUFFER, m_ogl.dynamic_vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * NUM_VERTS, nullptr, GL_DYNAMIC_DRAW);
  glVertexAttribPointer(1,                             // location 0 in the shader
                        4,                             // 4 color components
                        GL_UNSIGNED_BYTE,              // floats
                        GL_TRUE,                       // normalized, ignored,
                        sizeof(Vertex),                //
                        (void*)offsetof(Vertex, rgba)  // offset in array (why is this a pointer...)
  );
  glVertexAttribPointer(2,                          // location 0 in the shader
                        2,                          // 2 floats per vert
                        GL_FLOAT,                   // floats
                        GL_FALSE,                   // normalized, ignored,
                        sizeof(Vertex),             //
                        (void*)offsetof(Vertex, s)  // offset in array (why is this a pointer...)
  );
}

