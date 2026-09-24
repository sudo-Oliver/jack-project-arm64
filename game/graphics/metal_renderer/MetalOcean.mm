/*!
 * @file MetalOcean.mm
 * See MetalOcean.h.
 */

#include "MetalOcean.h"

#import <Metal/Metal.h>

#include <array>
#include <vector>

#include "common/log/log.h"

#include "game/graphics/gpu_resources.h"
#include "game/graphics/metal_renderer/MetalGpuResources.h"
#include "game/graphics/opengl_renderer/AdgifHandler.h"

#include "metal_shader_types.h"

namespace {

// Same constants Shader.cpp substitutes into the GLSL for Jak 1.
constexpr float kJak1ScissorAdjust = 512.0f / 448.0f;
constexpr float kJak1HeightScale = 1.0f;

constexpr int kOceanTexSize = 128;
constexpr int kOceanNumMips = 8;

// A pool of buffers safe to write while the GPU may still be running. See the one in MetalMerc2
// for why: Metal records a frame and runs it at the end, so a buffer written twice in one frame
// is only ever read with its last contents.
struct BufferPool {
  id<MTLDevice> device = nil;
  NSUInteger length = 0;
  std::vector<id<MTLBuffer>> buffers;
  size_t next = 0;
  u64 frame = UINT64_MAX;

  void begin_frame(u64 frame_index) {
    if (frame != frame_index) {
      frame = frame_index;
      next = 0;
    }
  }

  id<MTLBuffer> take() {
    if (next >= buffers.size()) {
      buffers.push_back([device newBufferWithLength:length options:MTLResourceStorageModeShared]);
    }
    return buffers[next++];
  }
};

}  // namespace

// -------------------------------------------------------------------------------------------
// MetalCommonOceanRenderer
// -------------------------------------------------------------------------------------------

struct MetalCommonOceanRenderer::Impl {
  id<MTLDevice> device = nil;
  bool ready = false;

  // The passes the ocean draws in, each a fixed blend mode, so each gets one pipeline rather than
  // a cache: near-texture, near-alpha, near-envmap, mid-texture, mid-envmap.
  std::array<id<MTLRenderPipelineState>, 5> pipelines = {nil, nil, nil, nil, nil};
  id<MTLDepthStencilState> depth_no_write = nil;
  id<MTLDepthStencilState> depth_always_write = nil;
  id<MTLSamplerState> sampler = nil;

  BufferPool vertices;
  BufferPool indices;
};

MetalCommonOceanRenderer::MetalCommonOceanRenderer() : m_impl(std::make_unique<Impl>()) {}
MetalCommonOceanRenderer::~MetalCommonOceanRenderer() = default;

