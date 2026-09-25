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

  // Where each wind draw's slice of the tree's wind index buffer starts. The loader packs every
  // wind draw's stream into one buffer, in draw order, and does not record the offsets.
  std::vector<u32> wind_index_offsets;
  std::vector<std::array<math::Vector4f, 4>> wind_matrices;
};

// The categories this renderer draws. NORMAL is the plain geometry; NORMAL_ENVMAP is the base
// pass of the shiny geometry, which uses ETIE_BASE rather than the plain shader: it has to round
// identically to the reflection pass that is drawn over it, or the two z-fight.
// NORMAL_ENVMAP is not in this list: it is drawn by draw_tree_envmap(), which does its base pass
// and its reflection pass together, in that order, which is the order the OpenGL backend uses.
constexpr tfrag3::TieCategory kCategories[] = {tfrag3::TieCategory::NORMAL};

}  // namespace

struct MetalTie3::Impl {
  id<MTLDevice> device = nil;
  MetalDrawStateCache states;
  bool ready = false;

  u64 cached_load_id = UINT64_MAX;
  std::vector<TieTreeCache> trees;

  // The wind sway is integrated over time, so this has to survive between frames.
  std::vector<float> wind_vectors;
  float wind_multiplier = 1.f;
  MetalDrawStateCache wind_states;
  // The shiny geometry: its base pass, then its reflection pass.
  MetalDrawStateCache etie_base_states;
  MetalDrawStateCache etie_states;

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
  // Only the shiny geometry reads these two: the normal, packed as 2-10-10-10, and the
  // per-instance tint. Declaring them for every tie pipeline costs nothing and keeps one
  // descriptor for all four shaders.
  vd.attributes[3].format = MTLVertexFormatInt1010102Normalized;
  vd.attributes[3].offset = 24;
  vd.attributes[3].bufferIndex = MetalBufferIndexVertex;
  vd.attributes[4].format = MTLVertexFormatUChar4Normalized;
  vd.attributes[4].offset = 12;
  vd.attributes[4].bufferIndex = MetalBufferIndexVertex;
  vd.layouts[MetalBufferIndexVertex].stride = sizeof(tfrag3::PreloadedVertex);
  vd.layouts[MetalBufferIndexVertex].stepFunction = MTLVertexStepFunctionPerVertex;

  m_impl->states.init(render_state->device, vert, frag, vd, render_state->color_format,
                      render_state->depth_format);

  // The swaying instances use their own shader: it is handed the instance's matrix rather than a
  // matrix with the perspective already folded in, so it does the divide itself.
  id<MTLFunction> wind_vert = [render_state->library newFunctionWithName:@"tie_wind_vert"];
  id<MTLFunction> wind_frag = [render_state->library newFunctionWithName:@"tie_wind_frag"];
  if (!wind_vert || !wind_frag) {
    lg::error("[Metal] tie_wind shader entry points missing from the library");
    return false;
  }
  m_impl->wind_states.init(render_state->device, wind_vert, wind_frag, vd,
                           render_state->color_format, render_state->depth_format);

  id<MTLFunction> etie_base_vert = [render_state->library newFunctionWithName:@"etie_base_vert"];
  id<MTLFunction> etie_vert = [render_state->library newFunctionWithName:@"etie_vert"];
  id<MTLFunction> etie_frag = [render_state->library newFunctionWithName:@"etie_frag"];
  if (!etie_base_vert || !etie_vert || !etie_frag) {
    lg::error("[Metal] etie shader entry points missing from the library");
    return false;
  }
  m_impl->etie_base_states.init(render_state->device, etie_base_vert, etie_frag, vd,
                                render_state->color_format, render_state->depth_format);
  m_impl->etie_states.init(render_state->device, etie_vert, etie_frag, vd,
                           render_state->color_format, render_state->depth_format);

  DrawMode probe;
  probe.set_depth_write_enable(true);
  if (!m_impl->states.pipeline(probe)) {
    return false;
  }
  if (!m_impl->wind_states.pipeline(probe)) {
    return false;
  }
  if (!m_impl->etie_base_states.pipeline(probe) || !m_impl->etie_states.pipeline(probe)) {
    return false;
  }
  m_impl->ready = true;
  return true;
}

