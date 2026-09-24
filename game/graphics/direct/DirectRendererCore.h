#pragma once

/*!
 * @file DirectRendererCore.h
 * The GS state machine behind the direct renderer, with no graphics API in it.
 *
 * The direct renderer draws GIFtags directly -- it is named after the DIRECT VIFcode, which sends
 * data straight to the GS without going through the VUs. The game uses it for debug drawing, the
 * subtitles, the sky, the ocean and as a fallback inside several other renderers.
 *
 * Everything that turns a GIF packet into triangles -- the register handlers, the primitive
 * assembly, the texture-state bookkeeping, the scissor and blit registers -- is identical for
 * OpenGL and Metal, and is most of the 1500 lines it takes. So it lives here, and a backend
 * implements one thing: flush_draws(), which applies the GS state and issues the draws.
 *
 * This is the older of the two direct renderers and the one Jak 1 actually uses. It handles
 * REGLIST packets and the PRIM and TEX0 registers in packed mode, which DirectRenderer2Core does
 * not, and which the sky sends.
 */

#include <array>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/dma/dma_chain_read.h"
#include "common/dma/gs.h"
#include "common/math/Vector.h"
#include "common/versions/versions.h"

#include "game/graphics/texture/TexturePool.h"

class DirectRendererCore {
 public:
  DirectRendererCore(const std::string& name, int batch_size);
  virtual ~DirectRendererCore();

  // What the core needs from the frame that is not its own. A backend fills this in before it
  // hands over any data.
  struct Context {
    TexturePool* texture_pool = nullptr;
    GameVersion version = GameVersion::Jak1;
    // Where this bucket's data ends, and where the default-register chain sits.
    u32 next_bucket = 0;
    u32 default_regs_buffer = 0;
    math::Vector<u8, 4> fog_color{0, 0, 0, 0};
    float fog_intensity = 1.f;
  };

  void reset_state();

  // Consume one bucket's DMA, which is VIF data with GIF packets inside it.
  void consume_bucket_dma(DmaFollower& dma);

  // Render directly from VIF data. vif0 and vif1 are the two tags in front of it, or 0.
  void render_vif(u32 vif0, u32 vif1, const u8* data, u32 size);
  // Render directly from GIF data.
  void render_gif(const u8* data, u32 size);

  // Issue whatever is pending. Call this at the end if you do not go through consume_bucket_dma.
  void flush();

  void hack_disable_blend() {
    m_blend_state.a = GsAlpha::BlendMode::SOURCE;
    m_blend_state.b = GsAlpha::BlendMode::SOURCE;
    m_blend_state.c = GsAlpha::BlendMode::SOURCE;
    m_blend_state.d = GsAlpha::BlendMode::SOURCE;
  }
  void set_mipmap(bool en) { m_debug_state.disable_mipmap = !en; }

  // Re-resolve every buffered texture state against the pool. Call it after something else has
  // changed what a VRAM address points at.
  void lookup_textures_again();

  // Forget what state the backend has applied, so the next flush applies all of it again.
  void mark_state_dirty() {
    m_prim_gl_state_needs_gl_update = true;
    m_test_state_needs_gl_update = true;
    m_blend_state_needs_gl_update = true;
  }

  const std::string& name() const { return m_name; }

 protected:
  // Apply the GS state below and draw m_prim_buffer. The backends implement this; everything else
  // is shared. It is called with a full primitive buffer as well as at the end of a bucket, and
  // must leave m_prim_buffer.vert_count at zero.
  virtual void flush_draws() = 0;

  // Bind the texture that buffered texture state `unit` names. Only needed by
  // lookup_textures_again, which re-resolves the states the backend currently has bound.
  virtual void backend_bind_texture(int unit) = 0;

  // Hooks for renderers that wrap this one (the ocean, for instance).
  virtual void pre_render() {}
  virtual void post_render() {}

  Context m_context;
  std::string m_name;

  struct TestState {
    void from_register(GsTest reg);

