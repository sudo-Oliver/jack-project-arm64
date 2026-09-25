/*!
 * @file DepthCueCore.cpp
 * See DepthCueCore.h.
 *
 * This is the depth-cue renderer's DMA walk and quad building, moved out of the OpenGL renderer
 * unchanged apart from the draw, which is now virtual, and the two render targets, whose sizes
 * are computed here and allocated by the backend.
 */

#include "DepthCueCore.h"

#include <cmath>

#include "third-party/imgui/imgui.h"

namespace {
// Converts fixed point (with 4 bits for decimal) to floating point.
float fixed_to_floating_point(int fixed) {
  return fixed / 16.0f;
}

math::Vector2f fixed_to_floating_point(const math::Vector<s32, 2>& fixed_vec) {
  return math::Vector2f(fixed_to_floating_point(fixed_vec.x()),
                        fixed_to_floating_point(fixed_vec.y()));
}
}  // namespace

DepthCueCore::DepthCueCore() {
  m_draw_slices.resize(TOTAL_DRAW_SLICES);
}

DepthCueCore::~DepthCueCore() = default;

math::Vector4f DepthCueCore::page_color() const {
  const auto& draw = m_draw_slices[0].depth_cue_page_draw;
  return math::Vector4f(draw.rgbaq.x() / 255.0f, draw.rgbaq.y() / 255.0f, draw.rgbaq.z() / 255.0f,
                        draw.rgbaq.w() / 255.0f);
}

math::Vector4f DepthCueCore::screen_color() const {
  const auto& draw = m_draw_slices[0].on_screen_draw;
  math::Vector4f colorf(draw.rgbaq.x() / 255.0f, draw.rgbaq.y() / 255.0f, draw.rgbaq.z() / 255.0f,
                        draw.rgbaq.w() / 255.0f);
  if (m_debug.override_alpha) {
    colorf.w() = m_debug.draw_alpha / 2.0f;
  }
  return colorf;
}

float DepthCueCore::screen_depth() const {
  if (m_debug.depth == 1.0f) {
    return m_debug.depth;
  }
  // Scale debug depth exponentially to make the slider easier to use
  return std::pow(m_debug.depth, 8);
}

void DepthCueCore::render_core(DmaFollower& dma, const Context& context) {
  // First thing should be a NEXT with two nops. this is a jump from buckets to depth-cue
  auto data0 = dma.read_and_advance();
  ASSERT(data0.vif1() == 0);
  ASSERT(data0.vif0() == 0);
  ASSERT(data0.size_bytes == 0);

  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    // depth-cue renderer didn't run, let's just get out of here.
    for (int i = 0; i < 4; i++) {
      dma.read_and_advance();
    }
    ASSERT(dma.current_tag_offset() == context.next_bucket);
    return;
  }

  read_dma(dma, context);

  if (!context.enabled) {
    // Renderer disabled, stop early
    return;
  }

  setup(context);
  do_draw();
}

/*!
 * Reads all depth-cue DMA packets.
 */
