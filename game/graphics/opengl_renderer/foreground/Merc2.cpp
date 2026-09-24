/*!
 * @file Merc2.cpp
 * See Merc2.h.
 */

#include "Merc2.h"

#include "common/global_profiler/GlobalProfiler.h"
#include "common/util/Assert.h"

#include "game/graphics/opengl_renderer/EyeRenderer.h"
#include "game/graphics/opengl_renderer/background/background_common.h"

#include "fmt/format.h"

namespace {
void set_uniform(GLuint uniform, const math::Vector3f& val) {
  glUniform3f(uniform, val.x(), val.y(), val.z());
}
void set_uniform(GLuint uniform, const math::Vector4f& val) {
  glUniform4f(uniform, val.x(), val.y(), val.z(), val.w());
}
}  // namespace


Merc2::Merc2(ShaderLibrary& shaders, const std::vector<GLuint>* anim_slot_array)
    : m_anim_slot_array(anim_slot_array) {
  // Main vertex array. It points at the level data the Loader uploaded from the .fr3.
  glGenVertexArrays(1, &m_vao);
  glBindVertexArray(m_vao);

  // Bone buffer: skinning matrices for many draws at once.
  glGenBuffers(1, &m_bones_buffer);
  glBindBuffer(GL_UNIFORM_BUFFER, m_bones_buffer);
  std::vector<u8> temp(MAX_SHADER_BONE_VECTORS * sizeof(math::Vector4f));
  glBufferData(GL_UNIFORM_BUFFER, MAX_SHADER_BONE_VECTORS * sizeof(math::Vector4f), temp.data(),
               GL_DYNAMIC_DRAW);
  glBindBuffer(GL_UNIFORM_BUFFER, 0);

  // glBindBufferRange has an alignment restriction that varies per platform, and the bone buffer
  // is addressed in 16-byte quadwords, so tell the core what a draw's first bone must divide by.
  GLint val;
  glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &val);
  if (val <= 16) {
    m_bone_buffer_alignment = 1;
  } else {
    m_bone_buffer_alignment = val / 16;
    if (m_bone_buffer_alignment * 16 != (u32)val) {
      ASSERT_MSG(false,
                 fmt::format("opengl uniform buffer alignment is {}, which is strange\n", val));
    }
  }

  init_shader_common(shaders[ShaderId::MERC2], &m_merc_uniforms, true);
  init_shader_common(shaders[ShaderId::EMERC], &m_emerc_uniforms, false);
  m_emerc_uniforms.fade = glGetUniformLocation(shaders[ShaderId::EMERC].id(), "fade");
}

Merc2::~Merc2() {
  for (auto& x : m_mod_vtx_buffers) {
    glDeleteBuffers(1, &x.vertex);
    glDeleteVertexArrays(1, &x.vao);
  }

  glDeleteBuffers(1, &m_bones_buffer);
  glDeleteVertexArrays(1, &m_vao);
}
void Merc2::init_shader_common(Shader& shader, Uniforms* uniforms, bool include_lights) {
  auto id = shader.id();
  shader.activate();
  if (include_lights) {
    uniforms->light_direction[0] = glGetUniformLocation(id, "light_dir0_fade");
    uniforms->light_direction[1] = glGetUniformLocation(id, "light_dir1_fade_en");
    uniforms->light_direction[2] = glGetUniformLocation(id, "light_dir2");
    uniforms->light_color[0] = glGetUniformLocation(id, "light_col0");
    uniforms->light_color[1] = glGetUniformLocation(id, "light_col1");
    uniforms->light_color[2] = glGetUniformLocation(id, "light_col2");
    uniforms->light_ambient = glGetUniformLocation(id, "light_ambient");
  }

  uniforms->hvdf_offset = glGetUniformLocation(id, "hvdf_offset");

  uniforms->fog = glGetUniformLocation(id, "fog_constants");
  uniforms->decal = glGetUniformLocation(id, "decal_enable");

  uniforms->fog_color = glGetUniformLocation(id, "fog_color");
  uniforms->perspective_matrix = glGetUniformLocation(id, "perspective_matrix");
  uniforms->ignore_alpha = glGetUniformLocation(id, "ignore_alpha");

  uniforms->gfx_hack_no_tex = glGetUniformLocation(id, "gfx_hack_no_tex");
}
void Merc2::switch_to_merc2() {
  m_current_render_state->shaders[ShaderId::MERC2].activate();

  // set uniforms that we know from render_state
  glUniform4f(m_merc_uniforms.fog_color, m_current_render_state->fog_color[0] / 255.f,
              m_current_render_state->fog_color[1] / 255.f, m_current_render_state->fog_color[2] / 255.f,
              m_current_render_state->fog_intensity / 255);
  glUniform1i(m_merc_uniforms.gfx_hack_no_tex, Gfx::g_global_settings.hack_no_tex);
}
void Merc2::switch_to_emerc() {
  m_current_render_state->shaders[ShaderId::EMERC].activate();
  // set uniforms that we know from render_state
  glUniform4f(m_emerc_uniforms.fog_color, m_current_render_state->fog_color[0] / 255.f,
              m_current_render_state->fog_color[1] / 255.f, m_current_render_state->fog_color[2] / 255.f,
              m_current_render_state->fog_intensity / 255);
  glUniform1i(m_emerc_uniforms.gfx_hack_no_tex, Gfx::g_global_settings.hack_no_tex);
}

