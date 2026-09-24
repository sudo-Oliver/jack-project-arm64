//
// Metal port of game/graphics/opengl_renderer/shaders/ocean_texture_mipmap.{vert,frag}
//
// Fills one mip level of the water texture from the level above it, fading the alpha out as the
// levels get smaller so that distant water loses its transparency variation.
//
// The GLSL scales the quad down instead of setting a viewport, because OpenGL's framebuffer for
// a mip level kept the full-size viewport. Here each level gets a pass of its own size and the
// quad covers it, which is the same picture without the arithmetic.
//

#include <metal_stdlib>
using namespace metal;

#include "metal_shader_types.h"

struct OceanMipmapVertexOut {
  float4 position [[position]];
  float2 tex_coord;
};

vertex OceanMipmapVertexOut ocean_texture_mipmap_vert(uint vid [[vertex_id]]) {
  const float2 corners[4] = {float2(-1, -1), float2(-1, 1), float2(1, -1), float2(1, 1)};
  float2 c = corners[vid];
  OceanMipmapVertexOut out;
  out.position = float4(c, 0.5, 1.0);
  out.tex_coord = c * 0.5 + 0.5;
  return out;
}

fragment float4 ocean_texture_mipmap_frag(OceanMipmapVertexOut in [[stage_in]],
                                          constant OceanMipmapUniforms& u
                                              [[buffer(MetalBufferIndexUniforms)]],
                                          texture2d<float> tex [[texture(0)]],
                                          sampler samp [[sampler(0)]]) {
  float4 color = tex.sample(samp, in.tex_coord);
  color.a *= u.alpha_intensity;
  return color;
}
