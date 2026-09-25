/*!
 * @file MetalDirect.mm
 * See MetalDirect.h.
 */

#include "MetalDirect.h"

#import <Metal/Metal.h>

#include <unordered_map>
#include <vector>

#include "common/log/log.h"

#include "game/graphics/metal_renderer/MetalGpuResources.h"

#include "metal_shader_types.h"

namespace {

// Same constants Shader.cpp substitutes into the GLSL for Jak 1.
constexpr float kJak1ScissorAdjust = 512.0f / 448.0f;
constexpr float kJak1HeightScale = 1.0f;
constexpr float kJak1GameHeight = 448.0f;
constexpr float kGameWidth = 512.0f;

// One flush's worth of vertices. A frame flushes many times, and Metal runs the whole frame at
// the end, so a buffer written twice in one frame would only ever be read with its last contents.
struct VertexChunk {
  id<MTLBuffer> buffer = nil;
};

// What a pipeline state has to be rebuilt for. Blend and the colour write mask live in the
// pipeline on Metal, and the fix value is the encoder's blend colour, so it is not in here.
struct PipelineKey {
  u8 blend_a = 0, blend_b = 0, blend_c = 0, blend_d = 0;
  bool blend_enable = false;
  bool write_rgb = true;

  u64 as_int() const {
    return (u64)blend_a | ((u64)blend_b << 8) | ((u64)blend_c << 16) | ((u64)blend_d << 24) |
           ((u64)blend_enable << 32) | ((u64)write_rgb << 33);
  }
};

}  // namespace

struct MetalDirect::Impl {
  id<MTLDevice> device = nil;
  id<MTLFunction> vert = nil;
  id<MTLFunction> frag = nil;
  MTLVertexDescriptor* vertex_desc = nil;
  MTLPixelFormat color_format = MTLPixelFormatInvalid;
  MTLPixelFormat depth_format = MTLPixelFormatInvalid;
  // Must match the render pass these pipelines are used in.
  u32 sample_count = 1;
  bool ready = false;

  u32 max_verts = 0;

  std::unordered_map<u64, id<MTLRenderPipelineState>> pipelines;
  std::unordered_map<u32, id<MTLDepthStencilState>> depth_states;
  std::unordered_map<u32, id<MTLSamplerState>> samplers;

  std::vector<VertexChunk> chunks;
  size_t next_chunk = 0;
  u64 chunk_frame = UINT64_MAX;

  // The texture and sampler the flush being encoded should use, set by backend_bind_texture.
  id<MTLTexture> texture = nil;
  id<MTLSamplerState> sampler = nil;

  // The colour and alpha multipliers a blend mode implies. OpenGL keeps these in m_ogl; they are
  // uniforms on both backends.
  float color_mult = 1.f;
  float alpha_mult = 1.f;

  id<MTLRenderPipelineState> pipeline(const PipelineKey& key);
  id<MTLDepthStencilState> depth_state(GsTest::ZTest test, bool depth_write);
  id<MTLSamplerState> sampler_for(bool filter, bool clamp_s, bool clamp_t);
  VertexChunk& take_chunk(u64 frame_index);
};

/*!
 * The Metal spelling of DirectRenderer::update_gl_blend. Keep the two in step: a divergence here
 * is a visual difference.
 */