/*!
 * Main merc2 rendering. The core does the reading and the deciding; the hooks below do the GPU.
 */
void Merc2::render(DmaFollower& dma,
                   SharedRenderState* render_state,
                   ScopedProfilerNode& prof,
                   MercDebugStats* stats) {
  m_current_render_state = render_state;
  m_current_prof = &prof;

  Context context;
  context.loader = render_state->loader.get();
  context.version = render_state->version;
  context.next_bucket = render_state->next_bucket;
  render_core(dma, context, stats);

  m_current_render_state = nullptr;
  m_current_prof = nullptr;
}

void Merc2::backend_set_low_memory(const LowMemory& low_memory) {
  switch_to_merc2();
  set_uniform(m_merc_uniforms.hvdf_offset, low_memory.hvdf_offset);
  set_uniform(m_merc_uniforms.fog, low_memory.fog);
  glUniformMatrix4fv(m_merc_uniforms.perspective_matrix, 1, GL_FALSE,
                     &low_memory.perspective[0].x());
  switch_to_emerc();
  set_uniform(m_emerc_uniforms.hvdf_offset, low_memory.hvdf_offset);
  set_uniform(m_emerc_uniforms.fog, low_memory.fog);
  glUniformMatrix4fv(m_emerc_uniforms.perspective_matrix, 1, GL_FALSE,
                     &low_memory.perspective[0].x());
}

void Merc2::backend_upload_mod_vtx(u32 buffer,
                                   const tfrag3::MercVertex* data,
                                   size_t num_vertices) {
  glBindBuffer(GL_ARRAY_BUFFER, m_mod_vtx_buffers.at(buffer).vertex);
  glBufferData(GL_ARRAY_BUFFER, num_vertices * sizeof(tfrag3::MercVertex), data, GL_DYNAMIC_DRAW);
}

void Merc2::backend_upload_bones(const math::Vector4f* data, u32 num_vectors) {
  glBindBuffer(GL_UNIFORM_BUFFER, m_bones_buffer);
  glBufferSubData(GL_UNIFORM_BUFFER, 0, num_vectors * sizeof(math::Vector4f), data);
  glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

void Merc2::backend_do_draws(const Draw* draws,
                             const LevelData* level,
                             u32 num_draws,
                             bool envmap,
                             bool set_fade,
                             MercDebugStats* stats) {
  glBindVertexArray(m_vao);
  glBindBuffer(GL_ARRAY_BUFFER, level->merc_vertices);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, level->merc_indices);
  setup_merc_vao();

  if (envmap) {
    switch_to_emerc();
    do_draws(draws, level, num_draws, m_emerc_uniforms, set_fade, stats);
  } else {
    switch_to_merc2();
    do_draws(draws, level, num_draws, m_merc_uniforms, set_fade, stats);
  }
}

void Merc2::backend_ensure_mod_vtx_buffer(u32 index, const LevelData* lev) {
  // The core numbers these from zero and restarts at each flush, so an index that already exists
  // is simply reused.
  while (index >= m_mod_vtx_buffers.size()) {
    GLuint b;
    glGenBuffers(1, &b);
    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, b);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, lev->merc_indices);
    setup_merc_vao();
    m_mod_vtx_buffers.push_back({vao, b});
  }
}

