#pragma once

/*!
 * @file MetalRenderState.h
 * What every Metal bucket renderer is handed, and the base class they all derive from.
 *
 * This mirrors SharedRenderState and BucketRenderer in the OpenGL backend on purpose: the bucket
 * table is the port's checklist, and a renderer that has the same shape as its OpenGL sibling is
 * a renderer whose differences are visible in a diff.
 *
 * Anything that decides how a pixel looks belongs in shared, backend-neutral code (see "How this
 * port is kept honest" in CODEX.md). What lives here is only what Metal needs and OpenGL does not:
 * the device, the command encoder, and the translation from GS draw modes to Metal state objects.
 */

#include <memory>
#include <string>
#include <unordered_map>

#include "common/dma/dma_chain_read.h"
#include "common/goal_constants.h"
#include "common/dma/gs.h"

#include "game/graphics/opengl_renderer/background/background_common.h"
#include "game/graphics/opengl_renderer/loader/Loader.h"
#include "game/graphics/texture/TexturePool.h"

#ifdef __OBJC__
#import <Metal/Metal.h>
#endif

#ifdef __OBJC__

// How many frames the CPU may be ahead of the GPU. Every renderer that writes GPU-visible memory
// per frame keeps this many copies, and the display holds the CPU back to match (see the
// semaphore in metal.mm). Both halves have to agree, or a buffer gets rewritten while the GPU is
// still reading it -- which looks like a flicker, not like an error.
constexpr int kMetalFramesInFlight = 3;

/*!
 * Blend, depth and sampler state for a GS DrawMode.
 *
 * OpenGL sets these with calls between draws; Metal bakes blend into the pipeline state, depth
 * test and write into a depth-stencil state, and clamp and filter into a sampler. So each distinct
 * mode needs its own objects. They are built once and cached by the mode's integer value.
 *
 * One cache per shader, because the pipeline state also carries that shader's functions and vertex
 * layout. The mapping from a mode to what it means is written once, here.
 */
class MetalDrawStateCache {
 public:
  void init(id<MTLDevice> device,
            id<MTLFunction> vert,
            id<MTLFunction> frag,
            MTLVertexDescriptor* vertex_desc,
            MTLPixelFormat color_format,
            MTLPixelFormat depth_format);

  // Returns nil and logs if the pipeline cannot be built. `write_mask` is for renderers that
  // draw colour and alpha in separate passes; on OpenGL that is glColorMask, on Metal it belongs
  // to the pipeline, so each mask needs its own object.
  id<MTLRenderPipelineState> pipeline(DrawMode mode,
                                      MTLColorWriteMask write_mask = MTLColorWriteMaskAll);
  // `force_no_depth_write` is for the second half of an alpha-fail double draw.
  id<MTLDepthStencilState> depth_state(DrawMode mode, bool force_no_depth_write);
  id<MTLSamplerState> sampler(DrawMode mode);

 private:
  id<MTLDevice> m_device = nil;
  id<MTLFunction> m_vert = nil;
  id<MTLFunction> m_frag = nil;
  MTLVertexDescriptor* m_vertex_desc = nil;
  MTLPixelFormat m_color_format = MTLPixelFormatInvalid;
  MTLPixelFormat m_depth_format = MTLPixelFormatInvalid;
  std::unordered_map<u64, id<MTLRenderPipelineState>> m_pipelines;
  std::unordered_map<u32, id<MTLDepthStencilState>> m_depth_states;
  std::unordered_map<u32, id<MTLSamplerState>> m_samplers;
};

/*!
 * Everything a bucket renderer needs that is not its own.
 */
struct MetalRenderState {
  id<MTLDevice> device = nil;
  id<MTLLibrary> library = nil;
  id<MTLRenderCommandEncoder> encoder = nil;

  // A command buffer for work that cannot go in the frame's render pass: rendering into a texture
  // that a later bucket samples. It is committed before the frame's own command buffer, and
  // command buffers run in the order they are committed, so everything recorded here has finished
  // before the first bucket draws. A renderer that used the frame's encoder for this would have
  // to end it and start another, which costs a full store and load of the colour and depth
  // buffers on a tile-based GPU.
  id<MTLCommandBuffer> offscreen_cmd = nil;
  MTLPixelFormat color_format = MTLPixelFormatInvalid;
  MTLPixelFormat depth_format = MTLPixelFormatInvalid;

  std::shared_ptr<TexturePool> texture_pool;
  std::shared_ptr<Loader> loader;

  // Where this bucket's data ends. A renderer must leave the DMA cursor exactly here, or every
  // later bucket reads the wrong data.
  u32 next_bucket = 0;

  // Per level slot, set from that slot's tfrag bucket, which is where the game sends the camera
  // and the name of the level drawn in that slot. Two slots are live at once during a transition,
  // with different levels in them, so this cannot be one value.
  struct LevelSlot {
    GoalBackgroundCameraData camera{};
    bool has_camera = false;
    std::string level_name;
  };
  LevelSlot level_slots[jak1::LEVEL_MAX];

  // One occlusion string per level slot, from the vis-copy bucket. Null means frustum culling
  // only.
  const u8* occlusion_for_level(int level_id) const;
  struct LevelVis {
    bool valid = false;
    u8 data[128 * 16];
  };
  LevelVis occlusion_vis[jak1::LEVEL_MAX];

  math::Vector<u8, 4> fog_color{0, 0, 0, 0};
  float fog_intensity = 1.f;

  // Counts up once per frame. Renderers that write into GPU buffers the GPU may still be reading
  // use it to pick which of their buffers is safe to touch.
  u64 frame_index = 0;

  // The EE's main memory, and where the GOAL symbol table sits in it. The texture-upload buckets
  // point into EE memory rather than carrying the texture data, so they need both.
  const void* ee_main_memory = nullptr;
  u32 offset_of_s7 = 0;
};

/*!
 * One bucket. The contract is the OpenGL backend's: consume exactly this bucket's data and leave
 * the cursor at render_state->next_bucket.
 */
class MetalBucketRenderer {
 public:
  MetalBucketRenderer(const std::string& name, int my_id) : m_name(name), m_my_id(my_id) {}
  virtual ~MetalBucketRenderer() = default;

  // Called once, after the shader library exists. Return false to disable this bucket.
  virtual bool init(MetalRenderState* /*render_state*/) { return true; }
  virtual void render(DmaFollower& dma, MetalRenderState* render_state) = 0;

  const std::string& name() const { return m_name; }
  int my_id() const { return m_my_id; }

 protected:
  std::string m_name;
  int m_my_id = 0;
};

/*!
 * A bucket with no renderer yet: skips its data so the ones after it still line up.
 */
class MetalEmptyBucketRenderer : public MetalBucketRenderer {
 public:
  using MetalBucketRenderer::MetalBucketRenderer;
  void render(DmaFollower& dma, MetalRenderState* render_state) override;
};

#endif  // __OBJC__
