/*!
 * @file Merc2Core.cpp
 * See Merc2Core.h. Moved here from the OpenGL renderer: the DMA reading, the model lookup, the
 * bone and light allocation, blerc and the vertex modification are unchanged, and every call that
 * touched OpenGL became one of the five backend_ methods.
 */

#include "Merc2Core.h"

#include "common/global_profiler/GlobalProfiler.h"
#include "common/util/Assert.h"
#include "common/util/fnv.h"
#include "common/util/simd_util.h"

#include "game/graphics/gfx.h"

#include "fmt/format.h"
#include "third-party/imgui/imgui.h"

std::mutex g_merc_data_mutex;

Merc2Core::Merc2Core() {
  ASSERT(fnv64("the quick brown fox jumps over the lazy dog") == 0x7404cea13ff89bb0);

  // The draw buffers, which hold lists of draws until a flush.
  for (int i = 0; i < MAX_LEVELS; i++) {
    auto& draws = m_level_draw_buckets.emplace_back();
    draws.draws.resize(MAX_DRAWS_PER_LEVEL);
    draws.envmap_draws.resize(MAX_ENVMAP_DRAWS_PER_LEVEL);
  }

  m_mod_vtx_temp.resize(MAX_MOD_VTX);
  m_mod_vtx_unpack_temp.resize(MAX_MOD_VTX);

  for (auto& x : m_effect_debug_mask) {
    x = true;
  }
}

Merc2Core::~Merc2Core() = default;

/*!
 * Modify vertices for blerc.
 */
void blerc_avx(const u32* i_data,
               const u32* i_data_end,
               const tfrag3::BlercFloatData* floats,
               const float* weights,
               tfrag3::MercVertex* out,
               float multiplier) {
  // store a table of weights. It's faster to load the 16-bytes of weights than load and broadcast
  // the float.
  __m128 weights_table[Merc2Core::kMaxBlerc];
  for (int i = 0; i < Merc2Core::kMaxBlerc; i++) {
    weights_table[i] = _mm_set1_ps(weights[i] * multiplier);
  }

  // loop over vertices
  while (i_data != i_data_end) {
    // load the base position
    __m128 pos = _mm_load_ps(floats->v);
    __m128 nrm = _mm_load_ps(floats->v + 4);
    floats++;

    // loop over targets
    while (*i_data != tfrag3::Blerc::kTargetIdxTerminator) {
      // get the weights for this target, from the game data.
      __m128 weight_multiplier = weights_table[*i_data];
      // get the pos/normal offset for this target.
      __m128 posm = _mm_load_ps(floats->v);
      __m128 nrmm = _mm_load_ps(floats->v + 4);
      floats++;

      // apply weights and add
      posm = _mm_mul_ps(posm, weight_multiplier);
      nrmm = _mm_mul_ps(nrmm, weight_multiplier);
      pos = _mm_add_ps(pos, posm);
      nrm = _mm_add_ps(nrm, nrmm);

      i_data++;
    }
    i_data++;

    // store final position/normal.
    _mm_store_ps(out[*i_data].pos, pos);
    _mm_store_ps(out[*i_data].normal, nrm);
    i_data++;
  }
}
namespace {
float blerc_multiplier = 1.f;
}

void Merc2Core::model_mod_blerc_draws(int num_effects,
                                  const tfrag3::MercModel* model,
                                  const LevelData* lev,
                                  u32* mod_vtx_buffers,
                                  const float* blerc_weights,
                                  MercDebugStats* stats) {
  // loop over effects.
  for (int ei = 0; ei < num_effects; ei++) {
    const auto& effect = model->effects[ei];
    // some effects might have no mod draw info, and no modifiable vertices
    if (effect.mod.mod_draw.empty()) {
      continue;
    }

    // grab the backend's buffer
    const u32 mod_buffer = m_next_mod_vtx_buffer++;
    backend_ensure_mod_vtx_buffer(mod_buffer, lev);
    mod_vtx_buffers[ei] = mod_buffer;

    // check that we have enough room for the finished thing.
    if (effect.mod.vertices.size() > MAX_MOD_VTX) {
      fmt::print("More mod vertices than MAX_MOD_VTX. {} > {}\n", effect.mod.vertices.size(),
                 MAX_MOD_VTX);
      ASSERT_NOT_REACHED();
    }

    // start with the correct vertices from the model data:
    memcpy(m_mod_vtx_temp.data(), effect.mod.vertices.data(),
           sizeof(tfrag3::MercVertex) * effect.mod.vertices.size());

    // do blerc math
    const auto* f_data = effect.mod.blerc.float_data.data();
    const u32* i_data = effect.mod.blerc.int_data.data();
    const u32* i_data_end = i_data + effect.mod.blerc.int_data.size();
    blerc_avx(i_data, i_data_end, f_data, blerc_weights, m_mod_vtx_temp.data(), blerc_multiplier);

    // and upload to GPU
    stats->num_uploads++;
    stats->num_upload_bytes += effect.mod.vertices.size() * sizeof(tfrag3::MercVertex);
    {
      backend_upload_mod_vtx(mod_buffer, m_mod_vtx_temp.data(), effect.mod.vertices.size());
    }
  }
}

