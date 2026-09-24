#pragma once

/*!
 * @file OceanTextureCore.h
 * The ocean texture, with no graphics API in it.
 *
 * The ocean is drawn with a texture the game builds every frame out of the water's height field.
 * Building it is VU1 code: the DMA carries vertex groups and calls into microprograms, and the
 * emulation of those produces a grid of vertices. All of that is the PS2, not a backend, so it
 * lives here.
 *
 * What a backend provides is the render target the grid is drawn into, the draw itself, and the
 * mipmap chain over the result.
 */

#include <vector>

#include "common/common_types.h"
#include "common/dma/dma_chain_read.h"
#include "common/math/Vector.h"
#include "common/versions/versions.h"

#include "game/common/vu.h"
#include "game/graphics/opengl_renderer/AdgifHandler.h"
#include "game/graphics/ocean/CommonOceanRendererCore.h"  // for OCEAN_TEX_TBP_JAK1
#include "game/graphics/texture/TexturePool.h"

class OceanTextureCore {
 public:
  OceanTextureCore(bool generate_mipmaps);
  virtual ~OceanTextureCore();

  void handle_ocean_texture_jak1(DmaFollower& dma, TexturePool* texture_pool);
  void handle_ocean_texture_jak2(DmaFollower& dma, TexturePool* texture_pool);

 protected:
  // Start drawing into the texture. `to_temp` picks the scratch target, which is where the grid
  // goes when a mipmap chain has to be built from it afterwards.
  virtual void backend_begin_pass(bool to_temp) = 0;
  virtual void backend_end_pass() = 0;
  // Draw the grid the VU emulation produced.
  virtual void backend_flush() = 0;
  // Fill the result texture and its mip levels from the scratch target.
  virtual void backend_make_texture_with_mipmaps() = 0;

  void run_L1_PC();
  void run_L2_PC();
  void run_L3_PC();
  void run_L5_PC();
  void xgkick_PC(Vf* src);

  void run_L1_PC_jak2();
  void run_L2_PC_jak2();
  void run_L3_PC_jak2();

  void init_pc();

  TexturePool* m_texture_pool = nullptr;

  bool m_generate_mipmaps;

  static constexpr int TEX0_SIZE = 128;
  static constexpr int NUM_MIPS = 8;
  GpuTexture* m_tex0_gpu = nullptr;

  // (deftype ocean-texture-constants (structure)
  struct OceanTextureConstants {
    //  ((giftag    qword    :inline :offset-assert 0) 985
    u8 giftag[16];
    //   (buffers   vector4w :inline :offset-assert 16) 986
    math::Vector<u32, 4> buffers;
    //   (dests     vector4w :inline :offset-assert 32) 987
    math::Vector<u32, 4> dests;
    //   (start     vector   :inline :offset-assert 48) 988
    math::Vector4f start;
    //   (offsets   vector   :inline :offset-assert 64) 989
    math::Vector4f offsets;
    //   (constants vector   :inline :offset-assert 80) 990
    math::Vector4f constants;
    //   (cam-nrm   vector   :inline :offset-assert 96) 991
    math::Vector4f cam_nrm;
    //   )
  } m_texture_constants;
  static_assert(sizeof(OceanTextureConstants) == 112);

  AdGifData m_envmap_adgif;

  Vf m_texture_vertices_a[192];
  Vf m_texture_vertices_b[192];

  static constexpr int DBUF_SIZE = 99;
  Vf m_dbuf_a[DBUF_SIZE];
  Vf m_dbuf_b[DBUF_SIZE];

  Vf* m_dbuf_x;
  Vf* m_dbuf_y;

  static constexpr int TBUF_SIZE = 199;
  Vf m_tbuf_a[TBUF_SIZE];
  Vf m_tbuf_b[TBUF_SIZE];

  Vf* m_tbuf_x;
  Vf* m_tbuf_y;

  Vf* m_texture_vertices_loading = nullptr;
  Vf* m_texture_vertices_drawing = nullptr;

  Vf* swap_vu_upload_buffers() {
    std::swap(m_texture_vertices_drawing, m_texture_vertices_loading);
    return m_texture_vertices_drawing;
  }

  void swap_dbuf() { std::swap(m_dbuf_x, m_dbuf_y); }

  void swap_tbuf() { std::swap(m_tbuf_x, m_tbuf_y); }

  Vf* get_dbuf() { return m_dbuf_x; }

  Vf* get_dbuf_other() { return m_dbuf_y; }

  Vf* get_tbuf() { return m_tbuf_x; }

  struct {
    Vf startx;  //           vf14
    // Vf base_pos;          vf15
    // Vf nrm0;              vf24
    Vf* dbuf_read_a;      // vi03
    Vf* dbuf_read_b;      // vi04
    Vf* in_ptr;           // vi05
    Vf* dbuf_write;       // vi06
    Vf* dbuf_write_base;  // vi07
    Vf* tptr;             // vi08
    Vf* tbase;            // vi09
  } vu;

  static constexpr u32 NUM_STRIPS = 32;
  static constexpr u32 NUM_VERTS_PER_STRIP = 66;
  static constexpr u32 NUM_VERTS = NUM_STRIPS * NUM_VERTS_PER_STRIP;

  // Could be 8 bytes with u16 s/t, but some GPUs dislike that format.
  struct Vertex {
    float s, t;
    math::Vector<u8, 4> rgba;
    u32 pad;
  };
  static_assert(sizeof(Vertex) == 16);

  struct {
    std::vector<math::Vector2f> vertex_positions;
    std::vector<Vertex> vertex_dynamic;
    std::vector<u32> index_buffer;
    u32 vtx_idx = 0;
  } m_pc;

  enum TexVu1Data {
    BUF0 = 384,
    BUF1 = 583,
    DEST0 = 782,
    DEST1 = 881,
    CONSTANTS = 985,
  };

  enum TexVu1Prog { START = 0, REST = 2, DONE = 4 };

  static constexpr int NUM_FRAG_LOOPS = 9;
};
