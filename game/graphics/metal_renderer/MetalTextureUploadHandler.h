#pragma once

/*!
 * @file MetalTextureUploadHandler.h
 * The texture-upload buckets on Metal.
 *
 * The game tells the renderer which PS2 VRAM page now holds which texture; the actual texture data
 * was preconverted by the loader. So this bucket is bookkeeping, not drawing, and the bookkeeping
 * lives in TexturePool, which both backends share. What is left here is reading the PC_PORT
 * records out of the DMA chain -- the same reading the OpenGL handler does.
 *
 * Eleven buckets use this. Without it every renderer looks textures up in a pool that was never
 * told about the uploads, so uploaded and animated textures stay at whatever was there before.
 */

#include <string>
#include <vector>

#include "common/common_types.h"

#include "game/graphics/metal_renderer/MetalRenderState.h"

#ifdef __OBJC__

class MetalTextureUploadHandler : public MetalBucketRenderer {
 public:
  using MetalBucketRenderer::MetalBucketRenderer;

  void render(DmaFollower& dma, MetalRenderState* render_state) override;

  int upload_count() const { return m_upload_count; }

 private:
  struct TextureUpload {
    u64 page;
    s64 mode;
  };
  void flush_uploads(std::vector<TextureUpload>& uploads, MetalRenderState* render_state);

  int m_upload_count = 0;
};

#endif  // __OBJC__
