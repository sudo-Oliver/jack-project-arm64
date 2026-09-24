/*!
 * @file MetalShrub.mm
 * See MetalShrub.h.
 */

#include "MetalShrub.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include "common/log/log.h"

#include "game/graphics/metal_renderer/MetalGpuResources.h"

#include "metal_shader_types.h"

namespace {

constexpr int kFramesInFlight = 3;

struct ShrubTreeCache {
  id<MTLBuffer> index_buffer = nil;  // static: shrub has no per-frame visibility
  std::array<id<MTLBuffer>, kFramesInFlight> time_of_day = {nil, nil, nil};
  std::vector<math::Vector<u8, 4>> color_scratch;
};

}  // namespace

struct MetalShrub::Impl {
  id<MTLDevice> device = nil;
  MetalDrawStateCache states;
  bool ready = false;

  u64 cached_load_id = UINT64_MAX;
  std::vector<ShrubTreeCache> trees;
  int frame = 0;

  void release_trees() {
    for (auto& tree : trees) {
      tree.index_buffer = nil;
      for (int i = 0; i < kFramesInFlight; i++) {
        tree.time_of_day[i] = nil;
      }
    }
    trees.clear();
  }
};

MetalShrub::MetalShrub(const std::string& name, int my_id, int level_id)
    : MetalBucketRenderer(name, my_id), m_impl(std::make_unique<Impl>()), m_level_id(level_id) {}

MetalShrub::~MetalShrub() {
  if (m_impl) {
    m_impl->release_trees();
  }
}

bool MetalShrub::init(MetalRenderState* render_state) {
  m_impl->device = render_state->device;

  id<MTLFunction> vert = [render_state->library newFunctionWithName:@"shrub_vert"];
  id<MTLFunction> frag = [render_state->library newFunctionWithName:@"shrub_frag"];
  if (!vert || !frag) {
    lg::error("[Metal] shrub shader entry points missing from the library");
    return false;
  }

  // tfrag3::ShrubGpuVertex: float x,y,z at 0; float s,t at 12; u32 pad at 20; u16 color_index at
  // 24; u16 pad at 26; u8 rgba_base[3] at 28.
  MTLVertexDescriptor* vd = [[MTLVertexDescriptor alloc] init];
  vd.attributes[0].format = MTLVertexFormatFloat3;
  vd.attributes[0].offset = 0;
  vd.attributes[0].bufferIndex = MetalBufferIndexVertex;
  vd.attributes[1].format = MTLVertexFormatFloat2;
  vd.attributes[1].offset = 12;
  vd.attributes[1].bufferIndex = MetalBufferIndexVertex;
  vd.attributes[2].format = MTLVertexFormatUShort;
  vd.attributes[2].offset = 24;
  vd.attributes[2].bufferIndex = MetalBufferIndexVertex;
  vd.attributes[3].format = MTLVertexFormatUChar3;
  vd.attributes[3].offset = 28;
  vd.attributes[3].bufferIndex = MetalBufferIndexVertex;
  vd.layouts[MetalBufferIndexVertex].stride = sizeof(tfrag3::ShrubGpuVertex);
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

void MetalShrub::render(DmaFollower& dma, MetalRenderState* render_state) {
  m_last_frame_tris = 0;

  // The geometry comes from the .fr3, so this bucket's chain only has to be skipped.
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

void MetalShrub::draw_level(MetalRenderState* render_state, const LevelData& level) {
  if (!level.level) {
    return;
  }
  const auto& in_trees = level.level->shrub_trees;
  if (in_trees.empty()) {
    return;
  }

  if (m_impl->cached_load_id != level.load_id) {
    m_impl->release_trees();
    m_impl->trees.resize(in_trees.size());
    for (size_t i = 0; i < in_trees.size(); i++) {
      const auto& in_tree = in_trees[i];
      auto& cache = m_impl->trees[i];
      cache.index_buffer =
          [m_impl->device newBufferWithBytes:in_tree.indices.data()
                                      length:in_tree.indices.size() * sizeof(u32)
                                     options:MTLResourceStorageModeShared];
      for (int f = 0; f < kFramesInFlight; f++) {
        cache.time_of_day[f] = [m_impl->device
            newBufferWithLength:in_tree.time_of_day_colors.color_count * sizeof(float) * 4
                        options:MTLResourceStorageModeShared];
      }
      cache.color_scratch.resize(in_tree.time_of_day_colors.color_count);
    }
    m_impl->cached_load_id = level.load_id;
    lg::info("[Metal] {}: cached {} shrub trees for load id {}", m_name, in_trees.size(),
             level.load_id);
  }

  const auto& camera = render_state->level_slots[m_level_id].camera;
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
  uniforms.fog_color = {render_state->fog_color[0] / 255.f, render_state->fog_color[1] / 255.f,
                        render_state->fog_color[2] / 255.f, 0.f};
  uniforms.fog_min = camera.fog.y();
  uniforms.fog_max = camera.fog.z();
  uniforms.scissor_adjust = 512.f / 448.f;
  uniforms.height_scale = 1.f;
  uniforms.gfx_hack_no_tex = 0;

  [encoder setBlendColorRed:0.5f green:0.5f blue:0.5f alpha:0.5f];

  const int frame = m_impl->frame;
  m_impl->frame = (m_impl->frame + 1) % kFramesInFlight;

  for (size_t tree_idx = 0; tree_idx < in_trees.size(); tree_idx++) {
    const auto& in_tree = in_trees[tree_idx];
    auto& cache = m_impl->trees[tree_idx];
    id<MTLBuffer> tod_buffer = cache.time_of_day[frame];
    id<MTLBuffer> vertex_buffer =
        metal_buffer_from_handle(level.shrub_vertex_data.at(tree_idx));
    if (!cache.index_buffer || !vertex_buffer || !tod_buffer) {
      continue;
    }

    interp_time_of_day(camera.itimes, in_tree.time_of_day_colors, cache.color_scratch.data());
    auto* tod = (float*)[tod_buffer contents];
    for (u32 i = 0; i < in_tree.time_of_day_colors.color_count; i++) {
      const auto& c = cache.color_scratch[i];
      tod[i * 4 + 0] = c[0] / 255.f;
      tod[i * 4 + 1] = c[1] / 255.f;
      tod[i * 4 + 2] = c[2] / 255.f;
      tod[i * 4 + 3] = c[3] / 255.f;
    }

    [encoder setVertexBuffer:vertex_buffer offset:0 atIndex:MetalBufferIndexVertex];
    [encoder setVertexBuffer:tod_buffer offset:0 atIndex:MetalBufferIndexTimeOfDay];

    for (const auto& draw : in_tree.static_draws) {
      if (draw.num_indices == 0) {
        continue;
      }
      if ((size_t)draw.tree_tex_id >= level.textures.size()) {
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
      [encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:MetalBufferIndexUniforms];
      [encoder setFragmentBytes:&uniforms
                         length:sizeof(uniforms)
                        atIndex:MetalBufferIndexUniforms];

      [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
                          indexCount:draw.num_indices
                           indexType:MTLIndexTypeUInt32
                         indexBuffer:cache.index_buffer
                   indexBufferOffset:draw.first_index_index * sizeof(u32)];

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
                            indexCount:draw.num_indices
                             indexType:MTLIndexTypeUInt32
                           indexBuffer:cache.index_buffer
                     indexBufferOffset:draw.first_index_index * sizeof(u32)];
      }
      m_last_frame_tris += draw.num_triangles;
    }
  }
}
