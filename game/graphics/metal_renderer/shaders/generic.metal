//
// Metal port of game/graphics/opengl_renderer/shaders/generic.{vert,frag}
//
// "Generic" is the fallback renderer: lightning, warps, eco effects, some HUD pieces, and
// whatever else a level hands it that does not fit a specialised renderer. Ten buckets use it.
//
// Differences from the GLSL, all mechanical: the uniforms are one struct, SCISSOR_ADJUST /
// HEIGHT_SCALE / SCISSOR_HEIGHT are fields rather than compile-time substitutions, and the depth
// range is remapped from OpenGL's [-w, w] to Metal's [0, w].
//
// Vertex layout is Generic2Core::Vertex.
//

#include <metal_stdlib>
using namespace metal;

#include "metal_shader_types.h"

struct GenericVertexIn {
  float3 position_in [[attribute(0)]];
  float4 rgba_in [[attribute(1)]];
  float2 tex_coord_in [[attribute(2)]];
  uchar4 byte_info [[attribute(3)]];
};

struct GenericVertexOut {
  float4 position [[position]];
  float2 tex_coord;
  float4 fragment_color;
  float fog;
  ushort2 tex_info [[flat]];
};

vertex GenericVertexOut generic_vert(GenericVertexIn in [[stage_in]],
                                     constant GenericUniforms& u
                                     [[buffer(MetalBufferIndexUniforms)]]) {
  GenericVertexOut out;
  float4 transformed;

  if (u.use_full_matrix != 0) {
    transformed = -u.full_matrix[3];
    transformed -= u.full_matrix[0] * in.position_in.x;
    transformed -= u.full_matrix[1] * in.position_in.y;
    transformed -= u.full_matrix[2] * in.position_in.z;
  } else {
    transformed.xyz = in.position_in * u.scale.xyz;
    transformed.z += u.mat_32;
    transformed.w = u.mat_23 * in.position_in.z + u.mat_33;
    transformed *= -1.0;
  }

  // perspective divide
  float Q = u.fog_constants.x / transformed.w;

  out.fog = 255.0 - clamp(-transformed.w + u.hvdf_offset.w, u.fog_constants.y, u.fog_constants.z);

  out.tex_coord = in.tex_coord_in / 4096.0;

  if (u.warp_sample_mode == 1) {
    const float warp_off = 1.0 - (u.scissor_height / 512.0);
    out.tex_coord = float2(out.tex_coord.x, (1.0 - out.tex_coord.y - warp_off) * u.scissor_adjust);
  }

  transformed.xyz *= Q;
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

  out.fragment_color = float4(in.rgba_in.rgb, in.rgba_in.a * 2.0);
  out.tex_info = ushort2(in.byte_info.x, in.byte_info.y);
  return out;
}

fragment float4 generic_frag(GenericVertexOut in [[stage_in]],
                             constant GenericUniforms& u [[buffer(MetalBufferIndexUniforms)]],
                             texture2d<float> tex_T0 [[texture(0)]],
                             sampler samp [[sampler(0)]]) {
  // 0x1 is tcc
  // 0x2 is decal
  // 0x4 is fog
  float4 color;

  if (u.warp_sample_mode == 1 || u.gfx_hack_no_tex == 0) {
    float4 T0 = tex_T0.sample(samp, in.tex_coord);
    if ((in.tex_info.y & 1u) == 0) {
      if ((in.tex_info.y & 2u) == 0) {
        // modulate + no tcc
        color.rgb = in.fragment_color.rgb * T0.rgb;
        color.a = in.fragment_color.a;
      } else {
        // decal + no tcc
        color.rgb = T0.rgb * 0.5;
        color.a = in.fragment_color.a;
      }
    } else {
      if ((in.tex_info.y & 2u) == 0) {
        // modulate + tcc
        color = in.fragment_color * T0;
      } else {
        // decal + tcc
        color.rgb = T0.rgb * 0.5;
        color.a = T0.a;
      }
    }
    color *= 2.0;
  } else {
    if ((in.tex_info.y & 1u) == 0) {
      if ((in.tex_info.y & 2u) == 0) {
        color.rgb = in.fragment_color.rgb;
        color.a = in.fragment_color.a * 2.0;
      } else {
        color.rgb = float3(1.0);
        color.a = in.fragment_color.a * 2.0;
      }
    } else {
      if ((in.tex_info.y & 2u) == 0) {
        color = in.fragment_color;
      } else {
        color.rgb = float3(0.5);
        color.a = 1.0;
      }
    }
  }
  color.rgb *= u.color_mult;

  if (color.a < u.alpha_reject) {
    discard_fragment();
  }
  if ((in.tex_info.y & 4u) != 0) {
    color.rgb = mix(color.rgb, u.fog_color.rgb, clamp(u.fog_color.a * in.fog, 0.0, 1.0));
  }
  return color;
}
