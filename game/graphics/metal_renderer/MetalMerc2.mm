/*!
 * @file MetalMerc2.mm
 * See MetalMerc2.h.
 */

#include "MetalMerc2.h"

#import <Metal/Metal.h>

#include <array>
#include <vector>

#include "common/log/log.h"

#include "game/graphics/metal_renderer/MetalGpuResources.h"

#include "metal_shader_types.h"

namespace {

// Same constant Shader.cpp substitutes into the GLSL for Jak 1.
constexpr float kJak1ScissorAdjust = 512.0f / 448.0f;
constexpr float kJak1HeightScale = 1.0f;

}  // namespace

/*!
 * A pool of buffers that is safe to write while the GPU may still be running.
 *
 * Metal records a whole frame and runs it at the end, so a buffer written twice in one frame is
 * only ever read with its last contents. merc flushes once per bucket and there are eight of
 * them, so every flush needs storage the earlier flushes are not still pointing at. A buffer is
 * handed out once per frame and only reused on a later frame, by which time the frame that used
 * it has been presented.
 */
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
      buffers.push_back([device newBufferWithLength:length
                                            options:MTLResourceStorageModeShared]);
    }
    return buffers[next++];
  }
};

struct MetalMerc2::Impl {
  id<MTLDevice> device = nil;
  // One cache per shader: a pipeline state carries the shader's functions and vertex layout, so
  // merc and emerc cannot share one.
  MetalDrawStateCache states;
  MetalDrawStateCache envmap_states;
  bool ready = false;

  // The skinning matrices, one buffer per flush.
  BufferPool bones;
  id<MTLBuffer> current_bones = nil;

  // The modifiable vertices. The core numbers these from zero again after every flush, so a
  // second table maps this frame's numbering onto buffers that are not yet in use.
  BufferPool mod_vtx;
  std::vector<id<MTLBuffer>> mod_vtx_this_flush;

  Merc2Uniforms uniforms{};
};

MetalMerc2::MetalMerc2() : m_impl(std::make_unique<Impl>()) {
  // Apple Silicon needs a vertex buffer offset to be a multiple of 4 bytes, and a bone vector is
  // 16, so a draw's first bone can sit anywhere.
  m_bone_buffer_alignment = 1;
}

MetalMerc2::~MetalMerc2() = default;

bool MetalMerc2::init(MetalRenderState* render_state) {
  if (m_impl->ready) {
    return true;
  }
  m_impl->device = render_state->device;

  id<MTLFunction> vert = [render_state->library newFunctionWithName:@"merc2_vert"];
  id<MTLFunction> frag = [render_state->library newFunctionWithName:@"merc2_frag"];
  if (!vert || !frag) {
    lg::error("[Metal] merc2 shader entry points missing from the library");
    return false;
  }

  // tfrag3::MercVertex, 64 bytes: pos at 0, normal at 16, weights at 32, st at 48, rgba at 56,
  // mats at 60.
  MTLVertexDescriptor* vd = [[MTLVertexDescriptor alloc] init];
  vd.attributes[0].format = MTLVertexFormatFloat3;
  vd.attributes[0].offset = offsetof(tfrag3::MercVertex, pos);
  vd.attributes[0].bufferIndex = MetalBufferIndexVertex;
  vd.attributes[1].format = MTLVertexFormatFloat3;
  vd.attributes[1].offset = offsetof(tfrag3::MercVertex, normal);
  vd.attributes[1].bufferIndex = MetalBufferIndexVertex;
  vd.attributes[2].format = MTLVertexFormatFloat3;
  vd.attributes[2].offset = offsetof(tfrag3::MercVertex, weights);
  vd.attributes[2].bufferIndex = MetalBufferIndexVertex;
  vd.attributes[3].format = MTLVertexFormatFloat2;
  vd.attributes[3].offset = offsetof(tfrag3::MercVertex, st);
  vd.attributes[3].bufferIndex = MetalBufferIndexVertex;
  vd.attributes[4].format = MTLVertexFormatUChar4Normalized;
  vd.attributes[4].offset = offsetof(tfrag3::MercVertex, rgba);
  vd.attributes[4].bufferIndex = MetalBufferIndexVertex;
  // mats is three bytes followed by one of padding, so a UChar4 reads it without overrunning.
  vd.attributes[5].format = MTLVertexFormatUChar4;
  vd.attributes[5].offset = offsetof(tfrag3::MercVertex, mats);
  vd.attributes[5].bufferIndex = MetalBufferIndexVertex;
  vd.layouts[MetalBufferIndexVertex].stride = sizeof(tfrag3::MercVertex);
  vd.layouts[MetalBufferIndexVertex].stepFunction = MTLVertexStepFunctionPerVertex;

  m_impl->states.init(render_state->device, vert, frag, vd, render_state->color_format,
                      render_state->depth_format);

  // emerc: the environment-mapped pass, same vertex layout, different shader.
  id<MTLFunction> envmap_vert = [render_state->library newFunctionWithName:@"emerc_vert"];
  id<MTLFunction> envmap_frag = [render_state->library newFunctionWithName:@"emerc_frag"];
  if (!envmap_vert || !envmap_frag) {
    lg::error("[Metal] emerc shader entry points missing from the library");
    return false;
  }
  m_impl->envmap_states.init(render_state->device, envmap_vert, envmap_frag, vd,
                             render_state->color_format, render_state->depth_format);

  m_impl->bones.device = render_state->device;
  m_impl->bones.length = MAX_SHADER_BONE_VECTORS * sizeof(math::Vector4f);
  m_impl->mod_vtx.device = render_state->device;
  // MAX_MOD_VTX is the core's cap on how many vertices one effect can modify.
  m_impl->mod_vtx.length = (NSUInteger)UINT16_MAX * sizeof(tfrag3::MercVertex);

  DrawMode probe;
  probe.set_depth_write_enable(true);
  if (!m_impl->states.pipeline(probe)) {
    return false;
  }

  m_impl->uniforms.scissor_adjust = kJak1ScissorAdjust;
  m_impl->uniforms.height_scale = kJak1HeightScale;
  m_impl->ready = true;
  return true;
}

