/*!
 * @file MetalTie3.mm
 * See MetalTie3.h.
 */

#include "MetalTie3.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include "common/log/log.h"

#include "game/graphics/metal_renderer/MetalGpuResources.h"

#include "metal_shader_types.h"

namespace {


struct TieTreeCache {
  std::array<id<MTLBuffer>, kMetalFramesInFlight> index_buffer = {nil, nil, nil};
  std::array<id<MTLBuffer>, kMetalFramesInFlight> time_of_day = {nil, nil, nil};
  std::vector<math::Vector<u8, 4>> color_scratch;
};

// The categories this renderer draws. NORMAL is the plain geometry; NORMAL_ENVMAP is the base pass
// of the shiny geometry, which the OpenGL backend draws with ETIE_BASE. That shader differs only
// in doing the camera transform in two steps to avoid a rounding difference against the shiny
// second draw it has to line up with -- and the second draw is not ported yet, so the plain shader
// is used for both here. It becomes wrong the moment the shiny pass lands; that is the reason to
// port ETIE next rather than later.
constexpr tfrag3::TieCategory kCategories[] = {tfrag3::TieCategory::NORMAL,
                                               tfrag3::TieCategory::NORMAL_ENVMAP};

}  // namespace

struct MetalTie3::Impl {
  id<MTLDevice> device = nil;
  MetalDrawStateCache states;
  bool ready = false;

  u64 cached_load_id = UINT64_MAX;
  std::vector<TieTreeCache> trees;

  std::vector<u8> vis_temp;
  std::vector<std::pair<int, int>> draw_idx_temp;
  std::vector<u32> index_temp;
  int frame = 0;

  void release_trees() {
    for (auto& tree : trees) {
      for (int i = 0; i < kMetalFramesInFlight; i++) {
        tree.index_buffer[i] = nil;
        tree.time_of_day[i] = nil;
      }
    }
    trees.clear();
  }
};

MetalTie3::MetalTie3(const std::string& name, int my_id, int level_id)
    : MetalBucketRenderer(name, my_id), m_impl(std::make_unique<Impl>()), m_level_id(level_id) {}

MetalTie3::~MetalTie3() {
  if (m_impl) {
    m_impl->release_trees();
  }
}

bool MetalTie3::init(MetalRenderState* render_state) {
  m_impl->device = render_state->device;

  // Tie's plain categories use the same shader as tfrag3, and the same vertex type.
  id<MTLFunction> vert = [render_state->library newFunctionWithName:@"tfrag3_vert"];
  id<MTLFunction> frag = [render_state->library newFunctionWithName:@"tfrag3_frag"];
  if (!vert || !frag) {
    lg::error("[Metal] tie3 shader entry points missing from the library");
    return false;
  }

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

  DrawMode probe;
  probe.set_depth_write_enable(true);
  if (!m_impl->states.pipeline(probe)) {
    return false;
  }
  m_impl->ready = true;
  return true;
}

void MetalTie3::render(DmaFollower& dma, MetalRenderState* render_state) {
  m_last_frame_tris = 0;

  while (dma.current_tag_offset() != render_state->next_bucket && !dma.ended()) {
    dma.read_and_advance();
  }

  const auto& slot = render_state->level_slots[m_level_id];
  if (!m_impl->ready || !slot.has_camera || slot.level_name.empty()) {
    return;
  }
  const auto* level = render_state->loader->get_tfrag3_level(slot.level_name);
  if (!level) {
    return;
  }
  draw_level(render_state, *level);
}

