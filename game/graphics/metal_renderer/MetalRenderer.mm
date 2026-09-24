/*!
 * @file MetalRenderer.mm
 * See MetalRenderer.h.
 */

#include "MetalRenderer.h"

#import <Metal/Metal.h>

#include "common/log/log.h"

#include "game/graphics/metal_renderer/MetalDirect.h"
#include "game/graphics/metal_renderer/MetalMerc2.h"
#include "game/graphics/metal_renderer/MetalShrub.h"
#include "game/graphics/metal_renderer/MetalSky.h"
#include "game/graphics/metal_renderer/MetalTFragment.h"
#include "game/graphics/metal_renderer/MetalTextureUploadHandler.h"
#include "game/graphics/metal_renderer/MetalTie3.h"
#include "game/graphics/opengl_renderer/buckets.h"
#include "game/runtime.h"

// kmachine.h declares this, but it cannot be included here: it pulls in the kernel's Ptr<>, and
// Metal.h pulls in MacTypes.h, which has its own `Ptr` typedef. The declaration is one line.
u32 offset_of_s7();

MetalRenderer::MetalRenderer(std::shared_ptr<TexturePool> texture_pool,
                             std::shared_ptr<Loader> loader) {
  m_render_state.texture_pool = std::move(texture_pool);
  m_render_state.loader = std::move(loader);
}

MetalRenderer::~MetalRenderer() = default;

