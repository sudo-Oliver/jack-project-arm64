/*!
 * @file MetalSprite3.mm
 * See MetalSprite3.h.
 */

#include "MetalSprite3.h"

#import <Metal/Metal.h>

#include <array>
#include <cstring>

#include "common/log/log.h"

#include "game/graphics/metal_renderer/MetalGpuResources.h"
#include "game/graphics/opengl_renderer/background/background_common.h"

#include "metal_shader_types.h"

namespace {

// How many sprites one frame's bump allocator holds, across every flush in that frame. The core
// can produce Sprite3Core::MAX_SPRITES per flush, but that is the size of its scratch buffer, not
// a number the game reaches: village1 draws a few hundred. Sizing the GPU buffer for the scratch
// buffer would cost 24 MB of always-resident memory for nothing, so this is the real budget and
// an overflow drops the rest of the frame's sprites with a log line rather than corrupting the
// draws already recorded.
constexpr u32 kSpriteBudget = 16 * 1024;
constexpr u32 kVertsPerSprite = 4;
constexpr u32 kIndicesPerSprite = 6;

}  // namespace

struct MetalSprite3::Impl {
  id<MTLDevice> device = nil;
  MetalDrawStateCache states;
  bool ready = false;

  // One vertex and one index buffer per frame in flight, bump-allocated within a frame.
  std::array<id<MTLBuffer>, kMetalFramesInFlight> vertex_buffers = {nil, nil, nil};
  std::array<id<MTLBuffer>, kMetalFramesInFlight> index_buffers = {nil, nil, nil};
  int frame = 0;
  u32 vertex_used = 0;  // in vertices
  u32 index_used = 0;   // in indices

  Sprite3Uniforms uniforms{};

  // The distorter. Its mesh is the sine table with the sprite-specific parts removed, one mesh
  // per sprite resolution, rebuilt only when the game sends a new aspect ratio.
  id<MTLRenderPipelineState> distort_pso = nil;
  id<MTLDepthStencilState> distort_depth = nil;
  id<MTLSamplerState> distort_sampler = nil;
  id<MTLBuffer> distort_mesh = nil;
  bool distort_mesh_dirty = true;
  std::array<id<MTLBuffer>, kMetalFramesInFlight> distort_instances = {nil, nil, nil};

  void release() {
    for (int i = 0; i < kMetalFramesInFlight; i++) {
      vertex_buffers[i] = nil;
      index_buffers[i] = nil;
      distort_instances[i] = nil;
    }
    distort_mesh = nil;
  }
};

MetalSprite3::MetalSprite3(const std::string& name, int my_id)
    : MetalBucketRenderer(name, my_id),
      m_impl(std::make_unique<Impl>()),
      // Same batch size the OpenGL Sprite3 gives its direct renderer.
      m_direct(name, 1024) {}

MetalSprite3::~MetalSprite3() {
  if (m_impl) {
    m_impl->release();
  }
}