// We can run into a problem where adding a PC model would overflow the
// preallocated draw/bone buffers.
// So we break this part into two functions:
// - init_pc_model, which doesn't allocate bones/draws

void Merc2Core::model_mod_draws(int num_effects,
                            const tfrag3::MercModel* model,
                            const LevelData* lev,
                            const u8* input_data,
                            const DmaTransfer& setup,
                            u32* mod_vtx_buffers,
                            MercDebugStats* stats) {
  auto p = scoped_prof("update-verts");

  // loop over effects. Mod vertices are done per effect (possibly a bad idea?)
  for (int ei = 0; ei < num_effects; ei++) {
    const auto& effect = model->effects[ei];
    // some effects might have no mod draw info, and no modifiable vertices
    if (effect.mod.mod_draw.empty()) {
      continue;
    }

    prof().begin_event("start1");
    // grab the backend's buffer
    const u32 mod_buffer = m_next_mod_vtx_buffer++;
    backend_ensure_mod_vtx_buffer(mod_buffer, lev);
    mod_vtx_buffers[ei] = mod_buffer;

    // check that we have enough room for the finished thing.
    if (effect.mod.vertices.size() > MAX_MOD_VTX) {
      fmt::print("More mod vertices than MAX_MOD_VTX. {} > {}\n", effect.mod.vertices.size(),
                 MAX_MOD_VTX);
      ASSERT_NOT_REACHED();
    }

    // check that we have enough room for unpack
    if (effect.mod.expect_vidx_end > MAX_MOD_VTX) {
      fmt::print("More mod vertices (temp) than MAX_MOD_VTX. {} > {}\n", effect.mod.expect_vidx_end,
                 MAX_MOD_VTX);
      ASSERT_NOT_REACHED();
    }

    // start with the "correct" vertices from the model data:
    memcpy(m_mod_vtx_temp.data(), effect.mod.vertices.data(),
           sizeof(tfrag3::MercVertex) * effect.mod.vertices.size());

    // get pointers to the fragment and fragment control data
    u32 goal_addr;
    memcpy(&goal_addr, input_data + 4 * ei, 4);
    const u8* ee0 = setup.data - setup.data_offset;
    const u8* merc_effect = ee0 + goal_addr;
    u16 frag_cnt;
    memcpy(&frag_cnt, merc_effect + 18, 2);
    ASSERT(frag_cnt >= effect.mod.fragment_mask.size());
    u32 frag_goal;
    memcpy(&frag_goal, merc_effect, 4);
    u32 frag_ctrl_goal;
    memcpy(&frag_ctrl_goal, merc_effect + 4, 4);
    const u8* frag = ee0 + frag_goal;
    const u8* frag_ctrl = ee0 + frag_ctrl_goal;

    // loop over frags
    u32 vidx = 0;
    // u32 st_vif_add = model->st_vif_add;
    float xyz_scale = model->xyz_scale;
    prof().end_event();
    {
      // we're going to look at data that the game may be modifying.
      // in the original game, they didn't have any lock, but I think that the
      // scratchpad access from the EE would effectively block the VIF1 DMA, so you'd
      // hopefully never get a partially updated model (which causes obvious holes).
      // this lock is not ideal, and can block the rendering thread while blerc_execute runs,
      // which can take up to 2ms on really blerc-heavy scenes
      std::unique_lock<std::mutex> lk(g_merc_data_mutex);
      [[maybe_unused]] int frags_done = 0;
      auto p = scoped_prof("vert-math");

      // loop over fragments
      for (u32 fi = 0; fi < effect.mod.fragment_mask.size(); fi++) {
        frags_done++;
        u8 mat_xfer_count = frag_ctrl[3];

        // we create a mask of fragments to skip because they have no vertices.
        // the indexing data assumes that we skip the other fragments.
        if (effect.mod.fragment_mask[fi]) {
          // read fragment metadata
          u8 unsigned_four_count = frag_ctrl[0];
          u8 lump_four_count = frag_ctrl[1];
          u32 mm_qwc_off = frag[10];
          float float_offsets[3];
          memcpy(float_offsets, &frag[mm_qwc_off * 16], 12);
          u32 my_u4_count = ((unsigned_four_count + 3) / 4) * 16;
          u32 my_l4_count = my_u4_count + ((lump_four_count + 3) / 4) * 16;

          // loop over vertices in the fragment and unpack
          for (u32 w = my_u4_count / 4; w < (my_l4_count / 4) - 2; w += 3) {
            // positions
            u32 q0w = 0x4b010000 + frag[w * 4 + (0 * 4) + 3];
            u32 q1w = 0x4b010000 + frag[w * 4 + (1 * 4) + 3];
            u32 q2w = 0x4b010000 + frag[w * 4 + (2 * 4) + 3];

            // normals
            u32 q0z = 0x47800000 + frag[w * 4 + (0 * 4) + 2];
            u32 q1z = 0x47800000 + frag[w * 4 + (1 * 4) + 2];
            u32 q2z = 0x47800000 + frag[w * 4 + (2 * 4) + 2];

            // uvs
            u32 q2x = model->st_vif_add + frag[w * 4 + (2 * 4) + 0];
            u32 q2y = model->st_vif_add + frag[w * 4 + (2 * 4) + 1];

            auto* pos_array = m_mod_vtx_unpack_temp[vidx].pos;
            memcpy(&pos_array[0], &q0w, 4);
            memcpy(&pos_array[1], &q1w, 4);
            memcpy(&pos_array[2], &q2w, 4);
            pos_array[0] += float_offsets[0];
            pos_array[1] += float_offsets[1];
            pos_array[2] += float_offsets[2];
            pos_array[0] *= xyz_scale;
            pos_array[1] *= xyz_scale;
            pos_array[2] *= xyz_scale;

            auto* nrm_array = m_mod_vtx_unpack_temp[vidx].nrm;
            memcpy(&nrm_array[0], &q0z, 4);
            memcpy(&nrm_array[1], &q1z, 4);
            memcpy(&nrm_array[2], &q2z, 4);
            nrm_array[0] += -65537;
            nrm_array[1] += -65537;
            nrm_array[2] += -65537;

            auto* uv_array = m_mod_vtx_unpack_temp[vidx].uv;
            memcpy(&uv_array[0], &q2x, 4);
            memcpy(&uv_array[1], &q2y, 4);
            uv_array[0] += model->st_magic;
            uv_array[1] += model->st_magic;

            vidx++;
          }
        }

        // next control
        frag_ctrl += 4 + 2 * mat_xfer_count;

        // next frag
        u32 mm_qwc_count = frag[11];
        frag += mm_qwc_count * 16;
      }

      // sanity check
      if (effect.mod.expect_vidx_end != vidx) {
        fmt::print("---------- BAD {}/{}\n", effect.mod.expect_vidx_end, vidx);
        ASSERT(false);
      }
    }

    {
      auto pp = scoped_prof("copy");
      // now copy the data in merc original vertex order to the output.
      for (u32 vi = 0; vi < effect.mod.vertices.size(); vi++) {
        u32 addr = effect.mod.vertex_lump4_addr[vi];
        if (addr < vidx) {
          memcpy(&m_mod_vtx_temp[vi], &m_mod_vtx_unpack_temp[addr], 32);
          m_mod_vtx_temp[vi].st[0] = m_mod_vtx_unpack_temp[addr].uv[0];
          m_mod_vtx_temp[vi].st[1] = m_mod_vtx_unpack_temp[addr].uv[1];
        }
      }
    }

    // and upload to GPU
    stats->num_uploads++;
    stats->num_upload_bytes += effect.mod.vertices.size() * sizeof(tfrag3::MercVertex);
    {
      auto pp = scoped_prof("update-verts-upload");
      backend_upload_mod_vtx(mod_buffer, m_mod_vtx_temp.data(), effect.mod.vertices.size());
    }
  }
}