bool MetalCommonOceanRenderer::init(MetalRenderState* render_state) {
  if (m_impl->ready) {
    return true;
  }
  m_impl->device = render_state->device;

  id<MTLFunction> vert = [render_state->library newFunctionWithName:@"ocean_common_vert"];
  id<MTLFunction> frag = [render_state->library newFunctionWithName:@"ocean_common_frag"];
  if (!vert || !frag) {
    lg::error("[Metal] ocean_common shader entry points missing from the library");
    return false;
  }

  // CommonOceanRendererCore::Vertex, 32 bytes: xyz at 0, rgba at 12, stq at 16, fog at 28.
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

  // The blend each pass needs, in the order the GLSL numbers them.
  struct PassBlend {
    bool enabled;
    MTLBlendFactor src;
    MTLBlendFactor dst;
  };
  const PassBlend blends[5] = {
      // near, water texture: (Cs - Cd) * As + Cd
      {true, MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha},
      // near, alpha only: keep the colour, take the alpha
      {true, MTLBlendFactorZero, MTLBlendFactorOne},
      // near, envmap: add, weighted by the destination alpha the pass above left
      {true, MTLBlendFactorDestinationAlpha, MTLBlendFactorOne},
      // mid, water texture: no blending
      {false, MTLBlendFactorOne, MTLBlendFactorZero},
      // mid, envmap
      {true, MTLBlendFactorDestinationAlpha, MTLBlendFactorOne},
  };

  for (int i = 0; i < 5; i++) {
    MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
    desc.vertexFunction = vert;
    desc.fragmentFunction = frag;
    desc.vertexDescriptor = vd;
    desc.depthAttachmentPixelFormat = render_state->depth_format;
    auto* color = desc.colorAttachments[0];
    color.pixelFormat = render_state->color_format;
    color.blendingEnabled = blends[i].enabled;
    color.rgbBlendOperation = MTLBlendOperationAdd;
    color.alphaBlendOperation = MTLBlendOperationAdd;
    color.sourceRGBBlendFactor = blends[i].src;
    color.destinationRGBBlendFactor = blends[i].dst;
    color.sourceAlphaBlendFactor = MTLBlendFactorOne;
    color.destinationAlphaBlendFactor = MTLBlendFactorZero;

    NSError* err = nil;
    m_impl->pipelines[i] = [render_state->device newRenderPipelineStateWithDescriptor:desc
                                                                                error:&err];
    if (!m_impl->pipelines[i]) {
      lg::error("[Metal] ocean_common pipeline {} failed: {}", i,
                err ? [[err localizedDescription] UTF8String] : "unknown error");
      return false;
    }
  }

  // The near ocean never writes depth; the mid ocean always does, and always passes.
  MTLDepthStencilDescriptor* ds = [[MTLDepthStencilDescriptor alloc] init];
  ds.depthCompareFunction = MTLCompareFunctionGreaterEqual;
  ds.depthWriteEnabled = NO;
  m_impl->depth_no_write = [render_state->device newDepthStencilStateWithDescriptor:ds];
  ds.depthCompareFunction = MTLCompareFunctionAlways;
  ds.depthWriteEnabled = YES;
  m_impl->depth_always_write = [render_state->device newDepthStencilStateWithDescriptor:ds];

  MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
  sd.sAddressMode = MTLSamplerAddressModeRepeat;
  sd.tAddressMode = MTLSamplerAddressModeRepeat;
  sd.minFilter = MTLSamplerMinMagFilterLinear;
  sd.magFilter = MTLSamplerMinMagFilterLinear;
  sd.mipFilter = MTLSamplerMipFilterLinear;
  m_impl->sampler = [render_state->device newSamplerStateWithDescriptor:sd];

  m_impl->vertices.device = render_state->device;
  m_impl->vertices.length = 4096 * 10 * sizeof(Vertex);
  m_impl->indices.device = render_state->device;
  m_impl->indices.length = 4096 * 10 * sizeof(u32);

  m_impl->ready = true;
  return true;
}

/*!
 * Upload what the kicks built and draw the passes named in `buckets`.
 */
void MetalCommonOceanRenderer::draw_buckets(const int* buckets, int count, bool near) {
  if (!m_impl->ready || !m_current_render_state || !m_current_render_state->encoder) {
    return;
  }
  id<MTLRenderCommandEncoder> encoder = m_current_render_state->encoder;

  m_impl->vertices.begin_frame(m_current_render_state->frame_index);
  m_impl->indices.begin_frame(m_current_render_state->frame_index);

  id<MTLBuffer> vertex_buffer = m_impl->vertices.take();
  memcpy([vertex_buffer contents], m_vertices.data(), m_next_free_vertex * sizeof(Vertex));
  [encoder setVertexBuffer:vertex_buffer offset:0 atIndex:MetalBufferIndexVertex];
  [encoder setDepthStencilState:near ? m_impl->depth_no_write : m_impl->depth_always_write];

  OceanCommonUniforms uniforms{};
  const auto& fog = m_current_render_state->fog_color;
  uniforms.fog_color = {fog[0] / 255.f, fog[1] / 255.f, fog[2] / 255.f,
                        m_current_render_state->fog_intensity / 255.f};
  uniforms.scissor_adjust = kJak1ScissorAdjust;
  uniforms.height_scale = kJak1HeightScale;

  for (int i = 0; i < count; i++) {
    const int index_bucket = i;
    const int pass = buckets[i];
    if (!m_next_free_index[index_bucket]) {
      continue;
    }

    // The water texture for the texture passes, the frame's envmap for the rest.
    const u32 tbp = (pass == 0 || pass == 3) ? OCEAN_TEX_TBP_JAK1 : m_envmap_tex;
    auto* pool = m_current_render_state->texture_pool.get();
    auto handle = pool->lookup(tbp);
    if (!handle) {
      handle = pool->get_placeholder_texture();
    }
    id<MTLTexture> texture = metal_texture_from_handle(*handle);
    if (!texture) {
      continue;
    }

    id<MTLBuffer> index_buffer = m_impl->indices.take();
    memcpy([index_buffer contents], m_indices[index_bucket].data(),
           m_next_free_index[index_bucket] * sizeof(u32));

    uniforms.bucket = pass;
    [encoder setRenderPipelineState:m_impl->pipelines[pass]];
    [encoder setFragmentTexture:texture atIndex:0];
    [encoder setFragmentSamplerState:m_impl->sampler atIndex:0];
    [encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:MetalBufferIndexUniforms];
    [encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:MetalBufferIndexUniforms];
    // UINT32_MAX restarts the strip; Metal does that unconditionally for 32-bit indices.
    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
                        indexCount:m_next_free_index[index_bucket]
                         indexType:MTLIndexTypeUInt32
                       indexBuffer:index_buffer
                 indexBufferOffset:0];
    m_last_frame_tris += m_next_free_index[index_bucket] / 3;
  }
}

