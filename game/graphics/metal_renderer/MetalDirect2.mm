/*!
 * @file MetalDirect2.mm
 * See MetalDirect2.h.
 */

#include "MetalDirect2.h"

#import <Metal/Metal.h>

#include <optional>
#include <unordered_map>
#include <vector>

#include "common/log/log.h"

#include "game/graphics/metal_renderer/MetalGpuResources.h"

#include "metal_shader_types.h"

namespace {

// The game hands the direct renderer its vertices already in GS screen space, and the GS drew a
// taller field than the window shows. Same constant Shader.cpp substitutes into the GLSL.
constexpr float kJak1ScissorAdjust = 512.0f / 448.0f;

// One flush's worth of vertex and index data. A frame can flush several times, so these are
// handed out from a list that grows on demand and is reused from the frame after next.
struct BufferChunk {
  id<MTLBuffer> vertices = nil;
  id<MTLBuffer> indices = nil;
};

}  // namespace

struct MetalDirect2::Impl {
  id<MTLDevice> device = nil;
  id<MTLFunction> vert = nil;
  id<MTLFunction> frag = nil;
  MTLVertexDescriptor* vertex_desc = nil;
  MTLPixelFormat color_format = MTLPixelFormatInvalid;
  MTLPixelFormat depth_format = MTLPixelFormatInvalid;
  bool ready = false;

  u32 max_verts = 0;
  u32 max_inds = 0;

  // Keyed by DrawMode::as_int(). The per-draw FIX value is not in the key: on Metal it is the
  // encoder's blend colour, not part of the pipeline.
  std::unordered_map<u32, id<MTLRenderPipelineState>> pipelines;
  std::unordered_map<u32, id<MTLDepthStencilState>> depth_states;
  std::unordered_map<u32, id<MTLSamplerState>> samplers;

  // Chunks in use this frame. `next_chunk` resets when the frame changes, which is what makes a
  // chunk safe to write again: by then the frame that used it has been presented.
  std::vector<BufferChunk> chunks;
  size_t next_chunk = 0;
  u64 chunk_frame = UINT64_MAX;

  // Textures bound for the draw group being built, so a group that uses fewer than ten units does
  // not leave the previous group's textures bound.
  id<MTLTexture> group_textures[MetalDirect2TexUnits];
  id<MTLSamplerState> group_samplers[MetalDirect2TexUnits];

  // The index buffer the flush being encoded is drawing from.
  id<MTLBuffer> current_indices = nil;

  id<MTLRenderPipelineState> pipeline(DrawMode mode);
  id<MTLDepthStencilState> depth_state(DrawMode mode);
  id<MTLSamplerState> sampler(bool filter, bool clamp_s, bool clamp_t);
  BufferChunk& take_chunk(u64 frame_index);
};

/*!
 * Blend state for one draw mode. This is the Metal spelling of
 * DirectRenderer2::setup_opengl_for_draw_mode -- keep the two in step.
 */