bool MetalSprite3::init(MetalRenderState* render_state) {
  m_impl->device = render_state->device;

  if (!m_direct.init(render_state)) {
    return false;
  }

  id<MTLFunction> vert = [render_state->library newFunctionWithName:@"sprite3_vert"];
  id<MTLFunction> frag = [render_state->library newFunctionWithName:@"sprite3_frag"];
  if (!vert || !frag) {
    lg::error("[Metal] sprite3 shader entry points missing from the library");
    return false;
  }

  // Sprite3Core::SpriteVertex3D: float4 xyz_sx at 0, float4 quat_sy at 16, float4 rgba at 32,
  // ushort2 flags_matrix at 48, ushort4 info at 52.
  MTLVertexDescriptor* vd = [[MTLVertexDescriptor alloc] init];
  vd.attributes[0].format = MTLVertexFormatFloat4;
  vd.attributes[0].offset = offsetof(SpriteVertex3D, xyz_sx);
  vd.attributes[0].bufferIndex = MetalBufferIndexVertex;
  vd.attributes[1].format = MTLVertexFormatFloat4;
  vd.attributes[1].offset = offsetof(SpriteVertex3D, quat_sy);
  vd.attributes[1].bufferIndex = MetalBufferIndexVertex;
  vd.attributes[2].format = MTLVertexFormatFloat4;
  vd.attributes[2].offset = offsetof(SpriteVertex3D, rgba);
  vd.attributes[2].bufferIndex = MetalBufferIndexVertex;
  vd.attributes[3].format = MTLVertexFormatUShort2;
  vd.attributes[3].offset = offsetof(SpriteVertex3D, flags_matrix);
  vd.attributes[3].bufferIndex = MetalBufferIndexVertex;
  vd.attributes[4].format = MTLVertexFormatUShort4;
  vd.attributes[4].offset = offsetof(SpriteVertex3D, info);
  vd.attributes[4].bufferIndex = MetalBufferIndexVertex;
  vd.layouts[MetalBufferIndexVertex].stride = sizeof(SpriteVertex3D);
  vd.layouts[MetalBufferIndexVertex].stepFunction = MTLVertexStepFunctionPerVertex;

  m_impl->states.init(render_state->device, vert, frag, vd, render_state->color_format,
                      render_state->depth_format, render_state->sample_count);

  for (int i = 0; i < kMetalFramesInFlight; i++) {
    m_impl->vertex_buffers[i] = [m_impl->device
        newBufferWithLength:(NSUInteger)kSpriteBudget * kVertsPerSprite * sizeof(SpriteVertex3D)
                    options:MTLResourceStorageModeShared];
    m_impl->index_buffers[i] =
        [m_impl->device newBufferWithLength:(NSUInteger)kSpriteBudget * kIndicesPerSprite *
                                            sizeof(u32)
                                    options:MTLResourceStorageModeShared];
    if (!m_impl->vertex_buffers[i] || !m_impl->index_buffers[i]) {
      lg::error("[Metal] sprite3 could not allocate its buffers");
      return false;
    }
  }

  if (!init_distort(render_state)) {
    return false;
  }

  // Build the pipeline the default draw mode needs now, rather than mid-frame.
  DrawMode probe;
  probe.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_SRC_DST);
  probe.set_ab(true);
  if (!m_impl->states.pipeline(probe)) {
    return false;
  }

  m_impl->ready = true;
  return true;
}

void MetalSprite3::render(DmaFollower& dma, MetalRenderState* render_state) {
  m_last_frame_tris = 0;
  m_direct.reset_tri_count();
  m_current_render_state = render_state;

  m_impl->frame = (int)(render_state->frame_index % kMetalFramesInFlight);
  m_impl->vertex_used = 0;
  m_impl->index_used = 0;

  Context context;
  context.version = GameVersion::Jak1;
  context.next_bucket = render_state->next_bucket;
  context.enabled = m_impl->ready;
  // The camera planes come from whichever level slot last sent them, the same as the OpenGL
  // backend's SharedRenderState::camera_planes.
  const math::Vector4f* planes = nullptr;
  for (int i = 0; i < (int)jak1::LEVEL_MAX; i++) {
    if (render_state->level_slots[i].has_camera) {
      planes = render_state->level_slots[i].camera.planes;
    }
  }
  context.camera_planes = planes;

  render_core(dma, context);

  m_last_frame_tris += m_direct.last_frame_tris();
  m_current_render_state = nullptr;
}

void MetalSprite3::direct_reset_state() {
  m_direct.reset_state();
}

void MetalSprite3::direct_render_vif(u32 vif0, u32 vif1, const u8* data, u32 size) {
  m_direct.render_vif(vif0, vif1, data, size, m_current_render_state);
}

void MetalSprite3::direct_flush() {
  m_direct.flush_pending(m_current_render_state);
}

void MetalSprite3::add_tri_count(u32 tris) {
  m_last_frame_tris += tris;
}

bool MetalSprite3::distort_wants_instancing() const {
  // Only the instanced form is ported. The OpenGL backend keeps a non-instanced path for drivers
  // without instancing; there is no such Metal driver.
  return true;
}

void MetalSprite3::distort_instanced_mesh_changed() {
  m_impl->distort_mesh_dirty = true;
}