void MetalRenderer::init_bucket_table() {
  using namespace jak1;
  m_bucket_renderers.clear();
  m_bucket_renderers.resize((int)BucketId::MAX_BUCKETS);

  // Every bucket gets an entry. The ones without a renderer skip their data; leaving a hole would
  // desynchronise every bucket after it.
  for (int i = 0; i < (int)BucketId::MAX_BUCKETS; i++) {
    m_bucket_renderers[i] = std::make_unique<MetalEmptyBucketRenderer>("empty", i);
  }

  // Same tree kinds the OpenGL bucket table assigns. Keep these in step: a kind drawn by the
  // wrong bucket gets the wrong blend and alpha settings.
  const std::vector<tfrag3::TFragmentTreeKind> normal_tfrags = {
      tfrag3::TFragmentTreeKind::NORMAL, tfrag3::TFragmentTreeKind::LOWRES};

  m_bucket_renderers[(int)BucketId::TFRAG_LEVEL0] = std::make_unique<MetalTFragment>(
      "l0-tfrag-tfrag", (int)BucketId::TFRAG_LEVEL0, normal_tfrags, 0);
  m_bucket_renderers[(int)BucketId::TFRAG_LEVEL1] = std::make_unique<MetalTFragment>(
      "l1-tfrag-tfrag", (int)BucketId::TFRAG_LEVEL1, normal_tfrags, 1);

  // The alpha pass draws the same trees again in two more kinds. They are separate buckets
  // because they come after the sky blend and the transparent tfrags, which is what puts the
  // dirt tracks and the ice on top of the ground rather than under it.
  const std::vector<tfrag3::TFragmentTreeKind> dirt_tfrags = {tfrag3::TFragmentTreeKind::DIRT};
  const std::vector<tfrag3::TFragmentTreeKind> ice_tfrags = {tfrag3::TFragmentTreeKind::ICE};
  m_bucket_renderers[(int)BucketId::TFRAG_DIRT_LEVEL0] = std::make_unique<MetalTFragment>(
      "l0-alpha-tfrag-dirt", (int)BucketId::TFRAG_DIRT_LEVEL0, dirt_tfrags, 0);
  m_bucket_renderers[(int)BucketId::TFRAG_DIRT_LEVEL1] = std::make_unique<MetalTFragment>(
      "l1-alpha-tfrag-dirt", (int)BucketId::TFRAG_DIRT_LEVEL1, dirt_tfrags, 1);
  m_bucket_renderers[(int)BucketId::TFRAG_ICE_LEVEL0] = std::make_unique<MetalTFragment>(
      "l0-alpha-tfrag-ice", (int)BucketId::TFRAG_ICE_LEVEL0, ice_tfrags, 0);
  m_bucket_renderers[(int)BucketId::TFRAG_ICE_LEVEL1] = std::make_unique<MetalTFragment>(
      "l1-alpha-tfrag-ice", (int)BucketId::TFRAG_ICE_LEVEL1, ice_tfrags, 1);

  m_bucket_renderers[(int)BucketId::TIE_LEVEL0] =
      std::make_unique<MetalTie3>("l0-tfrag-tie", (int)BucketId::TIE_LEVEL0, 0);
  m_bucket_renderers[(int)BucketId::TIE_LEVEL1] =
      std::make_unique<MetalTie3>("l1-tfrag-tie", (int)BucketId::TIE_LEVEL1, 1);

  m_bucket_renderers[(int)BucketId::SHRUB_NORMAL_LEVEL0] =
      std::make_unique<MetalShrub>("l0-shrub", (int)BucketId::SHRUB_NORMAL_LEVEL0, 0);
  m_bucket_renderers[(int)BucketId::SHRUB_NORMAL_LEVEL1] =
      std::make_unique<MetalShrub>("l1-shrub", (int)BucketId::SHRUB_NORMAL_LEVEL1, 1);

  // The texture-upload buckets, in the same places the OpenGL table puts them. They draw nothing;
  // they tell the TexturePool which PS2 page now holds which texture, and every renderer after
  // them looks its textures up in that pool.
  const std::pair<const char*, BucketId> texture_buckets[] = {
      {"l0-tfrag-tex", BucketId::TFRAG_TEX_LEVEL0},
      {"l1-tfrag-tex", BucketId::TFRAG_TEX_LEVEL1},
      {"l0-shrub-tex", BucketId::SHRUB_TEX_LEVEL0},
      {"l1-shrub-tex", BucketId::SHRUB_TEX_LEVEL1},
      {"l0-alpha-tex", BucketId::ALPHA_TEX_LEVEL0},
      {"l1-alpha-tex", BucketId::ALPHA_TEX_LEVEL1},
      {"l0-pris-tex", BucketId::PRIS_TEX_LEVEL0},
      {"l1-pris-tex", BucketId::PRIS_TEX_LEVEL1},
      {"l0-water-tex", BucketId::WATER_TEX_LEVEL0},
      {"l1-water-tex", BucketId::WATER_TEX_LEVEL1},
      {"common-tex", BucketId::PRE_SPRITE_TEX},
  };
  for (const auto& [name, id] : texture_buckets) {
    m_bucket_renderers[(int)id] = std::make_unique<MetalTextureUploadHandler>(name, (int)id);
  }

  // The sky: the blend that builds its textures, then the geometry that draws them.
  m_sky_blend = std::make_shared<MetalSkyBlend>();
  m_bucket_renderers[(int)BucketId::SKY_DRAW] =
      std::make_unique<MetalSkyRenderer>("sky", (int)BucketId::SKY_DRAW);
  m_bucket_renderers[(int)BucketId::TFRAG_TRANS0_AND_SKY_BLEND_LEVEL0] =
      std::make_unique<MetalSkyBlendHandler>(
          "l0-alpha-sky-blend-and-tfrag-trans",
          (int)BucketId::TFRAG_TRANS0_AND_SKY_BLEND_LEVEL0, 0, m_sky_blend);
  m_bucket_renderers[(int)BucketId::TFRAG_TRANS1_AND_SKY_BLEND_LEVEL1] =
      std::make_unique<MetalSkyBlendHandler>(
          "l1-alpha-sky-blend-and-tfrag-trans",
          (int)BucketId::TFRAG_TRANS1_AND_SKY_BLEND_LEVEL1, 1, m_sky_blend);

  // The merc buckets: the characters. All eight share one MetalMerc2, the way the OpenGL table
  // shares one Merc2 -- the draws are pooled per level, not per bucket.
  m_merc2 = std::make_shared<MetalMerc2>();
  const std::pair<const char*, BucketId> merc_buckets[] = {
      {"l0-tfrag-merc", BucketId::MERC_TFRAG_TEX_LEVEL0},
      {"l1-tfrag-merc", BucketId::MERC_TFRAG_TEX_LEVEL1},
      {"common-alpha-merc", BucketId::MERC_AFTER_ALPHA},
      {"l0-pris-merc", BucketId::MERC_PRIS_LEVEL0},
      {"l1-pris-merc", BucketId::MERC_PRIS_LEVEL1},
      {"common-pris-merc", BucketId::MERC_AFTER_PRIS},
      {"l0-water-merc", BucketId::MERC_WATER_LEVEL0},
      {"l1-water-merc", BucketId::MERC_WATER_LEVEL1},
  };
  for (const auto& [name, id] : merc_buckets) {
    m_bucket_renderers[(int)id] =
        std::make_unique<MetalMerc2BucketRenderer>(name, (int)id, m_merc2);
  }

  // The GIF buckets: debug draws and the subtitle text. Batch sizes are the OpenGL table's.
  m_bucket_renderers[(int)BucketId::DEBUG] =
      std::make_unique<MetalDirectBucketRenderer>("debug", (int)BucketId::DEBUG, 0x20000);
  m_bucket_renderers[(int)BucketId::DEBUG_NO_ZBUF] = std::make_unique<MetalDirectBucketRenderer>(
      "debug-no-zbuf", (int)BucketId::DEBUG_NO_ZBUF, 0x8000);
  m_bucket_renderers[(int)BucketId::SUBTITLE] =
      std::make_unique<MetalDirectBucketRenderer>("subtitle", (int)BucketId::SUBTITLE, 6000);
}