void MetalTie3::draw_level(MetalRenderState* render_state, const LevelData& level) {
  if (!level.level) {
    return;
  }
  // Geometry level 0 only, as with tfrag3.
  const auto& in_trees = level.level->tie_trees[0];
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
      for (int f = 0; f < kMetalFramesInFlight; f++) {
        cache.index_buffer[f] =
            [m_impl->device newBufferWithLength:in_tree.unpacked.indices.size() * sizeof(u32)
                                        options:MTLResourceStorageModeShared];
        cache.time_of_day[f] =
            [m_impl->device newBufferWithLength:in_tree.colors.color_count * sizeof(float) * 4
                                        options:MTLResourceStorageModeShared];
      }
      cache.color_scratch.resize(in_tree.colors.color_count);
      max_inds = std::max(max_inds, in_tree.unpacked.indices.size());
      max_draws = std::max(max_draws, in_tree.static_draws.size());
      max_vis = std::max(max_vis, in_tree.bvh.vis_nodes.size());
    }
    m_impl->index_temp.resize(max_inds);
    m_impl->draw_idx_temp.resize(max_draws);
    m_impl->vis_temp.resize(max_vis);
    m_impl->cached_load_id = level.load_id;
    lg::info("[Metal] {}: cached {} tie trees for load id {}", m_name, in_trees.size(),
             level.load_id);
  }

  const auto& camera = render_state->level_slots[m_level_id].camera;
  const u8* occlusion = render_state->occlusion_for_level(m_level_id);
  id<MTLRenderCommandEncoder> encoder = render_state->encoder;

  const auto new_cam =
      make_new_cam_mat(camera.rot, camera.perspective, camera.fog.x(), camera.hvdf_off.z());

  Tfrag3Uniforms uniforms{};
  for (int col = 0; col < 4; col++) {
    uniforms.pc_camera.col[col] = {new_cam[col][0], new_cam[col][1], new_cam[col][2],
                                   new_cam[col][3]};
  }
  uniforms.hvdf_offset = {camera.hvdf_off[0], camera.hvdf_off[1], camera.hvdf_off[2],
                          camera.hvdf_off[3]};
  uniforms.cam_trans = {camera.trans[0], camera.trans[1], camera.trans[2], camera.trans[3]};
  // Same values setup_tfrag_shader passes: the colour from the frame's default registers, the
  // intensity as the alpha.
  uniforms.fog_color = {render_state->fog_color[0] / 255.f, render_state->fog_color[1] / 255.f,
                        render_state->fog_color[2] / 255.f,
                        render_state->fog_intensity / 255.f};
  uniforms.fog_min = camera.fog.y();
  uniforms.fog_max = camera.fog.z();
  uniforms.scissor_adjust = 512.f / 448.f;
  uniforms.height_scale = 1.f;
  uniforms.gfx_hack_no_tex = 0;

  [encoder setBlendColorRed:0.5f green:0.5f blue:0.5f alpha:0.5f];

  const int frame = m_impl->frame;
  m_impl->frame = (m_impl->frame + 1) % kMetalFramesInFlight;

  for (size_t tree_idx = 0; tree_idx < in_trees.size(); tree_idx++) {
    const auto& in_tree = in_trees[tree_idx];
    auto& cache = m_impl->trees[tree_idx];
    id<MTLBuffer> index_buffer = cache.index_buffer[frame];
    id<MTLBuffer> tod_buffer = cache.time_of_day[frame];
    id<MTLBuffer> vertex_buffer =
        metal_buffer_from_handle(level.tie_data[0].at(tree_idx).vertex_buffer);
    if (!index_buffer || !vertex_buffer || !tod_buffer) {
      continue;
    }

    cull_check_all_slow(camera.planes, in_tree.bvh.vis_nodes, occlusion, m_impl->vis_temp.data());
    u32 total_tris = 0;
    const u32 index_count_total = make_index_list_from_vis_string(
        m_impl->draw_idx_temp.data(), m_impl->index_temp.data(), in_tree.static_draws,
        m_impl->vis_temp, in_tree.unpacked.indices.data(), &total_tris);
    if (index_count_total == 0) {
      continue;
    }
    memcpy([index_buffer contents], m_impl->index_temp.data(), index_count_total * sizeof(u32));

    interp_time_of_day(camera.itimes, in_tree.colors, cache.color_scratch.data());
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

    for (auto category : kCategories) {
      const size_t first = in_tree.category_draw_indices[(int)category];
      const size_t last = in_tree.category_draw_indices[(int)category + 1];
      for (size_t draw_idx = first; draw_idx < last; draw_idx++) {
        const auto& draw = in_tree.static_draws[draw_idx];
        const auto& visible = m_impl->draw_idx_temp[draw_idx];
        if (visible.second == 0) {
          continue;
        }
        if (draw.tree_tex_id < 0 || (size_t)draw.tree_tex_id >= level.textures.size()) {
          continue;
        }
        id<MTLTexture> texture = metal_texture_from_handle(level.textures[draw.tree_tex_id]);
        if (!texture) {
          continue;
        }

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
        m_last_frame_tris += draw.num_triangles;
      }
    }
  }
}