bool MetalSprite3::init_distort(MetalRenderState* render_state) {
  id<MTLFunction> vert = [render_state->library newFunctionWithName:@"sprite_distort_vert"];
  id<MTLFunction> frag = [render_state->library newFunctionWithName:@"sprite_distort_frag"];
  if (!vert || !frag) {
    lg::error("[Metal] sprite_distort shader entry points missing from the library");
    return false;
  }

  MTLVertexDescriptor* vd = [[MTLVertexDescriptor alloc] init];
  vd.attributes[0].format = MTLVertexFormatFloat3;
  vd.attributes[0].offset = offsetof(SpriteDistortVertex, xyz);
  vd.attributes[0].bufferIndex = MetalBufferIndexVertex;
  vd.attributes[1].format = MTLVertexFormatFloat2;
  vd.attributes[1].offset = offsetof(SpriteDistortVertex, st);
  vd.attributes[1].bufferIndex = MetalBufferIndexVertex;
  vd.attributes[2].format = MTLVertexFormatFloat4;
  vd.attributes[2].offset = offsetof(SpriteDistortInstanceData, x_y_z_s);
  vd.attributes[2].bufferIndex = MetalBufferIndexInstance;
  vd.attributes[3].format = MTLVertexFormatFloat4;
  vd.attributes[3].offset = offsetof(SpriteDistortInstanceData, sx_sy_sz_t);
  vd.attributes[3].bufferIndex = MetalBufferIndexInstance;
  vd.layouts[MetalBufferIndexVertex].stride = sizeof(SpriteDistortVertex);
  vd.layouts[MetalBufferIndexVertex].stepFunction = MTLVertexStepFunctionPerVertex;
  vd.layouts[MetalBufferIndexInstance].stride = sizeof(SpriteDistortInstanceData);
  vd.layouts[MetalBufferIndexInstance].stepFunction = MTLVertexStepFunctionPerInstance;

  MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
  desc.vertexFunction = vert;
  desc.fragmentFunction = frag;
  desc.vertexDescriptor = vd;
  desc.rasterSampleCount = render_state->sample_count;
  desc.depthAttachmentPixelFormat = render_state->depth_format;
  if (render_state->depth_format == MTLPixelFormatDepth32Float_Stencil8) {
    desc.stencilAttachmentPixelFormat = render_state->depth_format;
  }
  auto* color = desc.colorAttachments[0];
  color.pixelFormat = render_state->color_format;
  // The distorter's GS setup is always SOURCE/DEST/SOURCE/DEST, checked by the core's asserts.
  color.blendingEnabled = YES;
  color.rgbBlendOperation = MTLBlendOperationAdd;
  color.alphaBlendOperation = MTLBlendOperationAdd;
  color.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
  color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
  color.sourceAlphaBlendFactor = MTLBlendFactorOne;
  color.destinationAlphaBlendFactor = MTLBlendFactorZero;

  NSError* err = nil;
  m_impl->distort_pso = [m_impl->device newRenderPipelineStateWithDescriptor:desc error:&err];
  if (!m_impl->distort_pso) {
    lg::error("[Metal] sprite_distort pipeline failed: {}",
              err ? [[err localizedDescription] UTF8String] : "unknown error");
    return false;
  }

  // Its GS setup always has zmsk set, so it never writes depth. It does test it.
  MTLDepthStencilDescriptor* dd = [[MTLDepthStencilDescriptor alloc] init];
  dd.depthCompareFunction = MTLCompareFunctionGreaterEqual;
  dd.depthWriteEnabled = NO;
  m_impl->distort_depth = [m_impl->device newDepthStencilStateWithDescriptor:dd];

  // tex1.mmag is asserted to be 1, and the coordinates are clamped: this samples the frame.
  MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
  sd.minFilter = MTLSamplerMinMagFilterLinear;
  sd.magFilter = MTLSamplerMinMagFilterLinear;
  sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
  sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
  m_impl->distort_sampler = [m_impl->device newSamplerStateWithDescriptor:sd];

  m_impl->distort_mesh = [m_impl->device
      newBufferWithLength:m_sprite_distorter_vertices_instanced.size() * sizeof(SpriteDistortVertex)
                  options:MTLResourceStorageModeShared];
  for (int i = 0; i < kMetalFramesInFlight; i++) {
    m_impl->distort_instances[i] = [m_impl->device
        newBufferWithLength:(NSUInteger)MAX_DISTORT_SPRITES * sizeof(SpriteDistortInstanceData)
                    options:MTLResourceStorageModeShared];
    if (!m_impl->distort_instances[i]) {
      return false;
    }
  }
  return m_impl->distort_mesh != nil;
}

