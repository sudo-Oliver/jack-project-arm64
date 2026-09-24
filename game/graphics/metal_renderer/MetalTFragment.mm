/*!
 * @file MetalTFragment.mm
 * See MetalTFragment.h.
 */

#include "MetalTFragment.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <cstring>

#include "common/log/log.h"

#include "game/graphics/gpu_resources.h"
#include "game/graphics/metal_renderer/MetalGpuResources.h"

#include "metal_shader_types.h"

namespace {

// How many frames of index/colour buffers to rotate through. The GPU may still be reading last
// frame's buffer when this frame writes, and Metal does no renaming behind our back the way the
// OpenGL driver does for glBufferData.
constexpr int kFramesInFlight = 3;

// One tree's GPU-side data. The vertex buffer comes from the shared loader; the index list is
// ours, because it is rebuilt every frame from the visibility strings.
struct TreeCache {
  std::array<id<MTLBuffer>, kFramesInFlight> index_buffer = {nil, nil, nil};
  std::array<id<MTLBuffer>, kFramesInFlight> time_of_day = {nil, nil, nil};
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

  // Scratch shared by every tree, sized for the largest. Mirrors TFragment's m_cache.
  std::vector<u8> vis_temp;
  std::vector<std::pair<int, int>> draw_idx_temp;
  std::vector<u32> index_temp;
  int frame = 0;

  void release_trees() {
    for (auto& tree : trees) {
      for (int i = 0; i < kFramesInFlight; i++) {
        tree.index_buffer[i] = nil;
        tree.time_of_day[i] = nil;
      }
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
                            const GoalBackgroundCameraData& camera,
                            const u8* occlusion) {
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
    size_t max_inds = 0, max_draws = 0, max_vis = 0;
    for (size_t i = 0; i < in_trees.size(); i++) {
      const auto& in_tree = in_trees[i];
      auto& cache = m_impl->trees[i];
      for (int f = 0; f < kFramesInFlight; f++) {
        // Sized for the whole index list: that is the worst case, when everything is visible.
        // 0xFFFFFFFF is Metal's primitive-restart value for a 32-bit index buffer, which is the
        // same sentinel the .fr3 already uses for its strips.
        cache.index_buffer[f] =
            [m_impl->device newBufferWithLength:in_tree.unpacked.indices.size() * sizeof(u32)
                                        options:MTLResourceStorageModeShared];
        cache.time_of_day[f] =
            [m_impl->device newBufferWithLength:in_tree.colors.color_count * sizeof(float) * 4
                                        options:MTLResourceStorageModeShared];
      }
      cache.color_scratch.resize(in_tree.colors.color_count);
      max_inds = std::max(max_inds, in_tree.unpacked.indices.size());
      max_draws = std::max(max_draws, in_tree.draws.size());
      max_vis = std::max(max_vis, in_tree.bvh.vis_nodes.size());
    }
    m_impl->index_temp.resize(max_inds);
    m_impl->draw_idx_temp.resize(max_draws);
    m_impl->vis_temp.resize(max_vis);
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

  const int frame = m_impl->frame;
  m_impl->frame = (m_impl->frame + 1) % kFramesInFlight;

  for (size_t tree_idx = 0; tree_idx < in_trees.size(); tree_idx++) {
    const auto& in_tree = in_trees[tree_idx];
    // Only the kinds the TFRAG_LEVEL0 bucket owns. TRANS, WATER, DIRT and ICE trees belong to
    // other buckets with their own blend and alpha settings -- drawing them here put opaque
    // slabs of a neighbouring area in mid-air.
    if (in_tree.kind != tfrag3::TFragmentTreeKind::NORMAL &&
        in_tree.kind != tfrag3::TFragmentTreeKind::LOWRES) {
      continue;
    }
    auto& cache = m_impl->trees[tree_idx];
    id<MTLBuffer> index_buffer = cache.index_buffer[frame];
    id<MTLBuffer> tod_buffer = cache.time_of_day[frame];
    id<MTLBuffer> vertex_buffer =
        metal_buffer_from_handle(level.tfrag_vertex_data[0].at(tree_idx));
    if (!index_buffer || !vertex_buffer || !tod_buffer) {
      continue;
    }

    // Frustum culling against the BVH, then an index list built from the visibility strings --
    // the same two steps the OpenGL renderer's no_multidraw path takes. Without this, geometry the
    // game has hidden is drawn anyway: whole chunks of a neighbouring area float in mid-air.
    //
    cull_check_all_slow(camera.planes, in_tree.bvh.vis_nodes, occlusion, m_impl->vis_temp.data());
    u32 total_tris = 0;
    const u32 index_count_total = make_index_list_from_vis_string(
        m_impl->draw_idx_temp.data(), m_impl->index_temp.data(), in_tree.draws, m_impl->vis_temp,
        in_tree.unpacked.indices.data(), &total_tris);
    if (index_count_total == 0) {
      continue;
    }
    memcpy([index_buffer contents], m_impl->index_temp.data(), index_count_total * sizeof(u32));

    interp_time_of_day(camera.itimes, in_tree.colors, cache.color_scratch.data());
    // The shader reads these as float4; the interpolation produces bytes.
    auto* tod = (float*)[tod_buffer contents];
    for (u32 i = 0; i < in_tree.colors.color_count; i++) {
      const auto& c = cache.color_scratch[i];
      tod[i * 4 + 0] = c[0] / 255.f;
      tod[i * 4 + 1] = c[1] / 255.f;
      tod[i * 4 + 2] = c[2] / 255.f;
      tod[i * 4 + 3] = c[3] / 255.f;
    }

    [encoder setVertexBuffer:vertex_buffer offset:0 atIndex:MetalBufferIndexVertex];
    [encoder setVertexBuffer:tod_buffer offset:0 atIndex:MetalBufferIndexTimeOfDay];

    for (size_t draw_idx = 0; draw_idx < in_tree.draws.size(); draw_idx++) {
      const auto& draw = in_tree.draws[draw_idx];
      const auto& visible = m_impl->draw_idx_temp[draw_idx];
      if (visible.second == 0) {
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
                          indexCount:visible.second
                           indexType:MTLIndexTypeUInt32
                         indexBuffer:index_buffer
                   indexBufferOffset:visible.first * sizeof(u32)];
    }
    m_last_frame_tris += total_tris;
  }
}