    GsTest current_register;
    bool alpha_test_enable = false;
    bool prim_alpha_enable = false;
    GsTest::AlphaTest alpha_test = GsTest::AlphaTest::NOTEQUAL;
    u8 aref = 0;
    GsTest::AlphaFail afail = GsTest::AlphaFail::KEEP;
    bool date = false;
    bool datm = false;
    bool zte = true;
    GsTest::ZTest ztst = GsTest::ZTest::GEQUAL;
    bool write_rgb = true;
    bool depth_writes = true;

  } m_test_state;

  struct BlendState {
    void from_register(GsAlpha reg);

    GsAlpha current_register;
    GsAlpha::BlendMode a = GsAlpha::BlendMode::SOURCE;
    GsAlpha::BlendMode b = GsAlpha::BlendMode::DEST;
    GsAlpha::BlendMode c = GsAlpha::BlendMode::SOURCE;
    GsAlpha::BlendMode d = GsAlpha::BlendMode::DEST;
    bool alpha_blend_enable = false;
    u8 fix = 0;

  } m_blend_state;

  // state set through the prim register that requires changing GL stuff.
  struct PrimGlState {
    void from_register(GsPrim reg);

    GsPrim current_register;
    bool gouraud_enable = false;
    bool texture_enable = false;
    bool fogging_enable = false;

    bool aa_enable = false;
    bool use_uv = false;  // todo: might not require a gl state change
    bool ctxt = false;    // do they ever use ctxt2?
    bool fix = false;     // what does this even do?
    u32 ta0 = 0;
  } m_prim_gl_state;

  static constexpr int TEXTURE_STATE_COUNT = 1;

  struct TextureState {
    GsTex0 current_register;
    u32 texture_base_ptr = 0;
    bool using_mt4hh = false;
    bool tcc = false;
    bool decal = false;
    bool enable_tex_filt = true;

    struct ClampState {
      void from_register(u64 value) { current_register = value; }
      u64 current_register = 0b101;
      bool clamp_s = true;
      bool clamp_t = true;
    } m_clamp_state;

    bool used = false;

    bool compatible_with(const TextureState& other) {
      return current_register == other.current_register &&
             m_clamp_state.current_register == other.m_clamp_state.current_register &&
             enable_tex_filt == other.enable_tex_filt;
    }
  };

  // vertices will reference these texture states
  TextureState m_buffered_tex_state[TEXTURE_STATE_COUNT];
  bool m_buffered_tex_state_currently_bound[TEXTURE_STATE_COUNT] = {0};
  int m_next_free_tex_state = 0;

  // this texture state mirrors the current GS register.
  TextureState m_tex_state_from_reg;

  // if this is not -1, then it is the index of a texture state in m_buffered_tex_state that
  // matches m_tex_state_from_reg.
  int m_current_tex_state_idx = -1;

  int get_texture_unit_for_current_reg();

  // state set through the prim/rgbaq register that doesn't require changing GL stuff
  struct PrimBuildState {
    GsPrim::Kind kind = GsPrim::Kind::PRIM_7;
    math::Vector<u8, 4> rgba_reg = math::Vector<u8, 4>{0, 0, 0, 0};
    math::Vector<float, 2> st_reg;

    std::array<math::Vector<u8, 4>, 3> building_rgba;
    std::array<math::Vector<u32, 4>, 3> building_vert;
    std::array<math::Vector<float, 3>, 3> building_stq;
    int building_idx = 0;
    int tri_strip_startup = 0;

    float Q = 1.0;
  } m_prim_building;

  struct Vertex {
    math::Vector<float, 4> xyzf;
    math::Vector<float, 3> stq;
    math::Vector<u8, 4> rgba;
    u8 tex_unit;
    u8 tcc;
    u8 decal;
    u8 fog_enable;
    u8 use_uv;
    math::Vector<u8, 11> __pad;
    // this can be simplified to use gs coords, if needed
    math::Vector<float, 4> scissor;
  };
  static_assert(sizeof(Vertex) == 64);
  static_assert(offsetof(Vertex, tex_unit) == 32);

