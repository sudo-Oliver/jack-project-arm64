/*!
 * @file MetalGeneric2.mm
 * See MetalGeneric2.h.
 */

#include "MetalGeneric2.h"

#import <Metal/Metal.h>

#include <array>
#include <cstring>
#include <unordered_map>

#include "common/log/log.h"

#include "game/graphics/gfx.h"
#include "game/graphics/metal_renderer/MetalGpuResources.h"

#include "metal_shader_types.h"

namespace {

// The core's default budgets. Generic never comes close in jak 1, but the buffers are sized to
// what the core can hand over rather than to what has been seen.
constexpr u32 kMaxVerts = 500000;
// One strip of n vertices becomes (n - 2) triangles, so a triangle list is at most three indices
// per strip vertex. The core's index buffer is its own size; this is the expansion of it.
constexpr u32 kMaxIndices = 3 * 500000;

}  // namespace

struct MetalGeneric2::Impl {
  id<MTLDevice> device = nil;
  id<MTLFunction> vert = nil;
  id<MTLFunction> frag = nil;
  MTLVertexDescriptor* vertex_desc = nil;
  MTLPixelFormat color_format = MTLPixelFormatInvalid;
  MTLPixelFormat depth_format = MTLPixelFormatInvalid;
  // Must match the render pass these pipelines are used in.
  u32 sample_count = 1;
  bool ready = false;

  std::unordered_map<u32, id<MTLRenderPipelineState>> pipelines;
  std::unordered_map<u32, id<MTLDepthStencilState>> depth_states;
  std::unordered_map<u32, id<MTLSamplerState>> samplers;

  std::array<id<MTLBuffer>, kMetalFramesInFlight> vertex_buffers = {nil, nil, nil};
  std::array<id<MTLBuffer>, kMetalFramesInFlight> index_buffers = {nil, nil, nil};
  int frame = 0;

  GenericUniforms uniforms{};

  // Triangle-list ranges, one per bucket, in the same order the core's buckets are in.
  struct Range {
    u32 first_index = 0;
    u32 index_count = 0;
  };
  std::vector<Range> ranges;

  void release() {
    for (int i = 0; i < kMetalFramesInFlight; i++) {
      vertex_buffers[i] = nil;
      index_buffers[i] = nil;
    }
    pipelines.clear();
    depth_states.clear();
    samplers.clear();
  }