/*!
 * Setup draws for a model, given the DMA data generated by the GOAL code.
 */
void Merc2Core::handle_pc_model(const DmaTransfer& setup, MercDebugStats* stats) {
  auto p = scoped_prof("init-pc");

  // the format of the data is:
  //  ;; name   (128 char, 8 qw)
  //  ;; lights (7 qw x 1)
  //  ;; matrix slot string (128 char, 8 qw)
  //  ;; matrices (7 qw x N)
  //  ;; flags    (num-effects, effect-alpha-ignore, effect-disable)
  //  ;; fades    (u32 x N), padding to qw aligned
  //  ;; pointers (u32 x N), padding

  // Get the name
  const u8* input_data = setup.data;
  ASSERT(strlen((const char*)input_data) < 127);
  char name[128];
  strcpy(name, (const char*)setup.data);
  input_data += 128;

  // Look up the model by name in the loader.
  // This will return a reference to this model's data, plus a reference to the level's data
  // for stuff shared between models of the same level
  auto model_ref = m_context.loader->get_merc_model(name);
  if (!model_ref) {
    // it can fail, if the game is faster than the loader. In this case, we just don't draw.
    stats->num_missing_models++;
    return;
  }

  // next, we need to check if we have enough room to draw this effect.
  const LevelData* lev = model_ref->level;
  const tfrag3::MercModel* model = model_ref->model;

  // each model uses only 1 light.
  if (m_next_free_light >= MAX_LIGHTS) {
    fmt::print("MERC2 out of lights, consider increasing MAX_LIGHTS\n");
    flush_draw_buckets(stats);
  }

  // models use many bones. First check if we need to flush:
  int bone_count = model->max_bones + 1;
  if (m_next_free_bone_vector + m_bone_buffer_alignment + bone_count * 8 >
      MAX_SHADER_BONE_VECTORS) {
    fmt::print("MERC2 out of bones, consider increasing MAX_SHADER_BONE_VECTORS\n");
    flush_draw_buckets(stats);
  }

  // also sanity check that we have enough to draw the model
  if (m_bone_buffer_alignment + bone_count * 8 > MAX_SHADER_BONE_VECTORS) {
    fmt::print(
        "MERC2 doesn't have enough bones to draw a model, increase MAX_SHADER_BONE_VECTORS\n");
    ASSERT_NOT_REACHED();
  }

  // next, we need to find a bucket that holds draws for this level (will have the right buffers
  // bound for drawing)
  LevelDrawBucket* lev_bucket = nullptr;
  for (u32 i = 0; i < m_next_free_level_bucket; i++) {
    if (m_level_draw_buckets[i].level == lev) {
      lev_bucket = &m_level_draw_buckets[i];
      break;
    }
  }

  if (!lev_bucket) {
    // no existing bucket, allocate a new one.
    if (m_next_free_level_bucket >= m_level_draw_buckets.size()) {
      // out of room, flush
      // fmt::print("MERC2 out of levels, consider increasing MAX_LEVELS\n");
      flush_draw_buckets(stats);
    }
    // alloc a new one
    lev_bucket = &m_level_draw_buckets[m_next_free_level_bucket++];
    lev_bucket->reset();
    lev_bucket->level = lev;
  }

  // next check draws:
  if (lev_bucket->next_free_draw + model->max_draws >= lev_bucket->draws.size()) {
    // out of room, flush
    fmt::print("MERC2 out of draws, consider increasing MAX_DRAWS_PER_LEVEL\n");
    flush_draw_buckets(stats);
    if (model->max_draws >= lev_bucket->draws.size()) {
      ASSERT_NOT_REACHED_MSG("MERC2 draw buffer not big enough");
    }
  }

  // same for envmap draws
  if (lev_bucket->next_free_envmap_draw + model->max_draws >= lev_bucket->envmap_draws.size()) {
    // out of room, flush
    fmt::print("MERC2 out of envmap draws, consider increasing MAX_ENVMAP_DRAWS_PER_LEVEL\n");
    flush_draw_buckets(stats);
    if (model->max_draws >= lev_bucket->envmap_draws.size()) {
      ASSERT_NOT_REACHED_MSG("MERC2 envmap draw buffer not big enough");
    }
  }

  // Next part of input data is the lights
  VuLights current_lights;
  memcpy(&current_lights, input_data, sizeof(VuLights));
  input_data += sizeof(VuLights);

  u64 uses_water = 0;
  if (m_context.version == GameVersion::Jak1) {
    // jak 1 figures out water at runtime sadly
    memcpy(&uses_water, input_data, 8);
    input_data += 16;
  }

  // Next part is the matrix slot string. The game sends us a bunch of bone matrices,
  // but they may not be in order, or include all bones. The matrix slot string tells
  // us which bones go where. (the game doesn't go in order because it follows the merc format)
  ShaderMercMat skel_matrix_buffer[MAX_SKEL_BONES];
  auto* matrix_array = (const u32*)(input_data + 128);
  int i;
  for (i = 0; i < 128; i++) {
    if (input_data[i] == 0xff) {  // indicates end of string.
      break;
    }
    // read goal addr of matrix (matrix data isn't known at merc dma time, bones runs after)
    u32 addr;
    memcpy(&addr, &matrix_array[i * 4], 4);
    const u8* real_addr = setup.data - setup.data_offset + addr;
    ASSERT(input_data[i] < MAX_SKEL_BONES);
    // get the matrix data
    memcpy(&skel_matrix_buffer[input_data[i]], real_addr, sizeof(MercMat));
  }
  input_data += 128 + 16 * i;

  // Next part is some flags
  struct PcMercFlags {
    u64 enable_mask;
    u64 ignore_alpha_mask;
    u8 effect_count;
    u8 bitflags;
  };
  auto* flags = (const PcMercFlags*)input_data;
  int num_effects = flags->effect_count;  // mostly just a sanity check
  ASSERT(num_effects < kMaxEffect);
  u64 current_ignore_alpha_bits = flags->ignore_alpha_mask;  // shader settings
  u64 current_effect_enable_bits = flags->enable_mask;       // mask for game to disable an effect
  bool model_uses_mod = flags->bitflags & 1;  // if we should update vertices from game.
  bool model_disables_fog = flags->bitflags & 2;
  bool model_uses_pc_blerc = flags->bitflags & 4;
  bool model_disables_envmap = flags->bitflags & 8;
  bool model_no_texture = flags->bitflags & 16;
  input_data += 32;

  float blerc_weights[kMaxBlerc];
  if (model_uses_pc_blerc) {
    memcpy(blerc_weights, input_data, kMaxBlerc * sizeof(float));
    input_data += kMaxBlerc * sizeof(float);
  }

  // Next is "fade data", indicating the color/intensity of envmap effect
  u8 fade_buffer[4 * kMaxEffect];
  for (int ei = 0; ei < num_effects; ei++) {
    for (int j = 0; j < 4; j++) {
      fade_buffer[ei * 4 + j] = input_data[ei * 4 + j];
    }
  }
  input_data += (((num_effects * 4) + 15) / 16) * 16;

  // Next is pointers to merc data, needed so we can update vertices

  // custom models are likely to have a different number of effects than what GOAL reports, update
  // the count here (after reading DMA) so we don't potentially go out of bounds when we do
  // blerc/mod draws
  if (model->effects.at(0).all_draws.at(0).no_strip) {
    num_effects = model->effects.size();
  }

  // will hold the backend buffers for the updated vertices
  u32 mod_vtx_buffers[kMaxEffect];
  if (model_uses_pc_blerc) {
    model_mod_blerc_draws(num_effects, model, lev, mod_vtx_buffers, blerc_weights, stats);
  } else if (model_uses_mod) {  // only if we've enabled, this path is slow.
    model_mod_draws(num_effects, model, lev, input_data, setup, mod_vtx_buffers, stats);
  }

  // stats
  stats->num_models++;
  for (const auto& effect : model_ref->model->effects) {
    bool envmap = effect.has_envmap && !model_disables_envmap;
    stats->num_effects++;
    stats->num_predicted_draws += effect.all_draws.size();
    if (envmap) {
      stats->num_envmap_effects++;
      stats->num_predicted_draws += effect.all_draws.size();
    }
    for (const auto& draw : effect.all_draws) {
      stats->num_predicted_tris += draw.num_triangles;
      if (envmap) {
        stats->num_predicted_tris += draw.num_triangles;
      }
    }
  }

  if (stats->collect_debug_model_list) {
    auto& d = stats->model_list.emplace_back();
    d.name = model->name;
    d.level = model_ref->level->level->level_name;
    for (auto& e : model->effects) {
      auto& de = d.effects.emplace_back();
      de.envmap = e.has_envmap;
      de.envmap_mode = e.envmap_mode;
      for (auto& draw : e.all_draws) {
        auto& dd = de.draws.emplace_back();
        dd.mode = draw.mode;
        dd.num_tris = draw.num_triangles;
      }
    }
  }

  // allocate bones in shared bone buffer to be sent to GPU at flush-time
  u32 first_bone = alloc_bones(bone_count, skel_matrix_buffer);

  // allocate lights
  if (current_lights.w1) {
    if (m_context.version != GameVersion::Jak3) {
      current_lights.w1 = 0;  // force off merc fade in jak2/1 - a bunch of stuff uses this
    }
  }
  u32 lights = alloc_lights(current_lights);
  stats->num_lights++;

  u64 hash = fnv64(model->name);

  DrawArgs args;
  args.lev_bucket = lev_bucket;
  args.jak1_water_mode = uses_water;
  args.disable_fog = model_disables_fog;
  args.hash = hash;
  args.lights = lights;
  args.first_bone = first_bone;
  args.no_texture = m_context.version == GameVersion::Jak3 && model_no_texture;

  // loop over effects, creating draws for each
  for (size_t ei = 0; ei < model->effects.size(); ei++) {
    args.fade = fade_buffer + 4 * ei;

    // game has disabled it?
    if (!(current_effect_enable_bits & (1ull << ei))) {
      continue;
    }

    // imgui menu disabled it?
    if (!m_effect_debug_mask[ei]) {
      continue;
    }

    bool ignore_alpha = !!(current_ignore_alpha_bits & (1ull << ei));
    args.ignore_alpha = ignore_alpha;
    auto& effect = model->effects[ei];

    bool should_envmap = effect.has_envmap && !model_disables_envmap;
    bool should_mod = (model_uses_pc_blerc || model_uses_mod) && effect.has_mod_draw;

    if (should_mod) {
      // draw as two parts, fixed and mod

      // do fixed draws:
      for (auto& fdraw : effect.mod.fix_draw) {
        alloc_normal_draw(fdraw, args);
        if (should_envmap) {
          try_alloc_envmap_draw(fdraw, effect.envmap_mode, effect.envmap_texture, args);
        }
      }

      // do mod draws
      for (auto& mdraw : effect.mod.mod_draw) {
        auto n = alloc_normal_draw(mdraw, args);
        // modify the draw, set the mod flag and point it to that buffer
        n->flags |= MOD_VTX;
        n->mod_vtx_buffer = mod_vtx_buffers[ei];
        if (should_envmap) {
          auto e = try_alloc_envmap_draw(mdraw, effect.envmap_mode, effect.envmap_texture, args);
          if (e) {
            e->flags |= MOD_VTX;
            e->mod_vtx_buffer = mod_vtx_buffers[ei];
          }
        }
      }
    } else {
      // no mod, just do all_draws
      for (auto& draw : effect.all_draws) {
        if (should_envmap) {
          try_alloc_envmap_draw(draw, effect.envmap_mode, effect.envmap_texture, args);
        }
        alloc_normal_draw(draw, args);
      }
    }
  }
}