bool MetalRenderer::init(id<MTLDevice> device,
                         id<MTLLibrary> library,
                         MTLPixelFormat color_format,
                         MTLPixelFormat depth_format) {
  m_render_state.device = device;
  m_render_state.library = library;
  m_render_state.color_format = color_format;
  m_render_state.depth_format = depth_format;

  init_bucket_table();

  // The sky-blend textures have to exist in the pool before anything looks them up by their VRAM
  // address, and they are created once, not per bucket.
  if (m_sky_blend && m_render_state.texture_pool) {
    m_sky_blend->init_textures(*m_render_state.texture_pool, GameVersion::Jak1);
  }

  int ported = 0;
  for (auto& renderer : m_bucket_renderers) {
    if (!renderer->init(&m_render_state)) {
      lg::error("[Metal] bucket renderer {} failed to initialise", renderer->name());
      return false;
    }
    if (renderer->name() != "empty") {
      ported++;
    }
  }
  lg::info("[Metal] bucket table ready: {} of {} buckets have a renderer", ported,
           m_bucket_renderers.size());
  m_ready = true;
  return true;
}

void MetalRenderer::scan_frame_state(DmaFollower dma) {
  // The camera and the occlusion strings arrive in specific buckets, but the renderers that need
  // them run in others. The OpenGL backend passes them along its SharedRenderState as the buckets
  // execute; doing it in one pass first is the same information, and keeps the bucket renderers
  // from depending on each other's order.
  for (auto& slot : m_render_state.level_slots) {
    slot.has_camera = false;
  }

  u32 next_bucket = dma.current_tag_offset() + 16;
  dma.read_and_advance();  // the call into the default-regs chain
  dma.read_and_advance();  // the default register data itself
  dma.read_and_advance();  // its ret tag
  if (dma.current_tag_offset() != next_bucket) {
    return;
  }
  next_bucket += 16;

  for (int bucket_id = 0; bucket_id < (int)jak1::BucketId::MAX_BUCKETS; bucket_id++) {
    const bool is_tfrag_bucket = bucket_id == (int)jak1::BucketId::TFRAG_LEVEL0 ||
                                 bucket_id == (int)jak1::BucketId::TFRAG_LEVEL1;
    // The occlusion strings ride along in one bucket, one PC_PORT transfer per level slot, in slot
    // order. A 16-byte transfer means that slot has no vis this frame.
    const bool is_vis_copy_bucket = bucket_id == (int)jak1::BucketId::TFRAG_LEVEL0;
    int vis_slot = 0;

    while (dma.current_tag_offset() != next_bucket && !dma.ended()) {
      auto transfer = dma.read_and_advance();
      if (is_vis_copy_bucket && transfer.vifcode1().kind == VifCode::Kind::PC_PORT &&
          vis_slot < (int)jak1::LEVEL_MAX) {
        if (transfer.size_bytes == 128 * 16) {
          auto& vis = m_render_state.occlusion_vis[vis_slot];
          memcpy(vis.data, transfer.data, sizeof(vis.data));
          vis.valid = true;
          vis_slot++;
        } else if (transfer.size_bytes == 16) {
          m_render_state.occlusion_vis[vis_slot].valid = false;
          vis_slot++;
        }
      }
      if (is_tfrag_bucket && transfer.size_bytes == sizeof(TfragPcPortData)) {
        TfragPcPortData port_data;
        memcpy(&port_data, transfer.data, sizeof(TfragPcPortData));
        port_data.level_name[sizeof(port_data.level_name) - 1] = '\0';
        const int slot = bucket_id == (int)jak1::BucketId::TFRAG_LEVEL0 ? 0 : 1;
        m_render_state.level_slots[slot].camera = port_data.camera;
        m_render_state.level_slots[slot].level_name = port_data.level_name;
        m_render_state.level_slots[slot].has_camera = true;
      }
    }
    if (dma.ended()) {
      break;
    }
    next_bucket += 16;
  }
}