  /*!
   * The blend half of a DrawMode, as generic reads it. See the header for why this is not
   * MetalDrawStateCache.
   */
  id<MTLRenderPipelineState> pipeline(const DrawMode& mode) {
    const u32 key = mode.get_ab_enable() ? (1 + (u32)mode.get_alpha_blend()) : 0;
    auto it = pipelines.find(key);
    if (it != pipelines.end()) {
      return it->second;
    }

    MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
    desc.vertexFunction = vert;
    desc.fragmentFunction = frag;
    desc.vertexDescriptor = vertex_desc;
    desc.rasterSampleCount = sample_count;
    desc.depthAttachmentPixelFormat = depth_format;
    if (depth_format == MTLPixelFormatDepth32Float_Stencil8) {
      desc.stencilAttachmentPixelFormat = depth_format;
    }
    auto* color = desc.colorAttachments[0];
    color.pixelFormat = color_format;

    if (mode.get_ab_enable()) {
      color.blendingEnabled = YES;
      color.rgbBlendOperation = MTLBlendOperationAdd;
      color.alphaBlendOperation = MTLBlendOperationAdd;
      color.sourceAlphaBlendFactor = MTLBlendFactorOne;
      color.destinationAlphaBlendFactor = MTLBlendFactorZero;
      switch (mode.get_alpha_blend()) {
        case DrawMode::AlphaBlend::SRC_DST_SRC_DST:
          // Cs * As + (1 - As) * Cd
          color.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
          color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
          break;
        case DrawMode::AlphaBlend::SRC_0_SRC_DST:
          // Cs * As + Cd. `fix` is ignored here, the same as in the OpenGL renderer.
          color.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
          color.destinationRGBBlendFactor = MTLBlendFactorOne;
          break;
        case DrawMode::AlphaBlend::ZERO_SRC_SRC_DST:
          // Cd - Cs * As
          color.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
          color.destinationRGBBlendFactor = MTLBlendFactorOne;
          color.rgbBlendOperation = MTLBlendOperationReverseSubtract;
          break;
        case DrawMode::AlphaBlend::SRC_DST_FIX_DST:
          // Cs * fix + (1 - fix) * Cd, with fix coming from the draw as the blend colour's alpha.
          color.sourceRGBBlendFactor = MTLBlendFactorBlendAlpha;
          color.destinationRGBBlendFactor = MTLBlendFactorOneMinusBlendAlpha;
          break;
        case DrawMode::AlphaBlend::SRC_SRC_SRC_SRC:
          color.sourceRGBBlendFactor = MTLBlendFactorOne;
          color.destinationRGBBlendFactor = MTLBlendFactorZero;
          break;
        case DrawMode::AlphaBlend::SRC_0_DST_DST:
          // Cs * Ad + Cd
          color.sourceRGBBlendFactor = MTLBlendFactorDestinationAlpha;
          color.destinationRGBBlendFactor = MTLBlendFactorOne;
          break;
        case DrawMode::AlphaBlend::SRC_0_FIX_DST:
          color.sourceRGBBlendFactor = MTLBlendFactorOne;
          color.destinationRGBBlendFactor = MTLBlendFactorOne;
          break;
        default:
          color.blendingEnabled = NO;
          break;
      }
    }

    NSError* err = nil;
    id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:desc error:&err];
    if (!pso) {
      lg::error("[Metal] generic pipeline failed: {}",
                err ? [[err localizedDescription] UTF8String] : "unknown error");
    }
    g_metal_pipeline_builds++;
    pipelines[key] = pso;
    return pso;
  }

  id<MTLDepthStencilState> depth_state(const DrawMode& mode) {
    // Generic takes the depth-write bit literally, unlike the background renderers.
    const u32 key = ((u32)mode.get_depth_test() << 1) | (mode.get_depth_write_enable() ? 1 : 0);
    auto it = depth_states.find(key);
    if (it != depth_states.end()) {
      return it->second;
    }
    MTLDepthStencilDescriptor* dd = [[MTLDepthStencilDescriptor alloc] init];
    switch (mode.get_depth_test()) {
      case GsTest::ZTest::NEVER:
        dd.depthCompareFunction = MTLCompareFunctionNever;
        break;
      case GsTest::ZTest::ALWAYS:
        dd.depthCompareFunction = MTLCompareFunctionAlways;
        break;
      case GsTest::ZTest::GEQUAL:
        dd.depthCompareFunction = MTLCompareFunctionGreaterEqual;
        break;
      case GsTest::ZTest::GREATER:
        dd.depthCompareFunction = MTLCompareFunctionGreater;
        break;
      default:
        dd.depthCompareFunction = MTLCompareFunctionAlways;
        break;
    }
    dd.depthWriteEnabled = mode.get_depth_write_enable();
    id<MTLDepthStencilState> state = [device newDepthStencilStateWithDescriptor:dd];
    depth_states[key] = state;
    return state;
  }

  id<MTLSamplerState> sampler(bool filter, bool clamp_s, bool clamp_t) {
    const u32 key = (filter ? 1 : 0) | (clamp_s ? 2 : 0) | (clamp_t ? 4 : 0);
    auto it = samplers.find(key);
    if (it != samplers.end()) {
      return it->second;
    }
    MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
    sd.sAddressMode = clamp_s ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
    sd.tAddressMode = clamp_t ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
    sd.minFilter = filter ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.magFilter = filter ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.mipFilter = MTLSamplerMipFilterNotMipmapped;
    id<MTLSamplerState> s = [device newSamplerStateWithDescriptor:sd];
    samplers[key] = s;
    return s;
  }
};

MetalGeneric2::MetalGeneric2() : m_impl(std::make_unique<Impl>()) {}

MetalGeneric2::~MetalGeneric2() {
  if (m_impl) {
    m_impl->release();
  }
}

