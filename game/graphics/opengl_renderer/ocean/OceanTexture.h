#pragma once

/*!
 * @file OceanTexture.h
 * The OpenGL half of the ocean's texture.
 *
 * The VU1 emulation that builds the water's grid is in OceanTextureCore, shared with the Metal
 * backend. What is here is the render targets it draws into, the draw, and the mipmap chain.
 */

#include "game/graphics/ocean/OceanTextureCore.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/opengl_utils.h"

class OceanTexture : public OceanTextureCore {
 public:
  OceanTexture(bool generate_mipmaps);
  ~OceanTexture() override;

  void handle_ocean_texture_jak1(DmaFollower& dma,
                                 SharedRenderState* render_state,
                                 ScopedProfilerNode& prof);
  void handle_ocean_texture_jak2(DmaFollower& dma,
                                 SharedRenderState* render_state,
                                 ScopedProfilerNode& prof);
  void init_textures(TexturePool& pool, GameVersion version);
  void draw_debug_window();

  // Held for the length of a call into the core, which does not carry a render state.
  void set_frame_context(SharedRenderState* render_state, ScopedProfilerNode* prof) {
    m_current_render_state = render_state;
    m_current_prof = prof;
  }

 protected:
  void backend_begin_pass(bool to_temp) override;
  void backend_end_pass() override;
  void backend_flush() override;
  void backend_make_texture_with_mipmaps() override;

 private:
  void init_gl();

  SharedRenderState* m_current_render_state = nullptr;
  ScopedProfilerNode* m_current_prof = nullptr;

  FramebufferTexturePair m_result_texture;
  FramebufferTexturePair m_temp_texture;
  // Only alive for the length of one texture pass.
  std::optional<FramebufferTexturePairContext> m_pass_ctxt;

  struct {
    GLuint vao, static_vertex_buffer, dynamic_vertex_buffer, gl_index_buffer;
  } m_ogl;

  struct MipMap {
    GLuint vao, vtx_buffer;
    struct Vertex {
      float x, y;
      float s, t;
    };
    static_assert(sizeof(Vertex) == 16);
  } m_mipmap;
};
