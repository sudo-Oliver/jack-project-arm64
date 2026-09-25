/*!
 * @file MetalDepthCue.mm
 * See MetalDepthCue.h.
 */

#include "MetalDepthCue.h"

#import <Metal/Metal.h>

#include <cstring>

#include "common/log/log.h"

#include "metal_shader_types.h"

struct MetalDepthCue::Impl {
  id<MTLDevice> device = nil;
  bool ready = false;

  // Two pipelines: the pass into the scratch page replaces what is there, the pass back over the
  // frame blends. Both run the same shader.
  id<MTLRenderPipelineState> pso_page = nil;
  id<MTLRenderPipelineState> pso_screen = nil;
  // No depth test and no depth write: this is an overlay, and the game's own GS setup asserts
  // ZTest::ALWAYS with zmsk set.
  id<MTLDepthStencilState> depth_state = nil;
  id<MTLSamplerState> sampler = nil;

  // The scratch page. Sized by the core; recreated when it changes.
  id<MTLTexture> page_texture = nil;
  int page_w = 0, page_h = 0;

  id<MTLBuffer> page_vertices = nil;
  id<MTLBuffer> screen_vertices = nil;

  void release() {
    page_texture = nil;
    page_vertices = nil;
    screen_vertices = nil;
  }
};

MetalDepthCue::MetalDepthCue(const std::string& name, int my_id)
    : MetalBucketRenderer(name, my_id), m_impl(std::make_unique<Impl>()) {}

MetalDepthCue::~MetalDepthCue() {
  if (m_impl) {
    m_impl->release();
  }
}

bool MetalDepthCue::init(MetalRenderState* render_state) {
  m_impl->device = render_state->device;

  id<MTLFunction> vert = [render_state->library newFunctionWithName:@"depth_cue_vert"];
  id<MTLFunction> frag = [render_state->library newFunctionWithName:@"depth_cue_frag"];
  if (!vert || !frag) {
    lg::error("[Metal] depth_cue shader entry points missing from the library");
    return false;
  }

  MTLVertexDescriptor* vd = [[MTLVertexDescriptor alloc] init];
  vd.attributes[0].format = MTLVertexFormatFloat2;
  vd.attributes[0].offset = offsetof(SpriteVertex, xy);
  vd.attributes[0].bufferIndex = MetalBufferIndexVertex;
  vd.attributes[1].format = MTLVertexFormatFloat2;
  vd.attributes[1].offset = offsetof(SpriteVertex, st);
  vd.attributes[1].bufferIndex = MetalBufferIndexVertex;
  vd.layouts[MetalBufferIndexVertex].stride = sizeof(SpriteVertex);
  vd.layouts[MetalBufferIndexVertex].stepFunction = MTLVertexStepFunctionPerVertex;

  // The scratch-page pass: its own target, no depth attachment at all, and it overwrites.
  {
    MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
    desc.vertexFunction = vert;
    desc.fragmentFunction = frag;
    desc.vertexDescriptor = vd;
    desc.colorAttachments[0].pixelFormat = render_state->color_format;
    NSError* err = nil;
    m_impl->pso_page = [m_impl->device newRenderPipelineStateWithDescriptor:desc error:&err];
    if (!m_impl->pso_page) {
      lg::error("[Metal] depth_cue page pipeline failed: {}",
                err ? [[err localizedDescription] UTF8String] : "unknown error");
      return false;
    }
  }

  // The pass back over the frame: the frame's attachments, and the same blend the OpenGL
  // renderer uses -- src alpha over the colour, and the frame's alpha left alone.
  {
    MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
    desc.vertexFunction = vert;
    desc.fragmentFunction = frag;
    desc.vertexDescriptor = vd;
    desc.depthAttachmentPixelFormat = render_state->depth_format;
    if (render_state->depth_format == MTLPixelFormatDepth32Float_Stencil8) {
      desc.stencilAttachmentPixelFormat = render_state->depth_format;
    }
    auto* color = desc.colorAttachments[0];
    color.pixelFormat = render_state->color_format;
    color.blendingEnabled = YES;
    color.rgbBlendOperation = MTLBlendOperationAdd;
    color.alphaBlendOperation = MTLBlendOperationAdd;
    color.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    color.sourceAlphaBlendFactor = MTLBlendFactorZero;
    color.destinationAlphaBlendFactor = MTLBlendFactorOne;
    NSError* err = nil;
    m_impl->pso_screen = [m_impl->device newRenderPipelineStateWithDescriptor:desc error:&err];
    if (!m_impl->pso_screen) {
      lg::error("[Metal] depth_cue screen pipeline failed: {}",
                err ? [[err localizedDescription] UTF8String] : "unknown error");
      return false;
    }
  }

  MTLDepthStencilDescriptor* dd = [[MTLDepthStencilDescriptor alloc] init];
  dd.depthCompareFunction = MTLCompareFunctionAlways;
  dd.depthWriteEnabled = NO;
  m_impl->depth_state = [m_impl->device newDepthStencilStateWithDescriptor:dd];

  // The blur is the whole point: both passes sample with a linear filter, clamped.
  MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
  sd.minFilter = MTLSamplerMinMagFilterLinear;
  sd.magFilter = MTLSamplerMinMagFilterLinear;
  sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
  sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
  m_impl->sampler = [m_impl->device newSamplerStateWithDescriptor:sd];

  const NSUInteger vertex_bytes = 6 * TOTAL_DRAW_SLICES * sizeof(SpriteVertex);
  m_impl->page_vertices = [m_impl->device newBufferWithLength:vertex_bytes
                                                      options:MTLResourceStorageModeShared];
  m_impl->screen_vertices = [m_impl->device newBufferWithLength:vertex_bytes
                                                        options:MTLResourceStorageModeShared];
  if (!m_impl->page_vertices || !m_impl->screen_vertices) {
    return false;
  }

  m_impl->ready = true;
  return true;
}

