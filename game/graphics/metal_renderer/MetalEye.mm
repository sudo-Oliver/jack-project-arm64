/*!
 * @file MetalEye.mm
 * See MetalEye.h.
 */

#include "MetalEye.h"

#import <Metal/Metal.h>

#include <vector>

#include "common/log/log.h"

#include "game/graphics/gpu_resources.h"
#include "game/graphics/metal_renderer/MetalGpuResources.h"

#include "metal_shader_types.h"

namespace {
// The GS drew these at 32x32. Rendering them larger is what makes an eye look right at a modern
// resolution, and costs nothing: there are forty of them.
constexpr int kEyeTexSize = 128;
}  // namespace

struct MetalEyeRenderer::Impl {
  id<MTLDevice> device = nil;
  // Two pipelines: the background, iris and lid draws replace what is there, the pupil blends.
  id<MTLRenderPipelineState> opaque = nil;
  id<MTLRenderPipelineState> blended = nil;
  id<MTLSamplerState> sampler = nil;
  bool ready = false;

  u64 handles[NUM_EYE_PAIRS * 2] = {0};

  // One vertex buffer per frame, holding every quad of every eye drawn that frame.
  std::vector<id<MTLBuffer>> vertex_buffers;
  size_t next_buffer = 0;
  u64 buffer_frame = UINT64_MAX;
};

MetalEyeRenderer::MetalEyeRenderer(const std::string& name, int my_id)
    : MetalBucketRenderer(name, my_id), m_impl(std::make_unique<Impl>()) {}

MetalEyeRenderer::~MetalEyeRenderer() {
  for (auto handle : m_impl->handles) {
    if (handle) {
      gpu::destroy_texture(handle);
    }
  }
}

bool MetalEyeRenderer::init(MetalRenderState* render_state) {
  if (m_impl->ready) {
    return true;
  }
  m_impl->device = render_state->device;

  id<MTLFunction> vert = [render_state->library newFunctionWithName:@"eye_vert"];
  id<MTLFunction> frag = [render_state->library newFunctionWithName:@"eye_frag"];
  if (!vert || !frag) {
    lg::error("[Metal] eye shader entry points missing from the library");
    return false;
  }

  // Four floats per vertex: x, y, s, t.
  MTLVertexDescriptor* vd = [[MTLVertexDescriptor alloc] init];
  vd.attributes[0].format = MTLVertexFormatFloat4;
  vd.attributes[0].offset = 0;
  vd.attributes[0].bufferIndex = MetalBufferIndexVertex;
  vd.layouts[MetalBufferIndexVertex].stride = sizeof(float) * 4;
  vd.layouts[MetalBufferIndexVertex].stepFunction = MTLVertexStepFunctionPerVertex;

  for (int blended = 0; blended < 2; blended++) {
    MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
    desc.vertexFunction = vert;
    desc.fragmentFunction = frag;
    desc.vertexDescriptor = vd;
    auto* color = desc.colorAttachments[0];
    color.pixelFormat = MTLPixelFormatRGBA8Unorm;
    color.blendingEnabled = blended ? YES : NO;
    color.rgbBlendOperation = MTLBlendOperationAdd;
    color.alphaBlendOperation = MTLBlendOperationAdd;
    color.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    color.sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
    color.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

    NSError* err = nil;
    id<MTLRenderPipelineState> pso =
        [render_state->device newRenderPipelineStateWithDescriptor:desc error:&err];
    if (!pso) {
      lg::error("[Metal] eye pipeline failed: {}",
                err ? [[err localizedDescription] UTF8String] : "unknown error");
      return false;
    }
    (blended ? m_impl->blended : m_impl->opaque) = pso;
  }

  MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
  sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
  sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
  sd.minFilter = MTLSamplerMinMagFilterLinear;
  sd.magFilter = MTLSamplerMinMagFilterLinear;
  m_impl->sampler = [render_state->device newSamplerStateWithDescriptor:sd];

  m_impl->ready = true;
  return true;
}

u64 MetalEyeRenderer::backend_create_eye_texture(int slot) {
  gpu::TextureCreateInfo info;
  info.w = kEyeTexSize;
  info.h = kEyeTexSize;
  info.data = nullptr;
  info.mipmap = false;
  info.anisotropic = false;
  info.clamp_and_linear = true;
  info.render_target = true;
  m_impl->handles[slot] = gpu::create_texture_rgba8(info);
  return m_impl->handles[slot];
}

void MetalEyeRenderer::init_textures(TexturePool& pool, GameVersion version) {
  EyeRendererCore::init_textures(pool, version);
}