id<MTLRenderPipelineState> MetalDirect::Impl::pipeline(const PipelineKey& key) {
  auto it = pipelines.find(key.as_int());
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
  // The GS can write only alpha, which is how it fills a stencil-like mask without touching the
  // picture.
  color.writeMask = key.write_rgb ? MTLColorWriteMaskAll : MTLColorWriteMaskAlpha;

  using BM = GsAlpha::BlendMode;
  const BM a = (BM)key.blend_a, b = (BM)key.blend_b, c = (BM)key.blend_c, d = (BM)key.blend_d;
  bool blend = key.blend_enable;
  if (blend) {
    color.rgbBlendOperation = MTLBlendOperationAdd;
    color.alphaBlendOperation = MTLBlendOperationAdd;
    color.sourceAlphaBlendFactor = MTLBlendFactorOne;
    color.destinationAlphaBlendFactor = MTLBlendFactorZero;

    if (a == BM::SOURCE && b == BM::DEST && c == BM::SOURCE && d == BM::DEST) {
      // (Cs - Cd) * As + Cd
      color.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
      color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    } else if (a == BM::SOURCE && b == BM::ZERO_OR_FIXED && c == BM::SOURCE && d == BM::DEST) {
      // (Cs - 0) * As + Cd
      color.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
      color.destinationRGBBlendFactor = MTLBlendFactorOne;
    } else if (a == BM::ZERO_OR_FIXED && b == BM::SOURCE && c == BM::SOURCE && d == BM::DEST) {
      // Cd - Cs * As
      color.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
      color.destinationRGBBlendFactor = MTLBlendFactorOne;
      color.rgbBlendOperation = MTLBlendOperationReverseSubtract;
    } else if (a == BM::SOURCE && b == BM::DEST && c == BM::ZERO_OR_FIXED && d == BM::DEST) {
      // (Cs - Cd) * FIX + Cd. FIX is per draw, so it rides on the encoder's blend colour.
      color.sourceRGBBlendFactor = MTLBlendFactorBlendAlpha;
      color.destinationRGBBlendFactor = MTLBlendFactorOneMinusBlendAlpha;
    } else if (a == BM::SOURCE && b == BM::SOURCE && c == BM::SOURCE && d == BM::SOURCE) {
      // The trick the game uses to turn blending off.
      blend = false;
    } else if (a == BM::SOURCE && b == BM::ZERO_OR_FIXED && c == BM::DEST && d == BM::DEST) {
      // (Cs - 0) * Ad + Cd. The colour multiplier that goes with it is a uniform.
      color.sourceRGBBlendFactor = MTLBlendFactorDestinationAlpha;
      color.destinationRGBBlendFactor = MTLBlendFactorOne;
    } else {
      lg::error("unsupported blend (metal direct): a {} b {} c {} d {}", (int)a, (int)b, (int)c,
                (int)d);
      blend = false;
    }
  }
  color.blendingEnabled = blend;

  NSError* err = nil;
  id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:desc error:&err];
  if (!pso) {
    lg::error("[Metal] direct pipeline failed: {}",
              err ? [[err localizedDescription] UTF8String] : "unknown error");
  }
  pipelines[key.as_int()] = pso;
  return pso;
}

