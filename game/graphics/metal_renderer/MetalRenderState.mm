/*!
 * @file MetalRenderState.mm
 * See MetalRenderState.h.
 */

#include "MetalRenderState.h"

#import <Metal/Metal.h>

#include "common/goal_constants.h"
#include "common/log/log.h"
#include "common/util/Timer.h"

u32 g_metal_pipeline_builds = 0;
double g_metal_pipeline_build_seconds = 0;

void MetalDrawStateCache::init(id<MTLDevice> device,
                               id<MTLFunction> vert,
                               id<MTLFunction> frag,
                               MTLVertexDescriptor* vertex_desc,
                               MTLPixelFormat color_format,
                               MTLPixelFormat depth_format,
                               u32 sample_count) {
  m_sample_count = sample_count;
  m_device = device;
  m_vert = vert;
  m_frag = frag;
  m_vertex_desc = vertex_desc;
  m_color_format = color_format;
  m_depth_format = depth_format;
}

id<MTLRenderPipelineState> MetalDrawStateCache::pipeline(DrawMode mode,
                                                         MTLColorWriteMask write_mask) {
  const u64 key = mode.as_int() | ((u64)write_mask << 32);
  auto it = m_pipelines.find(key);
  if (it != m_pipelines.end()) {
    return it->second;
  }

  MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
  desc.vertexFunction = m_vert;
  desc.fragmentFunction = m_frag;
  desc.vertexDescriptor = m_vertex_desc;
  desc.rasterSampleCount = m_sample_count;
  desc.depthAttachmentPixelFormat = m_depth_format;
  // A pipeline's attachment formats have to match the render pass it is used in, and the frame's
  // pass carries a stencil attachment because the shadow renderer needs one. A combined
  // depth-stencil format therefore has to be declared on both slots, or every draw fails.
  if (m_depth_format == MTLPixelFormatDepth32Float_Stencil8 ||
      m_depth_format == MTLPixelFormatDepth24Unorm_Stencil8) {
    desc.stencilAttachmentPixelFormat = m_depth_format;
  }
  auto* color = desc.colorAttachments[0];
  color.pixelFormat = m_color_format;
  color.writeMask = write_mask;

  // Mirrors setup_opengl_from_draw_mode in background_common.cpp, which is the definition of what
  // each mode means. A divergence here is a visual difference.
  bool blend = mode.get_ab_enable() && mode.get_alpha_blend() != DrawMode::AlphaBlend::DISABLED;
  if (blend) {
    color.rgbBlendOperation = MTLBlendOperationAdd;
    color.alphaBlendOperation = MTLBlendOperationAdd;
    color.sourceAlphaBlendFactor = MTLBlendFactorOne;
    color.destinationAlphaBlendFactor = MTLBlendFactorZero;
    switch (mode.get_alpha_blend()) {
      case DrawMode::AlphaBlend::SRC_SRC_SRC_SRC:
        // (SRC - SRC) * alpha + SRC = SRC: nothing to blend.
        blend = false;
        break;
      case DrawMode::AlphaBlend::SRC_DST_SRC_DST:
        color.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        break;
      case DrawMode::AlphaBlend::SRC_0_SRC_DST:
        color.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        color.destinationRGBBlendFactor = MTLBlendFactorOne;
        break;
      case DrawMode::AlphaBlend::SRC_0_FIX_DST:
        color.sourceRGBBlendFactor = MTLBlendFactorOne;
        color.destinationRGBBlendFactor = MTLBlendFactorOne;
        break;
      case DrawMode::AlphaBlend::SRC_DST_FIX_DST:
        // Cv = (Cs - Cd) * FIX + Cd, with FIX = 0.5. OpenGL uses a blend constant; on Metal the
        // constant comes from the encoder, which the renderer sets once per frame.
        color.sourceRGBBlendFactor = MTLBlendFactorBlendColor;
        color.destinationRGBBlendFactor = MTLBlendFactorBlendColor;
        break;
      case DrawMode::AlphaBlend::ZERO_SRC_SRC_DST:
        color.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        color.destinationRGBBlendFactor = MTLBlendFactorOne;
        color.rgbBlendOperation = MTLBlendOperationReverseSubtract;
        break;
      case DrawMode::AlphaBlend::SRC_0_DST_DST:
        color.sourceRGBBlendFactor = MTLBlendFactorDestinationAlpha;
        color.destinationRGBBlendFactor = MTLBlendFactorOne;
        break;
      default:
        blend = false;
        break;
    }
  }
  color.blendingEnabled = blend;

  // Building a pipeline is a compile, and it is synchronous. Doing one in the middle of a frame
  // costs milliseconds, which is a visible hitch at 60 Hz -- so count them, and warm the cache
  // when a level is loaded rather than meeting a new draw mode mid-frame.
  Timer build_timer;
  NSError* err = nil;
  id<MTLRenderPipelineState> pso = [m_device newRenderPipelineStateWithDescriptor:desc error:&err];
  g_metal_pipeline_builds++;
  g_metal_pipeline_build_seconds += build_timer.getSeconds();
  if (!pso) {
    lg::error("[Metal] pipeline for draw mode {} failed: {}", mode.as_int(),
              err ? [[err localizedDescription] UTF8String] : "unknown error");
  }
  m_pipelines[key] = pso;
  return pso;
}