id<MTLRenderPipelineState> MetalDirect2::Impl::pipeline(DrawMode mode) {
  const u32 key = mode.as_int();
  auto it = pipelines.find(key);
  if (it != pipelines.end()) {
    return it->second;
  }

  MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
  desc.vertexFunction = vert;
  desc.fragmentFunction = frag;
  desc.vertexDescriptor = vertex_desc;
  desc.depthAttachmentPixelFormat = depth_format;
  auto* color = desc.colorAttachments[0];
  color.pixelFormat = color_format;

  bool blend = mode.get_ab_enable();
  if (blend) {
    color.rgbBlendOperation = MTLBlendOperationAdd;
    color.alphaBlendOperation = MTLBlendOperationAdd;
    color.sourceAlphaBlendFactor = MTLBlendFactorOne;
    color.destinationAlphaBlendFactor = MTLBlendFactorZero;
    switch (mode.get_alpha_blend()) {
      case DrawMode::AlphaBlend::SRC_DST_SRC_DST:
        // (Cs - Cd) * As + Cd
        color.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        break;
      case DrawMode::AlphaBlend::SRC_0_SRC_DST:
        // (Cs - 0) * As + Cd
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
        // (Cs - Cd) * FIX + Cd. FIX is per draw, so it rides on the encoder's blend colour.
        color.sourceRGBBlendFactor = MTLBlendFactorBlendAlpha;
        color.destinationRGBBlendFactor = MTLBlendFactorOneMinusBlendAlpha;
        break;
      case DrawMode::AlphaBlend::SRC_SRC_SRC_SRC:
        // Cs: the source wins outright.
        color.sourceRGBBlendFactor = MTLBlendFactorOne;
        color.destinationRGBBlendFactor = MTLBlendFactorZero;
        break;
      case DrawMode::AlphaBlend::SRC_0_DST_DST:
        // (Cs - 0) * Ad + Cd. The colour multiplier that goes with this one is in the uniforms.
        color.sourceRGBBlendFactor = MTLBlendFactorDestinationAlpha;
        color.destinationRGBBlendFactor = MTLBlendFactorOne;
        break;
      default:
        blend = false;
        break;
    }
  }
  color.blendingEnabled = blend;

  NSError* err = nil;
  id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:desc error:&err];
  if (!pso) {
    lg::error("[Metal] direct2 pipeline for mode {} failed: {}", key,
              err ? [[err localizedDescription] UTF8String] : "unknown error");
  }
  pipelines[key] = pso;
  return pso;
}

id<MTLDepthStencilState> MetalDirect2::Impl::depth_state(DrawMode mode) {
  const u32 key = mode.as_int();
  auto it = depth_states.find(key);
  if (it != depth_states.end()) {
    return it->second;
  }

  MTLDepthStencilDescriptor* ds = [[MTLDepthStencilDescriptor alloc] init];
  switch (mode.get_depth_test()) {
    case GsTest::ZTest::NEVER:
      ds.depthCompareFunction = MTLCompareFunctionNever;
      break;
    case GsTest::ZTest::ALWAYS:
      ds.depthCompareFunction = MTLCompareFunctionAlways;
      break;
    case GsTest::ZTest::GEQUAL:
      ds.depthCompareFunction = MTLCompareFunctionGreaterEqual;
      break;
    case GsTest::ZTest::GREATER:
      ds.depthCompareFunction = MTLCompareFunctionGreater;
      break;
    default:
      ds.depthCompareFunction = MTLCompareFunctionAlways;
      break;
  }
  ds.depthWriteEnabled = mode.get_depth_write_enable();

  id<MTLDepthStencilState> state = [device newDepthStencilStateWithDescriptor:ds];
  depth_states[key] = state;
  return state;
}

id<MTLSamplerState> MetalDirect2::Impl::sampler(bool filter, bool clamp_s, bool clamp_t) {
  const u32 key = (clamp_s ? 1 : 0) | (clamp_t ? 2 : 0) | (filter ? 4 : 0);
  auto it = samplers.find(key);
  if (it != samplers.end()) {
    return it->second;
  }

  MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
  sd.sAddressMode = clamp_s ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
  sd.tAddressMode = clamp_t ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
  sd.minFilter = filter ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
  sd.magFilter = filter ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
  // The direct renderer disables mipmapping: its textures are HUD and debug art drawn at a fixed
  // size, and DirectRenderer2's m_debug.disable_mip is on by default.
  sd.mipFilter = MTLSamplerMipFilterNotMipmapped;
  id<MTLSamplerState> s = [device newSamplerStateWithDescriptor:sd];
  samplers[key] = s;
  return s;
}

