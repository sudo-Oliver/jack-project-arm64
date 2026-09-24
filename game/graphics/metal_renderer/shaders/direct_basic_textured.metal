//
// Metal port of game/graphics/opengl_renderer/shaders/direct_basic_textured.{vert,frag}
//
// This is the shader the older direct renderer uses -- the one the sky, the ocean, the debug
// draws and the subtitles go through. Its vertices are already in GS screen coordinates, so the
// vertex shader is a fixed rescale rather than a camera transform.
//
// Differences from the GLSL:
//
//   - Every former `uniform` is a field in DirectUniforms.
//   - The scissor test reads the fragment's window position, whose origin is the top left on
//     Metal and the bottom left on OpenGL, so the y is flipped before the comparison.
//   - textureSize becomes get_width/get_height.
//

#include <metal_stdlib>
using namespace metal;

#include "metal_shader_types.h"

struct DirectVertexIn {
  // xyz plus the fog value in w.
  float4 position [[attribute(0)]];
  float4 rgba [[attribute(1)]];
  float3 tex_coord [[attribute(2)]];
  uchar4 tex_info [[attribute(3)]];
  uchar use_uv [[attribute(4)]];
  float4 gs_scissor [[attribute(5)]];
};

struct DirectVertexOut {
  float4 position [[position]];
  float4 fragment_color;
  float3 tex_coord;
  float fog;
  float4 gs_scissor;
  // Per draw, not per pixel: interpolating a mode bit would blend two modes along an edge.
  uint4 tex_info [[flat]];
  uint use_uv [[flat]];
};

vertex DirectVertexOut direct_basic_textured_vert(
    DirectVertexIn in [[stage_in]],
    constant DirectUniforms& u [[buffer(MetalBufferIndexUniforms)]]) {
  DirectVertexOut out;
  float4 pos;
  if (u.offscreen_mode == 1) {
    pos = float4((in.position.x - 0.453125) * 64.0,
                 (in.position.y - 0.5 + (2.25 / 64.0)) * 64.0,
                 in.position.z * 2.0 - 1.0, 1.0);
  } else {
    pos = float4((in.position.x - 0.5) * 16.0,
                 -(in.position.y - 0.5) * 32.0 * u.height_scale,
                 in.position.z * 2.0 - 1.0, 1.0);
    // Scissoring area adjust.
    pos.y *= u.scissor_adjust;
  }
  // OpenGL clips z to [-1, 1], Metal to [0, 1]. w is 1 here, so this is the whole remap.
  pos.z = in.position.z;
  out.position = pos;

  out.fragment_color = float4(in.rgba.x, in.rgba.y, in.rgba.z, in.rgba.w * 2.0);
  out.tex_coord = in.tex_coord;
  out.tex_info = uint4(in.tex_info);
  out.fog = 255.0 - in.position.w;
  out.use_uv = in.use_uv;
  out.gs_scissor = in.gs_scissor;
  return out;
}

fragment float4 direct_basic_textured_frag(DirectVertexOut in [[stage_in]],
                                           constant DirectUniforms& u
                                               [[buffer(MetalBufferIndexUniforms)]],
                                           texture2d<float> tex [[texture(0)]],
                                           sampler samp [[sampler(0)]]) {
  if (u.scissor_enable == 1) {
    float x = in.position.x;
    // Metal's window origin is the top left; the GS coordinates this compares against are
    // bottom-up, like OpenGL's.
    float y = u.game_sizes.w - in.position.y;
    float w = u.game_sizes.z / u.game_sizes.x;
    float h = u.game_sizes.w / u.game_sizes.y;
    float scax0 = in.gs_scissor.x * w + 0.5;
    float scax1 = in.gs_scissor.y * w + 0.5;
    float scay0 = (u.game_sizes.y - in.gs_scissor.w) * h + 0.5;
    float scay1 = (u.game_sizes.y - in.gs_scissor.z) * h + 0.5;
    if (x < scax0 || x > scax1) {
      discard_fragment();
    }
    if (y < scay0 || y > scay1) {
      discard_fragment();
    }
  }

  float4 T0;
  if (in.use_uv == 1) {
    // Texel coordinates rather than normalised ones, and no perspective correction. The current
    // users are quads at one depth, so the difference does not show.
    float2 coord_px = in.tex_coord.xy / 16.0;
    float2 tex_size = float2(tex.get_width(), tex.get_height());
    T0 = tex.sample(samp, coord_px / tex_size);
  } else {
    T0 = tex.sample(samp, in.tex_coord.xy / in.tex_coord.z);
  }

  if (T0.w == 0.0) {
    T0.w = u.ta0;
  }

  float4 color;
  // y is TCC (does the texture supply alpha), z is decal.
  if (in.tex_info.y == 0) {
    if (in.tex_info.z == 0) {
      color.rgb = in.fragment_color.rgb * T0.rgb;
    } else {
      color.rgb = T0.rgb * 0.5;
    }
    color.a = in.fragment_color.a;
  } else {
    if (in.tex_info.z == 0) {
      color = in.fragment_color * T0;
    } else {
      color.rgb = T0.rgb * 0.5;
      color.a = T0.a;
    }
  }

  color *= 2.0;
  color.rgb *= u.color_mult;
  color.a *= u.alpha_mult;

  if (u.greater != 0) {
    // A GREATER alpha test passes above the reference, so the two halves of a double draw each
    // take the value exactly at the reference once and only once.
    if (color.a <= u.alpha_min || color.a > u.alpha_max) {
      discard_fragment();
    }
  } else {
    if (color.a < u.alpha_min || color.a >= u.alpha_max) {
      discard_fragment();
    }
  }

  if (in.tex_info.w == 1) {
    color.rgb = mix(color.rgb, u.fog_color.rgb, clamp(u.fog_color.a * in.fog, 0.0, 1.0));
  }
  return color;
}
