//
// Metal port of game/graphics/opengl_renderer/shaders/shrub.{vert,frag}
//
// Same three differences from the GLSL as tfrag3.metal: the time-of-day colours come from a device
// buffer rather than a sampler1D, the uniforms are a struct, and the depth range is remapped for
// Metal. Everything else is the GLSL, including the /4096 on the texture coordinates, which is the
// PS2's fixed-point scale.
//
// Types are prefixed because every shader is compiled into one library from one concatenated
// source.
//
// Vertex layout is tfrag3::ShrubGpuVertex.
//

#include <metal_stdlib>
using namespace metal;

#include "metal_shader_types.h"

struct ShrubVertexIn {
  float3 position [[attribute(0)]];
  float2 tex_coord [[attribute(1)]];
  ushort color_index [[attribute(2)]];
  uchar3 rgba_base [[attribute(3)]];
};

struct ShrubVertexOut {
  float4 position [[position]];
  float4 fragment_color;
  float2 tex_coord;
  float fogginess;
};

vertex ShrubVertexOut shrub_vert(ShrubVertexIn in [[stage_in]],
                                 constant Tfrag3Uniforms& u [[buffer(MetalBufferIndexUniforms)]],
                                 const device float4* time_of_day
                                 [[buffer(MetalBufferIndexTimeOfDay)]]) {
  ShrubVertexOut out;

  float3 cam_trans = float3(u.cam_trans.x, u.cam_trans.y, u.cam_trans.z);
  float3 vert = in.position - cam_trans;
  // Note: unlike tfrag3, the GLSL does not zero w before accumulating.
  float4 transformed = -u.pc_camera[3];
  transformed -= u.pc_camera[0] * vert.x;
  transformed -= u.pc_camera[1] * vert.y;
  transformed -= u.pc_camera[2] * vert.z;

  out.fogginess = 255.0 - clamp(-transformed.w + u.hvdf_offset.w, u.fog_min, u.fog_max);

  transformed.y *= u.scissor_adjust * u.height_scale;
  // OpenGL clips z to [-w, w], Metal to [0, w].
  transformed.z = (transformed.z + transformed.w) * 0.5;
  out.position = transformed;

  // Start from the vertex colour (VIF filled in the alpha), then apply the time-of-day multiplier.
  float4 color = float4(float3(in.rgba_base) / 255.0, 1.0);
  color *= time_of_day[in.color_index] * 4.0;
  if (u.decal != 0) {
    color.rgb = float3(1.0, 1.0, 1.0);
  }
  out.fragment_color = color;

  out.tex_coord = in.tex_coord / 4096.0;
  return out;
}

fragment float4 shrub_frag(ShrubVertexOut in [[stage_in]],
                           constant Tfrag3Uniforms& u [[buffer(MetalBufferIndexUniforms)]],
                           texture2d<float> tex [[texture(0)]],
                           sampler samp [[sampler(0)]]) {
  float4 color;
  if (u.gfx_hack_no_tex == 0) {
    color = in.fragment_color * tex.sample(samp, in.tex_coord);
  } else {
    color = in.fragment_color;
  }

  if (color.a < u.alpha_min || color.a > u.alpha_max) {
    discard_fragment();
  }

  float4 fog_color = u.fog_color;
  color.rgb = mix(color.rgb, fog_color.rgb, clamp(in.fogginess * fog_color.a, 0.0, 1.0));
  return color;
}
