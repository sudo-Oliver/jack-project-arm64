/*!
 * @file MetalShadow.mm
 * See MetalShadow.h.
 */

#include "MetalShadow.h"

#import <Metal/Metal.h>

#include <array>
#include <cstring>

#include "common/log/log.h"

#include "metal_shader_types.h"

namespace {

// One volume is at most ShadowRendererCore::MAX_VERTICES vertices, and the bucket draws once per
// frame, so the buffers are sized for the worst case rather than bump-allocated.
constexpr NSUInteger kVertexBytes = ShadowRendererCore::MAX_VERTICES * sizeof(ShadowRendererCore::Vertex);
constexpr NSUInteger kIndexBytes = 2 * ShadowRendererCore::MAX_INDICES * sizeof(u32);

}  // namespace

struct MetalShadow::Impl {
  id<MTLDevice> device = nil;
  bool ready = false;

  // Three pipelines, differing only in the colour write mask: the two counting passes write no
  // colour at all, the shading pass writes RGB but not alpha.
  id<MTLRenderPipelineState> pso_count = nil;
  id<MTLRenderPipelineState> pso_shade = nil;

  // ...and three depth-stencil states, which is where the actual stencil work is described.
  id<MTLDepthStencilState> ds_front = nil;  // increment where the volume's front faces pass depth
  id<MTLDepthStencilState> ds_back = nil;   // decrement where its back faces do
  id<MTLDepthStencilState> ds_shade = nil;  // draw only where the two disagree

  std::array<id<MTLBuffer>, kMetalFramesInFlight> vertex_buffers = {nil, nil, nil};
  std::array<id<MTLBuffer>, kMetalFramesInFlight> index_buffers = {nil, nil, nil};
  int frame = 0;

  void release() {
    for (int i = 0; i < kMetalFramesInFlight; i++) {
      vertex_buffers[i] = nil;
      index_buffers[i] = nil;
    }
  }
};

MetalShadow::MetalShadow(const std::string& name, int my_id)
    : MetalBucketRenderer(name, my_id), m_impl(std::make_unique<Impl>()) {}

MetalShadow::~MetalShadow() {
  if (m_impl) {
    m_impl->release();
  }
}

