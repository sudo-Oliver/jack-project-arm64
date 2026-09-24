/*!
 * @file MetalTFragment.mm
 * See MetalTFragment.h.
 */

#include "MetalTFragment.h"

#import <Metal/Metal.h>

#include "common/log/log.h"

#include "game/graphics/gpu_resources.h"
#include "game/graphics/metal_renderer/MetalGpuResources.h"

#include "metal_shader_types.h"

namespace {

// One tree's GPU-side data. The vertex buffer comes from the shared loader; the index buffer and
// the time-of-day colours are ours, because the OpenGL renderer builds both per frame out of the
// visibility strings and we do not cull yet.
struct TreeCache {
  u64 index_buffer = gpu::kInvalidHandle;
  id<MTLBuffer> time_of_day = nil;
  std::vector<math::Vector<u8, 4>> color_scratch;
};

}  // namespace

struct MetalTFragment::Impl {
  id<MTLRenderPipelineState> pso = nil;
  id<MTLDepthStencilState> depth_state = nil;
  id<MTLSamplerState> sampler = nil;
  id<MTLDevice> device = nil;

  // Keyed by the loader's load id, so a level that is unloaded and loaded again is rebuilt rather
  // than drawn from buffers that no longer exist.
  u64 cached_load_id = UINT64_MAX;
  std::vector<TreeCache> trees;

  void release_trees() {
    for (auto& tree : trees) {
      if (tree.index_buffer != gpu::kInvalidHandle) {
        gpu::destroy_buffer(tree.index_buffer);
      }
      tree.time_of_day = nil;
    }
    trees.clear();
  }
};

MetalTFragment::MetalTFragment() : m_impl(std::make_unique<Impl>()) {}

MetalTFragment::~MetalTFragment() {
  if (m_impl) {
    m_impl->release_trees();
  }
}

bool MetalTFragment::init(id<MTLDevice> device,
                          id<MTLLibrary> library,
                          MTLPixelFormat color_format,
                          MTLPixelFormat depth_format) {
  m_impl->device = device;

  id<MTLFunction> vert = [library newFunctionWithName:@"tfrag3_vert"];
  id<MTLFunction> frag = [library newFunctionWithName:@"tfrag3_frag"];
  if (!vert || !frag) {
    lg::error("[Metal] tfrag3 shader entry points missing from the library");
    return false;
  }

  // Matches tfrag3::PreloadedVertex: float x,y,z at 0; u8 rgba at 12; float s,t at 16; u32 nor at
  // 24; u16 color_index at 28. The envmap tint and the normal are not read by this shader yet.
  MTLVertexDescriptor* vd = [[MTLVertexDescriptor alloc] init];
  vd.attributes[0].format = MTLVertexFormatFloat3;
  vd.attributes[0].offset = 0;
  vd.attributes[0].bufferIndex = MetalBufferIndexVertex;
  vd.attributes[1].format = MTLVertexFormatFloat2;
  vd.attributes[1].offset = 16;
  vd.attributes[1].bufferIndex = MetalBufferIndexVertex;
  vd.attributes[2].format = MTLVertexFormatUShort;
  vd.attributes[2].offset = 28;
  vd.attributes[2].bufferIndex = MetalBufferIndexVertex;
  vd.layouts[MetalBufferIndexVertex].stride = sizeof(tfrag3::PreloadedVertex);
  vd.layouts[MetalBufferIndexVertex].stepFunction = MTLVertexStepFunctionPerVertex;

  MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
  desc.vertexFunction = vert;
  desc.fragmentFunction = frag;
  desc.vertexDescriptor = vd;
  desc.colorAttachments[0].pixelFormat = color_format;
  desc.depthAttachmentPixelFormat = depth_format;

  NSError* err = nil;
  m_impl->pso = [device newRenderPipelineStateWithDescriptor:desc error:&err];
  if (!m_impl->pso) {
    lg::error("[Metal] tfrag3 pipeline state failed: {}",
              err ? [[err localizedDescription] UTF8String] : "unknown error");
    return false;
  }

  MTLDepthStencilDescriptor* ds = [[MTLDepthStencilDescriptor alloc] init];
  // The game's projection puts nearer geometry at a greater depth value, which is why the OpenGL
  // renderer uses GL_GEQUAL rather than GL_LEQUAL.
  ds.depthCompareFunction = MTLCompareFunctionGreaterEqual;
  ds.depthWriteEnabled = YES;
  m_impl->depth_state = [device newDepthStencilStateWithDescriptor:ds];

  MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
  sd.minFilter = MTLSamplerMinMagFilterLinear;
  sd.magFilter = MTLSamplerMinMagFilterLinear;
  sd.mipFilter = MTLSamplerMipFilterLinear;
  sd.sAddressMode = MTLSamplerAddressModeRepeat;
  sd.tAddressMode = MTLSamplerAddressModeRepeat;
  m_impl->sampler = [device newSamplerStateWithDescriptor:sd];

  lg::info("[Metal] tfrag3 pipeline ready");
  return true;
}

