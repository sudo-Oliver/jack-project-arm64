#pragma once

/*!
 * @file MetalMerc2.h
 * The Metal half of the merc2 renderer: characters, collectables, and some water.
 *
 * Everything that reads the DMA and decides what to draw is in Merc2Core, shared with the OpenGL
 * backend. What is here is the five methods the core leaves to a backend.
 *
 * Eight buckets use it. Merc2Core::render_core is called once per bucket, and the draws are
 * flushed at the end of each, so the levels' draw buckets never outlive the bucket that filled
 * them.
 */

#include <memory>
#include <string>

#include "common/common_types.h"

#include "game/graphics/foreground/Merc2Core.h"
#include "game/graphics/metal_renderer/MetalRenderState.h"

#ifdef __OBJC__

class MetalMerc2 : public Merc2Core {
 public:
  MetalMerc2();
  ~MetalMerc2() override;

  bool init(MetalRenderState* render_state);
  void render(DmaFollower& dma, MetalRenderState* render_state, MercDebugStats* stats);

  u32 last_frame_tris() const { return m_last_frame_tris; }
  void reset_tri_count() { m_last_frame_tris = 0; }

 protected:
  void backend_set_low_memory(const LowMemory& low_memory) override;
  void backend_ensure_mod_vtx_buffer(u32 index, const LevelData* level) override;
  void backend_upload_mod_vtx(u32 buffer,
                              const tfrag3::MercVertex* data,
                              size_t num_vertices) override;
  void backend_upload_bones(const math::Vector4f* data, u32 num_vectors) override;
  void backend_flush_finished() override;
  void backend_do_draws(const Draw* draws,
                        const LevelData* level,
                        u32 num_draws,
                        bool envmap,
                        bool set_fade,
                        MercDebugStats* stats) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
  MetalRenderState* m_current_render_state = nullptr;
  u32 m_last_frame_tris = 0;
};

/*!
 * One merc bucket.
 */
class MetalMerc2BucketRenderer : public MetalBucketRenderer {
 public:
  MetalMerc2BucketRenderer(const std::string& name,
                           int my_id,
                           std::shared_ptr<MetalMerc2> merc);

  bool init(MetalRenderState* render_state) override;
  void render(DmaFollower& dma, MetalRenderState* render_state) override;

 private:
  std::shared_ptr<MetalMerc2> m_merc;
  MercDebugStats m_stats;
};

#endif  // __OBJC__