bool MetalShadow::init(MetalRenderState* render_state) {
  m_impl->device = render_state->device;

  id<MTLFunction> vert = [render_state->library newFunctionWithName:@"shadow_vert"];
  id<MTLFunction> frag = [render_state->library newFunctionWithName:@"shadow_frag"];
  if (!vert || !frag) {
    lg::error("[Metal] shadow shader entry points missing from the library");
    return false;
  }

  MTLVertexDescriptor* vd = [[MTLVertexDescriptor alloc] init];
  vd.attributes[0].format = MTLVertexFormatFloat3;
  vd.attributes[0].offset = offsetof(Vertex, xyz);
  vd.attributes[0].bufferIndex = MetalBufferIndexVertex;
  vd.layouts[MetalBufferIndexVertex].stride = sizeof(Vertex);
  vd.layouts[MetalBufferIndexVertex].stepFunction = MTLVertexStepFunctionPerVertex;

  auto make_pipeline = [&](MTLColorWriteMask mask, bool blend) -> id<MTLRenderPipelineState> {
    MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
    desc.vertexFunction = vert;
    desc.fragmentFunction = frag;
    desc.vertexDescriptor = vd;
    desc.depthAttachmentPixelFormat = render_state->depth_format;
    desc.stencilAttachmentPixelFormat = render_state->depth_format;
    auto* color = desc.colorAttachments[0];
    color.pixelFormat = render_state->color_format;
    color.writeMask = mask;
    if (blend) {
      // glBlendFuncSeparate(GL_DST_COLOR, GL_ZERO, GL_ONE, GL_ZERO): the shadow multiplies what is
      // already there, which is what makes it darken rather than tint.
      color.blendingEnabled = YES;
      color.rgbBlendOperation = MTLBlendOperationAdd;
      color.alphaBlendOperation = MTLBlendOperationAdd;
      color.sourceRGBBlendFactor = MTLBlendFactorDestinationColor;
      color.destinationRGBBlendFactor = MTLBlendFactorZero;
      color.sourceAlphaBlendFactor = MTLBlendFactorOne;
      color.destinationAlphaBlendFactor = MTLBlendFactorZero;
    }
    NSError* err = nil;
    id<MTLRenderPipelineState> pso =
        [m_impl->device newRenderPipelineStateWithDescriptor:desc error:&err];
    if (!pso) {
      lg::error("[Metal] shadow pipeline failed: {}",
                err ? [[err localizedDescription] UTF8String] : "unknown error");
    }
    return pso;
  };

  m_impl->pso_count = make_pipeline(0, false);
  m_impl->pso_shade = make_pipeline(
      MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue, true);
  if (!m_impl->pso_count || !m_impl->pso_shade) {
    return false;
  }

  // The counting passes: depth test GEQUAL with no depth write, stencil always passes, and the
  // count moves only where the fragment also passed the depth test.
  auto make_counting_state = [&](MTLStencilOperation op) -> id<MTLDepthStencilState> {
    MTLStencilDescriptor* sd = [[MTLStencilDescriptor alloc] init];
    sd.stencilCompareFunction = MTLCompareFunctionAlways;
    sd.stencilFailureOperation = MTLStencilOperationKeep;
    sd.depthFailureOperation = MTLStencilOperationKeep;
    sd.depthStencilPassOperation = op;
    sd.readMask = 0xFF;
    sd.writeMask = 0xFF;

    MTLDepthStencilDescriptor* dd = [[MTLDepthStencilDescriptor alloc] init];
    dd.depthCompareFunction = MTLCompareFunctionGreaterEqual;
    dd.depthWriteEnabled = NO;
    dd.frontFaceStencil = sd;
    dd.backFaceStencil = sd;
    return [m_impl->device newDepthStencilStateWithDescriptor:dd];
  };
  m_impl->ds_front = make_counting_state(MTLStencilOperationIncrementClamp);
  m_impl->ds_back = make_counting_state(MTLStencilOperationDecrementClamp);

  {
    // The shading pass: no depth test at all, and only where the stencil count is not zero.
    MTLStencilDescriptor* sd = [[MTLStencilDescriptor alloc] init];
    sd.stencilCompareFunction = MTLCompareFunctionNotEqual;
    sd.stencilFailureOperation = MTLStencilOperationKeep;
    sd.depthFailureOperation = MTLStencilOperationKeep;
    sd.depthStencilPassOperation = MTLStencilOperationKeep;
    sd.readMask = 0xFF;
    sd.writeMask = 0xFF;

    MTLDepthStencilDescriptor* dd = [[MTLDepthStencilDescriptor alloc] init];
    dd.depthCompareFunction = MTLCompareFunctionAlways;
    dd.depthWriteEnabled = NO;
    dd.frontFaceStencil = sd;
    dd.backFaceStencil = sd;
    m_impl->ds_shade = [m_impl->device newDepthStencilStateWithDescriptor:dd];
  }

  for (int i = 0; i < kMetalFramesInFlight; i++) {
    m_impl->vertex_buffers[i] =
        [m_impl->device newBufferWithLength:kVertexBytes options:MTLResourceStorageModeShared];
    m_impl->index_buffers[i] =
        [m_impl->device newBufferWithLength:kIndexBytes options:MTLResourceStorageModeShared];
    if (!m_impl->vertex_buffers[i] || !m_impl->index_buffers[i]) {
      lg::error("[Metal] shadow could not allocate its buffers");
      return false;
    }
  }

  m_impl->ready = true;
  return true;
}