void MetalMerc2::render(DmaFollower& dma,
                        MetalRenderState* render_state,
                        MercDebugStats* stats) {
  if (!m_impl->ready) {
    while (dma.current_tag_offset() != render_state->next_bucket && !dma.ended()) {
      dma.read_and_advance();
    }
    return;
  }

  m_current_render_state = render_state;
  m_impl->bones.begin_frame(render_state->frame_index);
  m_impl->mod_vtx.begin_frame(render_state->frame_index);

  Context context;
  context.loader = render_state->loader.get();
  context.version = GameVersion::Jak1;
  context.next_bucket = render_state->next_bucket;
  render_core(dma, context, stats);

  m_current_render_state = nullptr;
}

void MetalMerc2::backend_set_low_memory(const LowMemory& low_memory) {
  auto& u = m_impl->uniforms;
  memcpy(&u.perspective_matrix, &low_memory.perspective[0].x(), sizeof(u.perspective_matrix));
  memcpy(&u.hvdf_offset, &low_memory.hvdf_offset.x(), sizeof(u.hvdf_offset));
  memcpy(&u.fog_constants, &low_memory.fog.x(), sizeof(u.fog_constants));
}

void MetalMerc2::backend_ensure_mod_vtx_buffer(u32 index, const LevelData* /*level*/) {
  while (index >= m_impl->mod_vtx_this_flush.size()) {
    m_impl->mod_vtx_this_flush.push_back(m_impl->mod_vtx.take());
  }
}

void MetalMerc2::backend_upload_mod_vtx(u32 buffer,
                                        const tfrag3::MercVertex* data,
                                        size_t num_vertices) {
  if (buffer >= m_impl->mod_vtx_this_flush.size() || !m_impl->mod_vtx_this_flush[buffer]) {
    return;
  }
  memcpy([m_impl->mod_vtx_this_flush[buffer] contents], data,
         num_vertices * sizeof(tfrag3::MercVertex));
}

void MetalMerc2::backend_upload_bones(const math::Vector4f* data, u32 num_vectors) {
  m_impl->current_bones = m_impl->bones.take();
  memcpy([m_impl->current_bones contents], data, num_vectors * sizeof(math::Vector4f));
}

