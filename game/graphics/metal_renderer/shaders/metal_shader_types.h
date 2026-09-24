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

// How many textures one grouped direct draw can use at once. Must match TEX_UNITS in
// DirectRenderer2Core and the array size in direct2.metal.
enum { MetalDirect2TexUnits = 10 };
