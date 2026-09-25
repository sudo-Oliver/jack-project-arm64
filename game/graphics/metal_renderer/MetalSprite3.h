#pragma once

/*!
 * @file MetalSprite3.h
 * The sprite bucket on Metal: the particles, the HUD, the menus and the fake shadows.
 *
 * The DMA walk and the VU emulation are in Sprite3Core, shared with the OpenGL backend. What is
 * here is the vertex upload and the draws.
 *
 * Two things differ from the OpenGL renderer by necessity rather than by choice:
 *
 * - Metal has no primitive restart. The core builds triangle strips of four vertices separated by
 *   a restart index; this expands each of those into the two triangles it means. That is six
 *   indices instead of five, and one draw instead of one, so it is not a regression -- strip
 *   restart on a tile GPU costs more than the extra index.
 *
 * - The vertex and index buffers are bump-allocated out of one per-frame buffer, because the
 *   bucket flushes more than once per frame (the world sprites, then the HUD) and the GPU may
 *   still be reading the earlier flush. kMetalFramesInFlight copies of that buffer are kept, the
 *   same as every other renderer here.
 */

#include <memory>
#include <string>

#include "common/common_types.h"

#include "game/graphics/metal_renderer/MetalDirect.h"
#include "game/graphics/metal_renderer/MetalRenderState.h"
#include "game/graphics/sprite/Sprite3Core.h"

#ifdef __OBJC__

class MetalSprite3 : public MetalBucketRenderer, public Sprite3Core {
 public:
  MetalSprite3(const std::string& name, int my_id);
  ~MetalSprite3() override;

  bool init(MetalRenderState* render_state) override;
  void render(DmaFollower& dma, MetalRenderState* render_state) override;

  u32 last_frame_tris() const { return m_last_frame_tris; }

 protected:
  void set_frame_constants() override;
  void set_hud_constants() override;
  void flush_sprites_gpu(bool double_draw) override;
  void distort_draw_gpu(bool instanced) override;
  bool distort_wants_instancing() const override;
  void distort_instanced_mesh_changed() override;
  void direct_reset_state() override;
  void direct_render_vif(u32 vif0, u32 vif1, const u8* data, u32 size) override;
  void direct_flush() override;
  void add_tri_count(u32 tris) override;

 private:
  bool init_distort(MetalRenderState* render_state);

  struct Impl;
  std::unique_ptr<Impl> m_impl;
  MetalDirect m_direct;
  MetalRenderState* m_current_render_state = nullptr;
  u32 m_last_frame_tris = 0;
};

#endif  // __OBJC__