void MetalTie3::render(DmaFollower& dma, MetalRenderState* render_state) {
  m_last_frame_tris = 0;

  // The wind state rides in this bucket, and nothing else here reads the chain, so pick it out on
  // the way past. It is the one transfer in the bucket the size of a TieWindWork.
  m_has_wind_data = false;
  bool want_envmap_color = false;
  m_envmap_color = math::Vector4f(1.f, 1.f, 1.f, 1.f);
  while (dma.current_tag_offset() != render_state->next_bucket && !dma.ended()) {
    auto transfer = dma.read_and_advance();
    if (!m_has_wind_data && transfer.size_bytes == (int)sizeof(TieWindWork)) {
      memcpy(&m_wind_data, transfer.data, sizeof(TieWindWork));
      m_has_wind_data = true;
      // The envmap tint is the next transfer, one quadword.
      want_envmap_color = true;
      continue;
    }
    if (want_envmap_color && transfer.size_bytes == 16) {
      memcpy(m_envmap_color.data(), transfer.data, 16);
      // Same scaling the OpenGL backend applies for Jak 1.
      m_envmap_color /= 128.f;
      m_envmap_color *= 2.f;
      want_envmap_color = false;
    }
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

      // The loader concatenates every wind draw's index stream into one buffer, in draw order.
      cache.wind_index_offsets.clear();
      u32 wind_off = 0;
      for (const auto& draw : in_tree.instanced_wind_draws) {
        cache.wind_index_offsets.push_back(wind_off);
        wind_off += draw.vertex_index_stream.size();
      }
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

    draw_tree_envmap(render_state, level, in_tree, tfrag3::TieCategory::NORMAL_ENVMAP);
    draw_tree_wind(render_state, level, tree_idx, in_tree);
  }
}

/*!
 * The shiny geometry: a base pass that looks like ordinary tie, then a second pass that adds the
 * reflection over it. Both use the same visibility the plain draws already computed, and the same
 * split camera transform, so the two line up exactly.
 */
