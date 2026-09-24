//
// Metal port of game/graphics/opengl_renderer/shaders/merc2.{vert,frag}
//
// merc2 is the foreground shader: it skins a vertex by up to three bones, lights it with the
// three directional lights plus ambient the game sends per model, and applies the same
// perspective divide the PS2's VU1 did.
//
// Differences from the GLSL:
//
//   - The bone matrices come from a device buffer instead of a std140 uniform block. The layout is
//     the one std140 produced, so the same bytes feed both backends.
//   - Every former `uniform` is a field in Merc2Uniforms.
//   - The clip-space z is remapped from OpenGL's [-w, w] to Metal's [0, w]. Same remap tfrag3 does.
//

#include <metal_stdlib>
using namespace metal;

#include "metal_shader_types.h"

struct MercMatrixData {
  // Columns, as std140 laid them out: X is a mat4, R a mat3 with each column padded to a float4.
  float4 X[4];
  float4 R[3];
  float4 pad;
};

struct Merc2VertexIn {
  float3 position [[attribute(0)]];
  float3 normal [[attribute(1)]];
  float3 weights [[attribute(2)]];
  float2 st [[attribute(3)]];
  float4 rgba [[attribute(4)]];
  uchar4 mats [[attribute(5)]];
};

struct Merc2VertexOut {
  float4 position [[position]];
  float4 vtx_color;
  float2 vtx_st;
  float fog;
};

static inline float4 merc_transform(constant MercMatrixData& bone, float4 p) {
  return bone.X[0] * p.x + bone.X[1] * p.y + bone.X[2] * p.z + bone.X[3] * p.w;
}

static inline float3 merc_rotate(constant MercMatrixData& bone, float3 n) {
  return bone.R[0].xyz * n.x + bone.R[1].xyz * n.y + bone.R[2].xyz * n.z;
}

vertex Merc2VertexOut merc2_vert(Merc2VertexIn in [[stage_in]],
                                 constant Merc2Uniforms& u [[buffer(MetalBufferIndexUniforms)]],
                                 constant MercMatrixData* bones [[buffer(MetalBufferIndexBones)]]) {
  Merc2VertexOut out;
  float4 p = float4(in.position, 1);

  float4 vtx_pos = -merc_transform(bones[in.mats[0]], p) * in.weights[0];
  float3 rotated_nrm = merc_rotate(bones[in.mats[0]], in.normal) * in.weights[0];

  // The game may send garbage bones when the weight is zero. Skipping them keeps NaNs out.
  if (in.weights[1] > 0) {
    vtx_pos += -merc_transform(bones[in.mats[1]], p) * in.weights[1];
    rotated_nrm += merc_rotate(bones[in.mats[1]], in.normal) * in.weights[1];
  }
  if (in.weights[2] > 0) {
    vtx_pos += -merc_transform(bones[in.mats[2]], p) * in.weights[2];
    rotated_nrm += merc_rotate(bones[in.mats[2]], in.normal) * in.weights[2];
  }

  float4x4 pm = u.perspective_matrix;
  float4 transformed = pm[0] * vtx_pos.x + pm[1] * vtx_pos.y + pm[2] * vtx_pos.z + pm[3] * vtx_pos.w;

  rotated_nrm = normalize(rotated_nrm);
  float3 light_intensity = u.light_dir0_fade.xyz * rotated_nrm.x +
                           u.light_dir1_fade_en.xyz * rotated_nrm.y +
                           u.light_dir2.xyz * rotated_nrm.z;
  light_intensity = max(light_intensity, float3(0.0));

  float4 light_color = u.light_ambient + light_intensity.x * u.light_col0 +
                       light_intensity.y * u.light_col1 + light_intensity.z * u.light_col2;

  float Q = u.fog_constants.x / transformed.w;
  out.fog = 255.0 - clamp(-transformed.w + u.hvdf_offset.w, u.fog_constants.y, u.fog_constants.z);

  // The perspective divide the VU1 code did by hand.
  transformed.xyz *= Q;
  transformed.xyz += u.hvdf_offset.xyz;
  transformed.xy -= 2048.0;
  transformed.z /= 8388608.0;
  transformed.z -= 1.0;
  transformed.x /= 256.0;
  transformed.y /= -128.0;
  transformed.xyz *= transformed.w;
  transformed.y *= u.scissor_adjust * u.height_scale;
  // OpenGL clips z to [-w, w], Metal to [0, w].
  transformed.z = (transformed.z + transformed.w) * 0.5;
  out.position = transformed;

  out.vtx_color = in.rgba * light_color;
  out.vtx_st = in.st;
  return out;
}

fragment float4 merc2_frag(Merc2VertexOut in [[stage_in]],
                           constant Merc2Uniforms& u [[buffer(MetalBufferIndexUniforms)]],
                           texture2d<float> tex [[texture(0)]],
                           sampler samp [[sampler(0)]]) {
  float4 color;
  if (u.gfx_hack_no_tex == 0) {
    float4 T0 = tex.sample(samp, in.vtx_st);
    // All merc is TCC=RGBA and modulate.
    if (u.decal_enable == 0) {
      color = in.vtx_color * T0 * 2.0;
    } else {
      color = T0;
    }
    color.a *= 2.0;
  } else {
    color.rgb = in.vtx_color.rgb;
    color.a = u.decal_enable == 0 ? in.vtx_color.a * 2.0 : 1.0;
  }

  // The w of light_dir1_fade_en selects what the fade in light_dir0_fade.w does: replace the
  // alpha, or scale it. This is how the two halves of the double draw tell themselves apart.
  if (u.light_dir1_fade_en.w > 0.0) {
    color.a = u.light_dir0_fade.w;
  } else if (u.light_dir1_fade_en.w < 0.0) {
    color.a *= u.light_dir0_fade.w;
  }

  if (u.ignore_alpha == 0 && color.a < 0.128) {
    discard_fragment();
  }

  color.rgb = mix(color.rgb, u.fog_color.rgb, clamp(u.fog_color.a * in.fog, 0.0, 1.0));
  return color;
}
