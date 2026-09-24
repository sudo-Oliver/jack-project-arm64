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
#else
#include "common/common_types.h"
struct MetalFloat4 {
  float x, y, z, w;
};
#define METAL_FLOAT4 MetalFloat4
#endif

// Buffer slots. Keep these in sync with the [[buffer(n)]] attributes in the shaders.
enum MetalBufferIndex {
  MetalBufferIndexVertex = 0,
  MetalBufferIndexUniforms = 1,
};

struct SolidColorUniforms {
  METAL_FLOAT4 fragment_color;
};