BufferChunk& MetalDirect2::Impl::take_chunk(u64 frame_index) {
  if (chunk_frame != frame_index) {
    chunk_frame = frame_index;
    next_chunk = 0;
  }
  if (next_chunk >= chunks.size()) {
    BufferChunk chunk;
    chunk.vertices = [device newBufferWithLength:max_verts * sizeof(Vertex)
                                         options:MTLResourceStorageModeShared];
    chunk.indices = [device newBufferWithLength:max_inds * sizeof(u32)
                                        options:MTLResourceStorageModeShared];
    chunks.push_back(chunk);
  }
  return chunks[next_chunk++];
}

MetalDirect2::MetalDirect2(u32 max_verts,
                           u32 max_inds,
                           u32 max_draws,
                           const std::string& name,
                           bool use_ftoi_mod)
    : DirectRenderer2Core(max_verts, max_inds, max_draws, name, use_ftoi_mod),
      m_impl(std::make_unique<Impl>()) {
  m_impl->max_verts = max_verts;
  m_impl->max_inds = max_inds;
}

MetalDirect2::~MetalDirect2() = default;

bool MetalDirect2::init(MetalRenderState* render_state) {
  m_impl->device = render_state->device;
  m_impl->color_format = render_state->color_format;
  m_impl->depth_format = render_state->depth_format;
  m_impl->vert = [render_state->library newFunctionWithName:@"direct2_vert"];
  m_impl->frag = [render_state->library newFunctionWithName:@"direct2_frag"];
  if (!m_impl->vert || !m_impl->frag) {
    lg::error("[Metal] direct2 shader entry points missing from the library");
    return false;
  }

  // DirectRenderer2Core::Vertex: float3 xyz at 0, u8[4] rgba at 12, float3 stq at 16,
  // then tex_unit, flags, fog, pad as four bytes at 28.
  MTLVertexDescriptor* vd = [[MTLVertexDescriptor alloc] init];
  vd.attributes[0].format = MTLVertexFormatFloat3;
  vd.attributes[0].offset = 0;
  vd.attributes[0].bufferIndex = MetalBufferIndexVertex;
  vd.attributes[1].format = MTLVertexFormatUChar4Normalized;
  vd.attributes[1].offset = 12;
  vd.attributes[1].bufferIndex = MetalBufferIndexVertex;
  vd.attributes[2].format = MTLVertexFormatFloat3;
  vd.attributes[2].offset = 16;
  vd.attributes[2].bufferIndex = MetalBufferIndexVertex;
  vd.attributes[3].format = MTLVertexFormatUChar4;
  vd.attributes[3].offset = 28;
  vd.attributes[3].bufferIndex = MetalBufferIndexVertex;
  vd.layouts[MetalBufferIndexVertex].stride = sizeof(Vertex);
  vd.layouts[MetalBufferIndexVertex].stepFunction = MTLVertexStepFunctionPerVertex;
  m_impl->vertex_desc = vd;

  DrawMode probe;
  probe.set_depth_write_enable(true);
  if (!m_impl->pipeline(probe)) {
    return false;
  }
  m_impl->ready = true;
  return true;
}

void MetalDirect2::render_gif_data(const u8* data, MetalRenderState* render_state) {
  if (!m_impl->ready) {
    return;
  }
  m_current_render_state = render_state;
  DirectRenderer2Core::render_gif_data(data);
  m_current_render_state = nullptr;
}

void MetalDirect2::render_vif_data(u32 vif0,
                                   u32 vif1,
                                   const u8* data,
                                   u32 size,
                                   MetalRenderState* render_state) {
  if (!m_impl->ready) {
    return;
  }
  m_current_render_state = render_state;
  DirectRenderer2Core::render_vif_data(vif0, vif1, data, size);
  m_current_render_state = nullptr;
}

void MetalDirect2::flush_pending(MetalRenderState* render_state) {
  if (!m_impl->ready) {
    return;
  }
  m_current_render_state = render_state;
  flush_draws();
  m_current_render_state = nullptr;
}

