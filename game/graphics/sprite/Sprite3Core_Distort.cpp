/*!
 * @file Sprite3Core_Distort.cpp
 * The sprite distorter: the heat haze, the eco vents, the underwater shimmer.
 *
 * The distorter draws a fan of triangle strips ("slices") per sprite, sampling the frame that has
 * been drawn so far and offsetting it. Building those strips is a straight transcription of the
 * VU program and has no graphics API in it, so it lives here with the rest of the core; the
 * backend supplies the framebuffer copy and the draw.
 */

#include "Sprite3Core.h"

#include "game/graphics/opengl_renderer/dma_helpers.h"

namespace {
/*!
 * Does the next DMA transfer look like the frame data for sprite distort?
 */
bool looks_like_distort_frame_data(const DmaFollower& dma) {
  return dma.current_tag().kind == DmaTag::Kind::CNT &&
         dma.current_tag_vifcode0().kind == VifCode::Kind::NOP &&
         dma.current_tag_vifcode1().kind == VifCode::Kind::UNPACK_V4_32;
}
}  // namespace

/*!
 * Run the sprite distorter.
 */
void Sprite3Core::render_distorter(DmaFollower& dma, const Context& context) {
  // Read DMA
  distort_dma(context.version, dma);

  if (!context.enabled || !m_distort_enable) {
    // Distort disabled, we can stop here since all the DMA has been read
    return;
  }

  const bool instanced = m_enable_distort_instancing && distort_wants_instancing();

  // Set up vertex data
  if (instanced) {
    distort_setup_instanced();
  } else {
    distort_setup();
  }

  // Draw
  distort_draw_gpu(instanced);
}

/*!
 * Reads all sprite distort related DMA packets.
 */
void Sprite3Core::distort_dma(GameVersion version, DmaFollower& dma) {
  // set the expected values per game version first
  u32 expect_zbp, expect_th;
  switch (version) {
    case GameVersion::Jak1:
      expect_zbp = 0x1c0;
      expect_th = 8;
      break;
    case GameVersion::Jak2:
    case GameVersion::Jak3:
    case GameVersion::JakX:
      expect_zbp = 0x130;
      expect_th = 9;
      break;
    default:
      ASSERT(false);
      return;
  }

  // First should be the GS setup
  auto sprite_distorter_direct_setup = dma.read_and_advance();
  ASSERT(sprite_distorter_direct_setup.vifcode0().kind == VifCode::Kind::NOP);
  ASSERT(sprite_distorter_direct_setup.vifcode1().kind == VifCode::Kind::DIRECT);
  ASSERT(sprite_distorter_direct_setup.vifcode1().immediate == 7);
  memcpy(&m_sprite_distorter_setup, sprite_distorter_direct_setup.data, 7 * 16);

  auto gif_tag = m_sprite_distorter_setup.gif_tag;
  ASSERT(gif_tag.nloop() == 1);
  ASSERT(gif_tag.eop() == true);
  ASSERT(gif_tag.nreg() == 6);
  ASSERT(gif_tag.reg(0) == GifTag::RegisterDescriptor::AD);

  auto zbuf1 = m_sprite_distorter_setup.zbuf;
  ASSERT(zbuf1.zbp() == expect_zbp);
  ASSERT(zbuf1.zmsk() == true);
  ASSERT(zbuf1.psm() == TextureFormat::PSMZ24);

  auto tex0 = m_sprite_distorter_setup.tex0;
  ASSERT(tex0.tbw() == 8);
  ASSERT(tex0.tw() == 9);
  ASSERT(tex0.th() == expect_th);

  auto tex1 = m_sprite_distorter_setup.tex1;
  ASSERT(tex1.mmag() == 1);
  ASSERT(tex1.mmin() == 1);

  auto alpha = m_sprite_distorter_setup.alpha;
  ASSERT(alpha.a_mode() == GsAlpha::BlendMode::SOURCE);
  ASSERT(alpha.b_mode() == GsAlpha::BlendMode::DEST);
  ASSERT(alpha.c_mode() == GsAlpha::BlendMode::SOURCE);
  ASSERT(alpha.d_mode() == GsAlpha::BlendMode::DEST);

  // Next is the aspect used by the sine tables (PC only)
  //
  // This was added to let the renderer reliably detect when the sine tables changed,
  // which is whenever the aspect ratio changed. However, the tables aren't always
  // updated on the same frame that the aspect changed, so this just lets the game
  // easily notify the renderer when it finally does get updated.
  auto sprite_distort_tables_aspect = dma.read_and_advance();
  ASSERT(sprite_distort_tables_aspect.size_bytes == 16);
  ASSERT(sprite_distort_tables_aspect.vifcode1().kind == VifCode::Kind::PC_PORT);
  memcpy(&m_sprite_distorter_sine_tables_aspect, sprite_distort_tables_aspect.data,
         sizeof(math::Vector4f));

  // Next thing should be the sine tables
  auto sprite_distorter_tables = dma.read_and_advance();
  unpack_to_stcycl(&m_sprite_distorter_sine_tables, sprite_distorter_tables,
                   VifCode::Kind::UNPACK_V4_32, 4, 4, 0x8b * 16, 0x160, false, false);

  ASSERT(GsPrim(m_sprite_distorter_sine_tables.gs_gif_tag.prim()).kind() ==
         GsPrim::Kind::TRI_STRIP);

  // Finally, should be frame data packets (containing sprites)
  // Up to 170 sprites will be DMA'd at a time followed by a mscalf,
  // and this process can happen twice up to a maximum of 256 sprites DMA'd
  // (256 is the size of sprite-aux-list which drives this).
  int sprite_idx = 0;
  m_distort_stats.total_sprites = 0;

  while (looks_like_distort_frame_data(dma)) {
    math::Vector<u32, 4> num_sprites_vec;

    // Read sprite packets
    do {
      int qwc = dma.current_tag().qwc;
      int dest = dma.current_tag_vifcode1().immediate;
      auto distort_data = dma.read_and_advance();

      if (dest == 511) {
        // VU address 511 specifies the number of sprites
        unpack_to_no_stcycl(&num_sprites_vec, distort_data, VifCode::Kind::UNPACK_V4_32, 16, dest,
                            false, false);
      } else {
        // VU address >= 512 is the actual vertex data
        ASSERT(dest >= 512);
        ASSERT(sprite_idx + (qwc / 3) <= (int)m_sprite_distorter_frame_data.capacity());

        unpack_to_no_stcycl(&m_sprite_distorter_frame_data.at(sprite_idx), distort_data,
                            VifCode::Kind::UNPACK_V4_32, qwc * 16, dest, false, false);

        sprite_idx += qwc / 3;
      }
    } while (looks_like_distort_frame_data(dma));

    // Sprite packets should always end with a mscalf flush
    ASSERT(dma.current_tag().kind == DmaTag::Kind::CNT);
    ASSERT(dma.current_tag_vifcode0().kind == VifCode::Kind::MSCALF);
    ASSERT(dma.current_tag_vifcode1().kind == VifCode::Kind::FLUSH);
    dma.read_and_advance();

    m_distort_stats.total_sprites += num_sprites_vec.x();
  }

  // Done
  ASSERT(m_distort_stats.total_sprites <= MAX_DISTORT_SPRITES);
}

