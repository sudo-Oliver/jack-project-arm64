#include "Sprite3.h"

#include "common/log/log.h"

#include "game/graphics/opengl_renderer/background/background_common.h"

#include "fmt/format.h"
#include "third-party/imgui/imgui.h"

Sprite3::Sprite3(const std::string& name, int my_id)
    : BucketRenderer(name, my_id), m_direct(name, my_id, 1024) {
  opengl_setup();
}

void Sprite3::opengl_setup() {
  // Set up OpenGL for 'normal' sprites
  opengl_setup_normal();

  // Set up OpenGL for distort sprites
  opengl_setup_distort();
}

void Sprite3::opengl_setup_normal() {
  glGenBuffers(1, &m_ogl.vertex_buffer);
  glGenVertexArrays(1, &m_ogl.vao);
  glBindVertexArray(m_ogl.vao);
  glBindBuffer(GL_ARRAY_BUFFER, m_ogl.vertex_buffer);
  auto verts = MAX_SPRITES * 4;
  auto bytes = verts * sizeof(SpriteVertex3D);
  glBufferData(GL_ARRAY_BUFFER, bytes, nullptr, GL_STREAM_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(
      0,                                       // location 0 in the shader
      4,                                       // 4 floats per vert (w unused)
      GL_FLOAT,                                // floats
      GL_TRUE,                                 // normalized, ignored,
      sizeof(SpriteVertex3D),                  //
      (void*)offsetof(SpriteVertex3D, xyz_sx)  // offset in array (why is this a pointer...)
  );

  glEnableVertexAttribArray(1);
  glVertexAttribPointer(
      1,                                        // location 1 in the shader
      4,                                        // 4 color components
      GL_FLOAT,                                 // floats
      GL_TRUE,                                  // normalized, ignored,
      sizeof(SpriteVertex3D),                   //
      (void*)offsetof(SpriteVertex3D, quat_sy)  // offset in array (why is this a pointer...)
  );

  glEnableVertexAttribArray(2);
  glVertexAttribPointer(
      2,                                     // location 2 in the shader
      4,                                     // 4 color components
      GL_FLOAT,                              // floats
      GL_TRUE,                               // normalized, ignored,
      sizeof(SpriteVertex3D),                //
      (void*)offsetof(SpriteVertex3D, rgba)  // offset in array (why is this a pointer...)
  );

  glEnableVertexAttribArray(3);
  glVertexAttribIPointer(
      3,                                             // location 3 in the shader
      2,                                             // 4 color components
      GL_UNSIGNED_SHORT,                             // floats
      sizeof(SpriteVertex3D),                        //
      (void*)offsetof(SpriteVertex3D, flags_matrix)  // offset in array (why is this a pointer...)
  );

  glEnableVertexAttribArray(4);
  glVertexAttribIPointer(
      4,                                     // location 4 in the shader
      4,                                     // 3 floats per vert
      GL_UNSIGNED_SHORT,                     // floats
      sizeof(SpriteVertex3D),                //
      (void*)offsetof(SpriteVertex3D, info)  // offset in array (why is this a pointer...)
  );
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  u32 idx_buffer_len = MAX_SPRITES * 5;
  glGenBuffers(1, &m_ogl.index_buffer);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ogl.index_buffer);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx_buffer_len * sizeof(u32), nullptr, GL_STREAM_DRAW);

  glBindVertexArray(0);
}

void Sprite3::render(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof) {
  m_current_render_state = render_state;
  m_current_prof = &prof;

  Context context;
  context.version = render_state->version;
  context.next_bucket = render_state->next_bucket;
  context.enabled = m_enabled;
  context.camera_planes = render_state->has_pc_data ? render_state->camera_planes : nullptr;
  render_core(dma, context);

  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glBlendEquation(GL_FUNC_ADD);

  m_current_render_state = nullptr;
  m_current_prof = nullptr;
}

void Sprite3::direct_reset_state() {
  m_direct.reset_state();
}

void Sprite3::direct_render_vif(u32 vif0, u32 vif1, const u8* data, u32 size) {
  m_direct.render_vif(vif0, vif1, data, size, m_current_render_state, *m_current_prof);
}

void Sprite3::direct_flush() {
  m_direct.flush_pending(m_current_render_state, *m_current_prof);
}

void Sprite3::glow_dma_and_draw(DmaFollower& dma) {
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glBlendEquation(GL_FUNC_ADD);
  glow_dma_and_draw_ogl(dma);
}

void Sprite3::add_tri_count(u32 tris) {
  if (m_current_prof) {
    m_current_prof->add_tri(tris);
  }
}

void Sprite3::set_frame_constants() {
  auto* render_state = m_current_render_state;
  render_state->shaders[ShaderId::SPRITE3].activate();
  auto shid = render_state->shaders[ShaderId::SPRITE3].id();
  glUniform4fv(glGetUniformLocation(shid, "hvdf_offset"), 1, m_3d_matrix_data.hvdf_offset.data());
  glUniform1f(glGetUniformLocation(shid, "pfog0"), m_frame_data.pfog0);
  glUniform1f(glGetUniformLocation(shid, "min_scale"), m_frame_data.min_scale);
  glUniform1f(glGetUniformLocation(shid, "max_scale"), m_frame_data.max_scale);
  glUniform1f(glGetUniformLocation(shid, "fog_min"), m_frame_data.fog_min);
  glUniform1f(glGetUniformLocation(shid, "fog_max"), m_frame_data.fog_max);
  // glUniform1f(glGetUniformLocation(shid, "bonus"), m_frame_data.bonus);
  // glUniform4fv(glGetUniformLocation(shid, "hmge_scale"), 1, m_frame_data.hmge_scale.data());
  glUniform1f(glGetUniformLocation(shid, "deg_to_rad"), m_frame_data.deg_to_rad);
  glUniform1f(glGetUniformLocation(shid, "inv_area"), m_frame_data.inv_area);
  glUniformMatrix4fv(glGetUniformLocation(shid, "camera"), 1, GL_FALSE,
                     m_3d_matrix_data.camera.data());
  glUniform4fv(glGetUniformLocation(shid, "xy_array"), 8, m_frame_data.xy_array[0].data());
  glUniform4fv(glGetUniformLocation(shid, "xyz_array"), 4, m_frame_data.xyz_array[0].data());
  glUniform4fv(glGetUniformLocation(shid, "st_array"), 4, m_frame_data.st_array[0].data());
  glUniform4fv(glGetUniformLocation(shid, "basis_x"), 1, m_frame_data.basis_x.data());
  glUniform4fv(glGetUniformLocation(shid, "basis_y"), 1, m_frame_data.basis_y.data());
}

