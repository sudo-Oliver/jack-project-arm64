/*!
 * @file OceanNear.cpp
 * See OceanNear.h.
 */

#include "OceanNear.h"

OceanNear::OceanNear(const std::string& name, int my_id)
    : BucketRenderer(name, my_id),
      m_texture_renderer(false),
      m_core(m_texture_renderer, m_common_ocean_renderer) {}

void OceanNear::draw_debug_window() {}

void OceanNear::init_textures(TexturePool& pool, GameVersion version) {
  m_texture_renderer.init_textures(pool, version);
}

void OceanNear::render(DmaFollower& dma,
                       SharedRenderState* render_state,
                       ScopedProfilerNode& prof) {
  if (!m_enabled) {
    while (dma.current_tag_offset() != render_state->next_bucket) {
      dma.read_and_advance();
    }
    return;
  }

  // The two renderers below need the render state for the length of the core's call, because the
  // core does not carry one.
  m_texture_renderer.set_frame_context(render_state, &prof);
  m_common_ocean_renderer.set_frame_context(render_state, &prof);

  switch (render_state->version) {
    case GameVersion::Jak1:
      m_core.render_jak1(dma, render_state->texture_pool.get(), render_state->next_bucket);
      break;
    case GameVersion::Jak2:
    case GameVersion::Jak3:
    case GameVersion::JakX:
      m_core.render_jak2(dma, render_state->texture_pool.get(), render_state->next_bucket);
      break;
  }

  m_texture_renderer.set_frame_context(nullptr, nullptr);
  m_common_ocean_renderer.set_frame_context(nullptr, nullptr);
}