void MetalCommonOceanRenderer::flush_near_draws() {
  const int passes[3] = {0, 1, 2};
  draw_buckets(passes, 3, true);
}

void MetalCommonOceanRenderer::flush_mid_draws() {
  const int passes[2] = {3, 4};
  draw_buckets(passes, 2, false);
}

// -------------------------------------------------------------------------------------------
// MetalOceanTexture
// -------------------------------------------------------------------------------------------

struct MetalOceanTexture::Impl {
  id<MTLDevice> device = nil;
  id<MTLRenderPipelineState> grid_pipeline = nil;
  id<MTLRenderPipelineState> mipmap_pipeline = nil;
  id<MTLDepthStencilState> no_depth = nil;
  id<MTLSamplerState> sampler = nil;
  bool ready = false;

  // The texture the ocean samples, and the scratch one the grid is drawn into when a mip chain
  // has to be built from it.
  u64 result_handle = gpu::kInvalidHandle;
  id<MTLTexture> temp_texture = nil;
  GpuTexture* tex0_gpu = nullptr;

  // The static grid positions, and the index list. Both are built once.
  id<MTLBuffer> positions = nil;
  id<MTLBuffer> index_buffer = nil;
  u32 index_count = 0;
  BufferPool dynamic;

  // Only alive between backend_begin_pass and backend_end_pass.
  id<MTLRenderCommandEncoder> pass_encoder = nil;
  bool pass_is_temp = false;
};

MetalOceanTexture::MetalOceanTexture(bool generate_mipmaps)
    : OceanTextureCore(generate_mipmaps), m_impl(std::make_unique<Impl>()) {}

MetalOceanTexture::~MetalOceanTexture() {
  gpu::destroy_texture(m_impl->result_handle);
}