bool MetalGeneric2::init(MetalRenderState* render_state) {
  // Ten buckets share one of these, so init() is called ten times. It only does the work once --
  // unless the sample count changed, which invalidates every pipeline it built.
  if (m_impl->ready && m_impl->sample_count == render_state->sample_count) {
    return true;
  }
  m_impl->pipelines.clear();
  m_impl->device = render_state->device;
  m_impl->color_format = render_state->color_format;
  m_impl->depth_format = render_state->depth_format;
  m_impl->sample_count = render_state->sample_count;

  m_impl->vert = [render_state->library newFunctionWithName:@"generic_vert"];
  m_impl->frag = [render_state->library newFunctionWithName:@"generic_frag"];
  if (!m_impl->vert || !m_impl->frag) {
    lg::error("[Metal] generic shader entry points missing from the library");
    return false;
  }

  // Generic2Core::Vertex: float3 xyz at 0, u8[4] rgba at 12, float2 st at 16, and the four
  // bytes tex_unit/flags/adc/pad at 24.
  MTLVertexDescriptor* vd = [[MTLVertexDescriptor alloc] init];
  vd.attributes[0].format = MTLVertexFormatFloat3;
  vd.attributes[0].offset = offsetof(Vertex, xyz);
  vd.attributes[0].bufferIndex = MetalBufferIndexVertex;
  vd.attributes[1].format = MTLVertexFormatUChar4Normalized;
  vd.attributes[1].offset = offsetof(Vertex, rgba);
  vd.attributes[1].bufferIndex = MetalBufferIndexVertex;
  vd.attributes[2].format = MTLVertexFormatFloat2;
  vd.attributes[2].offset = offsetof(Vertex, st);
  vd.attributes[2].bufferIndex = MetalBufferIndexVertex;
  vd.attributes[3].format = MTLVertexFormatUChar4;
  vd.attributes[3].offset = offsetof(Vertex, tex_unit);
  vd.attributes[3].bufferIndex = MetalBufferIndexVertex;
  vd.layouts[MetalBufferIndexVertex].stride = sizeof(Vertex);
  vd.layouts[MetalBufferIndexVertex].stepFunction = MTLVertexStepFunctionPerVertex;
  m_impl->vertex_desc = vd;

  for (int i = 0; i < kMetalFramesInFlight; i++) {
    m_impl->vertex_buffers[i] =
        [m_impl->device newBufferWithLength:(NSUInteger)kMaxVerts * sizeof(Vertex)
                                    options:MTLResourceStorageModeShared];
    m_impl->index_buffers[i] =
        [m_impl->device newBufferWithLength:(NSUInteger)kMaxIndices * sizeof(u32)
                                    options:MTLResourceStorageModeShared];
    if (!m_impl->vertex_buffers[i] || !m_impl->index_buffers[i]) {
      lg::error("[Metal] generic could not allocate its buffers");
      return false;
    }
  }

  DrawMode probe;
  probe.set_ab(true);
  probe.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_SRC_DST);
  if (!m_impl->pipeline(probe)) {
    return false;
  }

  m_impl->ready = true;
  return true;
}

void MetalGeneric2::render_in_mode(DmaFollower& dma, MetalRenderState* render_state, Mode mode) {
  m_current_render_state = render_state;
  m_impl->frame = (int)(render_state->frame_index % kMetalFramesInFlight);
  Generic2Core::render_in_mode(dma, GameVersion::Jak1, render_state->next_bucket, mode);
  m_current_render_state = nullptr;
}