void DepthCueCore::read_dma(DmaFollower& dma, const Context& context) {
  // First should be general GS register setup
  {
    auto gs_setup = dma.read_and_advance();
    ASSERT(gs_setup.size_bytes == sizeof(DepthCueGsSetup));
    ASSERT(gs_setup.vifcode0().kind == VifCode::Kind::NOP);
    ASSERT(gs_setup.vifcode1().kind == VifCode::Kind::DIRECT);
    memcpy(&m_gs_setup, gs_setup.data, sizeof(DepthCueGsSetup));

    ASSERT(m_gs_setup.gif_tag.nreg() == 6);
    ASSERT(m_gs_setup.gif_tag.reg(0) == GifTag::RegisterDescriptor::AD);

    ASSERT(m_gs_setup.test1.ztest() == GsTest::ZTest::ALWAYS);
    ASSERT(m_gs_setup.zbuf1.zmsk() == true);
    ASSERT(m_gs_setup.tex1.mmag() == true);
    ASSERT(m_gs_setup.tex1.mmin() == 1);
    ASSERT(m_gs_setup.miptbp1 == 0);
    ASSERT(m_gs_setup.alpha1.b_mode() == GsAlpha::BlendMode::DEST);
    ASSERT(m_gs_setup.alpha1.d_mode() == GsAlpha::BlendMode::DEST);
  }

  // Next is 64 DMAs to draw to the depth-cue-base-page and back to the on-screen framebuffer
  // We'll group these by each slice of the framebuffer being drawn to
  for (int i = 0; i < TOTAL_DRAW_SLICES; i++) {
    // Each 'slice' should be:
    // 1. GS setup for drawing from on-screen framebuffer to depth-cue-base-page
    // 2. Draw to depth-cue-base-page
    // 3. GS setup for drawing from depth-cue-base-page back to on-screen framebuffer
    // 4. Draw to on-screen framebuffer
    DrawSlice& slice = m_draw_slices.at(i);

    // depth-cue-base-page setup
    {
      auto depth_cue_page_setup = dma.read_and_advance();
      ASSERT(depth_cue_page_setup.size_bytes == sizeof(DepthCuePageGsSetup));
      ASSERT(depth_cue_page_setup.vifcode0().kind == VifCode::Kind::NOP);
      ASSERT(depth_cue_page_setup.vifcode1().kind == VifCode::Kind::DIRECT);
      memcpy(&slice.depth_cue_page_setup, depth_cue_page_setup.data, sizeof(DepthCuePageGsSetup));

      ASSERT(slice.depth_cue_page_setup.gif_tag.nreg() == 5);
      ASSERT(slice.depth_cue_page_setup.gif_tag.reg(0) == GifTag::RegisterDescriptor::AD);

      ASSERT(slice.depth_cue_page_setup.tex01.tcc() == 1);
      ASSERT(slice.depth_cue_page_setup.test1.ztest() == GsTest::ZTest::ALWAYS);
      ASSERT(slice.depth_cue_page_setup.alpha1.b_mode() == GsAlpha::BlendMode::SOURCE);
      ASSERT(slice.depth_cue_page_setup.alpha1.d_mode() == GsAlpha::BlendMode::SOURCE);
    }

    // depth-cue-base-page draw
    {
      auto depth_cue_page_draw = dma.read_and_advance();
      ASSERT(depth_cue_page_draw.size_bytes == sizeof(DepthCuePageDraw));
      ASSERT(depth_cue_page_draw.vifcode0().kind == VifCode::Kind::NOP);
      ASSERT(depth_cue_page_draw.vifcode1().kind == VifCode::Kind::DIRECT);
      memcpy(&slice.depth_cue_page_draw, depth_cue_page_draw.data, sizeof(DepthCuePageDraw));

      ASSERT(slice.depth_cue_page_draw.gif_tag.nloop() == 1);
      ASSERT(slice.depth_cue_page_draw.gif_tag.pre() == true);
      ASSERT(slice.depth_cue_page_draw.gif_tag.prim() == 6);
      ASSERT(slice.depth_cue_page_draw.gif_tag.flg() == GifTag::Format::PACKED);
      ASSERT(slice.depth_cue_page_draw.gif_tag.nreg() == 5);
      ASSERT(slice.depth_cue_page_draw.gif_tag.reg(0) == GifTag::RegisterDescriptor::RGBAQ);
    }

    // on-screen setup
    {
      auto on_screen_setup = dma.read_and_advance();
      ASSERT(on_screen_setup.size_bytes == sizeof(OnScreenGsSetup));
      ASSERT(on_screen_setup.vifcode0().kind == VifCode::Kind::NOP);
      ASSERT(on_screen_setup.vifcode1().kind == VifCode::Kind::DIRECT);
      memcpy(&slice.on_screen_setup, on_screen_setup.data, sizeof(OnScreenGsSetup));

      ASSERT(slice.on_screen_setup.gif_tag.nreg() == 5);
      ASSERT(slice.on_screen_setup.gif_tag.reg(0) == GifTag::RegisterDescriptor::AD);

      ASSERT(slice.on_screen_setup.tex01.tcc() == 0);
      ASSERT(slice.on_screen_setup.texa.ta0() == 0x80);
      ASSERT(slice.on_screen_setup.texa.ta1() == 0x80);
      ASSERT(slice.on_screen_setup.alpha1.b_mode() == GsAlpha::BlendMode::DEST);
      ASSERT(slice.on_screen_setup.alpha1.d_mode() == GsAlpha::BlendMode::DEST);
    }

    // on-screen draw
    {
      auto on_screen_draw = dma.read_and_advance();
      ASSERT(on_screen_draw.size_bytes == sizeof(OnScreenDraw));
      ASSERT(on_screen_draw.vifcode0().kind == VifCode::Kind::NOP);
      ASSERT(on_screen_draw.vifcode1().kind == VifCode::Kind::DIRECT);
      memcpy(&slice.on_screen_draw, on_screen_draw.data, sizeof(OnScreenDraw));

      ASSERT(slice.on_screen_draw.gif_tag.nloop() == 1);
      ASSERT(slice.on_screen_draw.gif_tag.pre() == true);
      ASSERT(slice.on_screen_draw.gif_tag.prim() == 6);
      ASSERT(slice.on_screen_draw.gif_tag.flg() == GifTag::Format::PACKED);
      ASSERT(slice.on_screen_draw.gif_tag.nreg() == 5);
      ASSERT(slice.on_screen_draw.gif_tag.reg(0) == GifTag::RegisterDescriptor::RGBAQ);
    }
  }

  // Finally, a packet to restore GS state
  {
    auto gs_restore = dma.read_and_advance();
    ASSERT(gs_restore.size_bytes == sizeof(DepthCueGsRestore));
    ASSERT(gs_restore.vifcode0().kind == VifCode::Kind::NOP);
    ASSERT(gs_restore.vifcode1().kind == VifCode::Kind::DIRECT);
    memcpy(&m_gs_restore, gs_restore.data, sizeof(DepthCueGsRestore));

    ASSERT(m_gs_restore.gif_tag.nreg() == 2);
    ASSERT(m_gs_restore.gif_tag.reg(0) == GifTag::RegisterDescriptor::AD);
  }

  // End with 'NEXT'
  {
    ASSERT(dma.current_tag().kind == DmaTag::Kind::NEXT);

    while (dma.current_tag_offset() != context.next_bucket) {
      dma.read_and_advance();
    }
  }
}

