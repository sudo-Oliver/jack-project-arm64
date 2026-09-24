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
};

struct SolidColorUniforms {
  METAL_FLOAT4 fragment_color;
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

// direct2.metal. Everything the GS draw mode cannot bake into a pipeline or a sampler.
struct Direct2Uniforms {
  METAL_FLOAT4 fog_color;  // rgb is the colour, a is the intensity
  float alpha_reject;
  float color_mult;
  float scissor_adjust;
  float pad;
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

// How many textures one grouped direct draw can use at once. Must match TEX_UNITS in
// DirectRenderer2Core and the array size in direct2.metal.
enum { MetalDirect2TexUnits = 10 };
