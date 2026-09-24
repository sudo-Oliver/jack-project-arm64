#pragma once

/*!
 * @file OceanMidAndFar.h
 * The first ocean bucket on OpenGL. It runs three renderers: the ocean texture, ocean-far (which
 * is a handful of direct draws) and ocean-mid.
 *
 * The VU1 emulation behind ocean-mid is in OceanMidCore, shared with the Metal backend.
 */

#include "game/graphics/ocean/OceanMidCore.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/DirectRenderer.h"
#include "game/graphics/opengl_renderer/ocean/CommonOceanRenderer.h"
#include "game/graphics/opengl_renderer/ocean/OceanTexture.h"

class OceanMidAndFar : public BucketRenderer {
 public:
  OceanMidAndFar(const std::string& name, int my_id);
  void render(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof) override;
  void render_jak1(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof);
  void render_jak2(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof);
  void draw_debug_window() override;
  void init_textures(TexturePool& pool, GameVersion version) override;

 private:
  void handle_ocean_far(DmaFollower& dma,
                        SharedRenderState* render_state,
                        ScopedProfilerNode& prof);
  void handle_ocean_mid(DmaFollower& dma,
                        SharedRenderState* render_state,
                        ScopedProfilerNode& prof);

  DirectRenderer m_direct;
  OceanTexture m_texture_renderer;
  CommonOceanRenderer m_common_ocean_renderer;
  OceanMidCore m_mid_renderer;
};
