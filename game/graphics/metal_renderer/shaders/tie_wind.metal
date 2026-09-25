//
// Metal port of game/graphics/opengl_renderer/shaders/tie_wind.{vert,frag}
//
// The swaying TIE instances: the palm trees, the banners, the tall grass. They are drawn one
// instance at a time with the instance's own wind matrix in place of the camera, which is why
// this cannot share tfrag3's shader -- tfrag3 is handed a matrix that already folds the
// perspective in (make_new_cam_mat), and this is handed the game's own camera matrix and does the
// perspective divide by hand, exactly as the VU program did.
//
// Differences from the GLSL: the uniforms are a struct, the time-of-day colours come from a
// device buffer rather than a sampler1D, SCISSOR_ADJUST / HEIGHT_SCALE are fields, and the depth
// range is remapped from OpenGL's [-w, w] to Metal's [0, w].
//
// Vertex layout is tfrag3::PreloadedVertex, the same as tfrag3 and the rest of TIE.
//

#include <metal_stdlib>
using namespace metal;

#include "metal_shader_types.h"

struct TieWindVertexIn {
  float3 position_in [[attribute(0)]];
  float2 tex_coord_in [[attribute(1)]];
  ushort time_of_day_index [[attribute(2)]];
};

struct TieWindVertexOut {
  float4 position [[position]];
  float4 fragment_color;
  float2 tex_coord;
  float fogginess;
};

vertex TieWindVertexOut tie_wind_vert(TieWindVertexIn in [[stage_in]],
                                      constant TieWindUniforms& u
                                      [[buffer(MetalBufferIndexUniforms)]],
                                      const device float4* time_of_day
                                      [[buffer(MetalBufferIndexTimeOfDay)]]) {
  TieWindVertexOut out;

  float4 transformed = -u.camera[3];
  transformed -= u.camera[0] * in.position_in.x;
  transformed -= u.camera[1] * in.position_in.y;
  transformed -= u.camera[2] * in.position_in.z;
  float Q = u.fog_constant / transformed.w;

  out.fogginess = 255.0 - clamp(-transformed.w + u.hvdf_offset.w, u.fog_min, u.fog_max);

  // perspective divide!
  transformed.xyz *= Q;
  // offset
  transformed.xyz += u.hvdf_offset.xyz;
  // correct xy offset
  transformed.xy -= 2048.0;
  // correct z scale
  transformed.z /= 8388608.0;
  transformed.z -= 1.0;
  // correct xy scale
  transformed.x /= 256.0;
  transformed.y /= -128.0;
  // hack
  transformed.xyz *= transformed.w;
  // scissoring area adjust
  transformed.y *= u.scissor_adjust * u.height_scale;
  // OpenGL clips z to [-w, w], Metal to [0, w].
  transformed.z = (transformed.z + transformed.w) * 0.5;
  out.position = transformed;

  // time of day lookup
  float4 color = time_of_day[in.time_of_day_index];
  // color adjustment
  color *= 2.0;
  color.a *= 2.0;
  if (u.decal != 0) {
    // tfrag/tie always use TCC=RGB, so even with decal, alpha comes from fragment.
    color.rgb = float3(1.0, 1.0, 1.0);
  }
  out.fragment_color = color;
  out.tex_coord = in.tex_coord_in;
  return out;
}

fragment float4 tie_wind_frag(TieWindVertexOut in [[stage_in]],
                              constant TieWindUniforms& u [[buffer(MetalBufferIndexUniforms)]],
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