id<MTLDepthStencilState> MetalDirect::Impl::depth_state(GsTest::ZTest test, bool depth_write) {
  const u32 key = (u32)test | (depth_write ? 0x100 : 0);
  auto it = depth_states.find(key);
  if (it != depth_states.end()) {
    return it->second;
  }

  MTLDepthStencilDescriptor* ds = [[MTLDepthStencilDescriptor alloc] init];
  switch (test) {
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
  ds.depthWriteEnabled = depth_write;

  id<MTLDepthStencilState> state = [device newDepthStencilStateWithDescriptor:ds];
  depth_states[key] = state;
  return state;
}

id<MTLSamplerState> MetalDirect::Impl::sampler_for(bool filter, bool clamp_s, bool clamp_t) {
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
  sd.mipFilter = MTLSamplerMipFilterNotMipmapped;
  id<MTLSamplerState> s = [device newSamplerStateWithDescriptor:sd];
  samplers[key] = s;
  return s;
}

VertexChunk& MetalDirect::Impl::take_chunk(u64 frame_index) {
  if (chunk_frame != frame_index) {
    chunk_frame = frame_index;
    next_chunk = 0;
  }
  if (next_chunk >= chunks.size()) {
    VertexChunk chunk;
    chunk.buffer = [device newBufferWithLength:max_verts * sizeof(Vertex)
                                       options:MTLResourceStorageModeShared];
    chunks.push_back(chunk);
  }
  return chunks[next_chunk++];
}

MetalDirect::MetalDirect(const std::string& name, int batch_size)
    : DirectRendererCore(name, batch_size), m_impl(std::make_unique<Impl>()) {
  // PrimitiveBuffer sizes itself at 3 vertices per triangle plus room to flush one last one.
  m_impl->max_verts = m_prim_buffer.max_verts;
}

MetalDirect::~MetalDirect() = default;

bool MetalDirect::init(MetalRenderState* render_state) {
  if (m_impl->ready) {
    return true;
  }
  m_impl->device = render_state->device;
  m_impl->color_format = render_state->color_format;
  m_impl->depth_format = render_state->depth_format;
  m_impl->sample_count = render_state->sample_count;
  m_impl->vert = [render_state->library newFunctionWithName:@"direct_basic_textured_vert"];
  m_impl->frag = [render_state->library newFunctionWithName:@"direct_basic_textured_frag"];
  if (!m_impl->vert || !m_impl->frag) {
    lg::error("[Metal] direct_basic_textured shader entry points missing from the library");
    return false;
  }

  // DirectRendererCore::Vertex, 64 bytes: xyzf at 0, stq at 16, rgba at 28, the four mode bytes
  // at 32, use_uv at 36, the scissor rectangle at 48.
  MTLVertexDescriptor* vd = [[MTLVertexDescriptor alloc] init];
  vd.attributes[0].format = MTLVertexFormatFloat4;
  vd.attributes[0].offset = 0;
  vd.attributes[0].bufferIndex = MetalBufferIndexVertex;
  vd.attributes[1].format = MTLVertexFormatUChar4Normalized;
  vd.attributes[1].offset = 28;
  vd.attributes[1].bufferIndex = MetalBufferIndexVertex;
  vd.attributes[2].format = MTLVertexFormatFloat3;
  vd.attributes[2].offset = 16;
  vd.attributes[2].bufferIndex = MetalBufferIndexVertex;
  vd.attributes[3].format = MTLVertexFormatUChar4;
  vd.attributes[3].offset = 32;
  vd.attributes[3].bufferIndex = MetalBufferIndexVertex;
  vd.attributes[4].format = MTLVertexFormatUChar;
  vd.attributes[4].offset = 36;
  vd.attributes[4].bufferIndex = MetalBufferIndexVertex;
  vd.attributes[5].format = MTLVertexFormatFloat4;
  vd.attributes[5].offset = 48;
  vd.attributes[5].bufferIndex = MetalBufferIndexVertex;
  vd.layouts[MetalBufferIndexVertex].stride = sizeof(Vertex);
  vd.layouts[MetalBufferIndexVertex].stepFunction = MTLVertexStepFunctionPerVertex;
  m_impl->vertex_desc = vd;

  if (!m_impl->pipeline(PipelineKey{})) {
    return false;
  }
  m_impl->ready = true;
  return true;
}

void MetalDirect::set_context(MetalRenderState* render_state) {
  m_current_render_state = render_state;
  Context ctx;
  ctx.texture_pool = render_state->texture_pool.get();
  ctx.version = GameVersion::Jak1;
  ctx.next_bucket = render_state->next_bucket;
  // The Metal renderer steps over the default-register chain before the buckets run, so a bucket
  // never meets it again.
  ctx.default_regs_buffer = UINT32_MAX;
  ctx.fog_color = render_state->fog_color;
  ctx.fog_intensity = render_state->fog_intensity;
  m_context = ctx;
}

void MetalDirect::consume_bucket_dma(DmaFollower& dma, MetalRenderState* render_state) {
  if (!m_impl->ready) {
    while (dma.current_tag_offset() != render_state->next_bucket && !dma.ended()) {
      dma.read_and_advance();
    }
    return;
  }
  set_context(render_state);
  DirectRendererCore::consume_bucket_dma(dma);
  m_current_render_state = nullptr;
}

void MetalDirect::render_vif(u32 vif0,
                             u32 vif1,
                             const u8* data,
                             u32 size,
                             MetalRenderState* render_state) {
  if (!m_impl->ready) {
    return;
  }
  set_context(render_state);
  DirectRendererCore::render_vif(vif0, vif1, data, size);
  m_current_render_state = nullptr;
}

void MetalDirect::render_gif(const u8* data, u32 size, MetalRenderState* render_state) {
  if (!m_impl->ready) {
    return;
  }
  set_context(render_state);
  DirectRendererCore::render_gif(data, size);
  m_current_render_state = nullptr;
}

void MetalDirect::flush_pending(MetalRenderState* render_state) {
  if (!m_impl->ready) {
    return;
  }
  set_context(render_state);
  flush();
  m_current_render_state = nullptr;
}

/*!
 * Look up the texture buffered texture state `unit` names and remember it for the next draw.
 * The OpenGL sibling binds it here; on Metal the binding is part of encoding the draw.
 */
void MetalDirect::backend_bind_texture(int unit) {
  auto& state = m_buffered_tex_state[unit];
  if (!m_current_render_state) {
    return;
  }
  std::optional<u64> handle;
  if (state.using_mt4hh) {
    handle = m_context.texture_pool->lookup_mt4hh(state.texture_base_ptr);
  } else {
    handle = m_context.texture_pool->lookup(state.texture_base_ptr);
  }
  if (!handle) {
    handle = m_context.texture_pool->get_placeholder_texture();
  }

  m_impl->texture = metal_texture_from_handle(*handle);
  m_impl->sampler = m_impl->sampler_for(state.enable_tex_filt, state.m_clamp_state.clamp_s,
                                        state.m_clamp_state.clamp_t);
}

void MetalDirect::flush_draws() {
  // The state is applied even when there is nothing to draw: the sky sets up its textures with an
  // empty flush, which is what the OpenGL sibling's comment about sky textures means.
  if (m_blend_state_needs_gl_update) {
    m_impl->color_mult = 1.f;
    m_impl->alpha_mult = 1.f;
    if (m_blend_state.alpha_blend_enable && m_blend_state.a == GsAlpha::BlendMode::SOURCE &&
        m_blend_state.b == GsAlpha::BlendMode::ZERO_OR_FIXED &&
        m_blend_state.c == GsAlpha::BlendMode::DEST &&
        m_blend_state.d == GsAlpha::BlendMode::DEST) {
      m_impl->color_mult = 0.5f;
    }
    m_prim_gl_state_needs_gl_update = true;
    m_blend_state_needs_gl_update = false;
  }
  if (m_prim_gl_state_needs_gl_update) {
    m_prim_gl_state_needs_gl_update = false;
  }
  if (m_test_state_needs_gl_update) {
    // Same rule DirectRenderer::update_gl_test uses: an alpha-fail that keeps only the
    // framebuffer, or only the colour, is a draw that has to happen twice.
    m_test_state_needs_double_draw = m_test_state.afail == GsTest::AlphaFail::FB_ONLY ||
                                     m_test_state.afail == GsTest::AlphaFail::RGB_ONLY;
    m_test_state_needs_gl_update = false;
  }

  for (int i = 0; i < TEXTURE_STATE_COUNT; i++) {
    auto& tex_state = m_buffered_tex_state[i];
    if (tex_state.used) {
      backend_bind_texture(i);
      tex_state.used = false;
      m_buffered_tex_state_currently_bound[i] = true;
    } else {
      m_buffered_tex_state_currently_bound[i] = false;
    }
  }
  m_next_free_tex_state = 0;
  m_current_tex_state_idx = -1;

  if (m_prim_buffer.vert_count == 0 || !m_current_render_state ||
      !m_current_render_state->encoder || !m_impl->texture) {
    m_prim_buffer.vert_count = 0;
    return;
  }

  id<MTLRenderCommandEncoder> encoder = m_current_render_state->encoder;
  VertexChunk& chunk = m_impl->take_chunk(m_current_render_state->frame_index);
  memcpy([chunk.buffer contents], m_prim_buffer.vertices.data(),
         m_prim_buffer.vert_count * sizeof(Vertex));

  // The alpha test, which is a uniform rather than state on both backends.
  float alpha_min = 0.f;
  float alpha_max = 10.f;
  int greater = 0;
  if (m_test_state.alpha_test_enable) {
    switch (m_test_state.alpha_test) {
      case GsTest::AlphaTest::GEQUAL:
        alpha_min = m_test_state.aref / 128.f;
        m_double_draw_aref = alpha_min;
        break;
      case GsTest::AlphaTest::GREATER:
        alpha_min = (1 + m_test_state.aref) / 128.f;
        m_double_draw_aref = alpha_min;
        greater = 1;
        break;
      default:
        break;
    }
  }

  DirectUniforms uniforms{};
  const auto& fog = m_current_render_state->fog_color;
  uniforms.fog_color = {fog[0] / 255.f, fog[1] / 255.f, fog[2] / 255.f,
                        m_current_render_state->fog_intensity / 255.f};
  uniforms.game_sizes = {kGameWidth, kJak1GameHeight,
                         (float)m_current_render_state->viewport_width,
                         (float)m_current_render_state->viewport_height};
  uniforms.alpha_min = alpha_min;
  uniforms.alpha_max = alpha_max;
  uniforms.color_mult = m_impl->color_mult;
  uniforms.alpha_mult = m_impl->alpha_mult;
  uniforms.ta0 = m_prim_gl_state.ta0 / 255.f;
  uniforms.scissor_adjust = kJak1ScissorAdjust;
  uniforms.height_scale = kJak1HeightScale;
  uniforms.scissor_enable = m_scissor_enable ? 1 : 0;
  uniforms.greater = greater;
  uniforms.offscreen_mode = 0;

  PipelineKey key;
  key.blend_a = (u8)m_blend_state.a;
  key.blend_b = (u8)m_blend_state.b;
  key.blend_c = (u8)m_blend_state.c;
  key.blend_d = (u8)m_blend_state.d;
  key.blend_enable = m_blend_state.alpha_blend_enable;
  key.write_rgb = m_test_state.write_rgb;

  id<MTLRenderPipelineState> pso = m_impl->pipeline(key);
  if (!pso) {
    m_prim_buffer.vert_count = 0;
    return;
  }

  // The game disables depth writes by setting the alpha test to NEVER with FB_ONLY.
  const bool alpha_trick_to_disable = m_test_state.alpha_test_enable &&
                                      m_test_state.alpha_test == GsTest::AlphaTest::NEVER &&
                                      m_test_state.afail == GsTest::AlphaFail::FB_ONLY;
  const bool depth_write = m_test_state.depth_writes && !alpha_trick_to_disable;

  [encoder setRenderPipelineState:pso];
  [encoder setBlendColorRed:0 green:0 blue:0 alpha:m_blend_state.fix / 127.f];
  [encoder setVertexBuffer:chunk.buffer offset:0 atIndex:MetalBufferIndexVertex];
  [encoder setFragmentTexture:m_impl->texture atIndex:0];
  [encoder setFragmentSamplerState:m_impl->sampler atIndex:0];

  auto encode = [&](u32 first, u32 count) {
    [encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:MetalBufferIndexUniforms];
    [encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:MetalBufferIndexUniforms];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:first vertexCount:count];
    m_last_frame_tris += count / 3;
  };

  if (m_test_state_needs_double_draw) {
    // The alpha-fail case: the pixels that pass the test write depth, the ones that fail write
    // only colour. Two draws with the same vertices and different depth state.
    [encoder setDepthStencilState:m_impl->depth_state(m_test_state.ztst, true)];
    uniforms.alpha_min = m_double_draw_aref;
    uniforms.alpha_max = 10.f;
    encode(0, m_prim_buffer.vert_count);

    [encoder setDepthStencilState:m_impl->depth_state(m_test_state.ztst, false)];
    uniforms.alpha_min = -10.f;
    uniforms.alpha_max = m_double_draw_aref;
    encode(0, m_prim_buffer.vert_count);

    m_test_state_needs_double_draw = false;
    m_test_state_needs_gl_update = true;
    m_prim_gl_state_needs_gl_update = true;
  } else {
    [encoder setDepthStencilState:m_impl->depth_state(m_test_state.ztst, depth_write)];
    encode(0, m_prim_buffer.vert_count);
  }

  m_stats.triangles += m_prim_buffer.vert_count / 3;
  m_prim_buffer.vert_count = 0;
}

MetalDirectBucketRenderer::MetalDirectBucketRenderer(const std::string& name,
                                                     int my_id,
                                                     int batch_size)
    : MetalBucketRenderer(name, my_id), m_direct(name, batch_size) {}

bool MetalDirectBucketRenderer::init(MetalRenderState* render_state) {
  return m_direct.init(render_state);
}

void MetalDirectBucketRenderer::render(DmaFollower& dma, MetalRenderState* render_state) {
  m_direct.reset_tri_count();
  m_direct.consume_bucket_dma(dma, render_state);
}