void Merc2Core::draw_debug_window(MercDebugStats* stats) {
  ImGui::Text("Models   : %d", stats->num_models);
  ImGui::Text("Effects  : %d", stats->num_effects);
  ImGui::Text("Draws (p): %d", stats->num_predicted_draws);
  ImGui::Text("Tris  (p): %d", stats->num_predicted_tris);
  ImGui::Text("Bones    : %d", stats->num_bones_uploaded);
  ImGui::Text("Lights   : %d", stats->num_lights);
  ImGui::Text("Dflush   : %d", stats->num_draw_flush);

  ImGui::Text("EEffects : %d", stats->num_envmap_effects);
  ImGui::Text("ETris    : %d", stats->num_envmap_tris);

  ImGui::Text("Uploads  : %d", stats->num_uploads);
  ImGui::Text("Upload kB: %d", stats->num_upload_bytes / 1024);

  ImGui::Checkbox("Debug", &stats->collect_debug_model_list);

  ImGui::SliderFloat("blerc-nightmare", &blerc_multiplier, -3, 3);

  if (stats->collect_debug_model_list) {
    for (int i = 0; i < kMaxEffect; i++) {
      ImGui::Checkbox(fmt::format("e{:02d}", i).c_str(), &m_effect_debug_mask[i]);
    }

    for (const auto& model : stats->model_list) {
      if (ImGui::TreeNode(model.name.c_str())) {
        ImGui::Text("Level: %s\n", model.level.c_str());
        for (const auto& e : model.effects) {
          for (const auto& d : e.draws) {
            ImGui::Text("%s", d.mode.to_string().c_str());
          }
          ImGui::Separator();
        }
        ImGui::TreePop();
      }
    }
  }
}