void MetalGeneric2::do_draws() {
  auto* render_state = m_current_render_state;
  id<MTLRenderCommandEncoder> encoder = render_state ? render_state->encoder : nil;
  if (!encoder || !m_impl->ready || m_next_free_bucket == 0 || m_next_free_vert == 0) {
    return;
  }
  if (m_next_free_vert > kMaxVerts) {
    lg::warn("[Metal] generic overflowed its vertex buffer ({} verts)", m_next_free_vert);
    return;
  }

  const int frame = m_impl->frame;
  id<MTLBuffer> vbuf = m_impl->vertex_buffers[frame];
  id<MTLBuffer> ibuf = m_impl->index_buffers[frame];
  memcpy([vbuf contents], m_verts.data(), m_next_free_vert * sizeof(Vertex));

  // Expand every bucket's triangle strips into triangles. Nothing here culls faces, so the
  // winding the naive expansion gives is fine.
  auto* inds = (u32*)[ibuf contents];
  u32 write = 0;
  m_impl->ranges.assign(m_next_free_bucket, {});
  for (u32 b = 0; b < m_next_free_bucket; b++) {
    const auto& bucket = m_buckets[b];
    const u32 first = write;
    u32 strip_len = 0;
    for (u32 i = 0; i < bucket.idx_count; i++) {
      const u32 idx = m_indices[bucket.idx_idx + i];
      if (idx == UINT32_MAX) {
        strip_len = 0;
        continue;
      }
      strip_len++;
      if (strip_len >= 3 && write + 3 <= kMaxIndices) {
        inds[write++] = m_indices[bucket.idx_idx + i - 2];
        inds[write++] = m_indices[bucket.idx_idx + i - 1];
        inds[write++] = idx;
      }
    }
    m_impl->ranges[b] = {first, write - first};
  }
  if (write == 0) {
    return;
  }

  auto& u = m_impl->uniforms;
  u.fog_color = {render_state->fog_color[0] / 255.f, render_state->fog_color[1] / 255.f,
                 render_state->fog_color[2] / 255.f, render_state->fog_intensity / 255.f};
  u.scale = {m_drawing_config.proj_scale[0], m_drawing_config.proj_scale[1],
             m_drawing_config.proj_scale[2], 0.f};
  u.mat_23 = m_drawing_config.proj_mat_23;
  u.mat_32 = m_drawing_config.proj_mat_32;
  u.mat_33 = 0.f;
  u.fog_constants = {m_drawing_config.pfog0, m_drawing_config.fog_min, m_drawing_config.fog_max,
                     0.f};
  u.hvdf_offset = {m_drawing_config.hvdf_offset[0], m_drawing_config.hvdf_offset[1],
                   m_drawing_config.hvdf_offset[2], m_drawing_config.hvdf_offset[3]};
  u.gfx_hack_no_tex = Gfx::g_global_settings.hack_no_tex ? 1 : 0;
  u.warp_sample_mode = 0;  // jak 2 and later only
  u.scissor_adjust = 512.f / 448.f;
  u.height_scale = 1.f;
  u.scissor_height = 448.f;
  u.use_full_matrix = m_drawing_config.uses_full_matrix ? 1 : 0;
  if (m_drawing_config.uses_full_matrix) {
    for (int col = 0; col < 4; col++) {
      const auto& c = m_drawing_config.full_matrix[col];
      u.full_matrix.col[col] = {c[0], c[1], c[2], c[3]};
    }
  }

  [encoder setVertexBuffer:vbuf offset:0 atIndex:MetalBufferIndexVertex];

  auto draw_bucket = [&](u32 b) {
    const auto& range = m_impl->ranges[b];
    if (range.index_count == 0) {
      return;
    }
    const auto& first = m_adgifs[m_buckets[b].start];
    const DrawMode& mode = first.mode;

    id<MTLRenderPipelineState> pso = m_impl->pipeline(mode);
    if (!pso) {
      return;
    }

    // The texture. The top bit of tbp picks the PSMT4HH pool, the same as in the OpenGL renderer.
    const u32 tbp_to_lookup = first.tbp & 0x7fff;
    const bool use_mt4hh = first.tbp & 0x8000;
    std::optional<u64> tex = use_mt4hh ? render_state->texture_pool->lookup_mt4hh(tbp_to_lookup)
                                       : render_state->texture_pool->lookup(tbp_to_lookup);
    if (!tex) {
      tex = render_state->texture_pool->get_placeholder_texture();
    }
    id<MTLTexture> texture = tex ? metal_texture_from_handle(*tex) : nil;
    if (!texture) {
      return;
    }

    float alpha_reject = 0.f;
    if (mode.get_at_enable() && mode.get_alpha_test() == DrawMode::AlphaTest::GEQUAL) {
      alpha_reject = mode.get_aref() / 128.f;
    }
    u.alpha_reject = alpha_reject;
    u.color_mult = 1.f;

    // SRC_DST_FIX_DST is the one blend whose constant is per draw.
    if (mode.get_ab_enable() && mode.get_alpha_blend() == DrawMode::AlphaBlend::SRC_DST_FIX_DST) {
      [encoder setBlendColorRed:0.f green:0.f blue:0.f alpha:first.fix / 127.f];
    }

    [encoder setRenderPipelineState:pso];
    [encoder setDepthStencilState:m_impl->depth_state(mode)];
    [encoder setFragmentSamplerState:m_impl->sampler(mode.get_filt_enable(),
                                                     mode.get_clamp_s_enable(),
                                                     mode.get_clamp_t_enable())
                             atIndex:0];
    [encoder setFragmentTexture:texture atIndex:0];
    [encoder setVertexBytes:&u length:sizeof(u) atIndex:MetalBufferIndexUniforms];
    [encoder setFragmentBytes:&u length:sizeof(u) atIndex:MetalBufferIndexUniforms];
    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                        indexCount:range.index_count
                         indexType:MTLIndexTypeUInt32
                       indexBuffer:ibuf
                 indexBufferOffset:range.first_index * sizeof(u32)];
    m_last_frame_tris += range.index_count / 3;
  };

  // The world draws, in the blend order the OpenGL renderer uses. The order matters: these are
  // transparent effects drawn without a depth pre-pass.
  constexpr DrawMode::AlphaBlend alpha_order[ALPHA_MODE_COUNT] = {
      DrawMode::AlphaBlend::SRC_0_FIX_DST,    DrawMode::AlphaBlend::SRC_SRC_SRC_SRC,
      DrawMode::AlphaBlend::SRC_DST_SRC_DST,  DrawMode::AlphaBlend::SRC_0_SRC_DST,
      DrawMode::AlphaBlend::ZERO_SRC_SRC_DST, DrawMode::AlphaBlend::SRC_DST_FIX_DST,
      DrawMode::AlphaBlend::SRC_0_DST_DST,
  };
  for (int i = 0; i < ALPHA_MODE_COUNT; i++) {
    if (!m_alpha_draw_enable[i]) {
      continue;
    }
    for (u32 b = 0; b < m_next_free_bucket; b++) {
      const auto& first = m_adgifs[m_buckets[b].start];
      if (first.mode.get_alpha_blend() == alpha_order[i] && !first.uses_hud) {
        draw_bucket(b);
      }
    }
  }

  // Then the HUD draws, which use a different projection.
  if (m_drawing_config.uses_hud) {
    u.scale = {m_drawing_config.hud_scale[0], m_drawing_config.hud_scale[1],
               m_drawing_config.hud_scale[2], 0.f};
    u.mat_23 = m_drawing_config.hud_mat_23;
    u.mat_32 = m_drawing_config.hud_mat_32;
    u.mat_33 = m_drawing_config.hud_mat_33;
    u.gfx_hack_no_tex = 0;
    for (u32 b = 0; b < m_next_free_bucket; b++) {
      if (m_adgifs[m_buckets[b].start].uses_hud) {
        draw_bucket(b);
      }
    }
  }
}

MetalGeneric2BucketRenderer::MetalGeneric2BucketRenderer(const std::string& name,
                                                         int my_id,
                                                         std::shared_ptr<MetalGeneric2> renderer,
                                                         MetalGeneric2::Mode mode)
    : MetalBucketRenderer(name, my_id), m_generic(std::move(renderer)), m_mode(mode) {}

bool MetalGeneric2BucketRenderer::init(MetalRenderState* render_state) {
  return m_generic->init(render_state);
}

void MetalGeneric2BucketRenderer::render(DmaFollower& dma, MetalRenderState* render_state) {
  m_generic->render_in_mode(dma, render_state, m_mode);
}