void MetalSprite3::distort_draw_gpu(bool /*instanced*/) {
  if (!m_impl->ready || !m_impl->distort_pso || m_distort_stats.total_tris == 0) {
    return;
  }
  auto* render_state = m_current_render_state;
  if (!render_state) {
    return;
  }

  // The distorter samples the frame drawn so far, so the pass has to be split here. This is the
  // only place in a Jak 1 frame that asks for it, and only on frames with a distorter in them.
  id<MTLTexture> snapshot = render_state->snapshot_scene();
  id<MTLRenderCommandEncoder> encoder = render_state->encoder;
  if (!snapshot || !encoder) {
    return;
  }

  if (m_impl->distort_mesh_dirty) {
    m_impl->distort_mesh_dirty = false;
    memcpy([m_impl->distort_mesh contents], m_sprite_distorter_vertices_instanced.data(),
           m_sprite_distorter_vertices_instanced.size() * sizeof(SpriteDistortVertex));
  }

  SpriteDistortUniforms u{};
  u.u_color = {m_sprite_distorter_sine_tables.color.x() / 255.f,
               m_sprite_distorter_sine_tables.color.y() / 255.f,
               m_sprite_distorter_sine_tables.color.z() / 255.f,
               m_sprite_distorter_sine_tables.color.w() / 255.f};
  u.height_scale = 1.f;
  u.scissor_height = 448.f;

  const int frame = m_impl->frame;
  id<MTLBuffer> inst_buf = m_impl->distort_instances[frame];
  auto* instances_out = (SpriteDistortInstanceData*)[inst_buf contents];

  [encoder setRenderPipelineState:m_impl->distort_pso];
  [encoder setDepthStencilState:m_impl->distort_depth];
  [encoder setFragmentSamplerState:m_impl->distort_sampler atIndex:0];
  [encoder setFragmentTexture:snapshot atIndex:0];
  [encoder setVertexBuffer:m_impl->distort_mesh offset:0 atIndex:MetalBufferIndexVertex];
  [encoder setVertexBytes:&u length:sizeof(u) atIndex:MetalBufferIndexUniforms];
  [encoder setFragmentBytes:&u length:sizeof(u) atIndex:MetalBufferIndexUniforms];

  // One draw per resolution group, the same split the OpenGL renderer makes: a group shares a
  // mesh, so its sprites are instances of it.
  u32 instance_write = 0;
  int vert_offset = 0;
  for (int res = 3; res < 12; res++) {
    const auto& instances = m_sprite_distorter_instances_by_res[res];
    const int num_verts = res * 5;
    if (!instances.empty() && instance_write + instances.size() <= (u32)MAX_DISTORT_SPRITES) {
      memcpy(instances_out + instance_write, instances.data(),
             instances.size() * sizeof(SpriteDistortInstanceData));
      [encoder setVertexBuffer:inst_buf
                        offset:instance_write * sizeof(SpriteDistortInstanceData)
                       atIndex:MetalBufferIndexInstance];
      [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                  vertexStart:vert_offset
                  vertexCount:num_verts
                instanceCount:instances.size()];
      instance_write += instances.size();
      m_last_frame_tris += res * 2 * instances.size();
    }
    vert_offset += num_verts;
  }
}