/*!
 * Builds the vertex and index data for each distort sprite.
 */
void Sprite3Core::distort_setup() {
  m_distort_stats.total_tris = 0;

  m_sprite_distorter_vertices.clear();
  m_sprite_distorter_indices.clear();

  int sprite_idx = 0;
  int sprites_left = m_distort_stats.total_sprites;

  // This part is mostly ripped from the VU program
  while (sprites_left != 0) {
    // flag seems to represent the 'resolution' of the circle sprite used to create the distortion
    // effect For example, a flag value of 3 will create a circle using 3 "pie-slice" shapes
    u32 flag = m_sprite_distorter_frame_data.at(sprite_idx).flag;
    u32 slices_left = flag;

    // flag has a minimum value of 3 which represents the first ientry
    // Additionally, the ientry index has 352 added to it (which is the start of the entry array
    // in VU memory), so we need to subtract that as well
    int entry_index = m_sprite_distorter_sine_tables.ientry[flag - 3].x() - 352;

    // Here would be adding the giftag, but we don't need that

    // Get the frame data for the next distort sprite
    SpriteDistortFrameData frame_data = m_sprite_distorter_frame_data.at(sprite_idx);
    sprite_idx++;

    // Build the vertex data for the sprite
    math::Vector2f vf03 = frame_data.st;
    math::Vector3f vf14 = frame_data.xyz;

    // Each slice shares a center vertex, we can use this fact and cut out duplicate vertices
    u32 center_vert_idx = m_sprite_distorter_vertices.size();
    m_sprite_distorter_vertices.push_back({vf14, vf03});

    do {
      math::Vector3f vf06 = m_sprite_distorter_sine_tables.entry[entry_index++].xyz();
      math::Vector2f vf07 = m_sprite_distorter_sine_tables.entry[entry_index++].xy();
      math::Vector3f vf08 = m_sprite_distorter_sine_tables.entry[entry_index + 0].xyz();
      math::Vector2f vf09 = m_sprite_distorter_sine_tables.entry[entry_index + 1].xy();

      slices_left--;

      math::Vector2f vf11 = (vf07 * frame_data.rgba.z()) + frame_data.st;
      math::Vector2f vf13 = (vf09 * frame_data.rgba.z()) + frame_data.st;
      math::Vector3f vf06_2 = (vf06 * frame_data.rgba.x()) + frame_data.xyz;
      math::Vector2f vf07_2 = (vf07 * frame_data.rgba.x()) + frame_data.st;
      math::Vector3f vf08_2 = (vf08 * frame_data.rgba.x()) + frame_data.xyz;
      math::Vector2f vf09_2 = (vf09 * frame_data.rgba.x()) + frame_data.st;
      math::Vector3f vf10 = (vf06 * frame_data.rgba.y()) + frame_data.xyz;
      math::Vector3f vf12 = (vf08 * frame_data.rgba.y()) + frame_data.xyz;
      math::Vector3f vf06_3 = vf06_2;
      math::Vector3f vf08_3 = vf08_2;

      m_sprite_distorter_indices.push_back(m_sprite_distorter_vertices.size());
      m_sprite_distorter_vertices.push_back({vf06_3, vf07_2});

      m_sprite_distorter_indices.push_back(m_sprite_distorter_vertices.size());
      m_sprite_distorter_vertices.push_back({vf08_3, vf09_2});

      m_sprite_distorter_indices.push_back(m_sprite_distorter_vertices.size());
      m_sprite_distorter_vertices.push_back({vf10, vf11});

      m_sprite_distorter_indices.push_back(m_sprite_distorter_vertices.size());
      m_sprite_distorter_vertices.push_back({vf12, vf13});

      // Originally, would add the shared center vertex, but in our case we can just add the index
      m_sprite_distorter_indices.push_back(center_vert_idx);

      m_distort_stats.total_tris += 2;
    } while (slices_left != 0);

    // Mark the end of the triangle strip
    m_sprite_distorter_indices.push_back(UINT32_MAX);

    sprites_left--;
  }
}

