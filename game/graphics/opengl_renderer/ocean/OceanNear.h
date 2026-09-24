#pragma once

/*!
 * @file OceanNear.h
 * The near ocean bucket on OpenGL.
 *
 * The VU1 emulation is in OceanNearCore, shared with the Metal backend. This owns the two
 * renderers it draws through and hands it the frame's texture pool.
 */

#include "game/graphics/ocean/OceanNearCore.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/ocean/CommonOceanRenderer.h"
#include "game/graphics/opengl_renderer/ocean/OceanTexture.h"

class OceanNear : public BucketRenderer {
 public:
  OceanNear(const std::string& name, int my_id);
  void render(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof) override;
  void draw_debug_window() override;
  void init_textures(TexturePool& pool, GameVersion version) override;

 private:
  OceanTexture m_texture_renderer;
  CommonOceanRenderer m_common_ocean_renderer;
  OceanNearCore m_core;
};