bool MetalOceanTexture::init(MetalRenderState* render_state) {
  if (m_impl->ready) {
    return true;
  }
  m_impl->device = render_state->device;

  id<MTLFunction> vert = [render_state->library newFunctionWithName:@"ocean_texture_vert"];
  id<MTLFunction> frag = [render_state->library newFunctionWithName:@"ocean_texture_frag"];
  id<MTLFunction> mip_vert =
      [render_state->library newFunctionWithName:@"ocean_texture_mipmap_vert"];
  id<MTLFunction> mip_frag =
      [render_state->library newFunctionWithName:@"ocean_texture_mipmap_frag"];
  if (!vert || !frag || !mip_vert || !mip_frag) {
    lg::error("[Metal] ocean_texture shader entry points missing from the library");
    return false;
  }

  // The positions are a separate, static buffer; the colours and texture coordinates change every
  // frame, so they come from a second one.
  MTLVertexDescriptor* vd = [[MTLVertexDescriptor alloc] init];
  vd.attributes[0].format = MTLVertexFormatFloat2;
  vd.attributes[0].offset = 0;
  vd.attributes[0].bufferIndex = MetalBufferIndexVertex;
  vd.attributes[1].format = MTLVertexFormatUChar4Normalized;
  vd.attributes[1].offset = offsetof(Vertex, rgba);
  vd.attributes[1].bufferIndex = MetalBufferIndexTimeOfDay;
  vd.attributes[2].format = MTLVertexFormatFloat2;
  vd.attributes[2].offset = offsetof(Vertex, s);
  vd.attributes[2].bufferIndex = MetalBufferIndexTimeOfDay;
  vd.layouts[MetalBufferIndexVertex].stride = sizeof(math::Vector2f);
  vd.layouts[MetalBufferIndexVertex].stepFunction = MTLVertexStepFunctionPerVertex;
  vd.layouts[MetalBufferIndexTimeOfDay].stride = sizeof(Vertex);
  vd.layouts[MetalBufferIndexTimeOfDay].stepFunction = MTLVertexStepFunctionPerVertex;

  MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
  desc.vertexFunction = vert;
  desc.fragmentFunction = frag;
  desc.vertexDescriptor = vd;
  desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
  NSError* err = nil;
  m_impl->grid_pipeline = [render_state->device newRenderPipelineStateWithDescriptor:desc
                                                                               error:&err];
  if (!m_impl->grid_pipeline) {
    lg::error("[Metal] ocean_texture pipeline failed: {}",
              err ? [[err localizedDescription] UTF8String] : "unknown error");
    return false;
  }

  MTLRenderPipelineDescriptor* mip_desc = [[MTLRenderPipelineDescriptor alloc] init];
  mip_desc.vertexFunction = mip_vert;
  mip_desc.fragmentFunction = mip_frag;
  mip_desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
  m_impl->mipmap_pipeline = [render_state->device newRenderPipelineStateWithDescriptor:mip_desc
                                                                                 error:&err];
  if (!m_impl->mipmap_pipeline) {
    lg::error("[Metal] ocean_texture_mipmap pipeline failed: {}",
              err ? [[err localizedDescription] UTF8String] : "unknown error");
    return false;
  }

  MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
  sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
  sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
  sd.minFilter = MTLSamplerMinMagFilterLinear;
  sd.magFilter = MTLSamplerMinMagFilterLinear;
  m_impl->sampler = [render_state->device newSamplerStateWithDescriptor:sd];

  m_impl->positions = [render_state->device newBufferWithBytes:m_pc.vertex_positions.data()
                                                        length:m_pc.vertex_positions.size() *
                                                               sizeof(math::Vector2f)
                                                       options:MTLResourceStorageModeShared];
  m_impl->index_count = (u32)m_pc.index_buffer.size();
  m_impl->index_buffer = [render_state->device newBufferWithBytes:m_pc.index_buffer.data()
                                                           length:m_impl->index_count * sizeof(u32)
                                                          options:MTLResourceStorageModeShared];
  m_impl->dynamic.device = render_state->device;
  m_impl->dynamic.length = NUM_VERTS * sizeof(Vertex);

  if (m_generate_mipmaps) {
    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:kOceanTexSize
                                                          height:kOceanTexSize
                                                       mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    m_impl->temp_texture = [render_state->device newTextureWithDescriptor:td];
  }

  m_impl->ready = true;
  return true;
}

void MetalOceanTexture::init_textures(TexturePool& pool, GameVersion version) {
  gpu::TextureCreateInfo info;
  info.w = kOceanTexSize;
  info.h = kOceanTexSize;
  info.data = nullptr;
  info.mipmap = m_generate_mipmaps;
  info.anisotropic = false;
  info.render_target = true;
  m_impl->result_handle = gpu::create_texture_rgba8(info);

  TextureInput in;
  in.gpu_texture = m_impl->result_handle;
  in.w = kOceanTexSize;
  in.h = kOceanTexSize;
  in.debug_page_name = "PC-OCEAN";
  in.debug_name = fmt::format("pc-ocean-mip-{}", m_generate_mipmaps);
  in.id = pool.allocate_pc_port_texture(version);
  m_impl->tex0_gpu = pool.give_texture_and_load_to_vram(in, OCEAN_TEX_TBP_JAK1);
  m_tex0_gpu = m_impl->tex0_gpu;
}