void MetalDirect2::flush_draws() {
  if (m_next_free_draw == 0 || !m_current_render_state || !m_current_render_state->encoder) {
    reset_buffers();
    return;
  }

  BufferChunk& chunk = m_impl->take_chunk(m_current_render_state->frame_index);
  memcpy([chunk.vertices contents], m_vertices.vertices.data(),
         m_vertices.next_vertex * sizeof(Vertex));
  memcpy([chunk.indices contents], m_vertices.indices.data(), m_vertices.next_index * sizeof(u32));
  m_stats.num_uploads++;
  m_stats.upload_bytes +=
      (m_vertices.next_vertex * sizeof(Vertex)) + (m_vertices.next_index * sizeof(u32));

  id<MTLRenderCommandEncoder> encoder = m_current_render_state->encoder;
  [encoder setVertexBuffer:chunk.vertices offset:0 atIndex:MetalBufferIndexVertex];
  m_impl->current_indices = chunk.indices;

  draw_call_loop_grouped();

  reset_buffers();
}

/*!
 * Draw the buffered draws, merging consecutive draws that differ only in which texture they use
 * into one draw call with several textures bound. Same grouping DirectRenderer2 does, and for the
 * same reason: the direct renderer's draws are small and the state changes cost more than the
 * triangles.
 */
void MetalDirect2::draw_call_loop_grouped() {
  id<MTLRenderCommandEncoder> encoder = m_current_render_state->encoder;

  u32 draw_idx = 0;
  while (draw_idx < m_next_free_draw) {
    const auto& draw = m_draw_buffer[draw_idx];
    u32 end_of_draw_group = draw_idx;  // inclusive

    for (int i = 0; i < MetalDirect2TexUnits; i++) {
      m_impl->group_textures[i] = nil;
      m_impl->group_samplers[i] = nil;
    }
    bind_texture(draw.tex_unit, draw.tbp, draw.mode);

    for (u32 draw_to_consider = draw_idx + 1; draw_to_consider < draw_idx + TEX_UNITS;
         draw_to_consider++) {
      if (draw_to_consider >= m_next_free_draw) {
        break;
      }
      const auto& next_draw = m_draw_buffer[draw_to_consider];
      if (next_draw.mode.as_int() != draw.mode.as_int() || next_draw.fix != draw.fix) {
        break;
      }
      m_stats.saved_draws++;
      end_of_draw_group++;
      bind_texture(next_draw.tex_unit, next_draw.tbp, next_draw.mode);
    }

    u32 end_idx = (end_of_draw_group == m_next_free_draw - 1)
                      ? m_vertices.next_index
                      : m_draw_buffer[end_of_draw_group + 1].start_index;
    const u32 index_count = end_idx - draw.start_index;

    id<MTLRenderPipelineState> pso = m_impl->pipeline(draw.mode);
    if (pso && index_count) {
      // alpha_reject and color_mult are the two things the OpenGL path also passes as uniforms
      // rather than baking into state. See setup_opengl_for_draw_mode.
      Direct2Uniforms uniforms{};
      uniforms.alpha_reject = 0.f;
      if (draw.mode.get_at_enable() &&
          draw.mode.get_alpha_test() == DrawMode::AlphaTest::GEQUAL) {
        uniforms.alpha_reject = draw.mode.get_aref() / 128.f;
      }
      uniforms.color_mult =
          (draw.mode.get_ab_enable() &&
           draw.mode.get_alpha_blend() == DrawMode::AlphaBlend::SRC_0_DST_DST)
              ? 0.5f
              : 1.f;
      uniforms.scissor_adjust = kJak1ScissorAdjust;
      const auto& fog = m_current_render_state->fog_color;
      uniforms.fog_color = {fog[0] / 255.f, fog[1] / 255.f, fog[2] / 255.f,
                            m_current_render_state->fog_intensity / 255.f};

      [encoder setRenderPipelineState:pso];
      [encoder setDepthStencilState:m_impl->depth_state(draw.mode)];
      // The FIX blend mode reads its constant from here rather than from the pipeline.
      [encoder setBlendColorRed:0 green:0 blue:0 alpha:draw.fix / 127.f];
      [encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:MetalBufferIndexUniforms];
      [encoder setFragmentBytes:&uniforms
                         length:sizeof(uniforms)
                        atIndex:MetalBufferIndexUniforms];
      // Every slot of the shader's texture array has to hold something, even the ones this group
      // does not use: a nil entry in an array argument is undefined, not merely unsampled.
      id<MTLTexture> filler = nil;
      id<MTLSamplerState> filler_sampler = nil;
      for (int i = 0; i < MetalDirect2TexUnits && !filler; i++) {
        filler = m_impl->group_textures[i];
        filler_sampler = m_impl->group_samplers[i];
      }
      for (int i = 0; i < MetalDirect2TexUnits; i++) {
        id<MTLTexture> t = m_impl->group_textures[i] ?: filler;
        id<MTLSamplerState> smp = m_impl->group_samplers[i] ?: filler_sampler;
        [encoder setFragmentTexture:t atIndex:i];
        [encoder setFragmentSamplerState:smp atIndex:i];
      }

      // UINT32_MAX in the index buffer restarts the strip. Metal does that unconditionally for
      // 32-bit indices, which is why there is no equivalent of glEnable(GL_PRIMITIVE_RESTART).
      [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
                          indexCount:index_count
                           indexType:MTLIndexTypeUInt32
                         indexBuffer:m_impl->current_indices
                   indexBufferOffset:draw.start_index * sizeof(u32)];
      m_last_frame_tris += index_count / 3;
    }

    draw_idx = end_of_draw_group + 1;
  }
}