void MetalEyeRenderer::render(DmaFollower& dma, MetalRenderState* render_state) {
  if (!m_impl->ready) {
    while (dma.current_tag_offset() != render_state->next_bucket && !dma.ended()) {
      dma.read_and_advance();
    }
    return;
  }

  // One buffer per eye pass per frame, reused a frame later. Metal runs the frame at the end, so
  // a buffer written twice in one frame is only read with its last contents.
  if (m_impl->buffer_frame != render_state->frame_index) {
    m_impl->buffer_frame = render_state->frame_index;
    m_impl->next_buffer = 0;
  }

  m_current_render_state = render_state;
  render_core(dma, render_state->texture_pool.get(), GameVersion::Jak1,
              render_state->next_bucket);
  m_current_render_state = nullptr;
}

void MetalEyeRenderer::backend_run_gpu(const std::vector<SingleEyeDraws>& draws) {
  if (draws.empty() || !m_current_render_state || !m_current_render_state->offscreen_cmd) {
    return;
  }

  // Build the vertices for every quad of every eye first, the way the OpenGL sibling does.
  int buffer_idx = 0;
  for (const auto& draw : draws) {
    buffer_idx = add_clear_draw_to_buffer(buffer_idx, m_gpu_vertex_buffer);
    if (draw.using_64) {
      buffer_idx =
          add_draw_to_buffer_64(buffer_idx, draw.iris, m_gpu_vertex_buffer, draw.pair, draw.lr);
      buffer_idx =
          add_draw_to_buffer_64(buffer_idx, draw.pupil, m_gpu_vertex_buffer, draw.pair, draw.lr);
      buffer_idx =
          add_draw_to_buffer_64(buffer_idx, draw.lid, m_gpu_vertex_buffer, draw.pair, draw.lr);
    } else {
      buffer_idx =
          add_draw_to_buffer_32(buffer_idx, draw.iris, m_gpu_vertex_buffer, draw.pair, draw.lr);
      buffer_idx =
          add_draw_to_buffer_32(buffer_idx, draw.pupil, m_gpu_vertex_buffer, draw.pair, draw.lr);
      buffer_idx =
          add_draw_to_buffer_32(buffer_idx, draw.lid, m_gpu_vertex_buffer, draw.pair, draw.lr);
    }
  }
  ASSERT(buffer_idx <= VTX_BUFFER_FLOATS);

  if (m_impl->next_buffer >= m_impl->vertex_buffers.size()) {
    m_impl->vertex_buffers.push_back(
        [m_impl->device newBufferWithLength:VTX_BUFFER_FLOATS * sizeof(float)
                                    options:MTLResourceStorageModeShared]);
  }
  id<MTLBuffer> vertex_buffer = m_impl->vertex_buffers[m_impl->next_buffer++];
  memcpy([vertex_buffer contents], m_gpu_vertex_buffer, buffer_idx * sizeof(float));

  buffer_idx = 0;
  for (const auto& draw : draws) {
    auto& out_tex = m_gpu_eye_textures[draw.tex_slot()];
    out_tex.fnv_name_hash = draw.fnv_name_hash;
    out_tex.lr = draw.lr;

    id<MTLTexture> target = metal_texture_from_handle(out_tex.handle);
    if (!target) {
      buffer_idx += 4 * 4 * 4;
      continue;
    }

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    // Red, so a missing eye is obvious rather than invisible. Same colour the OpenGL sibling
    // clears to, and for the same reason.
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(1.0, 0, 0, 0);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc =
        [m_current_render_state->offscreen_cmd renderCommandEncoderWithDescriptor:pass];
    [enc setVertexBuffer:vertex_buffer offset:0 atIndex:MetalBufferIndexVertex];
    [enc setFragmentSamplerState:m_impl->sampler atIndex:0];

    // The four quads, in order: background, iris, pupil, lid. The pupil is the only one that
    // blends.
    struct Quad {
      u64 handle;
      bool present;
      bool blend;
    };
    const Quad quads[4] = {{draw.iris_gl_tex, draw.iris_tex != nullptr, false},
                           {draw.iris_gl_tex, draw.iris_tex != nullptr, false},
                           {draw.pupil_gl_tex, draw.pupil_tex != nullptr, true},
                           {draw.lid_gl_tex, draw.lid_tex != nullptr, false}};

    for (const auto& quad : quads) {
      if (quad.present) {
        id<MTLTexture> texture = metal_texture_from_handle(quad.handle);
        if (texture) {
          [enc setRenderPipelineState:quad.blend ? m_impl->blended : m_impl->opaque];
          [enc setFragmentTexture:texture atIndex:0];
          [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip
                  vertexStart:buffer_idx / 4
                  vertexCount:4];
        }
      }
      buffer_idx += 4 * 4;
    }

    [enc endEncoding];

    m_texture_pool->move_existing_to_vram(out_tex.gpu_tex, out_tex.tbp);
  }
}