void MetalTie3::draw_tree_envmap(MetalRenderState* render_state,
                                 const LevelData& level,
                                 const tfrag3::TieTree& in_tree,
                                 tfrag3::TieCategory category) {
  const size_t first = in_tree.category_draw_indices[(int)category];
  const size_t last = in_tree.category_draw_indices[(int)category + 1];
  if (first >= last) {
    return;
  }
  id<MTLRenderCommandEncoder> encoder = render_state->encoder;
  const auto& camera = render_state->level_slots[m_level_id].camera;
  auto& cache = m_impl->trees[&in_tree - level.level->tie_trees[0].data()];
  const int frame = (m_impl->frame + kMetalFramesInFlight - 1) % kMetalFramesInFlight;
  id<MTLBuffer> index_buffer = cache.index_buffer[frame];
  if (!index_buffer || !encoder) {
    return;
  }

  EtieUniforms u{};
  for (int col = 0; col < 4; col++) {
    u.cam_no_persp.col[col] = {camera.rot[col][0], camera.rot[col][1], camera.rot[col][2],
                               camera.rot[col][3]};
  }
  // init_etie_cam_uniforms: the perspective, split into the two vectors the VU program used.
  {
    const float inv_fog = 1.f / camera.fog[0];
    const auto& hvdf_off = camera.hvdf_off;
    const float pxx = camera.perspective[0].x();
    const float pyy = camera.perspective[1].y();
    const float pzz = camera.perspective[2].z();
    const float pzw = camera.perspective[2].w();
    const float pwz = camera.perspective[3].z();
    const float scale = pzw * inv_fog;
    u.persp0 = {scale * hvdf_off.x(), scale * hvdf_off.y(), scale * hvdf_off.z() + pzz, scale};
    u.persp1 = {pxx, pyy, pwz, 0.f};
  }
  u.hvdf_offset = {camera.hvdf_off[0], camera.hvdf_off[1], camera.hvdf_off[2],
                   camera.hvdf_off[3]};
  u.fog_color = {render_state->fog_color[0] / 255.f, render_state->fog_color[1] / 255.f,
                 render_state->fog_color[2] / 255.f, render_state->fog_intensity / 255.f};
  u.envmap_tod_tint = {m_envmap_color[0], m_envmap_color[1], m_envmap_color[2],
                       m_envmap_color[3]};
  u.fog_min = camera.fog.y();
  u.fog_max = camera.fog.z();
  u.scissor_adjust = 512.f / 448.f;
  u.height_scale = 1.f;
  u.gfx_hack_no_tex = 0;

  // Pass one is the base, pass two the reflection; the only differences are the pipeline and the
  // alpha-fail double draw, which only the base does.
  for (int pass = 0; pass < 2; pass++) {
    auto& states = pass == 0 ? m_impl->etie_base_states : m_impl->etie_states;
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
      id<MTLRenderPipelineState> pso = states.pipeline(draw.mode);
      if (!pso) {
        continue;
      }

      const DoubleDraw double_draw = alpha_test_double_draw(draw.mode);
      u.alpha_min = double_draw.aref_first;
      u.alpha_max = 10.f;
      u.decal = draw.mode.get_decal() ? 1 : 0;

      [encoder setRenderPipelineState:pso];
      [encoder setDepthStencilState:states.depth_state(draw.mode, false)];
      [encoder setFragmentSamplerState:states.sampler(draw.mode) atIndex:0];
      [encoder setFragmentTexture:texture atIndex:0];
      [encoder setVertexBytes:&u length:sizeof(u) atIndex:MetalBufferIndexUniforms];
      [encoder setFragmentBytes:&u length:sizeof(u) atIndex:MetalBufferIndexUniforms];
      [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
                          indexCount:visible.second
                           indexType:MTLIndexTypeUInt32
                         indexBuffer:index_buffer
                   indexBufferOffset:visible.first * sizeof(u32)];
      m_last_frame_tris += draw.num_triangles;

      if (pass == 0 && double_draw.kind == DoubleDrawKind::AFAIL_NO_DEPTH_WRITE) {
        u.alpha_min = -10.f;
        u.alpha_max = double_draw.aref_second;
        [encoder setDepthStencilState:states.depth_state(draw.mode, true)];
        [encoder setVertexBytes:&u length:sizeof(u) atIndex:MetalBufferIndexUniforms];
        [encoder setFragmentBytes:&u length:sizeof(u) atIndex:MetalBufferIndexUniforms];
        [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
                            indexCount:visible.second
                             indexType:MTLIndexTypeUInt32
                           indexBuffer:index_buffer
                     indexBufferOffset:visible.first * sizeof(u32)];
      }
    }
  }
}

/*!
 * The swaying instances. One draw per instance group, because the matrix changes per instance.
 */