void MetalOceanTexture::backend_begin_pass(bool to_temp) {
  if (!m_impl->ready || !m_current_render_state || !m_current_render_state->offscreen_cmd) {
    return;
  }
  id<MTLTexture> target =
      to_temp ? m_impl->temp_texture : metal_texture_from_handle(m_impl->result_handle);
  if (!target) {
    return;
  }

  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = target;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  m_impl->pass_encoder =
      [m_current_render_state->offscreen_cmd renderCommandEncoderWithDescriptor:pass];
  m_impl->pass_is_temp = to_temp;
}

void MetalOceanTexture::backend_end_pass() {
  if (m_impl->pass_encoder) {
    [m_impl->pass_encoder endEncoding];
    m_impl->pass_encoder = nil;
  }
}

void MetalOceanTexture::backend_flush() {
  if (!m_impl->pass_encoder) {
    return;
  }
  m_impl->dynamic.begin_frame(m_current_render_state->frame_index);
  id<MTLBuffer> dynamic = m_impl->dynamic.take();
  memcpy([dynamic contents], m_pc.vertex_dynamic.data(), NUM_VERTS * sizeof(Vertex));

  GsTex0 tex0(m_envmap_adgif.tex0_data);
  auto handle = m_texture_pool->lookup(tex0.tbp0());
  if (!handle) {
    handle = m_texture_pool->get_placeholder_texture();
  }
  id<MTLTexture> texture = metal_texture_from_handle(*handle);
  if (!texture) {
    return;
  }

  id<MTLRenderCommandEncoder> enc = m_impl->pass_encoder;
  [enc setRenderPipelineState:m_impl->grid_pipeline];
  [enc setVertexBuffer:m_impl->positions offset:0 atIndex:MetalBufferIndexVertex];
  [enc setVertexBuffer:dynamic offset:0 atIndex:MetalBufferIndexTimeOfDay];
  [enc setFragmentTexture:texture atIndex:0];
  [enc setFragmentSamplerState:m_impl->sampler atIndex:0];
  [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
                  indexCount:m_impl->index_count
                   indexType:MTLIndexTypeUInt32
                 indexBuffer:m_impl->index_buffer
           indexBufferOffset:0];
}