id<MTLDepthStencilState> MetalDrawStateCache::depth_state(DrawMode mode,
                                                          bool force_no_depth_write) {
  const u32 key = mode.as_int() ^ (force_no_depth_write ? 0x80000000u : 0u);
  auto it = m_depth_states.find(key);
  if (it != m_depth_states.end()) {
    return it->second;
  }

  MTLDepthStencilDescriptor* ds = [[MTLDepthStencilDescriptor alloc] init];
  if (mode.get_zt_enable()) {
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
  } else {
    ds.depthCompareFunction = MTLCompareFunctionAlways;
  }

  // The game disables depth writes by setting alpha test NEVER with FB_ONLY, which is why this
  // looks at the alpha test to answer a depth question.
  const bool alpha_hack_disables_z_write =
      mode.get_at_enable() && mode.get_alpha_test() == DrawMode::AlphaTest::NEVER &&
      mode.get_alpha_fail() == GsTest::AlphaFail::FB_ONLY;
  ds.depthWriteEnabled =
      (mode.get_depth_write_enable() && !alpha_hack_disables_z_write && !force_no_depth_write);

  id<MTLDepthStencilState> state = [m_device newDepthStencilStateWithDescriptor:ds];
  m_depth_states[key] = state;
  return state;
}

id<MTLSamplerState> MetalDrawStateCache::sampler(DrawMode mode) {
  const u32 key = (mode.get_clamp_s_enable() ? 1 : 0) | (mode.get_clamp_t_enable() ? 2 : 0) |
                  (mode.get_filt_enable() ? 4 : 0);
  auto it = m_samplers.find(key);
  if (it != m_samplers.end()) {
    return it->second;
  }

  MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
  sd.sAddressMode =
      mode.get_clamp_s_enable() ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
  sd.tAddressMode =
      mode.get_clamp_t_enable() ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
  if (mode.get_filt_enable()) {
    sd.minFilter = MTLSamplerMinMagFilterLinear;
    sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.mipFilter = MTLSamplerMipFilterLinear;
    // Anisotropic filtering, which the OpenGL backend only ever sets on the texture animator's
    // output. Most of what this game shows is ground and water seen at a grazing angle, and
    // that is exactly what plain trilinear filtering blurs: the mip level is chosen for the
    // shortest axis of the footprint, so a surface stretching away from the camera is sampled
    // from a level far coarser than it needs. Sixteen taps is the cap every Apple GPU supports
    // and it costs nothing on the draws that do not need it, since the hardware only takes the
    // extra taps where the footprint is actually stretched.
    sd.maxAnisotropy = 16;
  } else {
    sd.minFilter = MTLSamplerMinMagFilterNearest;
    sd.magFilter = MTLSamplerMinMagFilterNearest;
    sd.mipFilter = MTLSamplerMipFilterNotMipmapped;
  }
  id<MTLSamplerState> sampler = [m_device newSamplerStateWithDescriptor:sd];
  m_samplers[key] = sampler;
  return sampler;
}

id<MTLTexture> MetalRenderState::pause_and_snapshot() {
  return pause_and_snapshot_fn ? pause_and_snapshot_fn(this) : nil;
}

void MetalRenderState::resume_scene() {
  if (resume_scene_fn) {
    resume_scene_fn(this);
  }
}

id<MTLTexture> MetalRenderState::snapshot_scene() {
  id<MTLTexture> tex = pause_and_snapshot();
  resume_scene();
  return tex;
}

const u8* MetalRenderState::occlusion_for_level(int level_id) const {
  if (level_id < 0 || level_id >= (int)jak1::LEVEL_MAX) {
    return nullptr;
  }
  const auto& vis = occlusion_vis[level_id];
  return vis.valid ? vis.data : nullptr;
}

void MetalEmptyBucketRenderer::render(DmaFollower& dma, MetalRenderState* render_state) {
  while (dma.current_tag_offset() != render_state->next_bucket && !dma.ended()) {
    dma.read_and_advance();
  }
}
