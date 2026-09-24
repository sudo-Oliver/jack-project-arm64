//
// Metal port of game/graphics/opengl_renderer/shaders/tfrag3.{vert,frag}
//
// Two deliberate differences from the GLSL:
//
//   - The time-of-day colours arrive in a device buffer instead of a `sampler1D` fetched from the
//     vertex shader. Metal has 1D textures, but the GLSL only ever did a `texelFetch` by integer
//     index, which is a buffer read spelled as a texture read.
//   - Every former `uniform` is a field in Tfrag3Uniforms (metal_shader_types.h). Metal has no
//     free-floating uniforms.
//
// The vertex layout is tfrag3::PreloadedVertex, unpacked from the .fr3 by the shared loader, so
// the attribute offsets here have to match that struct.
//

#include <metal_stdlib>
using namespace metal;

#include "metal_shader_types.h"

struct VertexIn {
  float3 position [[attribute(0)]];
  float2 tex_coord [[attribute(1)]];
  ushort color_index [[attribute(2)]];
};

struct VertexOut {
  float4 position [[position]];
  float4 fragment_color;
  float2 tex_coord;
  float fogginess;
};

vertex VertexOut tfrag3_vert(VertexIn in [[stage_in]],
                             constant Tfrag3Uniforms& u [[buffer(MetalBufferIndexUniforms)]],
                             const device float4* time_of_day [[buffer(MetalBufferIndexTimeOfDay)]]) {
  VertexOut out;

  // The camera transform, same as the GLSL: the PS2 did this on VU1 with the matrix already
  // negated, which is why every term is subtracted.
  float3 cam_trans = float3(u.cam_trans.x, u.cam_trans.y, u.cam_trans.z);
  float3 vert = in.position - cam_trans;
  float4 transformed = -u.pc_camera[3];
  transformed.w = 0;
  transformed -= u.pc_camera[0] * vert.x;
  transformed -= u.pc_camera[1] * vert.y;
  transformed -= u.pc_camera[2] * vert.z;

  out.fogginess = 255.0 - clamp(-transformed.w + u.hvdf_offset.w, u.fog_min, u.fog_max);

  // Scissoring area adjust. Metal's clip space matches OpenGL's in x and y; the depth range
  // difference is handled by the projection the game already hands us.
  transformed.y *= u.scissor_adjust * u.height_scale;
  out.position = transformed;

  float4 color = time_of_day[in.color_index];
  color *= 2.0;
  color.a *= 2.0;
  if (u.decal != 0) {
    // tfrag/tie always use TCC=RGB, so even with decal the alpha comes from the fragment.
    color.rgb = float3(1.0, 1.0, 1.0);
  }
  out.fragment_color = color;
  out.tex_coord = in.tex_coord;
  return out;
}

fragment float4 tfrag3_frag(VertexOut in [[stage_in]],
                            constant Tfrag3Uniforms& u [[buffer(MetalBufferIndexUniforms)]],
                            texture2d<float> tex [[texture(0)]],
                            sampler samp [[sampler(0)]]) {
  float4 color;
  if (u.gfx_hack_no_tex == 0) {
    color = in.fragment_color * tex.sample(samp, in.tex_coord);
  } else {
    color = in.fragment_color / 2.0;
  }

  if (color.a < u.alpha_min || color.a > u.alpha_max) {
    discard_fragment();
  }

  float4 fog_color = u.fog_color;
  color.rgb = mix(color.rgb, fog_color.rgb, clamp(in.fogginess * fog_color.a, 0.0, 1.0));
  return color;
}