void Merc2::setup_merc_vao() {
  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glEnableVertexAttribArray(2);
  glEnableVertexAttribArray(3);
  glEnableVertexAttribArray(4);
  glEnableVertexAttribArray(5);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_GEQUAL);

  glVertexAttribPointer(0,                                        // location 0 in the shader
                        3,                                        // 3 values per vert
                        GL_FLOAT,                                 // floats
                        GL_FALSE,                                 // normalized
                        sizeof(tfrag3::MercVertex),               // stride
                        (void*)offsetof(tfrag3::MercVertex, pos)  // offset (0)
  );

  glVertexAttribPointer(1,                                              // location 1 in the
                        3,                                              // 3 values per vert
                        GL_FLOAT,                                       // floats
                        GL_FALSE,                                       // normalized
                        sizeof(tfrag3::MercVertex),                     // stride
                        (void*)offsetof(tfrag3::MercVertex, normal[0])  // offset (0)
  );

  glVertexAttribPointer(2,                                               // location 1 in the
                        3,                                               // 3 values per vert
                        GL_FLOAT,                                        // floats
                        GL_FALSE,                                        // normalized
                        sizeof(tfrag3::MercVertex),                      // stride
                        (void*)offsetof(tfrag3::MercVertex, weights[0])  // offset (0)
  );

  glVertexAttribPointer(3,                                          // location 1 in the shader
                        2,                                          // 3 values per vert
                        GL_FLOAT,                                   // floats
                        GL_FALSE,                                   // normalized
                        sizeof(tfrag3::MercVertex),                 // stride
                        (void*)offsetof(tfrag3::MercVertex, st[0])  // offset (0)
  );

  glVertexAttribPointer(4,                                            // location 1 in the shader
                        4,                                            // 3 values per vert
                        GL_UNSIGNED_BYTE,                             // floats
                        GL_TRUE,                                      // normalized
                        sizeof(tfrag3::MercVertex),                   // stride
                        (void*)offsetof(tfrag3::MercVertex, rgba[0])  // offset (0)
  );

  glVertexAttribIPointer(5,                                            // location 0 in the
                         4,                                            // 3 floats per vert
                         GL_UNSIGNED_BYTE,                             // u8's
                         sizeof(tfrag3::MercVertex),                   //
                         (void*)offsetof(tfrag3::MercVertex, mats[0])  // offset in array
  );
}
void Merc2::do_draws(const Draw* draw_array,
                     const LevelData* lev,
                     u32 num_draws,
                     const Uniforms& uniforms,
                     bool set_fade,
                     MercDebugStats* stats) {
  SharedRenderState* render_state = m_current_render_state;
  ScopedProfilerNode& prof = *m_current_prof;
  (void)stats;
  glBindVertexArray(m_vao);
  s32 last_tex = INT32_MIN;
  int last_light = -1;
  bool normal_vtx_buffer_bound = true;

  bool fog_on = true;

  for (u32 di = 0; di < num_draws; di++) {
    auto& draw = draw_array[di];
    if (draw.flags & MOD_VTX) {
      glBindVertexArray(m_mod_vtx_buffers.at(draw.mod_vtx_buffer).vao);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, lev->merc_indices);
      glBindBuffer(GL_ARRAY_BUFFER, lev->merc_vertices);
      normal_vtx_buffer_bound = false;
    } else {
      if (!normal_vtx_buffer_bound) {
        glBindVertexArray(m_vao);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, lev->merc_indices);
        glBindBuffer(GL_ARRAY_BUFFER, lev->merc_vertices);
        normal_vtx_buffer_bound = true;
      }
    }
    glUniform1i(uniforms.ignore_alpha, draw.flags & DrawFlags::IGNORE_ALPHA);

    if (fog_on && !draw.mode.get_fog_enable()) {
      // on -> off
      glUniform4f(uniforms.fog_color, render_state->fog_color[0] / 255.f,
                  render_state->fog_color[1] / 255.f, render_state->fog_color[2] / 255.f, 0);
      fog_on = false;
    } else if (!fog_on && draw.mode.get_fog_enable()) {
      glUniform4f(uniforms.fog_color, render_state->fog_color[0] / 255.f,
                  render_state->fog_color[1] / 255.f, render_state->fog_color[2] / 255.f,
                  render_state->fog_intensity / 255);
      fog_on = true;
    }
    bool use_mipmaps_for_filtering = true;
    if (draw.texture != last_tex) {
      if (draw.texture < (int)lev->textures.size() && draw.texture >= 0) {
        glBindTexture(GL_TEXTURE_2D, lev->textures.at(draw.texture));
      } else if ((draw.texture & 0xffffff00) == 0xefffff00) {
        if (render_state->version == GameVersion::Jak3 ||
            render_state->version == GameVersion::JakX) {
          auto maybe_eye =
              render_state->eye_renderer->lookup_eye_texture_hash(draw.hash, (draw.texture & 1));
          if (maybe_eye) {
            glBindTexture(GL_TEXTURE_2D, *maybe_eye);
          }
        } else {
          auto maybe_eye = render_state->eye_renderer->lookup_eye_texture(draw.texture & 0xff);
          if (maybe_eye) {
            glBindTexture(GL_TEXTURE_2D, *maybe_eye);
          }
        }

        use_mipmaps_for_filtering = false;
      } else if (draw.texture < 0) {
        int slot = -(draw.texture + 1);
        glBindTexture(GL_TEXTURE_2D, m_anim_slot_array->at(slot));
      } else {
        fmt::print("Invalid draw.texture is {}, would have crashed.\n", draw.texture);
      }
      last_tex = draw.texture;
    }

    if ((int)draw.light_idx != last_light && !set_fade) {
      const auto& l0_dir = m_lights_buffer[draw.light_idx].direction0;
      const auto& l1_dir = m_lights_buffer[draw.light_idx].direction1;
      float fade = 1.f;
      float fade_enable = 0.f;
      if (m_lights_buffer[draw.light_idx].w1) {
        fade = m_lights_buffer[draw.light_idx].w2 / 128.f;
        fade_enable = 1.f;
      }
      math::Vector4f l0_dir_f(l0_dir.x(), l0_dir.y(), l0_dir.z(), fade);
      set_uniform(uniforms.light_direction[0], l0_dir_f);
      math::Vector4f l1_dir_f(l1_dir.x(), l1_dir.y(), l1_dir.z(), fade_enable);
      set_uniform(uniforms.light_direction[1], l1_dir_f);
      set_uniform(uniforms.light_direction[2], m_lights_buffer[draw.light_idx].direction2);
      set_uniform(uniforms.light_color[0], m_lights_buffer[draw.light_idx].color0);
      set_uniform(uniforms.light_color[1], m_lights_buffer[draw.light_idx].color1);
      set_uniform(uniforms.light_color[2], m_lights_buffer[draw.light_idx].color2);
      set_uniform(uniforms.light_ambient, m_lights_buffer[draw.light_idx].ambient);
      last_light = draw.light_idx;
    }

    glUniform1i(uniforms.decal, draw.mode.get_decal());
    glUniform1i(uniforms.gfx_hack_no_tex, (draw.flags & NO_TEXTURE) != 0);

    if (set_fade) {
      math::Vector4f fade =
          math::Vector4f(draw.fade[0], draw.fade[1], draw.fade[2], draw.fade[3]) / 255.f;
      set_uniform(uniforms.fade, fade);
      ASSERT(draw.mode.get_alpha_blend() == DrawMode::AlphaBlend::SRC_0_DST_DST);
    }

    if (m_lights_buffer[draw.light_idx].w1 && !set_fade) {
      DrawMode mode = draw.mode;
      mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_SRC_DST);
      mode.set_ab(true);
      setup_opengl_from_draw_mode(mode, GL_TEXTURE0, use_mipmaps_for_filtering);

      prof.add_draw_call(2);
      prof.add_tri(draw.num_triangles * 2);
      glBindBufferRange(GL_UNIFORM_BUFFER, 1, m_bones_buffer,
                        sizeof(math::Vector4f) * draw.first_bone, 128 * sizeof(ShaderMercMat));
      // draw rgb
      const auto& l1_dir = m_lights_buffer[draw.light_idx].direction1;
      math::Vector4f l1_dir_f(l1_dir.x(), l1_dir.y(), l1_dir.z(), 1);
      set_uniform(uniforms.light_direction[1], l1_dir_f);
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
      glDrawElements(draw.no_strip ? GL_TRIANGLES : GL_TRIANGLE_STRIP, draw.index_count,
                     GL_UNSIGNED_INT, (void*)(sizeof(u32) * draw.first_index));
      // draw a
      setup_opengl_from_draw_mode(draw.mode, GL_TEXTURE0, use_mipmaps_for_filtering);
      math::Vector4f l1_dir_f_off(l1_dir.x(), l1_dir.y(), l1_dir.z(), -1);
      set_uniform(uniforms.light_direction[1], l1_dir_f_off);
      glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
      glDrawElements(draw.no_strip ? GL_TRIANGLES : GL_TRIANGLE_STRIP, draw.index_count,
                     GL_UNSIGNED_INT, (void*)(sizeof(u32) * draw.first_index));
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    } else {
      setup_opengl_from_draw_mode(draw.mode, GL_TEXTURE0, use_mipmaps_for_filtering);
      prof.add_draw_call();
      prof.add_tri(draw.num_triangles);
      glBindBufferRange(GL_UNIFORM_BUFFER, 1, m_bones_buffer,
                        sizeof(math::Vector4f) * draw.first_bone, 128 * sizeof(ShaderMercMat));
      glDrawElements(draw.no_strip ? GL_TRIANGLES : GL_TRIANGLE_STRIP, draw.index_count,
                     GL_UNSIGNED_INT, (void*)(sizeof(u32) * draw.first_index));
    }
  }

  if (!normal_vtx_buffer_bound) {
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, lev->merc_indices);
    glBindBuffer(GL_ARRAY_BUFFER, lev->merc_vertices);
  }
}