/*!
 * Sets up the data for rendering distort sprites using instanced rendering.
 *
 * A mesh is built once for each possible sprite resolution and is only re-built
 * when the dimensions of the window are changed. These meshes are built just like
 * the triangle strips in the VU program, but with the sprite-specific data removed.
 *
 * Required sprite-specific frame data is kept as is and is grouped by resolution.
 */
void Sprite3Core::distort_setup_instanced() {
  if (m_distort_instanced_last_aspect_x != m_sprite_distorter_sine_tables_aspect.x() ||
      m_distort_instanced_last_aspect_y != m_sprite_distorter_sine_tables_aspect.y()) {
    m_distort_instanced_last_aspect_x = m_sprite_distorter_sine_tables_aspect.x();
    m_distort_instanced_last_aspect_y = m_sprite_distorter_sine_tables_aspect.y();
    // Aspect ratio changed, which means we have a new sine table
    m_sprite_distorter_vertices_instanced.clear();

    // Build a mesh for every possible distort sprite resolution
    auto vf03 = math::Vector2f(0, 0);
    auto vf14 = math::Vector3f(0, 0, 0);

    for (int res = 3; res < 12; res++) {
      int entry_index = m_sprite_distorter_sine_tables.ientry[res - 3].x() - 352;

      for (int i = 0; i < res; i++) {
        math::Vector3f vf06 = m_sprite_distorter_sine_tables.entry[entry_index++].xyz();
        math::Vector2f vf07 = m_sprite_distorter_sine_tables.entry[entry_index++].xy();
        math::Vector3f vf08 = m_sprite_distorter_sine_tables.entry[entry_index + 0].xyz();
        math::Vector2f vf09 = m_sprite_distorter_sine_tables.entry[entry_index + 1].xy();

        // Normally, there would be a bunch of transformations here against the sprite data.
        // Instead, we'll let the shader do it and just store the sine table specific parts here.

        m_sprite_distorter_vertices_instanced.push_back({vf06, vf07});
        m_sprite_distorter_vertices_instanced.push_back({vf08, vf09});
        m_sprite_distorter_vertices_instanced.push_back({vf06, vf07});
        m_sprite_distorter_vertices_instanced.push_back({vf08, vf09});
        m_sprite_distorter_vertices_instanced.push_back({vf14, vf03});
      }
    }

    distort_instanced_mesh_changed();
  }

  // Set up instance data for each sprite
  m_distort_stats.total_tris = 0;

  for (auto& [res, vec] : m_sprite_distorter_instances_by_res) {
    vec.clear();
  }

  for (int i = 0; i < m_distort_stats.total_sprites; i++) {
    SpriteDistortFrameData frame_data = m_sprite_distorter_frame_data.at(i);

    // Shader just needs the position, tex coords, and scale
    auto x_y_z_s = math::Vector4f(frame_data.xyz.x(), frame_data.xyz.y(), frame_data.xyz.z(),
                                  frame_data.st.x());
    auto sx_sy_sz_t = math::Vector4f(frame_data.rgba.x(), frame_data.rgba.y(), frame_data.rgba.z(),
                                     frame_data.st.y());

    int res = frame_data.flag;

    m_sprite_distorter_instances_by_res[res].push_back({x_y_z_s, sx_sy_sz_t});

    m_distort_stats.total_tris += res * 2;
  }
}