void MetalTie3::draw_tree_wind(MetalRenderState* render_state,
                               const LevelData& level,
                               size_t tree_idx,
                               const tfrag3::TieTree& in_tree) {
  if (in_tree.instanced_wind_draws.empty() || !m_has_wind_data) {
    return;
  }
  const auto& tie_gl = level.tie_data[0].at(tree_idx);
  if (!tie_gl.has_wind) {
    return;
  }
  id<MTLBuffer> wind_index_buffer = metal_buffer_from_handle(tie_gl.wind_indices);
  id<MTLBuffer> vertex_buffer = metal_buffer_from_handle(tie_gl.vertex_buffer);
  auto& cache = m_impl->trees[tree_idx];
  const int frame = (m_impl->frame + kMetalFramesInFlight - 1) % kMetalFramesInFlight;
  id<MTLBuffer> tod_buffer = cache.time_of_day[frame];
  if (!wind_index_buffer || !vertex_buffer || !tod_buffer) {
    return;
  }

  const auto& camera = render_state->level_slots[m_level_id].camera;
  std::array<math::Vector4f, 4> cam;
  for (int i = 0; i < 4; i++) {
    cam[i] = camera.camera[i];
  }
  compute_tie_wind_matrices(m_wind_data, in_tree.wind_instance_info, cam,
                            m_impl->wind_multiplier, m_impl->wind_vectors, cache.wind_matrices);

  id<MTLRenderCommandEncoder> encoder = render_state->encoder;
  TieWindUniforms u{};
  u.hvdf_offset = {camera.hvdf_off[0], camera.hvdf_off[1], camera.hvdf_off[2],
                   camera.hvdf_off[3]};
  u.fog_color = {render_state->fog_color[0] / 255.f, render_state->fog_color[1] / 255.f,
                 render_state->fog_color[2] / 255.f, render_state->fog_intensity / 255.f};
  u.fog_constant = camera.fog.x();
  u.fog_min = camera.fog.y();
  u.fog_max = camera.fog.z();
  u.scissor_adjust = 512.f / 448.f;
  u.height_scale = 1.f;
  u.gfx_hack_no_tex = 0;

  [encoder setVertexBuffer:vertex_buffer offset:0 atIndex:MetalBufferIndexVertex];
  [encoder setVertexBuffer:tod_buffer offset:0 atIndex:MetalBufferIndexTimeOfDay];

  for (size_t draw_idx = 0; draw_idx < in_tree.instanced_wind_draws.size(); draw_idx++) {
    const auto& draw = in_tree.instanced_wind_draws[draw_idx];
    if (draw.tree_tex_id < 0 || (size_t)draw.tree_tex_id >= level.textures.size()) {
      continue;
    }
    id<MTLTexture> texture = metal_texture_from_handle(level.textures[draw.tree_tex_id]);
    if (!texture) {
      continue;
    }
    id<MTLRenderPipelineState> pso = m_impl->wind_states.pipeline(draw.mode);
    if (!pso) {
      continue;
    }
    const DoubleDraw double_draw = alpha_test_double_draw(draw.mode);
    u.decal = draw.mode.get_decal() ? 1 : 0;

    [encoder setRenderPipelineState:pso];
    [encoder setFragmentSamplerState:m_impl->wind_states.sampler(draw.mode) atIndex:0];
    [encoder setFragmentTexture:texture atIndex:0];

    u32 off = 0;
    for (const auto& grp : draw.instance_groups) {
      if (grp.vis_idx >= m_impl->vis_temp.size() || !m_impl->vis_temp[grp.vis_idx]) {
        off += grp.num;
        continue;  // invisible, skip.
      }
      if (grp.instance_idx >= cache.wind_matrices.size()) {
        off += grp.num;
        continue;
      }
      const auto& mat = cache.wind_matrices[grp.instance_idx];
      for (int col = 0; col < 4; col++) {
        u.camera.col[col] = {mat[col][0], mat[col][1], mat[col][2], mat[col][3]};
      }

      u.alpha_min = double_draw.aref_first;
      u.alpha_max = 10.f;
      [encoder setDepthStencilState:m_impl->wind_states.depth_state(draw.mode, false)];
      [encoder setVertexBytes:&u length:sizeof(u) atIndex:MetalBufferIndexUniforms];
      [encoder setFragmentBytes:&u length:sizeof(u) atIndex:MetalBufferIndexUniforms];
      [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
                          indexCount:grp.num
                           indexType:MTLIndexTypeUInt32
                         indexBuffer:wind_index_buffer
                   indexBufferOffset:(off + cache.wind_index_offsets[draw_idx]) * sizeof(u32)];
      m_last_frame_tris += grp.num;

      if (double_draw.kind == DoubleDrawKind::AFAIL_NO_DEPTH_WRITE) {
        u.alpha_min = -10.f;
        u.alpha_max = double_draw.aref_second;
        [encoder setDepthStencilState:m_impl->wind_states.depth_state(draw.mode, true)];
        [encoder setVertexBytes:&u length:sizeof(u) atIndex:MetalBufferIndexUniforms];
        [encoder setFragmentBytes:&u length:sizeof(u) atIndex:MetalBufferIndexUniforms];
        [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
                            indexCount:grp.num
                             indexType:MTLIndexTypeUInt32
                           indexBuffer:wind_index_buffer
                     indexBufferOffset:(off + cache.wind_index_offsets[draw_idx]) * sizeof(u32)];
      }
      off += grp.num;
    }
  }
}
