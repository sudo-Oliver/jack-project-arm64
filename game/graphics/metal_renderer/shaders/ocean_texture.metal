//
// Metal port of game/graphics/opengl_renderer/shaders/ocean_texture.{vert,frag}
//
// Draws the grid the ocean's VU program produced into the water texture. The positions are a
// static 0-2048 grid; the colours and texture coordinates change every frame.
//

#include <metal_stdlib>
using namespace metal;

#include "metal_shader_types.h"

struct OceanTextureVertexIn {
  float2 position [[attribute(0)]];
  float4 rgba [[attribute(1)]];
  float2 tex_coord [[attribute(2)]];
};

struct OceanTextureVertexOut {
  float4 position [[position]];
  float4 fragment_color;
  float2 tex_coord;
};

vertex OceanTextureVertexOut ocean_texture_vert(OceanTextureVertexIn in [[stage_in]]) {
  OceanTextureVertexOut out;
  // Positions come in as 0 - 2048.
  out.position = float4((in.position.x - 1024.0) / 1024.0, (in.position.y - 1024.0) / 1024.0,
                        0.5, 1.0);
  out.fragment_color = float4(in.rgba.rgb * 2.0, 1.0);
  out.tex_coord = in.tex_coord;
  return out;
}

fragment float4 ocean_texture_frag(OceanTextureVertexOut in [[stage_in]],
                                   texture2d<float> tex [[texture(0)]],
                                   sampler samp [[sampler(0)]]) {
  return in.fragment_color * tex.sample(samp, in.tex_coord);
}
