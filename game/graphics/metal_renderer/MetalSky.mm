/*!
 * @file MetalSky.mm
 * See MetalSky.h.
 */

#include "MetalSky.h"

#import <Metal/Metal.h>

#include "common/log/log.h"

#include "game/graphics/gpu_resources.h"
#include "game/graphics/metal_renderer/MetalGpuResources.h"
#include "game/graphics/opengl_renderer/AdgifHandler.h"

#include "metal_shader_types.h"

namespace {
// The sky is a 32x32 texture and the clouds a 64x64 one, the sizes the GS rendered them at.
constexpr int kSkySizes[2] = {32, 64};
}  // namespace

struct MetalSkyBlend::Impl {
  id<MTLRenderPipelineState> pipeline = nil;
  id<MTLSamplerState> sampler = nil;
  bool ready = false;

  struct TexInfo {
    u64 handle = gpu::kInvalidHandle;
    u32 tbp = 0;
    GpuTexture* tex = nullptr;
  } textures[2];
};

MetalSkyBlend::MetalSkyBlend() : m_impl(std::make_unique<Impl>()) {}

MetalSkyBlend::~MetalSkyBlend() {
  for (auto& t : m_impl->textures) {
    gpu::destroy_texture(t.handle);
  }
}

bool MetalSkyBlend::init(MetalRenderState* render_state) {
  if (m_impl->ready) {
    return true;
  }

  id<MTLFunction> vert = [render_state->library newFunctionWithName:@"sky_blend_vert"];
  id<MTLFunction> frag = [render_state->library newFunctionWithName:@"sky_blend_frag"];
  if (!vert || !frag) {
    lg::error("[Metal] sky_blend shader entry points missing from the library");
    return false;
  }

  MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
  desc.vertexFunction = vert;
  desc.fragmentFunction = frag;
  // The targets are plain RGBA8 textures, not the drawable.
  auto* color = desc.colorAttachments[0];
  color.pixelFormat = MTLPixelFormatRGBA8Unorm;
  // Each blend adds onto what the earlier ones left. The first draw of a frame clears instead,
  // which is the load action rather than a second pipeline.
  color.blendingEnabled = YES;
  color.rgbBlendOperation = MTLBlendOperationAdd;
  color.alphaBlendOperation = MTLBlendOperationAdd;
  color.sourceRGBBlendFactor = MTLBlendFactorOne;
  color.destinationRGBBlendFactor = MTLBlendFactorOne;
  color.sourceAlphaBlendFactor = MTLBlendFactorOne;
  color.destinationAlphaBlendFactor = MTLBlendFactorOne;

  NSError* err = nil;
  m_impl->pipeline = [render_state->device newRenderPipelineStateWithDescriptor:desc error:&err];
  if (!m_impl->pipeline) {
    lg::error("[Metal] sky_blend pipeline failed: {}",
              err ? [[err localizedDescription] UTF8String] : "unknown error");
    return false;
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

void MetalSkyBlend::init_textures(TexturePool& tex_pool, GameVersion version) {
  for (int i = 0; i < 2; i++) {
    gpu::TextureCreateInfo info;
    info.w = kSkySizes[i];
    info.h = kSkySizes[i];
    info.data = nullptr;
    info.mipmap = false;
    info.anisotropic = false;
    info.render_target = true;
    m_impl->textures[i].handle = gpu::create_texture_rgba8(info);

    TextureInput in;
    in.gpu_texture = m_impl->textures[i].handle;
    in.w = kSkySizes[i];
    in.h = kSkySizes[i];
    in.debug_name = fmt::format("PC-SKY-METAL-{}", i);
    in.id = tex_pool.allocate_pc_port_texture(version);
    const u32 tbp = SKY_TEXTURE_VRAM_ADDRS[i];
    m_impl->textures[i].tbp = tbp;
    m_impl->textures[i].tex = tex_pool.give_texture_and_load_to_vram(in, tbp);
  }
}

SkyBlendStats MetalSkyBlend::do_sky_blends(DmaFollower& dma, MetalRenderState* render_state) {
  SkyBlendStats stats;
  if (!m_impl->ready || !render_state->offscreen_cmd) {
    while (dma.current_tag().qwc == 6) {
      dma.read_and_advance();
      dma.read_and_advance();
    }
    return stats;
  }

  while (dma.current_tag().qwc == 6) {
    auto setup_data = dma.read_and_advance();
    AdgifHelper adgif(setup_data.data + 16);
    ASSERT(adgif.is_normal_adgif());

    auto draw_data = dma.read_and_advance();
    ASSERT(draw_data.size_bytes == 6 * 16);

    GifTag draw_or_blend_tag(draw_data.data);
    // The first draw of a frame has alpha blending off, which means it replaces rather than adds.
    const bool is_first_draw = !GsPrim(draw_or_blend_tag.prim()).abe();

    u32 coord;
    u32 intensity;
    memcpy(&coord, draw_data.data + (5 * 16), 4);
    memcpy(&intensity, draw_data.data + 16, 4);

    // The render-to-texture setup was not parsed, so the drawing coordinates are what tells sky
    // from clouds: the sky target is the smaller one.
    int buffer_idx;
    if (coord == 0x200) {
      buffer_idx = 0;
    } else if (coord == 0x400) {
      buffer_idx = 1;
    } else {
      ASSERT_NOT_REACHED();
    }

    auto source = render_state->texture_pool->lookup(adgif.tex0().tbp0());
    if (!source) {
      continue;
    }
    id<MTLTexture> source_texture = metal_texture_from_handle(*source);
    id<MTLTexture> target = metal_texture_from_handle(m_impl->textures[buffer_idx].handle);
    if (!source_texture || !target) {
      continue;
    }

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    // A first draw starts the target from nothing; the ones after it add to what is there.
    pass.colorAttachments[0].loadAction =
        is_first_draw ? MTLLoadActionClear : MTLLoadActionLoad;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;

    id<MTLRenderCommandEncoder> enc =
        [render_state->offscreen_cmd renderCommandEncoderWithDescriptor:pass];

    // The intensities the GOAL code generates are 0-128.
    ASSERT(intensity <= 128);
    SkyBlendUniforms uniforms{};
    uniforms.intensity = intensity / 128.f;

    [enc setRenderPipelineState:m_impl->pipeline];
    [enc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:MetalBufferIndexUniforms];
    [enc setFragmentTexture:source_texture atIndex:0];
    [enc setFragmentSamplerState:m_impl->sampler atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
    [enc endEncoding];

    render_state->texture_pool->move_existing_to_vram(m_impl->textures[buffer_idx].tex,
                                                      m_impl->textures[buffer_idx].tbp);

    if (buffer_idx == 0) {
      is_first_draw ? stats.sky_draws++ : stats.sky_blends++;
    } else {
      is_first_draw ? stats.cloud_draws++ : stats.cloud_blends++;
    }
  }

  return stats;
}

MetalSkyBlendHandler::MetalSkyBlendHandler(const std::string& name,
                                           int my_id,
                                           int level_id,
                                           std::shared_ptr<MetalSkyBlend> blender)
    : MetalBucketRenderer(name, my_id),
      m_blender(std::move(blender)),
      // The same bucket carries the transparent tfrag trees, which is why the OpenGL sibling owns
      // a TFragment too.
      m_tfrag(fmt::format("tfrag-{}", name),
              my_id,
              {tfrag3::TFragmentTreeKind::TRANS, tfrag3::TFragmentTreeKind::LOWRES_TRANS},
              level_id) {}

bool MetalSkyBlendHandler::init(MetalRenderState* render_state) {
  return m_blender->init(render_state) && m_tfrag.init(render_state);
}

void MetalSkyBlendHandler::render(DmaFollower& dma, MetalRenderState* render_state) {
  m_stats = {};

  // A NEXT with two nops: the jump from the bucket list to the sky data.
  auto data0 = dma.read_and_advance();
  ASSERT(data0.size_bytes == 0);

  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    // The sky renderer did not run this frame.
    for (int i = 0; i < 4; i++) {
      dma.read_and_advance();
    }
    return;
  }

  if (dma.current_tag().qwc != 8) {
    m_tfrag.render(dma, render_state);
    return;
  }

  auto set_display = dma.read_and_advance();
  ASSERT(set_display.size_bytes == 8 * 16);

  m_stats = m_blender->do_sky_blends(dma, render_state);

  dma.read_and_advance();  // reset alpha
  dma.read_and_advance();  // reset gs state
  dma.read_and_advance();  // empty

  if (dma.current_tag().kind != DmaTag::Kind::CALL) {
    m_tfrag.render(dma, render_state);
  } else {
    dma.read_and_advance();
    dma.read_and_advance();  // cnt
    dma.read_and_advance();  // ret
    dma.read_and_advance();  // ret
  }
}

MetalSkyRenderer::MetalSkyRenderer(const std::string& name, int my_id)
    : MetalBucketRenderer(name, my_id),
      // 100 triangles, the batch size the OpenGL sky renderer uses.
      m_direct("sky-direct", 100) {}

bool MetalSkyRenderer::init(MetalRenderState* render_state) {
  return m_direct.init(render_state);
}

void MetalSkyRenderer::render(DmaFollower& dma, MetalRenderState* render_state) {
  m_direct.reset_tri_count();
  m_direct.reset_state();

  auto data0 = dma.read_and_advance();
  ASSERT(data0.size_bytes == 0);

  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    // The sky renderer did not run this frame.
    for (int i = 0; i < 4; i++) {
      dma.read_and_advance();
    }
    return;
  }

  auto setup_packet = dma.read_and_advance();
  ASSERT(setup_packet.size_bytes == 16 * 4);
  m_direct.render_gif(setup_packet.data, setup_packet.size_bytes, render_state);

  if (dma.current_tag().qwc == 5) {
    auto draw_setup_packet = dma.read_and_advance();
    m_direct.render_gif(draw_setup_packet.data, draw_setup_packet.size_bytes, render_state);

    while (dma.current_tag().kind == DmaTag::Kind::CNT) {
      auto data = dma.read_and_advance();
      m_direct.render_gif(data.data, data.size_bytes, render_state);
    }

    dma.read_and_advance();  // empty
    dma.read_and_advance();  // call
    dma.read_and_advance();  // cnt
    dma.read_and_advance();  // ret
    dma.read_and_advance();  // ret
  } else {
    while (dma.current_tag_offset() != render_state->next_bucket && !dma.ended()) {
      auto data = dma.read_and_advance();
      if (data.size_bytes) {
        m_direct.render_vif(data.vif0(), data.vif1(), data.data, data.size_bytes, render_state);
      }
    }
  }

  m_direct.flush_pending(render_state);
}
