#include "Generic2BucketRenderer.h"

#include "fmt/format.h"

Generic2BucketRenderer::Generic2BucketRenderer(const std::string& name,
                                               int id,
                                               std::shared_ptr<Generic2> renderer,
                                               Generic2::Mode mode)
    : BucketRenderer(name, id), m_generic(renderer), m_mode(mode) {}

void Generic2BucketRenderer::draw_debug_window() {
  m_generic->draw_debug_window();
}

void Generic2BucketRenderer::render(DmaFollower& dma,
                                    SharedRenderState* render_state,
                                    ScopedProfilerNode& prof) {
  auto mode_name = [](Generic2::Mode mode) {
    switch (mode) {
      case Generic2::Mode::NORMAL:
        return "normal";
      case Generic2::Mode::LIGHTNING:
        return "lightning";
      case Generic2::Mode::WARP:
        return "warp";
      case Generic2::Mode::PRIM:
        return "prim";
      default:
        return "unknown";
    }
  };

  static u32 s_generic2_bucket_calls = 0;
  s_generic2_bucket_calls++;
  const u32 start_offset = dma.current_tag_offset();

  // if the user has asked to disable the renderer, just advance the dma follower to the next
  // bucket and return immediately.
  if (!m_enabled) {
    while (dma.current_tag_offset() != render_state->next_bucket) {
      dma.read_and_advance();
    }
    return;
  }
  m_generic->render_in_mode(dma, render_state, prof, m_mode);
  m_empty = m_generic->empty();
  if (!m_empty || s_generic2_bucket_calls <= 160 || (s_generic2_bucket_calls % 1000) == 0) {
    fmt::print("[Generic2Bucket] call={} name={} id={} mode={} start=0x{:x} next=0x{:x} "
               "end=0x{:x} empty={}\n",
               s_generic2_bucket_calls, m_name, m_my_id, mode_name(m_mode), start_offset,
               render_state->next_bucket, dma.current_tag_offset(), m_empty);
  }
}

bool Generic2BucketRenderer::empty() const {
  return m_empty;
}
