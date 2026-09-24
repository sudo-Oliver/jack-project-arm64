#pragma once

/*!
 * @file EyeRenderer.h
 * The OpenGL half of the eye renderer.
 *
 * Working out which quads an eye is made of is in EyeRendererCore, shared with the Metal backend.
 * What is here is the forty render targets and the draws into them.
 */

#include <string>

#include "game/graphics/eye/EyeRendererCore.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/opengl_utils.h"
#include "game/graphics/pipelines/opengl.h"

class EyeRenderer : public BucketRenderer, public EyeRendererCore {
 public:
  EyeRenderer(const std::string& name, int id);
  ~EyeRenderer();
  void render(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof) override;
  void draw_debug_window() override;
  void init_textures(TexturePool& texture_pool, GameVersion) override;

  void handle_eye_dma2(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof);

  using EyeRendererCore::lookup_eye_texture;
  using EyeRendererCore::lookup_eye_texture_hash;

 protected:
  u64 backend_create_eye_texture(int slot) override;
  void backend_run_gpu(const std::vector<SingleEyeDraws>& draws) override;

 private:
  SharedRenderState* m_current_render_state = nullptr;

  // note: eye texture increased to 128x128 (originally 32x32) here.
  std::vector<FramebufferTexturePair> m_fbs;
  GLuint m_vao = 0;
  GLuint m_gl_vertex_buffer = 0;
  bool m_gl_ready = false;
};