  struct PrimitiveBuffer {
    PrimitiveBuffer(int max_triangles);
    std::vector<Vertex> vertices;
    int vert_count = 0;
    int max_verts = 0;
    float x_off = 0;
    float y_off = 0;
    // leave 6 free on the end so we always have room to flush one last primitive.
    bool is_full() { return max_verts < (vert_count + 18); }
    void push(const math::Vector<u8, 4>& rgba,
              const math::Vector<u32, 4>& vert,
              const math::Vector<float, 3>& stq,
              const math::Vector<float, 4>& scissor,
              int unit,
              bool tcc,
              bool decal,
              bool fog_enable,
              bool use_uv);
  } m_prim_buffer;

  // the scissor state tends to be shared across buckets, so it is static here
  static struct ScissorState {
    u16 scax0 = 0, scay0 = 0;
    u16 scax1 = 0, scay1 = 0;
  } m_scissor;
  // however the toggle for it is per-bucket
  bool m_scissor_enable = false;

  struct BufferBlitState {
    // used to keep track of blit progress
    u8 expect = 0;

    // blit buffer source+dest settings
    u16 sbp = 0, dbp = 0;
    u8 sbw = 0, dbw = 0;
    u8 spsm = 0, dpsm = 0;
    // transfer pos
    u16 ssax = 0, dsax = 0;
    u16 ssay = 0, dsay = 0;
    // transfer region
    u16 width = 0, height = 0;
    // transfer dir
    u8 pixel_dir = 0;

    // gif IMAGE transfer size
    u16 qwc = 0;
  } m_blit_buf_state;

  struct {
    bool disable_texture = false;
    bool wireframe = false;
    bool red = false;
    bool always_draw = false;
    bool disable_mipmap = true;
  } m_debug_state;

  struct {
    int triangles = 0;
    int draw_calls = 0;

    int flush_from_tex_0 = 0;
    int flush_from_tex_1 = 0;
    int flush_from_zbuf = 0;
    int flush_from_test = 0;
    int flush_from_ta0 = 0;
    int flush_from_alpha = 0;
    int flush_from_clamp = 0;
    int flush_from_prim = 0;
    int flush_from_state_exhaust = 0;
  } m_stats;

  // "gl" in these names is historical: they mean "the backend has not been told about this state
  // yet", and both backends use them.
  bool m_prim_gl_state_needs_gl_update = true;
  bool m_test_state_needs_gl_update = true;
  bool m_test_state_needs_double_draw = false;
  float m_double_draw_aref = 0;
  bool m_blend_state_needs_gl_update = true;

  struct SpriteMode {
    bool do_first_draw = true;
  } m_sprite_mode;


 public:
  // These are called from outside by renderers that feed the direct renderer register by register.
  void handle_prim(u64 val);
  void handle_ad(const u8* data);
  void handle_st_packed(const u8* data);
  void handle_rgbaq_packed(const u8* data);
  void handle_xyzf2_packed(const u8* data);
  void handle_xyz2_packed(const u8* data);
  void handle_prim_packed(const u8* data);
  void handle_tex0_1_packed(const u8* data);
  void handle_uv_packed(const u8* data);
  void handle_rgbaq(u64 val);
  void handle_xyzf2(u64 val);

 protected:
  virtual void handle_frame(u64 val);
  void handle_scissor(u64 val);
  void handle_zbuf1(u64 val);
  void handle_test1(u64 val);
  void handle_alpha1(u64 val);
  void handle_pabe(u64 val);
  void handle_clamp1(u64 val);
  void handle_tex0_1(u64 val);
  void handle_tex1_1(u64 val);
  void handle_texa(u64 val);
  void handle_xyoffset(u64 val);
  void handle_bitbltbuf(u64 val);
  void handle_trxpos(u64 val);
  void handle_trxreg(u64 val);
  void handle_trxdir(u64 dir);
  void handle_xyzf2_common(u32 x, u32 y, u32 z, u8 f, bool advance);
};