void MetalSprite3::set_frame_constants() {
  auto& u = m_impl->uniforms;
  for (int col = 0; col < 4; col++) {
    const float* src = m_3d_matrix_data.camera.data() + col * 4;
    u.camera.col[col] = {src[0], src[1], src[2], src[3]};
  }
  u.hvdf_offset = {m_3d_matrix_data.hvdf_offset[0], m_3d_matrix_data.hvdf_offset[1],
                   m_3d_matrix_data.hvdf_offset[2], m_3d_matrix_data.hvdf_offset[3]};
  u.basis_x = {m_frame_data.basis_x[0], m_frame_data.basis_x[1], m_frame_data.basis_x[2],
               m_frame_data.basis_x[3]};
  u.basis_y = {m_frame_data.basis_y[0], m_frame_data.basis_y[1], m_frame_data.basis_y[2],
               m_frame_data.basis_y[3]};
  for (int i = 0; i < 8; i++) {
    u.xy_array[i] = {m_frame_data.xy_array[i][0], m_frame_data.xy_array[i][1],
                     m_frame_data.xy_array[i][2], m_frame_data.xy_array[i][3]};
  }
  for (int i = 0; i < 4; i++) {
    u.xyz_array[i] = {m_frame_data.xyz_array[i][0], m_frame_data.xyz_array[i][1],
                      m_frame_data.xyz_array[i][2], m_frame_data.xyz_array[i][3]};
    u.st_array[i] = {m_frame_data.st_array[i][0], m_frame_data.st_array[i][1],
                     m_frame_data.st_array[i][2], m_frame_data.st_array[i][3]};
  }
  u.pfog0 = m_frame_data.pfog0;
  u.fog_min = m_frame_data.fog_min;
  u.fog_max = m_frame_data.fog_max;
  u.min_scale = m_frame_data.min_scale;
  u.max_scale = m_frame_data.max_scale;
  u.deg_to_rad = m_frame_data.deg_to_rad;
  u.inv_area = m_frame_data.inv_area;
  u.scissor_adjust = 512.f / 448.f;
  u.height_scale = 1.f;
}

void MetalSprite3::set_hud_constants() {
  auto& u = m_impl->uniforms;
  for (int col = 0; col < 4; col++) {
    const float* src = m_hud_matrix_data.matrix.data() + col * 4;
    u.hud_matrix.col[col] = {src[0], src[1], src[2], src[3]};
  }
  u.hud_hvdf_offset = {m_hud_matrix_data.hvdf_offset[0], m_hud_matrix_data.hvdf_offset[1],
                       m_hud_matrix_data.hvdf_offset[2], m_hud_matrix_data.hvdf_offset[3]};
  for (int i = 0; i < MetalSpriteHudUserCount; i++) {
    u.hud_hvdf_user[i] = {m_hud_matrix_data.user_hvdf[i][0], m_hud_matrix_data.user_hvdf[i][1],
                          m_hud_matrix_data.user_hvdf[i][2], m_hud_matrix_data.user_hvdf[i][3]};
  }
}

