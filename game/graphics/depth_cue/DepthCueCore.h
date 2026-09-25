#pragma once

/*!
 * @file DepthCueCore.h
 * The depth-cue renderer with no graphics API in it.
 *
 * Depth cue is the soft horizontal smear the PS2 applies over the whole frame: the game copies
 * the framebuffer into a scratch page slightly smaller than it came from, which makes the
 * bilinear filter blur it, and blends that back over the screen. It does this in sixteen vertical
 * 32-pixel slices.
 *
 * Reading that DMA and turning it into the two sets of sixteen quads is identical for every
 * backend, so it lives here. What a backend does is: copy the frame into a texture, draw the
 * first set of quads into a scratch target, and draw the second set back over the frame.
 */

#include <vector>

#include "common/common_types.h"
#include "common/dma/dma_chain_read.h"
#include "common/dma/gs.h"
#include "common/math/Vector.h"

class DepthCueCore {
 public:
  DepthCueCore();
  virtual ~DepthCueCore();

  // Total number of loops depth-cue performs to draw to the framebuffer
  static constexpr int TOTAL_DRAW_SLICES = 16;

  struct SpriteVertex {
    math::Vector2f xy;
    math::Vector2f st;

    SpriteVertex() = default;
    SpriteVertex(float x, float y, float s, float t) : xy(x, y), st(s, t) {}
  };

 protected:
  /*!
   * What the core needs from the frame that is not its own.
   */
  struct Context {
    u32 next_bucket = 0;
    bool enabled = true;
    // The size of the area the game is drawn into, in pixels. Both targets are sized from it.
    int draw_region_w = 0;
    int draw_region_h = 0;
  };

  // Walks one bucket's DMA, rebuilds the quads if anything changed, and calls do_draw().
  void render_core(DmaFollower& dma, const Context& context);

  // Copy the frame into a texture, draw m_depth_cue_page_vertices into a scratch target of
  // m_fbo_width x m_fbo_height sampling that texture, then draw m_on_screen_vertices back over
  // the frame sampling the scratch target. Six vertices per slice, triangles, no index buffer.
  virtual void do_draw() = 0;

  // The colour and depth each of the two draws uses, taken from the first slice: the core's
  // asserts have already established that every slice agrees.
  math::Vector4f page_color() const;
  math::Vector4f screen_color() const;
  float screen_depth() const;

  // One-time initial GS setup per frame
  struct DepthCueGsSetup {
    GifTag gif_tag;
    GsTest test1;
    u64 test1_addr;
    GsZbuf zbuf1;
    u64 zbuf1_addr;
    GsTex1 tex1;
    u64 tex1_addr;
    u64 miptbp1;
    u64 miptbp1_addr;
    u64 clamp1;
    u64 clamp1_addr;
    GsAlpha alpha1;
    u64 alpha1_addr;
  };
  static_assert(sizeof(DepthCueGsSetup) == (7 * 16));

  // One-time GS state restoration at the end of each depth-cue frame
  struct DepthCueGsRestore {
    GifTag gif_tag;
    u64 xyoffset1;
    u64 xyoffset1_addr;
    GsFrame frame1;
    u64 frame1_addr;
  };
  static_assert(sizeof(DepthCueGsRestore) == (3 * 16));

  // GS setup for drawing to the depth-cue-base-page framebuffer
  struct DepthCuePageGsSetup {
    GifTag gif_tag;
    GsXYOffset xyoffset1;
    u64 xyoffset1_addr;
    GsFrame frame1;
    u64 frame1_addr;
    GsTex0 tex01;
    u64 tex01_addr;
    GsTest test1;
    u64 test1_addr;
    GsAlpha alpha1;
    u64 alpha1_addr;
  };
  static_assert(sizeof(DepthCuePageGsSetup) == (6 * 16));

  // GS setup for drawing to the on-screen framebuffer
  struct OnScreenGsSetup {
    GifTag gif_tag;
    GsXYOffset xyoffset1;
    u64 xyoffset1_addr;
    GsFrame frame1;
    u64 frame1_addr;
    GsTexa texa;
    u64 texa_addr;
    GsTex0 tex01;
    u64 tex01_addr;
    GsAlpha alpha1;
    u64 alpha1_addr;
  };
  static_assert(sizeof(OnScreenGsSetup) == (6 * 16));

  // Sprite draw command to the depth-cue-base-page framebuffer
  struct DepthCuePageDraw {
    GifTag gif_tag;
    math::Vector4<s32> rgbaq;
    math::Vector4<s32> uv_1;
    math::Vector4<s32> xyzf2_1;
    math::Vector4<s32> uv_2;
    math::Vector4<s32> xyzf2_2;
  };
  static_assert(sizeof(DepthCuePageDraw) == (6 * 16));

  // Sprite draw command to the on-screen framebuffer
  struct OnScreenDraw {
    GifTag gif_tag;
    math::Vector4<s32> rgbaq;
    math::Vector4<s32> uv_1;
    math::Vector4<s32> xyzf2_1;
    math::Vector4<s32> uv_2;
    math::Vector4<s32> xyzf2_2;
  };
  static_assert(sizeof(OnScreenDraw) == (6 * 16));

  // A draw to the depth-cue-base-page and then back to the on-screen framebuffer.
  //
  // This is done 16 times per frame across the entire on-screen framebuffer in vertical strips.
  struct DrawSlice {
    DepthCuePageGsSetup depth_cue_page_setup;
    DepthCuePageDraw depth_cue_page_draw;
    OnScreenGsSetup on_screen_setup;
    OnScreenDraw on_screen_draw;
  };

  DepthCueGsSetup m_gs_setup;
  DepthCueGsRestore m_gs_restore;
  std::vector<DrawSlice> m_draw_slices;

  // What setup() produced. A backend uploads these when m_geometry_changed is set and leaves them
  // alone otherwise: they only change when the window is resized or a debug slider moves.
  std::vector<SpriteVertex> m_depth_cue_page_vertices;
  std::vector<SpriteVertex> m_on_screen_vertices;
  int m_fb_sample_width = 0;
  int m_fb_sample_height = 0;
  int m_fbo_width = 0;
  int m_fbo_height = 0;
  bool m_geometry_changed = true;

  struct {
    // false = recompute setup each frame
    // true = only recompute setup when draw dimensions change
    bool cache_setup = true;
    // true = render depth-cue at original 512px wide resolution
    bool force_original_res = false;
    // true = render with m_draw_alpha alpha
    bool override_alpha = false;
    // 0.4 = default in GOAL
    float draw_alpha = 0.4f;
    // true = render with m_sharpness sharpness
    bool override_sharpness = false;
    // 1.0 = pixel perfect, depth-cue has no effect
    // 0.999 = default in GOAL
    float sharpness = 0.999f;
    // lower to have effect only apply to further away pixels
    // 1.0 = default (apply to all)
    float depth = 1.0f;
    // depth-cue resolution multiplier
    float res_scale = 1.0f;
  } m_debug;

  void draw_debug_window_core();

 private:
  void read_dma(DmaFollower& dma, const Context& context);
  void setup(const Context& context);
  static void build_sprite(std::vector<SpriteVertex>& vertices,
                           float x1,
                           float y1,
                           float s1,
                           float t1,
                           float x2,
                           float y2,
                           float s2,
                           float t2);

  int m_last_draw_region_w = -1;
  int m_last_draw_region_h = -1;
  bool m_last_override_sharpness = false;
  float m_last_custom_sharpness = 0.999f;
  bool m_last_force_original_res = false;
  float m_last_res_scale = 1.0f;
};
