//
// Metal port of game/graphics/opengl_renderer/shaders/direct2.{vert,frag}
//
// The direct renderer draws whatever the game hands it as raw GS primitives: HUD, subtitles,
// debug lines, the parts of other renderers that fall back to it. Its vertices are already in
// GS screen coordinates, so the vertex shader is a fixed rescale rather than a camera transform.
//
// Two differences from the GLSL:
//
//   - The ten texture units are an array rather than ten named samplers with a switch. Same
//     thing, less source.
//   - alpha_reject, color_mult and fog_color are fields in Direct2Uniforms, because Metal has no
//     free-floating uniforms.
//

#include <metal_stdlib>
using namespace metal;

#include "metal_shader_types.h"

struct Direct2VertexIn {
  float3 position [[attribute(0)]];
  float4 rgba [[attribute(1)]];
  float3 tex_coord [[attribute(2)]];
  uchar4 byte_info [[attribute(3)]];
};

struct Direct2VertexOut {
  float4 position [[position]];
  float4 fragment_color;
  float3 tex_coord;
  float fog;
  // `flat`: the texture unit and the mode bits are per-draw, not per-pixel, and interpolating an
  // index would sample the wrong texture along an edge.
  uint2 tex_info [[flat]];
};

vertex Direct2VertexOut direct2_vert(Direct2VertexIn in [[stage_in]],
                                     constant Direct2Uniforms& u [[buffer(MetalBufferIndexUniforms)]]) {
  Direct2VertexOut out;
  float4 pos = float4((in.position.x - 0x8000) / 0x1000,
                      -(in.position.y - 0x8000) / 0x800,
                      0.0, 1.0);
  // Scissoring area adjust.
  pos.y *= u.scissor_adjust;
  // The GLSL maps z into OpenGL's [-1, 1] with `z / 0x800000 - 1`. Metal's clip space is [0, 1],
  // so the same value is `z / 0x1000000`. Halving the divisor here rather than remapping after
  // keeps the precision the 24-bit GS depth value had.
  pos.z = in.position.z / 16777216.0;
  out.position = pos;

  // The alpha doubling matches the GS, where 128 means fully opaque.
  out.fragment_color = float4(in.rgba.x, in.rgba.y, in.rgba.z, in.rgba.w * 2.0);
  out.tex_coord = in.tex_coord;
  out.tex_info = uint2(in.byte_info.x, in.byte_info.y);
  out.fog = 255.0 - float(in.byte_info.z);
  return out;
}

fragment float4 direct2_frag(Direct2VertexOut in [[stage_in]],
                             constant Direct2Uniforms& u [[buffer(MetalBufferIndexUniforms)]],
                             array<texture2d<float>, MetalDirect2TexUnits> textures [[texture(0)]],
                             array<sampler, MetalDirect2TexUnits> samplers [[sampler(0)]]) {
  float4 color;
  uint unit = min(in.tex_info.x, (uint)(MetalDirect2TexUnits - 1));
  float4 T0 = textures[unit].sample(samplers[unit], in.tex_coord.xy / in.tex_coord.z);

  // bit 0 is TCC (does the texture supply alpha), bit 1 is decal.
  const bool tcc = (in.tex_info.y & 1u) != 0;
  const bool decal = (in.tex_info.y & 2u) != 0;
  if (!tcc) {
    if (!decal) {
      color.rgb = in.fragment_color.rgb * T0.rgb;
    } else {
      color.rgb = T0.rgb * 0.5;
    }
    color.a = in.fragment_color.a;
  } else {
    if (!decal) {
      color = in.fragment_color * T0;
    } else {
      color.rgb = T0.rgb * 0.5;
      color.a = T0.a;
    }
  }

  color *= 2.0;
  color.rgb *= u.color_mult;
  if (color.a < u.alpha_reject) {
    discard_fragment();
  }
  // bit 2 is fog.
  if ((in.tex_info.y & 4u) != 0) {
    color.rgb = mix(color.rgb, u.fog_color.rgb, clamp(u.fog_color.a * in.fog, 0.0, 1.0));
  }
  return color;
}
