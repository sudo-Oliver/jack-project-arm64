//
// Metal port of game/graphics/opengl_renderer/shaders/depth_cue.{vert,frag}
//
// Depth cue is the soft horizontal smear the PS2 lays over the whole frame. Both of its passes --
// frame into the scratch page, scratch page back over the frame -- use this one shader; what
// differs is the target, the blend and the colour.
//
// The vertices arrive in [0,1] screen space and are converted to clip space here, which is the
// only place the Metal depth range differs from the GLSL: OpenGL's clip z is [-1, 1] and Metal's
// is [0, 1], so the z conversion the GLSL does is simply not applied.
//
// Vertex layout is DepthCueCore::SpriteVertex.
//

#include <metal_stdlib>
using namespace metal;

#include "metal_shader_types.h"

struct DepthCueVertexIn {
  float2 xy [[attribute(0)]];
  float2 st [[attribute(1)]];
};

struct DepthCueVertexOut {
  float4 position [[position]];
  float4 fragment_color [[flat]];
  float2 tex_coord;
};

vertex DepthCueVertexOut depth_cue_vert(DepthCueVertexIn in [[stage_in]],
                                        constant DepthCueUniforms& u
                                        [[buffer(MetalBufferIndexUniforms)]]) {
  DepthCueVertexOut out;

  // Calculate color
  float4 color = u.u_color;
  color *= 2.0;  // correct
  out.fragment_color = color;

  out.tex_coord = in.st;

  // [0,1] to clip space. x and y span [-1, 1]; z spans [0, 1] on Metal, so it passes through.
  out.position = float4(in.xy.x * 2.0 - 1.0, in.xy.y * 2.0 - 1.0, u.u_depth, 1.0);
  return out;
}

fragment float4 depth_cue_frag(DepthCueVertexOut in [[stage_in]],
                               texture2d<float> tex [[texture(0)]],
                               sampler samp [[sampler(0)]]) {
  return in.fragment_color * tex.sample(samp, in.tex_coord);
}
