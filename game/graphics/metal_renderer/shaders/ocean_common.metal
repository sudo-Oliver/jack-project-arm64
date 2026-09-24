//
// Metal port of game/graphics/opengl_renderer/shaders/ocean_common.{vert,frag}
//
// Both ocean renderers draw through this. `bucket` says which pass a draw is, and the numbering
// is the OpenGL shader's: 0 and 3 are the water with its texture, 1 and 2 the alpha and envmap
// passes of the near ocean, 4 the envmap pass of the mid ocean.
//

#include <metal_stdlib>
using namespace metal;

#include "metal_shader_types.h"

struct OceanCommonVertexIn {
  float3 position [[attribute(0)]];
  float4 rgba [[attribute(1)]];
  float3 tex_coord [[attribute(2)]];
  uchar4 fog [[attribute(3)]];
};

struct OceanCommonVertexOut {
  float4 position [[position]];
  float4 fragment_color;
  float3 tex_coord;
  float fog;
};

vertex OceanCommonVertexOut ocean_common_vert(
    OceanCommonVertexIn in [[stage_in]],
    constant OceanCommonUniforms& u [[buffer(MetalBufferIndexUniforms)]]) {
  OceanCommonVertexOut out;
  float4 pos = float4((in.position.x - 0.5) * 16.0, -(in.position.y - 0.5) * 32.0,
                      in.position.z * 2.0 - 1.0, 1.0);
  // Scissoring area adjust.
  pos.y *= u.scissor_adjust * u.height_scale;
  // OpenGL clips z to [-1, 1], Metal to [0, 1]. w is 1, so this is the whole remap.
  pos.z = in.position.z;
  out.position = pos;

  out.fragment_color = float4(in.rgba.rgb, in.rgba.a * 2.0);
  out.tex_coord = in.tex_coord;
  out.fog = 255.0 - float(in.fog.x);

  if (u.bucket == 0) {
    out.fragment_color.rgb *= 2.0;
  } else if (u.bucket == 1 || u.bucket == 3) {
    out.fragment_color *= 2.0;
  } else if (u.bucket == 4) {
    // Zeroing the destination alpha is what makes the low-poly mesh drawn afterwards vanish.
    out.fragment_color.a = 0.0;
  }
  return out;
}

fragment float4 ocean_common_frag(OceanCommonVertexOut in [[stage_in]],
                                  constant OceanCommonUniforms& u
                                      [[buffer(MetalBufferIndexUniforms)]],
                                  texture2d<float> tex [[texture(0)]],
                                  sampler samp [[sampler(0)]]) {
  float4 T0 = tex.sample(samp, in.tex_coord.xy / in.tex_coord.z);
  float4 color = float4(0.0);
  if (u.bucket == 0) {
    color.rgb = in.fragment_color.rgb * T0.rgb;
    color.a = in.fragment_color.a;
    color.rgb = mix(color.rgb, u.fog_color.rgb, clamp(u.fog_color.a * in.fog, 0.0, 1.0));
  } else if (u.bucket == 1 || u.bucket == 2 || u.bucket == 4) {
    color = in.fragment_color * T0;
  } else if (u.bucket == 3) {
    color = in.fragment_color * T0;
    color.rgb = mix(color.rgb, u.fog_color.rgb, clamp(u.fog_color.a * in.fog, 0.0, 1.0));
  }
  return color;
}