void MetalDepthCue::render(DmaFollower& dma, MetalRenderState* render_state) {
  m_last_frame_tris = 0;
  m_current_render_state = render_state;

  Context context;
  context.next_bucket = render_state->next_bucket;
  context.enabled = m_impl->ready;
  context.draw_region_w = (int)render_state->viewport_width;
  context.draw_region_h = (int)render_state->viewport_height;
  render_core(dma, context);

  m_current_render_state = nullptr;
}

void MetalDepthCue::do_draw() {
  auto* render_state = m_current_render_state;
  if (!render_state || !m_impl->ready || m_fbo_width <= 0 || m_fbo_height <= 0) {
    return;
  }

  // The core only rebuilds the quads and the target size when something changed.
  if (m_geometry_changed) {
    memcpy([m_impl->page_vertices contents], m_depth_cue_page_vertices.data(),
           m_depth_cue_page_vertices.size() * sizeof(SpriteVertex));
    memcpy([m_impl->screen_vertices contents], m_on_screen_vertices.data(),
           m_on_screen_vertices.size() * sizeof(SpriteVertex));
  }
  if (!m_impl->page_texture || m_impl->page_w != m_fbo_width || m_impl->page_h != m_fbo_height) {
    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:render_state->color_format
                                                           width:m_fbo_width
                                                          height:m_fbo_height
                                                       mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    m_impl->page_texture = [m_impl->device newTextureWithDescriptor:td];
    m_impl->page_w = m_fbo_width;
    m_impl->page_h = m_fbo_height;
  }
  if (!m_impl->page_texture) {
    return;
  }

  // Split the frame's pass: the scratch page has to be filled from the frame, and then read back
  // into it.
  id<MTLTexture> frame = render_state->pause_and_snapshot();
  id<MTLCommandBuffer> cmd = render_state->frame_cmd;
  if (!frame || !cmd) {
    // No hooks installed: resume anyway so the rest of the frame still has an encoder.
    render_state->resume_scene();
    return;
  }

  DepthCueUniforms u{};

  // Pass one: the frame into the scratch page, which is very slightly smaller than it came from.
  // That size difference is the whole effect -- it makes the linear filter smear.
  {
    const auto color = page_color();
    u.u_color = {color.x(), color.y(), color.z(), color.w()};
    u.u_depth = 1.f;

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = m_impl->page_texture;
    // Every texel of the page is written by the sixteen slices, so there is nothing to load.
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> page_enc = [cmd renderCommandEncoderWithDescriptor:pass];
    [page_enc setRenderPipelineState:m_impl->pso_page];
    [page_enc setVertexBuffer:m_impl->page_vertices offset:0 atIndex:MetalBufferIndexVertex];
    [page_enc setVertexBytes:&u length:sizeof(u) atIndex:MetalBufferIndexUniforms];
    [page_enc setFragmentTexture:frame atIndex:0];
    [page_enc setFragmentSamplerState:m_impl->sampler atIndex:0];
    [page_enc drawPrimitives:MTLPrimitiveTypeTriangle
                 vertexStart:0
                 vertexCount:6 * TOTAL_DRAW_SLICES];
    [page_enc endEncoding];
    m_last_frame_tris += 2 * TOTAL_DRAW_SLICES;
  }

  // Back into the frame's pass, and draw the page over it.
  render_state->resume_scene();
  id<MTLRenderCommandEncoder> encoder = render_state->encoder;
  if (!encoder) {
    return;
  }
  {
    const auto color = screen_color();
    u.u_color = {color.x(), color.y(), color.z(), color.w()};
    u.u_depth = screen_depth();

    [encoder setRenderPipelineState:m_impl->pso_screen];
    [encoder setDepthStencilState:m_impl->depth_state];
    [encoder setVertexBuffer:m_impl->screen_vertices offset:0 atIndex:MetalBufferIndexVertex];
    [encoder setVertexBytes:&u length:sizeof(u) atIndex:MetalBufferIndexUniforms];
    [encoder setFragmentTexture:m_impl->page_texture atIndex:0];
    [encoder setFragmentSamplerState:m_impl->sampler atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0
                vertexCount:6 * TOTAL_DRAW_SLICES];
    m_last_frame_tris += 2 * TOTAL_DRAW_SLICES;
  }
}
