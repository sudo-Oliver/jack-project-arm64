#pragma once

/*!
 * @file MetalRenderer.h
 * The bucket table, and the frame loop that walks it.
 *
 * The OpenGL backend is a table of 70 buckets built by init_bucket_renderer calls; that table is
 * the checklist for this port. The same table lives here, with a MetalEmptyBucketRenderer standing
 * in for every bucket not ported yet -- an entry that skips its data, so the buckets after it
 * still line up.
 *
 * Adding a renderer is therefore a local change: write the class, swap one table entry.
 */

#include <memory>
#include <vector>

#include "common/dma/dma_chain_read.h"

#include "game/graphics/metal_renderer/MetalRenderState.h"

#ifdef __OBJC__

class MetalRenderer {
 public:
  MetalRenderer(std::shared_ptr<TexturePool> texture_pool, std::shared_ptr<Loader> loader);
  ~MetalRenderer();

  // Builds the bucket table and every renderer's pipeline state. Safe to call once the shader
  // library exists.
  bool init(id<MTLDevice> device,
            id<MTLLibrary> library,
            MTLPixelFormat color_format,
            MTLPixelFormat depth_format);

  // Walks one frame's DMA chain and draws it into `encoder`.
  void render(DmaFollower dma, id<MTLRenderCommandEncoder> encoder);

  // Triangles drawn on the last frame, summed over every bucket that counts them.
  u32 last_frame_tris() const { return m_last_frame_tris; }

 private:
  void init_bucket_table();
  // Pulls the camera and the occlusion strings out of the chain before the buckets run. The
  // renderers that need them are not the ones the game sends them in.
  void scan_frame_state(DmaFollower dma);

  MetalRenderState m_render_state;
  std::vector<std::unique_ptr<MetalBucketRenderer>> m_bucket_renderers;
  bool m_ready = false;
  u32 m_last_frame_tris = 0;
};

#endif  // __OBJC__
