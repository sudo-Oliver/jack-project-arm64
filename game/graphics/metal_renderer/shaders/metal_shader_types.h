#pragma once

//
// Layouts shared between the MSL shaders and the C++ that feeds them.
//
// This header is included from both .metal sources and normal C++, so it must stay free of
// anything that only one side understands. Metal has no free-floating uniforms the way GLSL
// does, so every former `uniform` becomes a field in one of these structs.
//
// If you change a struct here, change the shader that reads it in the same commit -- a mismatch
// is silent and shows up as wrong pixels, not an error.
//

#ifdef __METAL_VERSION__
#include <metal_stdlib>
#define METAL_FLOAT4 ::metal::float4
#define METAL_FLOAT4X4 ::metal::float4x4
#else
#include "common/common_types.h"
struct MetalFloat4 {
  float x, y, z, w;
};
struct MetalFloat4x4 {
  MetalFloat4 col[4];
};
#define METAL_FLOAT4 MetalFloat4
#define METAL_FLOAT4X4 MetalFloat4x4
#endif

// Buffer slots. Keep these in sync with the [[buffer(n)]] attributes in the shaders.
enum MetalBufferIndex {
  MetalBufferIndexVertex = 0,
  MetalBufferIndexUniforms = 1,
  MetalBufferIndexTimeOfDay = 2,
  // Per-instance vertex data, for the one renderer that instances: the sprite distorter.
  MetalBufferIndexInstance = 4,
};


// tfrag3.metal. Field order matters: the float4s come first so the scalars that follow do not sit
// in the padding Metal would insert before a 16-byte-aligned member.
struct Tfrag3Uniforms {
  METAL_FLOAT4X4 pc_camera;
  METAL_FLOAT4 hvdf_offset;
  METAL_FLOAT4 cam_trans;
  METAL_FLOAT4 fog_color;
  float fog_min;
  float fog_max;
  float alpha_min;
  float alpha_max;
  // Substituted into the GLSL at compile time by Shader.cpp; passed in here instead, so one
  // compiled library serves every game version.
  float scissor_adjust;
  float height_scale;
  int decal;
  int gfx_hack_no_tex;
};


// direct_basic_textured.metal. Everything the GS state cannot bake into a pipeline object.
struct DirectUniforms {
  METAL_FLOAT4 fog_color;
  // game width, game height, viewport width, viewport height
  METAL_FLOAT4 game_sizes;
  float alpha_min;
  float alpha_max;
  float color_mult;
  float alpha_mult;
  float ta0;
  float scissor_adjust;
  float height_scale;
  int scissor_enable;
  int greater;
  int offscreen_mode;
  int pad0, pad1;
};

// ocean_common.metal. `bucket` picks which of the ocean's passes this draw is; the numbers match
// the OpenGL shader's.
struct OceanCommonUniforms {
  METAL_FLOAT4 fog_color;
  float scissor_adjust;
  float height_scale;
  int bucket;
  int pad0;
};

// ocean_texture_mipmap.metal.
struct OceanMipmapUniforms {
  float alpha_intensity;
  float pad0, pad1, pad2;
};

// sky_blend.metal.
struct SkyBlendUniforms {
  float intensity;
  float pad0, pad1, pad2;
};

// merc2.metal. The lights are per draw, the camera constants per frame; both live here because
// Metal has no free-floating uniforms and one struct per shader is cheaper to set than two.
struct Merc2Uniforms {
  METAL_FLOAT4X4 perspective_matrix;

  METAL_FLOAT4 light_dir0_fade;
  METAL_FLOAT4 light_dir1_fade_en;
  METAL_FLOAT4 light_dir2;
  METAL_FLOAT4 light_col0;
  METAL_FLOAT4 light_col1;
  METAL_FLOAT4 light_col2;
  METAL_FLOAT4 light_ambient;

  METAL_FLOAT4 hvdf_offset;
  METAL_FLOAT4 fog_constants;
  METAL_FLOAT4 fog_color;
  METAL_FLOAT4 fade;  // emerc only

  float scissor_adjust;
  float height_scale;
  int ignore_alpha;
  int decal_enable;
  int gfx_hack_no_tex;
  int pad0, pad1, pad2;
};

// The skinning matrix for one bone, as the GLSL std140 block laid it out: a mat4 (4 float4s), a
// mat3 (3 float4s, since std140 pads each column) and one float4 of padding. That is exactly
// Merc2Core::ShaderMercMat, so the same buffer feeds both backends.
enum { MetalMercVectorsPerBone = 8 };

// Buffer slot the bone vectors are bound to in merc2.metal.
enum { MetalBufferIndexBones = 3 };

// sprite3.metal. Everything the sprite VU program needs, set once per frame. The 75-entry HUD
// offset table dominates the size, which is why the two alpha-test bounds are in their own struct
// below rather than in here: they change per draw, and re-sending 1.7 KB for two floats is waste.
//
// Field order is the GLSL's, with the matrices and float4s first so the scalars do not sit in the
// padding Metal inserts before a 16-byte-aligned member.
enum { MetalSpriteHudUserCount = 75 };
struct Sprite3Uniforms {
  METAL_FLOAT4X4 camera;
  METAL_FLOAT4X4 hud_matrix;
  METAL_FLOAT4 hvdf_offset;
  METAL_FLOAT4 hud_hvdf_offset;
  METAL_FLOAT4 basis_x;
  METAL_FLOAT4 basis_y;
  METAL_FLOAT4 xy_array[8];
  METAL_FLOAT4 xyz_array[4];
  METAL_FLOAT4 st_array[4];
  METAL_FLOAT4 hud_hvdf_user[MetalSpriteHudUserCount];
  float pfog0;
  float fog_min;
  float fog_max;
  float min_scale;
  float max_scale;
  float deg_to_rad;
  float inv_area;
  float scissor_adjust;
  float height_scale;
  float pad0, pad1, pad2;
};

// The per-draw half of sprite3.metal, and of the distorter: the alpha-test window the fragment
// shader discards outside of.
struct SpriteDrawUniforms {
  float alpha_min;
  float alpha_max;
  float pad0, pad1;
};

// generic.metal. One struct for the whole renderer: the projection the VU program would have
// applied, the fog, and the per-draw alpha reject and colour multiplier.
struct GenericUniforms {
  METAL_FLOAT4X4 full_matrix;
  METAL_FLOAT4 scale;
  METAL_FLOAT4 hvdf_offset;
  METAL_FLOAT4 fog_color;
  METAL_FLOAT4 fog_constants;  // pfog0, fog_min, fog_max, unused
  float mat_23;
  float mat_32;
  float mat_33;
  float alpha_reject;
  float color_mult;
  float scissor_adjust;
  float height_scale;
  float scissor_height;
  int use_full_matrix;
  int warp_sample_mode;
  int gfx_hack_no_tex;
  int pad0;
};

// shadow.metal. The flat colour of the pass being drawn, and the vertical scissor adjust that
// the GLSL had substituted in at compile time.
struct ShadowUniforms {
  METAL_FLOAT4 color;
  float scissor_adjust;
  float pad0, pad1, pad2;
};

// sprite_distort.metal. u_color is the distorter's global tint, from its sine table.
struct SpriteDistortUniforms {
  METAL_FLOAT4 u_color;
  float height_scale;
  float scissor_height;
  float pad0, pad1;
};