void MetalOceanTexture::backend_make_texture_with_mipmaps() {
  if (!m_impl->ready || !m_current_render_state || !m_current_render_state->offscreen_cmd) {
    return;
  }
  id<MTLTexture> result = metal_texture_from_handle(m_impl->result_handle);
  if (!result || !m_impl->temp_texture) {
    return;
  }

  // One pass per mip level, each the size of that level, fading the alpha out as they shrink.
  // The OpenGL sibling scales a quad instead, because its framebuffer for a mip level kept the
  // full-size viewport; giving each level a pass of its own size is the same picture.
  for (int i = 0; i < kOceanNumMips && i < (int)result.mipmapLevelCount; i++) {
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = result;
    pass.colorAttachments[0].level = i;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc =
        [m_current_render_state->offscreen_cmd renderCommandEncoderWithDescriptor:pass];

    OceanMipmapUniforms uniforms{};
    uniforms.alpha_intensity = std::max(0.f, 1.f - 0.51f * i);
    [enc setRenderPipelineState:m_impl->mipmap_pipeline];
    [enc setFragmentTexture:m_impl->temp_texture atIndex:0];
    [enc setFragmentSamplerState:m_impl->sampler atIndex:0];
    [enc setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:MetalBufferIndexUniforms];
    [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [enc endEncoding];
  }
}

// -------------------------------------------------------------------------------------------
// The buckets
// -------------------------------------------------------------------------------------------

MetalOceanMidAndFar::MetalOceanMidAndFar(const std::string& name, int my_id)
    : MetalBucketRenderer(name, my_id),
      m_direct(name, 4096),
      m_texture_renderer(true),
      m_mid_renderer(m_common) {}

bool MetalOceanMidAndFar::init(MetalRenderState* render_state) {
  return m_direct.init(render_state) && m_texture_renderer.init(render_state) &&
         m_common.init(render_state);
}

void MetalOceanMidAndFar::init_textures(TexturePool& pool, GameVersion version) {
  m_texture_renderer.init_textures(pool, version);
}

void MetalOceanMidAndFar::handle_ocean_far(DmaFollower& dma, MetalRenderState* render_state) {
  auto init_data = dma.read_and_advance();
  ASSERT(init_data.size_bytes == 160);
  u8 init_data_buffer[160];
  memcpy(init_data_buffer, init_data.data, 160);

  // Patch ta0 to 0, the same hack the OpenGL sibling does.
  u8 val = 0;
  memcpy(init_data_buffer + 80, &val, 1);
  m_direct.render_gif(init_data_buffer, 160, render_state);

  while (dma.current_tag().kind == DmaTag::Kind::CNT &&
         dma.current_tag_vifcode0().kind == VifCode::Kind::NOP) {
    auto data = dma.read_and_advance();
    m_direct.render_gif(data.data, data.size_bytes, render_state);
  }
}

namespace {
bool ocean_is_end_tag(const DmaTag& tag, const VifCode& v0, const VifCode& v1) {
  return tag.qwc == 0 && tag.kind == DmaTag::Kind::NEXT && v0.kind == VifCode::Kind::NOP &&
         v1.kind == VifCode::Kind::NOP;
}
}  // namespace

void MetalOceanMidAndFar::handle_ocean_mid(DmaFollower& dma, MetalRenderState* render_state) {
  if (dma.current_tag_vifcode0().kind == VifCode::Kind::BASE) {
    m_common.set_frame_context(render_state);
    m_mid_renderer.run(dma);
    m_common.set_frame_context(nullptr);
  } else {
    return;
  }

  while (!ocean_is_end_tag(dma.current_tag(), dma.current_tag_vifcode0(),
                           dma.current_tag_vifcode1())) {
    dma.read_and_advance();
  }
}

void MetalOceanMidAndFar::render(DmaFollower& dma, MetalRenderState* render_state) {
  m_common.reset_tri_count();
  m_direct.reset_tri_count();

  auto data0 = dma.read_and_advance();
  ASSERT(data0.size_bytes == 0);

  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    // The renderer did not run this frame.
    for (int i = 0; i < 4; i++) {
      dma.read_and_advance();
    }
    return;
  }
  m_direct.reset_state();

  m_texture_renderer.set_frame_context(render_state);
  m_texture_renderer.handle_ocean_texture_jak1(dma, render_state->texture_pool.get());
  m_texture_renderer.set_frame_context(nullptr);

  handle_ocean_far(dma, render_state);
  m_direct.flush_pending(render_state);

  m_direct.set_mipmap(true);
  handle_ocean_mid(dma, render_state);

  dma.read_and_advance();  // the final NEXT
  for (int i = 0; i < 4; i++) {
    dma.read_and_advance();
  }

  m_direct.flush_pending(render_state);
  m_direct.set_mipmap(false);
}

MetalOceanNear::MetalOceanNear(const std::string& name, int my_id)
    : MetalBucketRenderer(name, my_id),
      m_texture_renderer(false),
      m_core(m_texture_renderer, m_common) {}

bool MetalOceanNear::init(MetalRenderState* render_state) {
  return m_texture_renderer.init(render_state) && m_common.init(render_state);
}

void MetalOceanNear::init_textures(TexturePool& pool, GameVersion version) {
  m_texture_renderer.init_textures(pool, version);
}

void MetalOceanNear::render(DmaFollower& dma, MetalRenderState* render_state) {
  m_common.reset_tri_count();
  m_texture_renderer.set_frame_context(render_state);
  m_common.set_frame_context(render_state);
  m_core.render_jak1(dma, render_state->texture_pool.get(), render_state->next_bucket);
  m_texture_renderer.set_frame_context(nullptr);
  m_common.set_frame_context(nullptr);
}