void Sprite3::set_hud_constants() {
  auto* render_state = m_current_render_state;
  render_state->shaders[ShaderId::SPRITE3].activate();
  auto shid = render_state->shaders[ShaderId::SPRITE3].id();
  glUniform4fv(glGetUniformLocation(shid, "hud_hvdf_offset"), 1,
               m_hud_matrix_data.hvdf_offset.data());
  glUniform4fv(glGetUniformLocation(shid, "hud_hvdf_user"), 75,
               m_hud_matrix_data.user_hvdf[0].data());
  glUniformMatrix4fv(glGetUniformLocation(shid, "hud_matrix"), 1, GL_FALSE,
                     m_hud_matrix_data.matrix.data());
}

void Sprite3::draw_debug_window() {
  ImGui::Checkbox("Glow", &m_enable_glow);
  ImGui::Checkbox("new glow", &m_glow_renderer.new_mode);
  ImGui::Separator();
  ImGui::Text("Distort sprites: %d", m_distort_stats.total_sprites);
  ImGui::Text("2D Group 0 (World) blocks: %d sprites: %d", m_debug_stats.blocks_2d_grp0,
              m_debug_stats.count_2d_grp0);
  ImGui::Text("2D Group 1 (HUD) blocks: %d sprites: %d", m_debug_stats.blocks_2d_grp1,
              m_debug_stats.count_2d_grp1);
  ImGui::Checkbox("Culling", &m_enable_culling);
  ImGui::Checkbox("2d", &m_2d_enable);
  ImGui::SameLine();
  ImGui::Checkbox("3d", &m_3d_enable);
  ImGui::Checkbox("Distort", &m_distort_enable);
  ImGui::Checkbox("Distort instancing", &m_enable_distort_instancing);
  ImGui::Separator();
  m_glow_renderer.draw_debug_window();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Render (for real)

void Sprite3::flush_sprites_gpu(bool double_draw) {
  auto* render_state = m_current_render_state;
  auto& prof = *m_current_prof;

  glBindVertexArray(m_ogl.vao);

  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);

  // upload vertex buffer
  glBindBuffer(GL_ARRAY_BUFFER, m_ogl.vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, m_sprite_idx * sizeof(SpriteVertex3D) * 4, m_vertices_3d.data(),
               GL_STREAM_DRAW);

  // the core already packed every bucket's indices into one buffer; upload it
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ogl.index_buffer);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_index_buffer_used * sizeof(u32),
               m_index_buffer_data.data(), GL_STREAM_DRAW);

  // now do draws!
  for (const auto bucket : m_bucket_list) {
    u32 tbp = bucket->tbp();
    DrawMode mode = bucket->mode();

    std::optional<u64> tex;
    tex = render_state->texture_pool->lookup(tbp);

    if (!tex) {
      lg::warn("Failed to find texture at {}, using random (sprite)", tbp);
      tex = render_state->texture_pool->get_placeholder_texture();
    }
    ASSERT(tex);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, *tex);

    auto settings = setup_opengl_from_draw_mode(mode, GL_TEXTURE0, false);

    glUniform1f(glGetUniformLocation(render_state->shaders[ShaderId::SPRITE3].id(), "alpha_min"),
                double_draw ? settings.aref_first : 0.016);
    glUniform1f(glGetUniformLocation(render_state->shaders[ShaderId::SPRITE3].id(), "alpha_max"),
                10.f);
    glUniform1i(glGetUniformLocation(render_state->shaders[ShaderId::SPRITE3].id(), "tex_T0"), 0);

    prof.add_draw_call();
    prof.add_tri(2 * (bucket->ids.size() / 5));

    glDrawElements(GL_TRIANGLE_STRIP, bucket->ids.size(), GL_UNSIGNED_INT,
                   (void*)(bucket->offset_in_idx_buffer * sizeof(u32)));

    if (double_draw) {
      switch (settings.kind) {
        case DoubleDrawKind::NONE:
          break;
        case DoubleDrawKind::AFAIL_NO_DEPTH_WRITE:
          prof.add_draw_call();
          prof.add_tri(2 * (bucket->ids.size() / 5));
          glUniform1f(
              glGetUniformLocation(render_state->shaders[ShaderId::SPRITE3].id(), "alpha_min"),
              -10.f);
          glUniform1f(
              glGetUniformLocation(render_state->shaders[ShaderId::SPRITE3].id(), "alpha_max"),
              settings.aref_second);
          glDepthMask(GL_FALSE);
          glDrawElements(GL_TRIANGLE_STRIP, bucket->ids.size(), GL_UNSIGNED_INT,
                         (void*)(bucket->offset_in_idx_buffer * sizeof(u32)));
          break;
        default:
          ASSERT(false);
      }
    }
  }

  glBindVertexArray(0);
}