/*!
 * Main merc2 rendering.
 */
void Merc2Core::render_core(DmaFollower& dma, const Context& context, MercDebugStats* stats) {
  m_context = context;
  bool hack = stats->collect_debug_model_list;
  *stats = {};
  stats->collect_debug_model_list = hack;
  if (stats->collect_debug_model_list) {
    stats->model_list.clear();
  }


  {
    auto pp = scoped_prof("handle-all-dma");
    // iterate through the dma chain, filling buckets
    handle_all_dma(dma, stats);
  }

  {
    auto pp = scoped_prof("flush-buckets");
    // flush buckets to draws
    flush_draw_buckets(stats);
  }
}

u32 Merc2Core::alloc_lights(const VuLights& lights) {
  ASSERT(m_next_free_light < MAX_LIGHTS);
  u32 light_idx = m_next_free_light;
  m_lights_buffer[m_next_free_light++] = lights;
  static_assert(sizeof(VuLights) == 7 * 16);
  return light_idx;
}

std::string Merc2Core::ShaderMercMat::to_string() const {
  return fmt::format("tmat:\n{}\n{}\n{}\n{}\n", tmat[0].to_string_aligned(),
                     tmat[1].to_string_aligned(), tmat[2].to_string_aligned(),
                     tmat[3].to_string_aligned());
}