bool MetalDirect2::bind_texture(u8 unit, u16 tbp, DrawMode mode) {
  if (unit >= MetalDirect2TexUnits) {
    return false;
  }
  std::optional<u64> handle;
  const u32 tbp_to_lookup = tbp & 0x7fff;
  if (tbp & 0x8000) {
    handle = m_current_render_state->texture_pool->lookup_mt4hh(tbp_to_lookup);
  } else {
    handle = m_current_render_state->texture_pool->lookup(tbp_to_lookup);
  }
  if (!handle) {
    handle = m_current_render_state->texture_pool->get_placeholder_texture();
  }

  id<MTLTexture> texture = metal_texture_from_handle(*handle);
  if (!texture) {
    return false;
  }
  m_impl->group_textures[unit] = texture;
  m_impl->group_samplers[unit] = m_impl->sampler(
      mode.get_filt_enable(), mode.get_clamp_s_enable(), mode.get_clamp_t_enable());
  return true;
}

MetalDirectBucketRenderer::MetalDirectBucketRenderer(const std::string& name,
                                                     int my_id,
                                                     u32 batch_size)
    : MetalBucketRenderer(name, my_id),
      // Same shape the OpenGL DirectRenderer is given: the batch size is a triangle count, and
      // each triangle needs three vertices and three indices.
      m_direct(batch_size * 3, batch_size * 4, batch_size, name, false) {}

MetalDirectBucketRenderer::~MetalDirectBucketRenderer() = default;

bool MetalDirectBucketRenderer::init(MetalRenderState* render_state) {
  return m_direct.init(render_state);
}

void MetalDirectBucketRenderer::render(DmaFollower& dma, MetalRenderState* render_state) {
  m_direct.reset_tri_count();
  m_direct.reset_state();

  while (dma.current_tag_offset() != render_state->next_bucket && !dma.ended()) {
    auto data = dma.read_and_advance();
    if (data.size_bytes) {
      m_direct.render_vif_data(data.vif0(), data.vif1(), data.data, data.size_bytes,
                               render_state);
    }
  }
  m_direct.flush_pending(render_state);
}
