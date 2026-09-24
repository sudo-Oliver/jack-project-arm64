#pragma once

/*!
 * @file Merc2.h
 * The OpenGL half of the merc2 renderer.
 *
 * Everything that reads the DMA and decides what to draw is in Merc2Core, shared with the Metal
 * backend. What is here is the five methods the core leaves to a backend, plus the shader
 * uniforms, the vertex array and the bone uniform buffer they need.
 */

#include "game/graphics/foreground/Merc2Core.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"

class Merc2 : public Merc2Core {
 public:
  Merc2(ShaderLibrary& shaders, const std::vector<GLuint>* anim_slot_array);
  ~Merc2() override;

  void render(DmaFollower& dma,
              SharedRenderState* render_state,
              ScopedProfilerNode& prof,
              MercDebugStats* stats);

 protected:
  void backend_set_low_memory(const LowMemory& low_memory) override;
  void backend_ensure_mod_vtx_buffer(u32 index, const LevelData* level) override;
  void backend_upload_mod_vtx(u32 buffer,
                              const tfrag3::MercVertex* data,
                              size_t num_vertices) override;
  void backend_upload_bones(const math::Vector4f* data, u32 num_vectors) override;
  void backend_do_draws(const Draw* draws,
                        const LevelData* level,
                        u32 num_draws,
                        bool envmap,
                        bool set_fade,
                        MercDebugStats* stats) override;

 private:
  struct Uniforms {
    GLuint light_direction[3];
    GLuint light_color[3];
    GLuint light_ambient;

    GLuint hvdf_offset;
    GLuint fog;

    GLuint tbone;
    GLuint nbone;

    GLuint fog_color;
    GLuint perspective_matrix;

    GLuint ignore_alpha;
    GLuint decal;

    GLuint gfx_hack_no_tex;

    GLuint fade;
  };

  struct ModBuffers {
    GLuint vao, vertex;
  };

  void init_shader_common(Shader& shader, Uniforms* uniforms, bool include_lights);
  void switch_to_merc2();
  void switch_to_emerc();
  void setup_merc_vao();
  void do_draws(const Draw* draw_array,
                const LevelData* lev,
                u32 num_draws,
                const Uniforms& uniforms,
                bool set_fade,
                MercDebugStats* stats);

  const std::vector<GLuint>* m_anim_slot_array;
  Uniforms m_merc_uniforms, m_emerc_uniforms;
  GLuint m_vao;
  GLuint m_bones_buffer;
  std::vector<ModBuffers> m_mod_vtx_buffers;

  // Only set for the duration of a render() call: the core's hooks are called from inside it and
  // need the same render state and profiler node.
  SharedRenderState* m_current_render_state = nullptr;
  ScopedProfilerNode* m_current_prof = nullptr;
};