/*!
 * Main MERC2 function to handle DMA
 */
void Merc2Core::handle_all_dma(DmaFollower& dma, MercDebugStats* stats) {
  // process the first tag. this is just jumping to the merc-specific dma.
  auto data0 = dma.read_and_advance();
  ASSERT(data0.vif1() == 0 || data0.vifcode1().kind == VifCode::Kind::NOP);
  ASSERT(data0.vif0() == 0 || data0.vifcode0().kind == VifCode::Kind::NOP ||
         data0.vifcode0().kind == VifCode::Kind::MARK);
  ASSERT(data0.size_bytes == 0);
  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    // renderer didn't run, let's just get out of here.
    for (int i = 0; i < 4; i++) {
      dma.read_and_advance();
    }
    ASSERT(dma.current_tag_offset() == m_context.next_bucket);
    return;
  }

  if (dma.current_tag_offset() == m_context.next_bucket) {
    return;
  }
  // if we reach here, there's stuff to draw
  // this handles merc-specific setup DMA
  handle_setup_dma(dma);

  // handle each merc transfer
  while (dma.current_tag_offset() != m_context.next_bucket) {
    handle_merc_chain(dma, stats);
  }
  ASSERT(dma.current_tag_offset() == m_context.next_bucket);
}

void Merc2Core::handle_setup_dma(DmaFollower& dma) {
  auto first = dma.read_and_advance();

  // 10 quadword setup packet
  ASSERT(first.size_bytes == 10 * 16);

  // transferred vifcodes
  {
    auto vif0 = first.vifcode0();
    auto vif1 = first.vifcode1();
    // STCYCL 4, 4
    ASSERT(vif0.kind == VifCode::Kind::STCYCL);
    auto vif0_st = VifCodeStcycl(vif0);
    ASSERT(vif0_st.cl == 4 && vif0_st.wl == 4);
    // STMOD
    ASSERT(vif1.kind == VifCode::Kind::STMOD);
    ASSERT(vif1.immediate == 0);
  }

  // 1 qw with 4 vifcodes.
  u32 vifcode_data[4];
  memcpy(vifcode_data, first.data, 16);
  {
    auto vif0 = VifCode(vifcode_data[0]);
    ASSERT(vif0.kind == VifCode::Kind::BASE);
    ASSERT(vif0.immediate == MercDataMemory::BUFFER_BASE);
    auto vif1 = VifCode(vifcode_data[1]);
    ASSERT(vif1.kind == VifCode::Kind::OFFSET);
    ASSERT((s16)vif1.immediate == MercDataMemory::BUFFER_OFFSET);
    auto vif2 = VifCode(vifcode_data[2]);
    ASSERT(vif2.kind == VifCode::Kind::NOP);
    auto vif3 = VifCode(vifcode_data[3]);
    ASSERT(vif3.kind == VifCode::Kind::UNPACK_V4_32);
    VifCodeUnpack up(vif3);
    ASSERT(up.addr_qw == MercDataMemory::LOW_MEMORY);
    ASSERT(!up.use_tops_flag);
    ASSERT(vif3.num == 8);
  }

  // 8 qw's of low memory data
  memcpy(&m_low_memory, first.data + 16, sizeof(LowMemory));

  backend_set_low_memory(m_low_memory);

  // 1 qw with another 4 vifcodes.
  u32 vifcode_final_data[4];
  memcpy(vifcode_final_data, first.data + 16 + sizeof(LowMemory), 16);
  {
    ASSERT(VifCode(vifcode_final_data[0]).kind == VifCode::Kind::FLUSHE);
    ASSERT(vifcode_final_data[1] == 0);
    ASSERT(vifcode_final_data[2] == 0);
    VifCode mscal(vifcode_final_data[3]);
    ASSERT(mscal.kind == VifCode::Kind::MSCAL);
    ASSERT(mscal.immediate == 0);
  }

  // TODO: process low memory initialization

  if (m_context.version == GameVersion::Jak1) {
    auto second = dma.read_and_advance();
    ASSERT(second.size_bytes == 32);  // setting up test register.
    auto nothing = dma.read_and_advance();
    ASSERT(nothing.size_bytes == 0);
    ASSERT(nothing.vif0() == 0);
    ASSERT(nothing.vif1() == 0);
  } else {
    auto second = dma.read_and_advance();
    ASSERT(second.size_bytes == 48);  // setting up test/zbuf register.
    // todo z write mask stuff.
    auto nothing = dma.read_and_advance();
    ASSERT(nothing.size_bytes == 0);
    ASSERT(nothing.vif0() == 0);
    ASSERT(nothing.vif1() == 0);
  }
}