void MetalMerc2::backend_do_draws(const Draw* draws,
                                  const LevelData* level,
                                  u32 num_draws,
                                  bool envmap,
                                  bool set_fade,
                                  MercDebugStats* /*stats*/) {
  if (!m_current_render_state || !m_current_render_state->encoder) {
    return;
  }
  MetalDrawStateCache& states = envmap ? m_impl->envmap_states : m_impl->states;

  id<MTLRenderCommandEncoder> encoder = m_current_render_state->encoder;
  id<MTLBuffer> vertices = metal_buffer_from_handle(level->merc_vertices);
  id<MTLBuffer> indices = metal_buffer_from_handle(level->merc_indices);
  if (!vertices || !indices || !m_impl->current_bones) {
    return;
  }

  auto& u = m_impl->uniforms;
  const auto& fog = m_current_render_state->fog_color;
  u.gfx_hack_no_tex = 0;

  for (u32 di = 0; di < num_draws; di++) {
    const auto& draw = draws[di];

    // Animated-texture slots and eye textures both need renderers that are not ported yet.
    if (draw.texture < 0 || (u32)draw.texture >= level->textures.size()) {
      continue;
    }
    id<MTLTexture> texture = metal_texture_from_handle(level->textures[draw.texture]);
    if (!texture) {
      continue;
    }

    const bool fog_on = draw.mode.get_fog_enable();
    u.fog_color = {fog[0] / 255.f, fog[1] / 255.f, fog[2] / 255.f,
                   fog_on ? m_current_render_state->fog_intensity / 255.f : 0.f};

    const auto& light = m_lights_buffer[draw.light_idx];
    float fade = 1.f;
    float fade_enable = 0.f;
    if (light.w1) {
      fade = light.w2 / 128.f;
      fade_enable = 1.f;
    }
    u.light_dir0_fade = {light.direction0.x(), light.direction0.y(), light.direction0.z(), fade};
    u.light_dir1_fade_en = {light.direction1.x(), light.direction1.y(), light.direction1.z(),
                            fade_enable};
    u.light_dir2 = {light.direction2.x(), light.direction2.y(), light.direction2.z(), 0.f};
    memcpy(&u.light_col0, &light.color0.x(), sizeof(u.light_col0));
    memcpy(&u.light_col1, &light.color1.x(), sizeof(u.light_col1));
    memcpy(&u.light_col2, &light.color2.x(), sizeof(u.light_col2));
    memcpy(&u.light_ambient, &light.ambient.x(), sizeof(u.light_ambient));

    if (set_fade) {
      // The envmap pass is faded by a colour the game sends per draw.
      u.fade = {draw.fade[0] / 255.f, draw.fade[1] / 255.f, draw.fade[2] / 255.f,
                draw.fade[3] / 255.f};
    }
    u.ignore_alpha = (draw.flags & DrawFlags::IGNORE_ALPHA) ? 1 : 0;
    u.decal_enable = draw.mode.get_decal() ? 1 : 0;
    u.gfx_hack_no_tex = (draw.flags & DrawFlags::NO_TEXTURE) ? 1 : 0;

    id<MTLBuffer> vtx_buffer = vertices;
    if ((draw.flags & DrawFlags::MOD_VTX) &&
        draw.mod_vtx_buffer < m_impl->mod_vtx_this_flush.size() &&
        m_impl->mod_vtx_this_flush[draw.mod_vtx_buffer]) {
      vtx_buffer = m_impl->mod_vtx_this_flush[draw.mod_vtx_buffer];
    }
    [encoder setVertexBuffer:vtx_buffer offset:0 atIndex:MetalBufferIndexVertex];
    [encoder setVertexBuffer:m_impl->current_bones
                      offset:draw.first_bone * sizeof(math::Vector4f)
                     atIndex:MetalBufferIndexBones];
    [encoder setFragmentTexture:texture atIndex:0];
    [encoder setFragmentSamplerState:states.sampler(draw.mode) atIndex:0];
    [encoder setDepthStencilState:states.depth_state(draw.mode, false)];

    const MTLPrimitiveType prim =
        draw.no_strip ? MTLPrimitiveTypeTriangle : MTLPrimitiveTypeTriangleStrip;

    auto encode = [&](id<MTLRenderPipelineState> pso) {
      if (!pso) {
        return;
      }
      [encoder setRenderPipelineState:pso];
      [encoder setVertexBytes:&u length:sizeof(u) atIndex:MetalBufferIndexUniforms];
      [encoder setFragmentBytes:&u length:sizeof(u) atIndex:MetalBufferIndexUniforms];
      [encoder drawIndexedPrimitives:prim
                          indexCount:draw.index_count
                           indexType:MTLIndexTypeUInt32
                         indexBuffer:indices
                   indexBufferOffset:draw.first_index * sizeof(u32)];
    };

    if (light.w1 && !set_fade) {
      // The fade case is two draws: colour with the model's own blending forced on, then alpha
      // alone with the draw's real mode. OpenGL does it with glColorMask; on Metal the write mask
      // belongs to the pipeline, so each half asks the cache for its own.
      DrawMode rgb_mode = draw.mode;
      rgb_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_SRC_DST);
      rgb_mode.set_ab(true);

      u.light_dir1_fade_en.w = 1.f;
      encode(states.pipeline(rgb_mode, MTLColorWriteMaskRed | MTLColorWriteMaskGreen |
                                           MTLColorWriteMaskBlue));
      u.light_dir1_fade_en.w = -1.f;
      encode(states.pipeline(draw.mode, MTLColorWriteMaskAlpha));
      m_last_frame_tris += draw.num_triangles * 2;
    } else {
      encode(states.pipeline(draw.mode));
      m_last_frame_tris += draw.num_triangles;
    }
  }
}

void MetalMerc2::backend_flush_finished() {
  // The core restarts its mod-vertex numbering after a flush, so this table restarts with it.
  m_impl->mod_vtx_this_flush.clear();
}

MetalMerc2BucketRenderer::MetalMerc2BucketRenderer(const std::string& name,
                                                   int my_id,
                                                   std::shared_ptr<MetalMerc2> merc)
    : MetalBucketRenderer(name, my_id), m_merc(std::move(merc)) {}

bool MetalMerc2BucketRenderer::init(MetalRenderState* render_state) {
  // Every bucket shares one MetalMerc2; init() is idempotent, so the first one through does the
  // work and the rest see it is already done.
  return m_merc->init(render_state);
}

void MetalMerc2BucketRenderer::render(DmaFollower& dma, MetalRenderState* render_state) {
  m_merc->render(dma, render_state, &m_stats);
}