void MetalTFragment::render(id<MTLRenderCommandEncoder> encoder,
                            const LevelData& level,
                            const GoalBackgroundCameraData& camera) {
  m_last_frame_tris = 0;
  if (!m_impl->pso || !level.level) {
    return;
  }

  // Geometry level 0 only for now: the game picks a lower-detail set at distance, which needs the
  // per-tree distance check the OpenGL renderer does.
  const auto& in_trees = level.level->tfrag_trees[0];
  if (in_trees.empty()) {
    return;
  }

  if (m_impl->cached_load_id != level.load_id) {
    m_impl->release_trees();
    m_impl->trees.resize(in_trees.size());
    for (size_t i = 0; i < in_trees.size(); i++) {
      const auto& in_tree = in_trees[i];
      auto& cache = m_impl->trees[i];
      // The whole index list, not a per-frame list built from the visibility strings. 0xFFFFFFFF
      // is Metal's primitive-restart value for a 32-bit index buffer, which is the same sentinel
      // the .fr3 already uses.
      cache.index_buffer =
          gpu::create_buffer(gpu::BufferKind::Index, in_tree.unpacked.indices.size() * sizeof(u32),
                             in_tree.unpacked.indices.data());
      cache.time_of_day =
          [m_impl->device newBufferWithLength:in_tree.colors.color_count * sizeof(float) * 4
                                      options:MTLResourceStorageModeShared];
      cache.color_scratch.resize(in_tree.colors.color_count);
    }
    m_impl->cached_load_id = level.load_id;
    lg::info("[Metal] tfrag3: cached {} trees for load id {}", in_trees.size(), level.load_id);
  }

  const auto new_cam = make_new_cam_mat(camera.rot, camera.perspective, camera.fog.x(),
                                        camera.hvdf_off.z());

  Tfrag3Uniforms uniforms{};
  for (int col = 0; col < 4; col++) {
    uniforms.pc_camera.col[col] = {new_cam[col][0], new_cam[col][1], new_cam[col][2],
                                   new_cam[col][3]};
  }
  uniforms.hvdf_offset = {camera.hvdf_off[0], camera.hvdf_off[1], camera.hvdf_off[2],
                          camera.hvdf_off[3]};
  uniforms.cam_trans = {camera.trans[0], camera.trans[1], camera.trans[2], camera.trans[3]};
  // Fog is off until the bucket that carries the fog colour is ported; alpha = 0 leaves the
  // colour untouched in the shader's mix().
  uniforms.fog_color = {0.f, 0.f, 0.f, 0.f};
  uniforms.fog_min = camera.fog.y();
  uniforms.fog_max = camera.fog.z();
  // No alpha test yet: the per-draw modes come with the draw loop below.
  uniforms.alpha_min = -1.f;
  uniforms.alpha_max = 2.f;
  uniforms.scissor_adjust = 512.f / 448.f;  // Jak 1; 416 for Jak 2 and 3
  uniforms.height_scale = 1.f;
  uniforms.decal = 0;
  uniforms.gfx_hack_no_tex = 0;

  [encoder setRenderPipelineState:m_impl->pso];
  [encoder setDepthStencilState:m_impl->depth_state];
  [encoder setFragmentSamplerState:m_impl->sampler atIndex:0];
  [encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:MetalBufferIndexUniforms];
  [encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:MetalBufferIndexUniforms];

  for (size_t tree_idx = 0; tree_idx < in_trees.size(); tree_idx++) {
    const auto& in_tree = in_trees[tree_idx];
    auto& cache = m_impl->trees[tree_idx];
    id<MTLBuffer> index_buffer = metal_buffer_from_handle(cache.index_buffer);
    id<MTLBuffer> vertex_buffer =
        metal_buffer_from_handle(level.tfrag_vertex_data[0].at(tree_idx));
    if (!index_buffer || !vertex_buffer || !cache.time_of_day) {
      continue;
    }

    interp_time_of_day(camera.itimes, in_tree.colors, cache.color_scratch.data());
    // The shader reads these as float4; the interpolation produces bytes.
    auto* tod = (float*)[cache.time_of_day contents];
    for (u32 i = 0; i < in_tree.colors.color_count; i++) {
      const auto& c = cache.color_scratch[i];
      tod[i * 4 + 0] = c[0] / 255.f;
      tod[i * 4 + 1] = c[1] / 255.f;
      tod[i * 4 + 2] = c[2] / 255.f;
      tod[i * 4 + 3] = c[3] / 255.f;
    }

    [encoder setVertexBuffer:vertex_buffer offset:0 atIndex:MetalBufferIndexVertex];
    [encoder setVertexBuffer:cache.time_of_day offset:0 atIndex:MetalBufferIndexTimeOfDay];

    for (const auto& draw : in_tree.draws) {
      u32 index_count = 0;
      for (const auto& group : draw.vis_groups) {
        index_count += group.num_inds;
      }
      if (index_count == 0) {
        continue;
      }

      // A negative tree_tex_id means an animated texture slot, which needs the TextureAnimator.
      // Skip those draws rather than bind the wrong texture.
      if (draw.tree_tex_id < 0 || (size_t)draw.tree_tex_id >= level.textures.size()) {
        continue;
      }
      id<MTLTexture> texture = metal_texture_from_handle(level.textures[draw.tree_tex_id]);
      if (!texture) {
        continue;
      }
      [encoder setFragmentTexture:texture atIndex:0];

      [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
                          indexCount:index_count
                           indexType:MTLIndexTypeUInt32
                         indexBuffer:index_buffer
                   indexBufferOffset:draw.unpacked.idx_of_first_idx_in_full_buffer * sizeof(u32)];
      m_last_frame_tris += draw.num_triangles;
    }
  }
}