namespace {
bool tag_is_nothing_next(const DmaFollower& dma) {
  return dma.current_tag().kind == DmaTag::Kind::NEXT && dma.current_tag().qwc == 0 &&
         dma.current_tag_vif0() == 0 && dma.current_tag_vif1() == 0;
}
}  // namespace

void Merc2Core::handle_merc_chain(DmaFollower& dma, MercDebugStats* stats) {
  while (tag_is_nothing_next(dma)) {
    auto nothing = dma.read_and_advance();
    ASSERT(nothing.size_bytes == 0);
  }
  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    for (int i = 0; i < 4; i++) {
      dma.read_and_advance();
    }
    return;
  }

  auto init = dma.read_and_advance();
  int skip_count = 2;
  if (m_context.version >= GameVersion::Jak2) {
    skip_count = 1;
  }

  while (init.vifcode1().kind == VifCode::Kind::PC_PORT) {
    // flush_pending_model();
    handle_pc_model(init, stats);
    for (int i = 0; i < skip_count; i++) {
      auto link = dma.read_and_advance();
      ASSERT(link.vifcode0().kind == VifCode::Kind::NOP);
      ASSERT(link.vifcode1().kind == VifCode::Kind::NOP);
      ASSERT(link.size_bytes == 0);
    }
    init = dma.read_and_advance();
  }

  if (init.vifcode0().kind == VifCode::Kind::FLUSHA) {
    int num_skipped = 0;
    while (dma.current_tag_offset() != m_context.next_bucket) {
      dma.read_and_advance();
      num_skipped++;
    }
    ASSERT(num_skipped < 4);
    return;
  }
}

/*!
 * Queue up some bones to be included in the bone buffer.
 * Returns the index of the first bone vector.
 */
