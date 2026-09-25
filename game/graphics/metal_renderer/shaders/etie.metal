//
// Metal port of game/graphics/opengl_renderer/shaders/etie.{vert,frag} and
// etie_base.{vert,frag}.
//
// ETIE is the environment-mapped TIE geometry: the shiny props. It is drawn twice -- a base pass
// that looks like ordinary TIE, and a second pass that adds the reflection. The two have to line
// up to the pixel, which is why the base pass cannot use the tfrag3 shader: tfrag3 folds the
// perspective into one matrix, and ETIE keeps the camera rotation and the perspective apart, so
// the rounding differs and the second pass would z-fight the first.
//
// The reflection maths is a transcription of the VU program, kept in the same order and with the
// original instruction comments, because the order is what makes it match.
//
// Vertex layout is tfrag3::PreloadedVertex, using two fields nothing else here reads: the packed
// normal and the per-instance tint.
//

#include <metal_stdlib>
using namespace metal;

#include "metal_shader_types.h"

struct EtieVertexIn {
  float3 position_in [[attribute(0)]];
  float2 tex_coord_in [[attribute(1)]];
  ushort time_of_day_index [[attribute(2)]];
  float4 normal [[attribute(3)]];
  float4 proto_tint [[attribute(4)]];
};

struct EtieVertexOut {
  float4 position [[position]];
  float4 fragment_color;
  float2 tex_coord;
  float fogginess;
};

// The half of the transform both passes share: camera rotation, then the perspective split into
// two vectors the way the VU program had it.
static float4 etie_transform(constant EtieUniforms& u, float3 position, thread float4& vf17_out) {
  float4 vf17 = u.cam_no_persp[3];
  vf17 += u.cam_no_persp[0] * position.x;
  vf17 += u.cam_no_persp[1] * position.y;
  vf17 += u.cam_no_persp[2] * position.z;
  vf17_out = vf17;

  //;; perspective transform
  float4 p_proj = float4(u.persp1.x * vf17.x, u.persp1.y * vf17.y, u.persp1.z, u.persp1.w);
  p_proj += u.persp0 * vf17.z;

  //;; perspective divide
  float pQ = 1.0 / p_proj.w;
  float4 transformed = p_proj * pQ;
  transformed.w = p_proj.w;

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
  return transformed;
}

vertex EtieVertexOut etie_base_vert(EtieVertexIn in [[stage_in]],
                                    constant EtieUniforms& u [[buffer(MetalBufferIndexUniforms)]],
                                    const device float4* time_of_day
                                    [[buffer(MetalBufferIndexTimeOfDay)]]) {
  EtieVertexOut out;

  // Note: the GLSL computes this from a `camera` uniform that nothing sets, so it reads as zero
  // and the fog is the same for every vertex. Kept as-is: this is the reference backend's
  // appearance, and changing it here would be a difference between the two.
  out.fogginess = 255.0 - clamp(u.hvdf_offset.w, u.fog_min, u.fog_max);

  float4 vf17;
  out.position = etie_transform(u, in.position_in, vf17);

  if (u.decal == 1) {
    out.fragment_color = float4(1.0, 1.0, 1.0, 1.0);
  } else {
    // time of day lookup
    float4 color = time_of_day[in.time_of_day_index];
    // color adjustment
    color *= 2.0;
    color.a *= 2.0;
    out.fragment_color = color;
  }

  out.tex_coord = in.tex_coord_in;
  return out;
}

vertex EtieVertexOut etie_vert(EtieVertexIn in [[stage_in]],
                               constant EtieUniforms& u [[buffer(MetalBufferIndexUniforms)]]) {
  EtieVertexOut out;
  out.fogginess = 0.0;

  // rotate the normal
  float3 nrm_vf23 = u.cam_no_persp[0].xyz * in.normal.x + u.cam_no_persp[1].xyz * in.normal.y +
                    u.cam_no_persp[2].xyz * in.normal.z;

  float4 vf17;
  out.position = etie_transform(u, in.position_in, vf17);

  // This is the ETIE math. It is only right if nrm_vf23 is normalized first -- see the note in
  // the GLSL; the PS2's normal matrix carried a scale correction this one does not.
  {
    // nrm.z -= 1
    nrm_vf23.z -= 1.0;

    // dot = nrm.xyz * pt.xyz
    float nrm_dot = dot(vf17.xyz, nrm_vf23);

    // rfl = pt.xyz * nrm.z
    float3 rfl_vf14 = vf17.xyz * nrm_vf23.z;

    //;; Q_envmap = vf02.w / norm(rfl.xyz)
    float Q_envmap = -0.5 / length(rfl_vf14);

    //;; nrm.xy *= dot.x
    nrm_vf23.xy *= nrm_dot;

    //;; nrm.xy += rfl.xy
    nrm_vf23.xy += rfl_vf14.xy;

    //;; nrm.z = 1.0
    nrm_vf23.z = 1.0;

    //;; nrm.xy *= Q_envmap
    nrm_vf23.xy *= Q_envmap;

    //;; nrm.xy += vf03.w
    nrm_vf23.xy += 0.5;

    out.tex_coord = nrm_vf23.xy;
  }

  out.fragment_color = in.proto_tint * u.envmap_tod_tint;
  return out;
}

fragment float4 etie_frag(EtieVertexOut in [[stage_in]],
                          constant EtieUniforms& u [[buffer(MetalBufferIndexUniforms)]],
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