void DepthCueCore::setup(const Context& context) {
  m_geometry_changed = false;
  if (m_debug.cache_setup &&
      (m_last_draw_region_w == context.draw_region_w &&
       m_last_draw_region_h == context.draw_region_h &&
       // Also recompute when certain debug settings change
       m_last_override_sharpness == m_debug.override_sharpness &&
       m_last_custom_sharpness == m_debug.sharpness &&
       m_last_force_original_res == m_debug.force_original_res &&
       m_last_res_scale == m_debug.res_scale)) {
    // Draw region didn't change, everything is already set up
    return;
  }

  m_last_draw_region_w = context.draw_region_w;
  m_last_draw_region_h = context.draw_region_h;
  m_last_override_sharpness = m_debug.override_sharpness;
  m_last_custom_sharpness = m_debug.sharpness;
  m_last_force_original_res = m_debug.force_original_res;
  m_last_res_scale = m_debug.res_scale;
  m_geometry_changed = true;

  // ASSUMPTIONS
  // --------------------------
  // Assert some assumptions that most of the data for each depth-cue draw is the same.
  // The way the game wants to render this effect is very inefficient, we can use these
  // assumptions to only alter state once, do setup once, and group multiple draw calls.
  const DrawSlice& first_slice = m_draw_slices[0];

  // 1. Assume each draw slice has the exact same width of 32
  //    We'll compare each slice to the first as we go
  float slice_width = fixed_to_floating_point(first_slice.on_screen_draw.xyzf2_2.x() -
                                              first_slice.on_screen_draw.xyzf2_1.x());
  // NOTE: Y-coords will range between [0,1/2 output res], usually 224 but not always.
  // We'll capture it here so we can convert to coords to [0,1] ranges later.
  float slice_height = fixed_to_floating_point(first_slice.on_screen_draw.xyzf2_2.y());

  ASSERT(slice_width == 32.0f);
  ASSERT(first_slice.on_screen_draw.xyzf2_1.y() == 0);

  // 2. Assume that the framebuffer is sampled as a 1024x256 texel view and that the game thinks
  // the framebuffer is 512 pixels wide.
  int fb_sample_width = (int)pow(2, first_slice.depth_cue_page_setup.tex01.tw());
  int fb_sample_height = (int)pow(2, first_slice.depth_cue_page_setup.tex01.th());
  int fb_width = first_slice.depth_cue_page_setup.tex01.tbw() * 64;

  ASSERT(fb_sample_width == 1024);
  ASSERT(fb_sample_height == 256);
  ASSERT(fb_width == 512);
  ASSERT(fb_width * 2 == fb_sample_width);

  // 3. Finally, assert that all slices match the above assumptions
  for (const DrawSlice& slice : m_draw_slices) {
    float _slice_width = fixed_to_floating_point(slice.on_screen_draw.xyzf2_2.x() -
                                                 slice.on_screen_draw.xyzf2_1.x());
    float _slice_height = fixed_to_floating_point(slice.on_screen_draw.xyzf2_2.y());

    ASSERT(slice_width == _slice_width);
    ASSERT(slice_height == _slice_height);
    ASSERT(slice.on_screen_draw.xyzf2_1.y() == 0);

    int _fb_sample_width = (int)pow(2, slice.depth_cue_page_setup.tex01.tw());
    int _fb_sample_height = (int)pow(2, slice.depth_cue_page_setup.tex01.th());
    int _fb_width = slice.depth_cue_page_setup.tex01.tbw() * 64;

    ASSERT(fb_sample_width == _fb_sample_width);
    ASSERT(fb_sample_height == _fb_sample_height);
    ASSERT(fb_width == _fb_width);
  }

  // FRAMEBUFFER SAMPLE TEXTURE
  // --------------------------
  // The effect needs a copy of the frame to sample from. The original game code would have
  // created this as a view into the framebuffer whose width is 2x as large, however this isn't
  // necessary for the effect to work.
  m_fb_sample_width = context.draw_region_w;
  m_fb_sample_height = context.draw_region_h;

  // DEPTH CUE BASE PAGE TARGET
  // --------------------------
  // Next, a target to draw slices of the sample texture to. The depth-cue effect usually does
  // this in 16 vertical slices that are 32 pixels wide each. The destination drawn to is smaller
  // than the source by a very small amount (defined by sharpness in the GOAL code), which kicks
  // in the bilinear filtering effect. Normally, a 32x224 texture will be reused for each slice
  // but for the sake of efficient rendering, we use a target that can store all 16 slices
  // side-by-side and draw all slices to it all at once.
  int pc_depth_cue_fb_width = context.draw_region_w;
  int pc_depth_cue_fb_height = context.draw_region_h;

  if (m_debug.force_original_res) {
    pc_depth_cue_fb_width = 512;
  }

  pc_depth_cue_fb_width *= m_debug.res_scale;

  m_fbo_width = pc_depth_cue_fb_width;
  m_fbo_height = pc_depth_cue_fb_height;

  // DEPTH CUE BASE PAGE VERTEX DATA
  // --------------------------
  // Now that there is a target to draw each slice to, the actual vertex data. Take the exact
  // data DMA'd and scale it up to the PC dimensions.
  m_depth_cue_page_vertices.clear();

  // U-coordinates here will range from [0,512], but the maximum U value in the original is
  // 1024 since the sample texel width is usually 1024. Since we're not using a texture with
  // 2x the width, the maximum U value used to convert UVs to [0,1] should be 512.
  float max_u = fb_sample_width / 2.0f;
  ASSERT(max_u == 512.0f);

  for (const auto& slice : m_draw_slices) {
    math::Vector2f xyoffset = fixed_to_floating_point(
        math::Vector2<s32>((s32)slice.depth_cue_page_setup.xyoffset1.ofx(),
                           (s32)slice.depth_cue_page_setup.xyoffset1.ofy()));

    math::Vector2f xy1 = fixed_to_floating_point(slice.depth_cue_page_draw.xyzf2_1.xy());
    math::Vector2f xy2 = fixed_to_floating_point(slice.depth_cue_page_draw.xyzf2_2.xy());
    math::Vector2f uv1 = fixed_to_floating_point(slice.depth_cue_page_draw.uv_1.xy());
    math::Vector2f uv2 = fixed_to_floating_point(slice.depth_cue_page_draw.uv_2.xy());

    ASSERT(xy1.x() == 0);
    ASSERT(xy1.y() == 0);
    ASSERT(xy2.x() <= 32.0f);
    ASSERT(xy2.y() <= slice_height);

    if (m_debug.override_sharpness) {
      // Undo sharpness from GOAL code and apply custom
      xy2.x() = 32.0f * m_debug.sharpness;
      xy2.y() = 224.0f * m_debug.sharpness;
    }

    // Apply xyoffset GS register
    xy1.x() += xyoffset.x() / 4096.0f;
    xy1.y() += xyoffset.y() / 4096.0f;
    xy2.x() += xyoffset.x() / 4096.0f;
    xy2.y() += xyoffset.y() / 4096.0f;

    // U-coord will range from [0,512], which is half of the original framebuffer sample width
    // Let's also use it to determine the X offset into the depth-cue framebuffer since the
    // original draw assumes each slice is at 0,0.
    float x_offset = (uv1.x() / 512.0f) * (xy2.x() / 32.0f);

    build_sprite(m_depth_cue_page_vertices,
                 // Top-left
                 (xy1.x() / 512.0f) + x_offset,  // x1
                 xy1.y() / slice_height,         // y1
                 uv1.x() / max_u,                // s1
                 uv1.y() / slice_height,         // t1
                 // Bottom-right
                 (xy2.x() / 512.0f) + x_offset,  // x2
                 xy2.y() / slice_height,         // y2
                 uv2.x() / max_u,                // s2
                 uv2.y() / slice_height          // t2
    );
  }

  // ON SCREEN VERTEX DATA
  // --------------------------
  // Finally, draw pixels from the depth-cue-base-page back over the frame.
  m_on_screen_vertices.clear();

  for (const auto& slice : m_draw_slices) {
    math::Vector2f xyoffset = fixed_to_floating_point(math::Vector2<s32>(
        (s32)slice.on_screen_setup.xyoffset1.ofx(), (s32)slice.on_screen_setup.xyoffset1.ofy()));

    math::Vector2f xy1 = fixed_to_floating_point(slice.on_screen_draw.xyzf2_1.xy());
    math::Vector2f xy2 = fixed_to_floating_point(slice.on_screen_draw.xyzf2_2.xy());
    math::Vector2f uv1 = fixed_to_floating_point(slice.on_screen_draw.uv_1.xy());
    math::Vector2f uv2 = fixed_to_floating_point(slice.on_screen_draw.uv_2.xy());

    ASSERT(uv1.x() == 0);
    ASSERT(uv1.y() == 0);
    ASSERT(uv2.x() <= 32.0f);
    ASSERT(uv2.y() <= slice_height);

    if (m_debug.override_sharpness) {
      // Undo sharpness from GOAL code and apply custom
      uv2.x() = 32.0f * m_debug.sharpness;
      uv2.y() = 224.0f * m_debug.sharpness;
    }

    // Apply xyoffset GS register
    xy1.x() += xyoffset.x() / 4096.0f;
    xy1.y() += xyoffset.y() / 4096.0f;
    xy2.x() += xyoffset.x() / 4096.0f;
    xy2.y() += xyoffset.y() / 4096.0f;

    // X-coord will range from [0,512], which is half of the original framebuffer sample width
    // Let's also use it to determine the U offset into the on-screen framebuffer since the
    // original draw assumes each slice is at 0,0.
    float u_offset = (xy1.x() / 512.0f) * (uv2.x() / 32.0f);

    build_sprite(m_on_screen_vertices,
                 // Top-left
                 xy1.x() / 512.0f,               // x1
                 xy1.y() / slice_height,         // y1
                 (uv1.x() / 512.0f) + u_offset,  // s1
                 uv1.y() / slice_height,         // t1
                 // Bottom-right
                 xy2.x() / 512.0f,               // x2
                 xy2.y() / slice_height,         // y2
                 (uv2.x() / 512.0f) + u_offset,  // s2
                 uv2.y() / slice_height          // t2
    );
  }
}