void MetalSprite3::flush_sprites_gpu(bool double_draw) {
  if (!m_impl->ready || m_bucket_list.empty() || m_sprite_idx == 0) {
    return;
  }
  auto* render_state = m_current_render_state;
  id<MTLRenderCommandEncoder> encoder = render_state ? render_state->encoder : nil;
  if (!encoder) {
    return;
  }

  const u32 sprites = (u32)m_sprite_idx;
  const u32 vert_count = sprites * kVertsPerSprite;
  if (m_impl->vertex_used + vert_count > kSpriteBudget * kVertsPerSprite) {
    static int complaints = 0;
    if (complaints++ < 4) {
      lg::warn("[Metal] sprite3 ran out of vertex budget ({} sprites this frame)", sprites);
    }
    return;
  }

  const int frame = m_impl->frame;
  id<MTLBuffer> vbuf = m_impl->vertex_buffers[frame];
  id<MTLBuffer> ibuf = m_impl->index_buffers[frame];
  const u32 vert_base = m_impl->vertex_used;
  auto* verts = (SpriteVertex3D*)[vbuf contents] + vert_base;
  memcpy(verts, m_vertices_3d.data(), vert_count * sizeof(SpriteVertex3D));
  m_impl->vertex_used += vert_count;

  // Expand each bucket's four-vertices-plus-restart strips into triangles. The vertex ids the
  // core produced are relative to this flush, so add the offset this flush was uploaded at.
  auto* inds = (u32*)[ibuf contents];
  const u32 index_base = m_impl->index_used;
  u32 index_write = index_base;
  struct BucketRange {
    u32 first_index;
    u32 index_count;
    u32 tbp;
    DrawMode mode;
  };
  std::vector<BucketRange> ranges;
  ranges.reserve(m_bucket_list.size());

  for (const auto* bucket : m_bucket_list) {
    const u32 first = index_write;
    for (size_t i = 0; i + 4 < bucket->ids.size(); i += 5) {
      // a, b, c, d and then the restart. Strip a-b-c-d is triangles a-b-c and b-d-c.
      const u32 a = vert_base + bucket->ids[i + 0];
      const u32 b = vert_base + bucket->ids[i + 1];
      const u32 c = vert_base + bucket->ids[i + 2];
      const u32 d = vert_base + bucket->ids[i + 3];
      if (index_write + 6 > kSpriteBudget * kIndicesPerSprite) {
        break;
      }
      inds[index_write++] = a;
      inds[index_write++] = b;
      inds[index_write++] = c;
      inds[index_write++] = b;
      inds[index_write++] = d;
      inds[index_write++] = c;
    }
    if (index_write > first) {
      ranges.push_back({first, index_write - first, bucket->tbp(), bucket->mode()});
    }
  }
  m_impl->index_used = index_write;
  if (ranges.empty()) {
    return;
  }

  [encoder setVertexBuffer:vbuf offset:0 atIndex:MetalBufferIndexVertex];
  [encoder setVertexBytes:&m_impl->uniforms
                   length:sizeof(m_impl->uniforms)
                  atIndex:MetalBufferIndexUniforms];
  // SRC_DST_FIX_DST wants a blend constant of 0.5, the same one every other renderer here sets.
  [encoder setBlendColorRed:0.5f green:0.5f blue:0.5f alpha:0.5f];

  for (const auto& range : ranges) {
    std::optional<u64> tex = render_state->texture_pool->lookup(range.tbp);
    if (!tex) {
      tex = render_state->texture_pool->get_placeholder_texture();
    }
    id<MTLTexture> texture = tex ? metal_texture_from_handle(*tex) : nil;
    if (!texture) {
      continue;
    }

    id<MTLRenderPipelineState> pso = m_impl->states.pipeline(range.mode);
    if (!pso) {
      continue;
    }

    const DoubleDraw dd = alpha_test_double_draw(range.mode);
    SpriteDrawUniforms draw_u{};
    // Matches the OpenGL renderer: the world pass uses a fixed floor, the HUD pass uses the
    // alpha test's own reference value.
    draw_u.alpha_min = double_draw ? dd.aref_first : 0.016f;
    draw_u.alpha_max = 10.f;

    [encoder setRenderPipelineState:pso];
    [encoder setDepthStencilState:m_impl->states.depth_state(range.mode, false)];
    [encoder setFragmentSamplerState:m_impl->states.sampler(range.mode) atIndex:0];
    [encoder setFragmentTexture:texture atIndex:0];
    [encoder setFragmentBytes:&draw_u length:sizeof(draw_u) atIndex:MetalBufferIndexUniforms];
    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                        indexCount:range.index_count
                         indexType:MTLIndexTypeUInt32
                       indexBuffer:ibuf
                 indexBufferOffset:range.first_index * sizeof(u32)];
    m_last_frame_tris += range.index_count / 3;

    if (double_draw && dd.kind == DoubleDrawKind::AFAIL_NO_DEPTH_WRITE) {
      draw_u.alpha_min = -10.f;
      draw_u.alpha_max = dd.aref_second;
      [encoder setDepthStencilState:m_impl->states.depth_state(range.mode, true)];
      [encoder setFragmentBytes:&draw_u length:sizeof(draw_u) atIndex:MetalBufferIndexUniforms];
      [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                          indexCount:range.index_count
                           indexType:MTLIndexTypeUInt32
                         indexBuffer:ibuf
                   indexBufferOffset:range.first_index * sizeof(u32)];
      m_last_frame_tris += range.index_count / 3;
    }
  }
}
