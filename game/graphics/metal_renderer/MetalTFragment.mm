/*!
 * @file MetalTFragment.mm
 * See MetalTFragment.h.
 */

#include "MetalTFragment.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>

#include "common/log/log.h"

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
  id<MTLDevice> device = nil;
  MetalDrawStateCache states;
  bool ready = false;

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

MetalTFragment::MetalTFragment(const std::string& name,
                               int my_id,
                               std::vector<tfrag3::TFragmentTreeKind> tree_kinds,
                               int level_id)
    : MetalBucketRenderer(name, my_id),
      m_impl(std::make_unique<Impl>()),
      m_tree_kinds(std::move(tree_kinds)),
      m_level_id(level_id) {}

MetalTFragment::~MetalTFragment() {
  if (m_impl) {
    m_impl->release_trees();
  }
}

bool MetalTFragment::init(MetalRenderState* render_state) {
  m_impl->device = render_state->device;

  id<MTLFunction> vert = [render_state->library newFunctionWithName:@"tfrag3_vert"];
  id<MTLFunction> frag = [render_state->library newFunctionWithName:@"tfrag3_frag"];
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

  m_impl->states.init(render_state->device, vert, frag, vd, render_state->color_format,
                      render_state->depth_format);

  // Build one now, so a broken shader or vertex layout is reported here rather than on the first
  // frame that happens to use that draw mode.
  DrawMode probe;
  probe.set_depth_write_enable(true);
  if (!m_impl->states.pipeline(probe)) {
    return false;
  }
  m_impl->ready = true;

  lg::info("[Metal] tfrag3 pipeline ready");
  return true;
}

void MetalTFragment::render(DmaFollower& dma, MetalRenderState* render_state) {
  m_last_frame_tris = 0;

  // The camera and the occlusion strings are lifted out of the chain by MetalRenderer before the
  // buckets run, so this only has to skip to the end of its own bucket. Consuming the chain the
  // way TFragment::render does -- walking the VIF unpack sequence -- is what the near variants
  // will need; this renderer takes its geometry from the .fr3 instead.
  while (dma.current_tag_offset() != render_state->next_bucket && !dma.ended()) {
    dma.read_and_advance();
  }

  const auto& slot = render_state->level_slots[m_level_id];
  if (!m_impl->ready || !slot.has_camera || slot.level_name.empty()) {
    return;
  }
  const auto* level_ptr = render_state->loader->get_tfrag3_level(slot.level_name);
  if (!level_ptr) {
    return;
  }
  draw_level(render_state, *level_ptr);
}

void MetalTFragment::draw_level(MetalRenderState* render_state, const LevelData& level) {
  if (!level.level) {
    return;
  }
  const auto& camera = render_state->level_slots[m_level_id].camera;
  const u8* occlusion = render_state->occlusion_for_level(m_level_id);
  id<MTLRenderCommandEncoder> encoder = render_state->encoder;

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
    lg::info("[Metal] {}: cached {} trees for load id {}", m_name, in_trees.size(),
             level.load_id);
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
  uniforms.fog_color = {render_state->fog_color[0] / 255.f, render_state->fog_color[1] / 255.f,
                        render_state->fog_color[2] / 255.f, 0.f};
  uniforms.fog_min = camera.fog.y();
  uniforms.fog_max = camera.fog.z();
  // alpha_min/max and decal are per draw; set in the loop below.
  uniforms.scissor_adjust = 512.f / 448.f;  // Jak 1; 416 for Jak 2 and 3
  uniforms.height_scale = 1.f;
  uniforms.gfx_hack_no_tex = 0;

  // SRC_DST_FIX_DST blends against a constant of 0.5; GL sets it with glBlendColor.
  [encoder setBlendColorRed:0.5f green:0.5f blue:0.5f alpha:0.5f];

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

      // The per-draw GS state. alpha_max stays at 10 to match setup_tfrag_shader, which only ever
      // constrains the low end.
      const DoubleDraw double_draw = alpha_test_double_draw(draw.mode);
      uniforms.alpha_min = double_draw.aref_first;
      uniforms.alpha_max = 10.f;
      uniforms.decal = draw.mode.get_decal() ? 1 : 0;

      id<MTLRenderPipelineState> pso = m_impl->states.pipeline(draw.mode);
      if (!pso) {
        continue;
      }
      [encoder setRenderPipelineState:pso];
      [encoder setDepthStencilState:m_impl->states.depth_state(draw.mode, false)];
      [encoder setFragmentSamplerState:m_impl->states.sampler(draw.mode) atIndex:0];
      [encoder setFragmentTexture:texture atIndex:0];
      [encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:MetalBufferIndexUniforms];
      [encoder setFragmentBytes:&uniforms
                         length:sizeof(uniforms)
                        atIndex:MetalBufferIndexUniforms];

      [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
                          indexCount:visible.second
                           indexType:MTLIndexTypeUInt32
                         indexBuffer:index_buffer
                   indexBufferOffset:visible.first * sizeof(u32)];

      // A draw whose alpha test fails to framebuffer-only, with depth writes on, is two draws:
      // the first writes depth for the pixels that pass, the second fills in the rest without
      // touching depth.
      if (double_draw.kind == DoubleDrawKind::AFAIL_NO_DEPTH_WRITE) {
        uniforms.alpha_min = -10.f;
        uniforms.alpha_max = double_draw.aref_second;
        [encoder setDepthStencilState:m_impl->states.depth_state(draw.mode, true)];
        [encoder setVertexBytes:&uniforms
                         length:sizeof(uniforms)
                        atIndex:MetalBufferIndexUniforms];
        [encoder setFragmentBytes:&uniforms
                           length:sizeof(uniforms)
                          atIndex:MetalBufferIndexUniforms];
        [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
                            indexCount:visible.second
                             indexType:MTLIndexTypeUInt32
                           indexBuffer:index_buffer
                     indexBufferOffset:visible.first * sizeof(u32)];
      }
    }
    m_last_frame_tris += total_tris;
  }
}
