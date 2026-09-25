#pragma once

/*!
 * @file Sprite3.h
 * The OpenGL half of the sprite renderer.
 *
 * Everything that reads the DMA and decides what a sprite looks like is in Sprite3Core, shared
 * with the Metal backend. What is here is the buffers, the uniforms and the draw calls.
 */

#include "common/dma/gs.h"
#include "common/math/Vector.h"
#include "common/versions/versions.h"

#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/DirectRenderer.h"
#include "game/graphics/opengl_renderer/sprite/GlowRenderer.h"
#include "game/graphics/sprite/Sprite3Core.h"
#include "game/graphics/sprite/sprite_common.h"

class Sprite3 : public BucketRenderer, public Sprite3Core {
 public:
  Sprite3(const std::string& name, int my_id);
  void render(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof) override;
  void draw_debug_window() override;

 protected:
  // Sprite3Core's GPU hooks.
  void set_frame_constants() override;
  void set_hud_constants() override;
  void flush_sprites_gpu(bool double_draw) override;
  void distort_draw_gpu(bool instanced) override;
  void distort_instanced_mesh_changed() override;
  void direct_reset_state() override;
  void direct_render_vif(u32 vif0, u32 vif1, const u8* data, u32 size) override;
  void direct_flush() override;
  void glow_dma_and_draw(DmaFollower& dma) override;
  void add_tri_count(u32 tris) override;

 private:
  void opengl_setup();
  void opengl_setup_normal();
  void opengl_setup_distort();

  void glow_dma_and_draw_ogl(DmaFollower& dma);
  void distort_draw(bool instanced);
  void distort_draw_common();
  void distort_setup_framebuffer_dims();

  GlowRenderer m_glow_renderer;
  bool m_enable_glow = true;

  struct {
    GLuint vao;
    GLuint vertex_buffer;
    GLuint index_buffer;
    GLuint fbo;
    GLuint fbo_texture;
    int fbo_width = 640;
    int fbo_height = 480;
  } m_distort_ogl;

  struct {
    GLuint vao;
    GLuint vertex_buffer;    // contains vertex data for each possible sprite resolution (3-11)
    GLuint instance_buffer;  // contains all instance specific data for each sprite per frame
    bool vertex_data_changed = false;
  } m_distort_instanced_ogl;

  struct {
    GLuint vertex_buffer;
    GLuint vao;
    GLuint index_buffer;
  } m_ogl;

  DirectRenderer m_direct;

  // The frame's render state and profiler node, for the hooks -- the core does not carry either.
  SharedRenderState* m_current_render_state = nullptr;
  ScopedProfilerNode* m_current_prof = nullptr;
};
