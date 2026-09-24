//
// Metal port of game/graphics/opengl_renderer/shaders/emerc.{vert,frag}
//
// emerc is the environment-mapped pass over a merc model: the same skinned vertex, but the
// texture coordinates come from the reflection of the view direction off the vertex normal,
// worked out with the arithmetic VU1 did. It is drawn on top of the merc pass, faded by a colour
// the game sends per draw.
//
// Differences from the GLSL are the ones merc2.metal has: the bone matrices come from a device
// buffer with the layout std140 produced, the uniforms are a struct, and the clip-space z is
// remapped from OpenGL's [-w, w] to Metal's [0, w].
//

#include <metal_stdlib>
using namespace metal;

#include "metal_shader_types.h"

struct EmercMatrixData {
  float4 X[4];
  float4 R[3];
  float4 pad;
};

struct EmercVertexIn {
  float3 position [[attribute(0)]];
  float3 normal [[attribute(1)]];
  float3 weights [[attribute(2)]];
  float2 st [[attribute(3)]];
  float4 rgba [[attribute(4)]];
  uchar4 mats [[attribute(5)]];
};

struct EmercVertexOut {
  float4 position [[position]];
  float3 vtx_color;
  float2 vtx_st;
  float fog;
};

static inline float4 emerc_transform(constant EmercMatrixData& bone, float4 p) {
  return bone.X[0] * p.x + bone.X[1] * p.y + bone.X[2] * p.z + bone.X[3] * p.w;
}

static inline float3 emerc_rotate(constant EmercMatrixData& bone, float3 n) {
  return bone.R[0].xyz * n.x + bone.R[1].xyz * n.y + bone.R[2].xyz * n.z;
}

vertex EmercVertexOut emerc_vert(EmercVertexIn in [[stage_in]],
                                 constant Merc2Uniforms& u [[buffer(MetalBufferIndexUniforms)]],
                                 constant EmercMatrixData* bones [[buffer(MetalBufferIndexBones)]]) {
  EmercVertexOut out;
  float4 p = float4(in.position, 1);

  float4 vtx_pos = -emerc_transform(bones[in.mats[0]], p) * in.weights[0];
  float3 rotated_nrm = emerc_rotate(bones[in.mats[0]], in.normal) * in.weights[0];

  if (in.weights[1] > 0) {
    vtx_pos += -emerc_transform(bones[in.mats[1]], p) * in.weights[1];
    rotated_nrm += emerc_rotate(bones[in.mats[1]], in.normal) * in.weights[1];
  }
  if (in.weights[2] > 0) {
    vtx_pos += -emerc_transform(bones[in.mats[2]], p) * in.weights[2];
    rotated_nrm += emerc_rotate(bones[in.mats[2]], in.normal) * in.weights[2];
  }

  float4x4 pm = u.perspective_matrix;
  float4 transformed = pm[0] * vtx_pos.x + pm[1] * vtx_pos.y + pm[2] * vtx_pos.z + pm[3] * vtx_pos.w;

  rotated_nrm = normalize(rotated_nrm);

  float Q = u.fog_constants.x / transformed.w;
  out.fog = 255.0 - clamp(-transformed.w + u.hvdf_offset.w, u.fog_constants.y, u.fog_constants.z);

  // The envmap texture coordinates, straight out of the VU1 code. The comments name the original
  // instructions so this stays checkable against them.
  float2 st_mod = in.st;
  {
    float4 vf10 = float4(rotated_nrm, 1);
    float4 vf08 = transformed;
    // unperspect (1/P(0,0), 1/P(1,1), 0.5, 1/P(2,3))
    float4 vf23 = float4(1.0 / pm[0][0], 1.0 / pm[1][1], 0.5, 1.0 / pm[2][3]);
    // mul.xyzw vf09, vf08, vf23 -- do unperspect
    float4 vf09 = vf08 * vf23;
    // subw.z vf10, vf10, vf00
    vf10.z -= 1.0;
    // addw.z vf09, vf00, vf09 -- xyww the unperspected thing
    vf09.z = vf09.w;
    // mul.xyz vf15, vf09, vf10
    float3 vf15 = vf09.xyz * vf10.xyz;
    // adday.xyzw + maddz.x
    float vf15_x = vf15.x + vf15.y + vf15.z;
    // div Q, vf15.x, vf10.z
    float qq = vf15_x / vf10.z;
    // mulaw.xyzw ACC, vf09, vf00
    float4 ACC = vf09;
    // mul.xyzw vf09, vf08, vf23
    vf09 = vf08 * vf23;
    // madd.xyzw vf10, vf10, Q
    vf10 = ACC + vf10 * qq;
    // eleng.xyz P, vf10
    float P = length(vf10.xyz);
    // div Q, vf23.z, vf10.w
    float qqq = vf23.z / P;
    // addaz.xyzw vf00, vf23
    ACC = float4(vf23.z, vf23.z, vf23.z, vf23.z + 1.0);
    // madd.xyzw vf10, vf10, Q
    vf10 = ACC + vf10 * qqq;
    // Jak 1's envmap comes out mirrored otherwise, because vtx_pos is negated above.
    st_mod.x = 1.0 - vf10.x;
    st_mod.y = 1.0 - vf10.y;
  }

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

  out.vtx_color = float3(u.fade.x, u.fade.y, u.fade.z);
  out.vtx_st = st_mod;
  return out;
}

fragment float4 emerc_frag(EmercVertexOut in [[stage_in]],
                           constant Merc2Uniforms& u [[buffer(MetalBufferIndexUniforms)]],
                           texture2d<float> tex [[texture(0)]],
                           sampler samp [[sampler(0)]]) {
  float4 color;
  if (u.gfx_hack_no_tex == 0) {
    float4 T0 = tex.sample(samp, in.vtx_st);
    color.a = T0.a;
    color.rgb = T0.rgb * in.vtx_color;
    color *= 2.0;
  } else {
    color.rgb = in.vtx_color;
    color.a = 1.0;
  }
  return color;
}
