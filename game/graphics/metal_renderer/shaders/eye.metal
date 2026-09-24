//
// Metal port of game/graphics/opengl_renderer/shaders/eye.{vert,frag}
//
// Draws one quad of an eye into that eye's own texture. The coordinates are the GS's, so the
// vertex shader is a fixed rescale.
//

#include <metal_stdlib>
using namespace metal;

#include "metal_shader_types.h"

struct EyeVertexIn {
  float4 xyst [[attribute(0)]];
};

struct EyeVertexOut {
  float4 position [[position]];
  float2 st;
};

vertex EyeVertexOut eye_vert(EyeVertexIn in [[stage_in]]) {
  EyeVertexOut out;
  out.position = float4((in.xyst.x - 768.0) / 256.0, (in.xyst.y - 768.0) / 256.0, 0.0, 1.0);
  out.st = in.xyst.zw;
  return out;
}

fragment float4 eye_frag(EyeVertexOut in [[stage_in]],
                         texture2d<float> tex [[texture(0)]],
                         sampler samp [[sampler(0)]]) {
  float4 color = tex.sample(samp, in.st);
  color.a *= 2.0;
  return color;
}