u32 Merc2Core::alloc_bones(int count, ShaderMercMat* data) {
  u32 first_bone_vector = m_next_free_bone_vector;
  ASSERT(count * 8 + first_bone_vector <= MAX_SHADER_BONE_VECTORS);

  // model should have under 128 bones.
  ASSERT(count <= MAX_SKEL_BONES);

  // iterate over each bone we need
  for (int i = 0; i < count; i++) {
    auto& skel_mat = data[i];
    auto* shader_mat = &m_shader_bone_vector_buffer[m_next_free_bone_vector];
    int bv = 0;

    // and copy to the large bone buffer.
    for (int j = 0; j < 4; j++) {
      shader_mat[bv++] = skel_mat.tmat[j];
    }

    for (int j = 0; j < 3; j++) {
      shader_mat[bv++] = skel_mat.nmat[j];
    }

    m_next_free_bone_vector += 8;
  }

  auto b0 = m_next_free_bone_vector;
  m_next_free_bone_vector += m_bone_buffer_alignment - 1;
  m_next_free_bone_vector /= m_bone_buffer_alignment;
  m_next_free_bone_vector *= m_bone_buffer_alignment;
  ASSERT(b0 <= m_next_free_bone_vector);
  ASSERT(first_bone_vector + count * 8 <= m_next_free_bone_vector);
  return first_bone_vector;
}

Merc2Core::Draw* Merc2Core::try_alloc_envmap_draw(const tfrag3::MercDraw& mdraw,
                                          const DrawMode& envmap_mode,
                                          u32 envmap_texture,
                                          const DrawArgs& args) {
  bool nonzero_fade = false;
  for (int i = 0; i < 4; i++) {
    if (args.fade[i]) {
      nonzero_fade = true;
      break;
    }
  }
  if (!nonzero_fade) {
    return nullptr;
  }

  Draw* draw = &args.lev_bucket->envmap_draws[args.lev_bucket->next_free_envmap_draw++];
  draw->flags = 0;
  draw->first_index = mdraw.first_index;
  draw->index_count = mdraw.index_count;
  draw->mode = envmap_mode;
  draw->hash = 0;
  if (args.jak1_water_mode) {
    draw->mode.enable_ab();
    draw->mode.disable_depth_write();
  }
  draw->texture = envmap_texture;
  draw->first_bone = args.first_bone;
  draw->light_idx = args.lights;
  draw->num_triangles = mdraw.num_triangles;
  draw->no_strip = mdraw.no_strip;
  for (int i = 0; i < 4; i++) {
    draw->fade[i] = args.fade[i];
  }
  return draw;
}

Merc2Core::Draw* Merc2Core::alloc_normal_draw(const tfrag3::MercDraw& mdraw, const DrawArgs& args) {
  Draw* draw = &args.lev_bucket->draws[args.lev_bucket->next_free_draw++];
  draw->flags = 0;
  draw->first_index = mdraw.first_index;
  draw->index_count = mdraw.index_count;
  draw->mode = mdraw.mode;
  draw->hash = args.hash;
  if (args.jak1_water_mode) {
    draw->mode.set_ab(true);
    draw->mode.disable_depth_write();
  }

  if (args.disable_fog) {
    draw->mode.set_fog(false);
    // but don't toggle it the other way?
  }

  draw->texture = mdraw.eye_id == 0xff ? mdraw.tree_tex_id : (0xefffff00 | mdraw.eye_id);
  draw->first_bone = args.first_bone;
  draw->light_idx = args.lights;
  draw->num_triangles = mdraw.num_triangles;
  draw->no_strip = mdraw.no_strip;
  if (args.ignore_alpha) {
    draw->flags |= IGNORE_ALPHA;
  }
  if (args.no_texture) {
    draw->flags |= NO_TEXTURE;
  }
  for (int i = 0; i < 4; i++) {
    draw->fade[i] = 0;
  }
  return draw;
}

void Merc2Core::flush_draw_buckets(MercDebugStats* stats) {
  stats->num_draw_flush++;
  for (u32 li = 0; li < m_next_free_level_bucket; li++) {
    const auto& lev_bucket = m_level_draw_buckets[li];
    const auto* lev = lev_bucket.level;
    stats->num_bones_uploaded += m_next_free_bone_vector;
    backend_upload_bones(m_shader_bone_vector_buffer, m_next_free_bone_vector);

    backend_do_draws(lev_bucket.draws.data(), lev, lev_bucket.next_free_draw, false, false, stats);
    if (lev_bucket.next_free_envmap_draw) {
      backend_do_draws(lev_bucket.envmap_draws.data(), lev, lev_bucket.next_free_envmap_draw, true,
                       true, stats);
    }
  }

  backend_flush_finished();

  m_next_free_light = 0;
  m_next_free_bone_vector = 0;
  m_next_free_level_bucket = 0;
  m_next_mod_vtx_buffer = 0;
}