void MetalShadow::render(DmaFollower& dma, MetalRenderState* render_state) {
  m_last_frame_tris = 0;
  m_current_render_state = render_state;
  m_impl->frame = (int)(render_state->frame_index % kMetalFramesInFlight);

  Context context;
  context.next_bucket = render_state->next_bucket;
  context.enabled = m_impl->ready;
  render_core(dma, context);

  m_current_render_state = nullptr;
}

void MetalShadow::draw_volumes() {
  auto* render_state = m_current_render_state;
  id<MTLRenderCommandEncoder> encoder = render_state ? render_state->encoder : nil;
  if (!encoder || !m_impl->ready) {
    return;
  }
  // The core always appends the six indices of the full-screen shading quad, so a frame with no
  // shadow in it arrives here with exactly those six and nothing to count.
  if (m_next_front_index <= 6 && m_next_back_index == 0) {
    return;
  }

  const int frame = m_impl->frame;
  id<MTLBuffer> vbuf = m_impl->vertex_buffers[frame];
  id<MTLBuffer> ibuf = m_impl->index_buffers[frame];
  memcpy([vbuf contents], m_vertices, m_next_vertex * sizeof(Vertex));
  // Front indices first, then back, in one buffer.
  auto* inds = (u32*)[ibuf contents];
  memcpy(inds, m_front_indices, m_next_front_index * sizeof(u32));
  memcpy(inds + m_next_front_index, m_back_indices, m_next_back_index * sizeof(u32));
  const u32 back_offset = m_next_front_index;
  const u32 volume_indices = m_next_front_index - 6;

  ShadowUniforms u{};
  u.scissor_adjust = 512.f / 448.f;

  [encoder setVertexBuffer:vbuf offset:0 atIndex:MetalBufferIndexVertex];
  [encoder setRenderPipelineState:m_impl->pso_count];
  [encoder setStencilReferenceValue:0];

  // Pass 1: the volume's front faces increment the count.
  u.color = {0.f, 128.f / 256.f, 0.f, 127.f / 256.f};
  [encoder setDepthStencilState:m_impl->ds_front];
  [encoder setVertexBytes:&u length:sizeof(u) atIndex:MetalBufferIndexUniforms];
  [encoder setFragmentBytes:&u length:sizeof(u) atIndex:MetalBufferIndexUniforms];
  if (volume_indices > 0) {
    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                        indexCount:volume_indices
                         indexType:MTLIndexTypeUInt32
                       indexBuffer:ibuf
                 indexBufferOffset:0];
    m_last_frame_tris += volume_indices / 3;
  }

  // Pass 2: its back faces take the count back out.
  u.color = {128.f / 256.f, 0.f, 0.f, 130.f / 256.f};
  [encoder setDepthStencilState:m_impl->ds_back];
  [encoder setVertexBytes:&u length:sizeof(u) atIndex:MetalBufferIndexUniforms];
  [encoder setFragmentBytes:&u length:sizeof(u) atIndex:MetalBufferIndexUniforms];
  if (m_next_back_index > 0) {
    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                        indexCount:m_next_back_index
                         indexType:MTLIndexTypeUInt32
                       indexBuffer:ibuf
                 indexBufferOffset:back_offset * sizeof(u32)];
    m_last_frame_tris += m_next_back_index / 3;
  }

  // Pass 3: one quad over the whole screen, kept only where the count did not come back to zero.
  u.color = {m_color.x(), m_color.y(), m_color.z(), m_color.w()};
  [encoder setRenderPipelineState:m_impl->pso_shade];
  [encoder setDepthStencilState:m_impl->ds_shade];
  [encoder setVertexBytes:&u length:sizeof(u) atIndex:MetalBufferIndexUniforms];
  [encoder setFragmentBytes:&u length:sizeof(u) atIndex:MetalBufferIndexUniforms];
  [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                      indexCount:6
                       indexType:MTLIndexTypeUInt32
                     indexBuffer:ibuf
               indexBufferOffset:volume_indices * sizeof(u32)];
  m_last_frame_tris += 2;
}