void MetalRenderer::render(DmaFollower dma,
                           id<MTLRenderCommandEncoder> encoder,
                           id<MTLCommandBuffer> offscreen_cmd,
                           u32 viewport_width,
                           u32 viewport_height) {
  if (!m_ready) {
    return;
  }
  m_last_frame_tris = 0;
  if (m_merc2) {
    m_merc2->reset_tri_count();
  }
  m_render_state.encoder = encoder;
  m_render_state.offscreen_cmd = offscreen_cmd;
  m_render_state.viewport_width = viewport_width;
  m_render_state.viewport_height = viewport_height;
  m_render_state.ee_main_memory = g_ee_main_mem;
  m_render_state.offset_of_s7 = offset_of_s7();
  m_render_state.frame_index++;

  scan_frame_state(dma);

  // Same shape as OpenGLRenderer::dispatch_buckets_jak1: a call into the default-registers chain,
  // then one 16-byte slot per bucket. Each renderer must leave the cursor exactly at the next
  // bucket boundary.
  u32 next_bucket = dma.current_tag_offset() + 16;
  dma.read_and_advance();  // the call into the default-regs chain
  // The default register data. Its 145th byte onwards is the frame's fog colour, which every
  // renderer that fogs needs and no bucket carries.
  auto default_data = dma.read_and_advance();
  if (default_data.size_bytes > 148) {
    memcpy(m_render_state.fog_color.data(), default_data.data + 144, 4);
  }
  dma.read_and_advance();  // its ret tag
  if (dma.current_tag_offset() != next_bucket) {
    lg::error("[Metal] frame did not start with the default-register chain");
    return;
  }
  next_bucket += 16;

  for (size_t bucket_id = 0; bucket_id < m_bucket_renderers.size(); bucket_id++) {
    m_render_state.next_bucket = next_bucket;
    m_bucket_renderers[bucket_id]->render(dma, &m_render_state);

    // The OpenGL backend asserts on this. Log instead: a renderer that leaves the cursor short
    // desynchronises every bucket after it, and that has to be visible rather than silent.
    if (dma.current_tag_offset() != next_bucket && !dma.ended()) {
      static int complaints = 0;
      if (complaints < 8) {
        lg::warn("[Metal] bucket {} ({}) left the DMA cursor at {}, expected {}", bucket_id,
                 m_bucket_renderers[bucket_id]->name(), dma.current_tag_offset(), next_bucket);
        complaints++;
      }
      while (dma.current_tag_offset() != next_bucket && !dma.ended()) {
        dma.read_and_advance();
      }
    }
    if (dma.ended()) {
      break;
    }
    next_bucket += 16;
  }

  for (auto& renderer : m_bucket_renderers) {
    if (auto* tfrag = dynamic_cast<MetalTFragment*>(renderer.get())) {
      m_last_frame_tris += tfrag->last_frame_tris();
    } else if (auto* shrub = dynamic_cast<MetalShrub*>(renderer.get())) {
      m_last_frame_tris += shrub->last_frame_tris();
    } else if (auto* tie = dynamic_cast<MetalTie3*>(renderer.get())) {
      m_last_frame_tris += tie->last_frame_tris();
    } else if (auto* direct = dynamic_cast<MetalDirectBucketRenderer*>(renderer.get())) {
      m_last_frame_tris += direct->last_frame_tris();
    } else if (auto* sky = dynamic_cast<MetalSkyRenderer*>(renderer.get())) {
      m_last_frame_tris += sky->last_frame_tris();
    } else if (auto* sky_blend = dynamic_cast<MetalSkyBlendHandler*>(renderer.get())) {
      m_last_frame_tris += sky_blend->last_frame_tris();
    }
  }
  if (m_merc2) {
    m_last_frame_tris += m_merc2->last_frame_tris();
  }
  m_render_state.encoder = nil;
  m_render_state.offscreen_cmd = nil;
}
