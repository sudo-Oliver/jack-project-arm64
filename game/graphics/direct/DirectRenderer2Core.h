#pragma once

/*!
 * @file DirectRenderer2Core.h
 * The GS state machine behind the direct renderer, with no graphics API in it.
 *
 * Everything that turns a GIF packet into vertices and draws -- the register handlers, the
 * primitive assembly, the DrawMode bookkeeping -- is identical for OpenGL and Metal, and subtle
 * enough that a second copy would be a second set of bugs. So it lives here, and each backend
 * subclasses it to implement one thing: flush_draws(), which uploads the buffers and issues the
 * draw calls.
 */

#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/dma/gs.h"
#include "common/math/Vector.h"

class DirectRenderer2Core {
 public:
  DirectRenderer2Core(u32 max_verts,
                      u32 max_inds,
                      u32 max_draws,
                      const std::string& name,
                      bool use_ftoi_mod);
  virtual ~DirectRenderer2Core();

  void reset_state();
  // Consumes a GIF packet, appending to the vertex and draw buffers. Calls flush_draws() when
  // they are close to full.
  void render_gif_data(const u8* data);

  const std::string& name() const { return m_name; }

 protected:
  static constexpr u8 TEX_UNITS = 10;

  // Upload m_vertices and issue m_draw_buffer, then let reset_buffers() run. The backends
  // implement this; everything above it is shared.
  virtual void flush_draws() = 0;

  void reset_buffers();

  // the GsState is the state of all Gs Registers.
  struct GsState {
    DrawMode as_mode;
    u16 tbp;
    GsTest gs_test;
    GsTex0 gs_tex0;
    GsPrim gs_prim;
    GsAlpha gs_alpha;
    u8 tex_unit = 0;

    float s, t, Q;
    math::Vector<u8, 4> rgba;
    bool next_vertex_starts_strip = true;
    u8 vertex_flags = 0;
    void set_tcc_flag(bool value) { vertex_flags ^= (-(u8)value ^ vertex_flags) & 1; }
    void set_decal_flag(bool value) { vertex_flags ^= (-(u8)value ^ vertex_flags) & 2; }
    void set_fog_flag(bool value) { vertex_flags ^= (-(u8)value ^ vertex_flags) & 4; }
  } m_state;

  // if this is true, then drawing a vertex can just get pushed directly to the vertex buffer.
  // if not, we need to set up a new draw
  bool m_current_state_has_open_draw = false;

  struct Draw {
    DrawMode mode;
    u32 start_index = -1;
    u16 tbp = UINT16_MAX;
    u8 fix = 0;
    u8 tex_unit = 0;

    std::string to_string() const;
    std::string to_single_line_string() const;
  };

  std::vector<Draw> m_draw_buffer;
  u32 m_next_free_draw = 0;

  struct Vertex {
    math::Vector<float, 3> xyz;
    math::Vector<u8, 4> rgba;
    math::Vector<float, 3> stq;
    u8 tex_unit;
    u8 flags;
    u8 fog;
    u8 pad;

    std::string print() const;
  };
  static_assert(sizeof(Vertex) == 32);

  struct VertexBuffer {
    std::vector<Vertex> vertices;
    std::vector<u32> indices;
    u32 next_vertex = 0;
    u32 next_index = 0;

    void push_reset() { indices[next_index++] = UINT32_MAX; }

    Vertex& push() {
      indices[next_index++] = next_vertex;
      return vertices[next_vertex++];
    }

    bool close_to_full() {
      return (next_vertex + 40 > vertices.size()) || (next_index + 40 > indices.size());
    }
  } m_vertices;

  struct Stats {
    u32 upload_bytes = 0;
    u32 num_uploads = 0;
    u32 flush_due_to_full = 0;
    float upload_wait = 0;
    u32 saved_draws = 0;
  } m_stats;

  struct Debug {
    bool disable_mip = true;
  } m_debug;

  std::string m_name;
  bool m_use_ftoi_mod = false;

  // gif handlers
  void handle_ad(const u8* data);

  void handle_test1(u64 val);
  void handle_tex0_1(u64 val);
  void handle_tex1_1(u64 val);
  void handle_clamp1(u64 val);
  void handle_prim(u64 val);
  void handle_alpha1(u64 val);
  void handle_zbuf1(u64 val);

  // packed
  void handle_st_packed(const u8* data);
  void handle_rgbaq_packed(const u8* data);
  void handle_xyzf2_packed(const u8* data);
  void handle_xyzf2_mod_packed(const u8* data);
};