void DepthCueCore::build_sprite(std::vector<SpriteVertex>& vertices,
                                float x1,
                                float y1,
                                float s1,
                                float t1,
                                float x2,
                                float y2,
                                float s2,
                                float t2) {
  // First triangle
  // -------------
  // Top-left
  vertices.push_back(SpriteVertex(x1, y1, s1, t1));

  // Top-right
  vertices.push_back(SpriteVertex(x2, y1, s2, t1));

  // Bottom-left
  vertices.push_back(SpriteVertex(x1, y2, s1, t2));

  // Second triangle
  // -------------
  // Top-right
  vertices.push_back(SpriteVertex(x2, y1, s2, t1));

  // Bottom-left
  vertices.push_back(SpriteVertex(x1, y2, s1, t2));

  // Bottom-right
  vertices.push_back(SpriteVertex(x2, y2, s2, t2));
}

void DepthCueCore::draw_debug_window_core() {
  ImGui::Text("NOTE: depth-cue may be disabled by '*vu1-enable-user-menu*'!");

  ImGui::Checkbox("Cache setup", &m_debug.cache_setup);
  ImGui::Checkbox("Force original resolution", &m_debug.force_original_res);

  ImGui::Checkbox("Override alpha", &m_debug.override_alpha);
  if (m_debug.override_alpha) {
    ImGui::SliderFloat("Alpha", &m_debug.draw_alpha, 0.0f, 1.0f);
  }

  ImGui::Checkbox("Override sharpness", &m_debug.override_sharpness);
  if (m_debug.override_sharpness) {
    ImGui::SliderFloat("Sharpness", &m_debug.sharpness, 0.001f, 1.0f);
  }

  ImGui::SliderFloat("Depth", &m_debug.depth, 0.0f, 1.0f);
  ImGui::SliderFloat("Resolution scale", &m_debug.res_scale, 0.001f, 2.0f);

  if (ImGui::Button("Reset")) {
    m_debug.draw_alpha = 0.4f;
    m_debug.sharpness = 0.999f;
    m_debug.depth = 1.0f;
    m_debug.res_scale = 1.0f;
  }
}
